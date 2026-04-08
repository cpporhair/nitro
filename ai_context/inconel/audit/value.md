# Value 模块 Phase 0 审计

> Spec-only audit。严守 nitro/CLAUDE.md 最高优先级规则：**不读** `apps/inconel/test/test_value.cc`、`test_tree_value.cc`。所有证据来自 design doc + production 源码。

## 1. 审计范围

### 1.1 已读 spec

| 文档 | 章节 |
|------|------|
| `runtime_state_machine.md` | §6 value_alloc_sched 全节（lines 857-1147） |
| `write_path_and_pipeline.md` | §5 Value 分配流程（lines 235-344） |
| `runtime_memory_and_cache.md` | §6 Cache/Pool 视图、§7 Owner、§8 Placement 耦合（lines 507-790） |
| `on_disk_formats.md` | §5 Value Object 格式（已在 tree audit 读过） |
| `cross_doc_contracts.md` | 全文 |
| `code_quality_standard.md` | 全文 |

### 1.2 已读源码

| 文件 | 行数 |
|------|------|
| `format/value_object.hh` | 162 |
| `format/types.hh` | 70（tree audit 已读） |
| `value/allocator.hh` | 237 |
| `value/scheduler.hh` | 1095 |
| `value/sender.hh` | 120 |

### 1.3 显式未读

| 文件 | 原因 |
|------|------|
| `test/test_value.cc` | 测试，规则禁止 |
| `test/test_tree_value.cc` | 测试，规则禁止 |
| `plan/006_value_scheduler.md` | plan 文件，与 spec 区分（同 tree audit 纪律） |
| `plan/007_value_cache_integration.md` | 同上 |

---

## 2. Spec 覆盖矩阵

| Spec 要求 | 实现状态 |
|----------|---------|
| `value_object_header` POD + sizeof=12 + magic/body_len/body_crc | ✓ format/value_object.hh + static_assert |
| Sub-LBA / LBA-equal / multi-LBA 三种 size class 支持 | ✓ format helpers + allocator |
| `persist_put_values` leader-follower | ✓ handle_persist + drain followers + commit/rollback |
| `read_value` (Point GET) | ✓ sender.hh + handle_read + handle_fill |
| **`read_page_values`（MultiGet/Scan 按页批读）** | **✗ 完全缺失** |
| **`freed_slots`（sub-LBA 回收 handle）** | **✗ 完全缺失** |
| **`recycle_whole`（整页回收 handle）** | **✗ 缺失**（allocator.recycle_whole_page 存在但无 scheduler-level handle） |
| **`install_recovered_state`（boot recovery 入口）** | **✗ 完全缺失** |
| **Per-class `hole_pages` (RSM §6.3)** | **✗ 完全缺失**——allocator.hh 注释明确 "v6 scope: no hole_pool" |
| **`dirty_pages` 跟踪（flat_hash_set<paddr>）** | **✗ 完全缺失** |
| **`deferred_freed`（dirty 期间回收暂存）** | **✗ 完全缺失** |
| **`generic_free_spans`（recovery 用）** | **✗ 完全缺失** |
| **`hole_reuse_watermark` + fresh_first/hole_first 双模式** | **✗ 完全缺失**——只有 fresh_first |
| **TRIM 顺序协议（RSM §6.9）** | **✗ 完全缺失** |
| **碰撞检测 `data_area_heads`** | **✗ 完全缺失**——allocator.hh 注释明确"v6: no shared collision detection" |
| dirty open frame 当 cache hit（同 batch 内复用） | partial via writable_pages_ 线性扫，跟 spec open_frames 概念不完全对应 |
| Readonly value_page cache | ✓ readonly_cache_ (Cache template) |
| CRC 校验在读路径 | ✓ handle_fill / serve_hit_or_fail 都过 try_decode_value |

**所占比例**：spec 列出的 6 个 handle 入口（persist / read / read_page_values / freed_slots / recycle_whole / install_recovered_state），代码只实现了 2 个（persist / read）。覆盖率约 33%。

这跟 audit/tree.md 的 INC-006 同源——是一个**写侧整体未实现**的 partial module，但**模块本身没在源码顶部声明这个 partial 状态**。

---

## 3. Critical Findings

### V1 — CRC corruption 走 throw / fail()，不是 panic

**位置**：
- `scheduler.hh:817-820` (handle_fill, post-NVMe corruption)
- `scheduler.hh:870-876` (serve_hit_or_fail, in-memory image corruption)

```cpp
// handle_fill
if (!body_opt) {
    item->fail(std::make_exception_ptr(std::runtime_error(
        "value::read: corrupt value object on disk (post-NVMe)")));
    delete item;
    return;
}
```

**fail() 走 PUMP exception path**——和 INC-004 (F1) 我们刚拒绝的 Option B 完全一样的 shape。原因和 INC-004 一致：

- catch 窗口存在，将来上层 any_exception 可能 silent 吞掉
- 强一致存储不应给 corruption 留可恢复路径
- value CRC fail 跟 tree CRC fail 是同质的"已检测到 corruption 仍 serve" 风险

**这是 Tier 1 + Tier 2**：Tier 1 因为是 silent corruption 路径；Tier 2 因为 (a) cb/fail 双 callback pattern 在整个 scheduler 全部 4 个 op 上都用（_value_persist / _value_finalize / _value_read / _value_fill），(b) 这就是 INC-004 plan 一开始想抄的 shape。**value 模块已经把"错的 fix"提前抄进去了**。

修复方向：corruption 路径换 panic（同 INC-004/005 共用 panic helper）；recoverable 错误（class 找不到、out-of-space）保留 fail() 走 exception 继续 OK。

---

### V2 — handle_persist out-of-space rollback 时 bump head 永久泄漏（已知，但只在注释里）

**位置**：`scheduler.hh:511-516`

```cpp
// fresh_bump / whole_page sources are simply
// dropped (they were never durable so this is safe — except the
// bump head has already moved, which leaks Data Area capacity).
// v6 accepts this leak: persist failures here only happen on
// out-of-space conditions and the runtime is about to terminate.
```

**问题**：rollback_pages 对 fresh_bump 和 whole_page source 直接 drop——bump head 已经向下移过的位置不回退。注释假设 "runtime is about to terminate"，但：

- 如果 runtime 不立即终止（caller catch 了 fail() 异常继续运行），泄漏的 LBA 永远不可恢复
- v1 还没有 free pool 重用机制（INC-009 的 hole_pool 不存在），泄漏直接等于丢容量
- 这是**有意接受的状态损坏**，但 spec 没说"out-of-space 必然终止"——是代码注释自己假定的

**Tier 1**：silent state degradation。

修复方向：要么 bump head 真的回退（需要 allocator 加 push_back_bump 接口），要么 panic 不接受 caller 继续运行（跟 V1 同方向，corruption + 资源耗尽都 panic）。**第二种和强一致原则一致，更简单**。

---

### V3 — 6 个 spec handle 缺 4 个，scheduler.hh 顶部没声明

跟 audit/tree.md F4 / INC-006 同质，但更严重——value 缺的 4 个 handle 全是恢复 / 回收路径：
- `read_page_values` (MultiGet 用)
- `freed_slots` (sub-LBA 回收，连带整套 dirty_pages / deferred_freed / hole_pages 体系)
- `recycle_whole` (整页回收 + TRIM)
- `install_recovered_state` (boot recovery)

allocator.hh 顶部有局部 scope 注释（"v6 scope: no hole_pool / freed_slots..."）但 scheduler.hh 没有。读 scheduler.hh 的人不知道这是 partial implementation。

**Tier 2**：未来 AI 实现 sider/aisaq 类似的 value scheduler 时会照着这个文件抄，连带把"4 个 handle 缺失没声明"这件事抄走。

修复方向：scheduler.hh 顶部加 module-level 注释说明 partial scope（同 INC-006 plan）。这一条是 INC-006 的 value 镜像。

---

### V4 — `value::scheduler` 命名跟 spec `value_alloc_sched` 不一致

**位置**：`scheduler.hh:320` `template <core::cache_concept Cache> struct scheduler : scheduler_base`

类型名是裸 `scheduler`（在 `apps::inconel::value` namespace 下用就是 `value::scheduler`）。spec 用 `value_alloc_sched`。

**这是 F13/INC-014 的 value 镜像**：同样的"通用名 + 模块 namespace"模式，user 已经报告过 context 大时 AI 会混淆名字。在已经有 `tree::lookup_scheduler` 和 `value::scheduler` 共存的情况下，AI 写跨模块代码时已经在 confusion 区间。

**Tier 2 urgent**——直接命中 user 报告过的 AI failure mode。

修复方向：重命名 `value::scheduler` → `value::value_alloc_sched` (或 `value_alloc_sched_t`)，`value::scheduler_base` → `value::value_alloc_sched_base`。

---

### V5 — std::unordered / std::vector 而不是 absl 容器

跟 INC-009/010 同源，value 内部全是 std::* 而不是 spec 用的 abseil 容器：

| Spec | 实际代码 |
|------|---------|
| `flat_hash_map<paddr, hole_page_descriptor> hole_pages` | 没实现，但实现时按 std::* 写的可能性 100% |
| `flat_hash_set<paddr> dirty_pages` | 同上 |
| `flat_hash_map<paddr, bitset> deferred_freed` | 同上 |
| `small_vector<per_class_state, 16> classes` | `std::vector<per_class>` |
| `small_vector<value_page_frame*, 16> open_frames` | `std::vector<std::vector<page_data>>` writable_pages_ |
| `inflight_rounds_` 用 `std::map<uint64_t, ...>` | spec 没显式说，但 ordered map 在这里没必要——只按 round_id lookup，flat_hash_map 更合适 |

**Tier 2**：value 模块当前用 std::* 等于把"和 spec 不一致的 pattern"巩固。INC-010 的全面替换需要把 value 模块也覆盖。

修复方向：直接归到 INC-010（"全面替换为 absl 等价物"）的实施范围内，不开新条目。

---

## 4. 其他 Findings（Tier 2 weak / Tier 3）

### V6 — `writable_pages_` 概念跟 spec 不直接对应

**位置**：`scheduler.hh:350` `std::vector<std::vector<page_data>> writable_pages_;`

spec §6.3 区分两个概念：
- `hole_pages`：被回收路径放回的 partially-free page（recovery / reclaim 来源）
- `open_frames`：当前 round 正在填充的 page（in-flight）

代码把这两个概念合成一个 `writable_pages_[ci]`——表示"已 durable 但还有 free slot 的 page，下一轮直接复用"。这跟 spec 的 open_frames 更接近，但 spec 的 open_frames 是单 page per class（最多 1 个），代码是 vector 多个。

不算 bug——concept drift 但内部一致。但是**当 INC-006 等价的写侧落地、加 hole_pages 时，这个 vector 会跟 hole_pages 撞**。需要决定：合并 / 分开 / rename 哪个。

Tier 2 weak（concept drift 会被新代码继承）。

### V7 — `try_decode_value` 把 4 种 decode 错误折叠成 nullopt，丢失诊断信息

**位置**：`scheduler.hh:854-861`

```cpp
auto d = format::decode_value_object(slot, vr.len);
if (!d.ok()) return std::nullopt;
```

`value_decode_status` 有 4 个值（truncated / bad_magic / bad_body_len / bad_crc），decode 函数返回了具体哪种，但 `try_decode_value` 把它们合并成 nullopt。caller 拿到的 fail() exception 只有 "corrupt value object" 字符串，不知道具体是哪种 corruption。

Tier 3（局部）—— 不影响正确性，只影响 incident 发生时的诊断信息。修复跟 INC-004/005 panic helper 走同一种"详细诊断"路径合并：panic 时把具体 decode_status 写到 stderr。

### V8 — `handle_persist` 用 goto round_failed 跳出嵌套循环

**位置**：`scheduler.hh:467-470`

```cpp
for (auto* item : all_items) {
    for (auto& entry : item->entries) {
        if (!persist_one_entry(*rnd, entry)) {
            failure = std::make_exception_ptr(...);
            goto round_failed;
        }
    }
}
```

嵌套循环里跳出用 goto。可读性 OK 但是个 smell。Tier 3。

### V9 — `~scheduler()` 手动 drain cache buffer

**位置**：`scheduler.hh:399-403`

```cpp
~scheduler() {
    while (auto e = readonly_cache_.evict_one()) {
        delete[] e->buf;
    }
}
```

cache concept 不负责 free buffer（只 own slot/node 数组），所以析构时 scheduler 必须手动 drain。这是 cache_concept 接口设计问题，不是 scheduler 问题。Tier 3，可以延后。

### V10 — `class_sizes_storage_` + `class_sizes_view_` 双字段

**位置**：`scheduler.hh:891-897`

`storage_` 是 owning vector，`view_` 是 span 指向 storage 的内容。目的是让构造时传入的 span 不必 outlive scheduler。不算 bug，但用 `std::vector<uint32_t>` 直接做 const 字段一样能解决。Tier 3 quality。

### V11 — `inflight_rounds_` 用 `std::map<uint64_t, unique_ptr<round>>` 而不是 hash map

**位置**：`scheduler.hh:373`

round_id 是单调递增 uint64_t，按 id lookup，不需要 ordered。`std::map` 是 RB-tree，每次操作 O(log N) + node alloc。flat_hash_map 是 O(1) + open addressing。

包含在 INC-010 的范围内（用 absl::flat_hash_map 替换）。

### V12 — `read_q_` 和 `fill_q_` 用 .drain()，`persist_q_` 用 while + try_dequeue

**位置**：`scheduler.hh:412-435`

asymmetry。原因有解释（leader-follower 需要从内部 drain followers），不是 bug。但是 pattern 不一致，新人读会困惑。Tier 3 quality。

### V13 — `read_page_values` 完全缺失，意味着 MultiGet / Scan 走不通

spec §6.5 把 `read_page_values` 列为读路径的核心入口（按 page 分组批量读），代码只有 single-value `read_value`。MultiGet / Scan 性能路径在 value 这一层断掉。

Tier 2（功能缺失会影响后续 read pipeline 实现的形态）。修复方向：等 read pipeline 落地时一起加。可以单独建条目或归到 read pipeline 阻塞的 INC。

### V14 — `handle_persist` 没有 batch size 上限

`handle_persist` drain 整个 persist_q_ 当成一轮，理论上一轮可以包含任意多个 entries（所有当前排队的 + leader）。memory 增长有限但单轮延迟不可控。

Tier 3（性能 / latency 问题，不是正确性）。

---

## 5. 与现有 INC 的关系

| audit/value finding | 对应 / 扩展的 INC |
|---|---|
| V1（CRC fail throw not panic） | **新条目**——是 INC-004/005 的 value 镜像 |
| V2（bump head leak on rollback） | **新条目**——独立的 silent state degradation |
| V3（4 个 handle 缺失没声明） | **INC-006 镜像**——可以扩 INC-006 也可以新建 |
| V4（value::scheduler 命名） | **INC-014 镜像**——可以扩 INC-014 也可以新建 |
| V5（std::* 容器） | **INC-009/010 范围**，不开新条目 |
| V6（writable_pages_ vs hole_pages concept drift） | **新条目**或归到 V3 一起 |
| V7（decode_status 合并） | 跟 V1 一起处理（panic helper 加诊断） |
| V13（read_page_values 缺失） | **新条目**或归到 read pipeline 阻塞 |

约 4-5 条新 INC 需要建。

---

## 6. 跟 tree audit 的对照

value 比 tree 状况**略好的地方**：

- `value_object_header` 是 packed POD + static_assert（INC-008 在 internal_record 上的问题，value 没有同型问题）
- `handle_finalize` 的 unknown round_id 用 `std::abort + fprintf`——**这正是我们刚决定 INC-004/005 应该用的形态**（panic + diagnostic）。是个**正面例子**，可以参考做 panic helper

value 比 tree 状况**更差的地方**：

- 6 个 spec handle 缺 4 个（tree 只缺写侧整体）
- cb/fail 双 callback pattern 已经建立——INC-004 的 Option B (throw) 错误结构已经在 value 模块"先实现了"
- 模块顶部没有 scope 声明（同 tree 的 INC-006 问题）
- 命名问题（value::scheduler）— Tier 2 已经被 user 报告过

**最重要的发现**：value 模块的 cb/fail dual callback pattern 已经把 INC-004 我们想避免的 throw 风格"提前抄了进去"。如果不和 INC-004/005 一起重做，未来 panic helper 改 tree 不改 value 会留下两套不一致的 corruption 处理路径——AI 写跨模块代码时一定会被绊倒。

---

## 7. 审计纪律记录

| 项目 | 状态 |
|------|------|
| 打开过任何 test 文件 | 否 |
| 打开过任何 plan 文件 | 否 |
| "想读测试"的冲动 | 0 次（已经习惯 spec-anchored 节奏） |
| 读完整 spec 章节作为信任源 | 是（RSM §6 全节、WP §5、RMC §6-8） |
| 每条 finding 标 spec 依据 | 是 |
| 每条 finding 标 Tier | 是 |
