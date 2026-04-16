# Tree 模块 Phase 0 审计

> Spec-only audit。本审计严格遵守 nitro/CLAUDE.md 最高优先级规则：实现阶段禁止读测试文件。审计同样不读 `apps/inconel/test/test_tree_*.cc`、`test_tree_value.cc`、`test_runtime.cc`。审计的唯一证据来源是 design doc 和 production 源码。

## 1. 审计范围

### 1.1 已读的 spec（信任源）

| 文档 | 章节 |
|------|------|
| `design_doc/on_disk_formats.md` | 全文（重点 §4 Tree Page 格式）|
| `design_doc/design_overview.md` | §10 Tree 与 Value Area 规范（lines 1228-1473）|
| `design_doc/runtime_state_machine.md` | §4 tree_sched + tree_lookup_sched（lines 548-761）|
| `design_doc/read_api_and_pipeline.md` | §4 Point GET（lines 123-300）|
| `design_doc/flush_and_frontier_switch.md` | §3 Phase 2 Fold + Write Tree（lines 63-279）, §9 Consolidation（lines 572-613）|
| `design_doc/cross_doc_contracts.md` | 全文（24 个 handle 签名 + 三条红线 + struct 字段）|
| `design_doc/code_quality_standard.md` | 全文（hot path、ownership、抽象边界质量红线）|
| `design_doc/code_modules.md` | tree/ 模块归属段落 |

### 1.2 已读的 production 源码

| 文件 | 行数 |
|------|------|
| `apps/inconel/format/types.hh` | 70 |
| `apps/inconel/format/tree_page.hh` | 74 |
| `apps/inconel/core/tree_manifest.hh` | 45 |
| `apps/inconel/core/registry.hh` | 163 |
| `apps/inconel/tree/lookup.hh` | 38 |
| `apps/inconel/tree/page_builder.hh` | 161 |
| `apps/inconel/tree/page_reader.hh` | 183 |
| `apps/inconel/tree/scheduler.hh` | 403 |
| `apps/inconel/tree/sender.hh` | 95 |

总计 production 源码 ~1232 行。

### 1.3 显式未读的文件

| 文件 | 原因 |
|------|------|
| `apps/inconel/test/test_tree_lookup.cc` | 测试文件，规则禁止 |
| `apps/inconel/test/test_tree_lookup_multicore.cc` | 测试文件，规则禁止 |
| `apps/inconel/test/test_tree_value.cc` | 测试文件，规则禁止 |
| `apps/inconel/test/test_runtime.cc` | 测试文件，规则禁止 |
| `apps/inconel/ai_context/inconel/plan/002_tree_page_data_structure.md` | 项目 plan，与 spec 区分；本轮审计只对照 spec |
| `apps/inconel/ai_context/inconel/plan/003_tree_lookup_scheduler.md` | 同上 |

**审计期间的"想读测试"冲动记录**：
- 第 1 次：grep 出 `internal_page_builder` / `leaf_page_builder` 只在 4 个 test 文件被使用，自然想知道 tests 用哪种 page shape 构造 tree。**没读**——只用 grep 的"文件名出现位置"作为结构性事实，没有打开任何 test 文件读其内容。

---

## 2. Spec 摘要：tree 模块应当实现什么

按 spec，tree 模块（含 tree_sched 写侧 + tree_lookup_sched 读侧）必须提供：

### 2.1 写侧（`tree_sched`，RSM §4.1-4.6, FF §3, ODF §4）

- `tree_state { tree_allocator alloc, flush_max_lsn, superblock_safe_lsn, recovery_safe_lsn, reclaim_q }` (RSM §4.1)
- `tree_allocator { head, free_ranges, shared_heads }`，bump 分配 + free pool（RSM §4.4）
- `tree_manifest` 构造：immutable snapshot，每次 flush 产新 manifest（RSM §4.5）
- `checkpoint_guard { manifest, retired { old_slots, old_ranges, old_tree_values } }` 析构函数挂 reclaim（RSM §4.6）
- `flush(eligible_gens)` handle：fold（§3.2）→ 写新 leaf slots（§3.4）→ shadow slot 选择（§3.5）→ consolidation（§3.6, §9）→ root 变化处理（§3.7）→ 新 manifest 构造（§3.8）→ NVMe FLUSH（§3.9）
- `reclaim(reclaim_task)` handle：TRIM 旧 slot/range，投递 value 回收
- `update_superblock(root_base_paddr, covered_lsn)` handle，root-change flush 后异步更新
- Data Area 碰撞检测（与 value_alloc_sched 共享 atomic heads，RSM §4.3）
- `recovery_safe_lsn` 推进（RSM §4.8）

### 2.2 读侧（`tree_read_domain` / lookup arm，RSM §4.7, RAP §4）

- `tree_read_domain<Cache>` owns `node_cache`（`readonly_frame_cache`, tree_node domain）+ `partitions` snapshot + lookup/worker unique_ptrs；lookup 持 `tree_read_domain<Cache>*` back-ref，通过 `read_domain_->node_cache.*` 访问 cache，零虚调用（step 030 §2.3 / §6.1 A）
- `shard_idx = core::registry::current_shard_partitions()->route(key)` shard-partition 路由（INC-040 / step 030 §2.7 收敛）：单次二分（成本 `log2(shard_count)`），不触碰 leaf；同 shard 的 key 永远落到同一 read_domain；map 由 builder 启动时装占位，`tree_sched` 每次 flush 完成后基于新 `leaf_order` 重建并 `install_shard_partitions()` 原子替换（B1 目标设计，step 030 暂未触发 rebuild）；NUMA 只影响 read_domain 实例的核心安放、不改变路由算法
- `tree_lookup(key, manifest)` handle，按 RSM §4.7 步骤 1-8：
  1. 空 manifest → absent
  2. 从 root_slot 开始
  3. 沿 manifest 下降 internal levels
  4. leaf record lookup
  5. 返回 `variant<leaf_value_record { data_ver, value_ref }, leaf_tombstone { data_ver }, absent>`
- 数据源契约（cross_doc_contracts §4）：manifest 必须来自 `read_handle.cat->prs->tree_guard->manifest`（snapshot），**不是** tree_sched 的当前 manifest

### 2.3 格式（ODF §4）

- `tree_slot_header` 19 bytes：magic / format_version / type / record_count / free_space_offset / page_crc
- `internal_record` 变长：`uint16_t key_len + key_bytes + paddr child_base`，**child_base 是 range_base，不是 slot paddr**
- internal node layout：header + N 个 internal_record + 末尾 rightmost_child_base (paddr)
- `leaf_record_header` 11 bytes：data_ver + kind + key_len
- value record：header + key_bytes + value_ref (18 bytes)
- tombstone record：header + key_bytes
- shadow range：X 个连续 slot，X = `shadow_slots_per_range`
- 空 slot 识别（§4.5）：前 4 bytes 全零 → TRIM 后未使用；== TREE_PAGE_MAGIC → 校验 CRC；其他值 → 非法
- CRC：CRC-32C，覆盖整页除 page_crc 字段本身的 4 bytes
- 典型扇出：leaf ~268（avg key 32B），internal ~371

### 2.4 红线（cross_doc_contracts §6.2）

1. ❌ 运行时 cache miss 时扫描 shadow range 的多个 slot 选最新（应由 manifest.resolve 直接给出精确 slot）
2. ❌ 需要 "每个 range 只剩一个 slot" 才能正常读
3. ❌ 把 flush/recovery 理解为 "重写回 slot 0"

---

## 3. 现存实现：实际是什么

| 模块组件 | 实现位置 | 状态 |
|---------|---------|------|
| 格式 POD（slot_header / leaf_record_header）| `format/tree_page.hh` | 与 spec 一致（19 / 11 字节） |
| `internal_record` POD 定义 | — | **缺失**：spec 在 §4.2 列了 struct 定义，源码无对应 type；只在 page_builder/page_reader 里散文 memcpy |
| `paddr` / `value_ref` | `format/types.hh` | 与 spec 一致 |
| CRC 函数 | `format/tree_page.hh::tree_page_compute_crc` | 与 spec 一致：覆盖整页除 page_crc 4 bytes |
| `tree_manifest` runtime container | `core/tree_manifest.hh` | 部分一致：有 root_slot + slot_map + resolve；模块归属可议（见 F6）|
| `leaf_page_builder` / `internal_page_builder` | `tree/page_builder.hh` | 单页构造原语；**没有任何 production 调用方**（grep 显示只在 4 个 test 文件被引用）|
| `leaf_page_reader` / `internal_page_reader` | `tree/page_reader.hh` | 单页解析；linear scan |
| `lookup_scheduler<Cache>` | `tree/scheduler.hh` | tree_lookup_sched 的实现，PUMP 6 组件齐全；持有 tree_node cache 模板参数 |
| `lookup()` sender | `tree/sender.hh` | 多 key batch lookup pipeline，loop + concurrent NVMe read |
| `tree_sched`（写侧） | — | **完全缺失**；registry.hh L53-55 已显式标 "Future scheduler slots (placeholder)" |
| `tree_allocator` | — | 缺失 |
| `checkpoint_guard` | — | 缺失 |
| `flush()` handle | — | 缺失 |
| `reclaim()` handle | — | 缺失 |
| Data Area 碰撞检测 | — | 缺失 |
| `current_shard_partitions()->route(key)` 路由 | ✓（shard_partition 二分，INC-040 / step 030 最终形态） | `core/shard_partition.hh` + `core/registry.hh::{install,current}_shard_partitions` + `tree/sender.hh::lookup(keys, manifest)` 内部 fan-out via `tree_read_domain_at(idx)->lookup_sched` |

---

## 4. 关于"B+ tree 硬编码两层"问题的直接回答

用户的具体问题：当年的硬编码两层作弊，现在还在不在？

### 4.1 我在 production 代码里直接找了什么

| 检查点 | 文件 | 结论 |
|--------|------|------|
| 下降循环是否有 hardcoded depth | `tree/scheduler.hh::process_entries` L255-294 | **没有**。`while (true)` + 内层 `if (hdr->type == leaf) break` + 内部节点 `e.next_page = manifest->resolve(child)` continue。深度由 manifest 驱动 |
| `tree_manifest` 是否限定容量 | `core/tree_manifest.hh` | **没有**。`std::unordered_map<paddr, uint32_t>` 任意大小，有几个 range 装几个 |
| `make_lookup_state` 是否预设深度 | `tree/scheduler.hh` L46-62 | **没有**。e.next_page 初始化为 root，descent 全靠 manifest |
| `internal_page_reader::find_child` 是否假设深度 | `tree/page_reader.hh` L145-157 | **没有**。返回 `entry.child_base`，由调用方继续下降 |
| `prepare_reads` 是否只读一层 | `tree/scheduler.hh` L296-318 | **没有**。每轮处理"所有未解析 entry 的 next_page"，pipeline 多轮迭代直到 all_done |
| Grep 硬编码常量 (depth, level, max_levels 等) | tree/ 全目录 | 无命中 |

### 4.2 我没法直接验证的事

production 代码本身不限制深度。**但**：

```
grep "internal_page_builder|leaf_page_builder" apps/inconel/
→ tree/page_builder.hh                       (定义)
→ test/test_runtime.cc                       (使用)
→ test/test_tree_lookup.cc                   (使用)
→ test/test_tree_lookup_multicore.cc         (使用)
→ test/test_tree_value.cc                    (使用)
```

**没有任何 production 代码调用 page builder。** 所有 tree 页构造都发生在 4 个 test 文件里。tree_sched 写侧整体缺失，所以正常运行时根本不存在"production 路径构造 tree 页"这件事。

这意味着：
- production 的 lookup descent 代码看上去深度无关，但在真实运行环境里**从未跑过深度 > 1 的下降**——因为根本没有 production 代码生成多层树
- descent 是不是真的对 ≥ 3 层正确，**只取决于 4 个 test 文件构造了多深的树**
- 我没法直接验证 tests 构造的树深度，**所以我无法肯定 multi-level descent 在被实际验证的范围内**

### 4.3 结论

- 当年的硬编码两层作弊是否还在 production 源码里：**没找到**。下降逻辑本身是 depth-agnostic 的
- 但这个发现的可信度**有上限**：production 代码完全没人调用 page builder，所以"代码看起来支持任意深度"这件事在 production 路径上零证据
- 如果 tests 只构造 2 层树（这是用户当时报告的状态），那么 multi-level descent 自从那天起就**没被任何东西真正验证过**——它可能是对的，也可能在某次重构里悄悄碎了，没人会知道
- **这是 constraint A 直接命中的情况**：实现实际只在某个 shape 上被验证过，但代码没声明这个限制，名字也不带限定词

需要用户回答的问题：
1. 现在的 4 个 test 文件构造的 tree 最深是几层？（我不能读 tests）
2. 如果只到 2 层，那 multi-level descent 是否要在 tree_sched 写侧落地之前先用 property test 单独验证一次？

---

## 5. Critical Findings（必须修，constraint A/B/C 直接命中）

### F1 — CRC 失败被静默映射成 lookup_absent

**位置**：`tree/scheduler.hh:263-267`
```cpp
if (!tree_page_validate(page, s.page_size)) {
    e.result = lookup_absent{};
    e.resolved = true;
    break;
}
```

**问题**：tree_page_validate 同时检查 magic 和 page_crc。如果 page CRC 校验失败（torn write / 物理损坏），代码把它当成"key 不存在"返回。读者拿到 absent 完全无法分辨"key 真的不存在"和"页损坏，结果不可信"。

**Spec 依据**：ODF §4.5 把"== magic 但 CRC 不通过"明确定为 "torn write / 损坏 slot" 状态，是异常路径而不是数据语义。

**Constraint A 直接违反**：`silent fallback`。规则要求一旦输入超出覆盖范围必须 fail-fast（"返回 unsupported_*、抛错或断言失败，按模块层级选择"）。当前是把异常路径默默并入正常路径。

**修复方向**：lookup_result variant 增加 `lookup_corrupted { paddr slot, uint32_t expected_crc, uint32_t actual_crc }` 分支；或者抛 `tree_page_corrupted_error` 让 pipeline 异常路径接管。不能继续保留 silent absent。

---

### F2 — `tree_manifest::resolve` 用 assert 在 Release 退化为 UB

**位置**：`core/tree_manifest.hh:24-28`
```cpp
paddr resolve(paddr range_base) const {
    auto it = slot_map.find(range_base);
    assert(it != slot_map.end());
    uint32_t idx = it->second;
    return { range_base.device_id, ... };
}
```

**问题**：`-O3 -DNDEBUG` Release 构建里 `assert` 被去掉。slot_map 找不到时 `it == end()`，下一行 `it->second` 是 UB（解引用 end iterator）。

**Memory 中 `bug_release_assert_sideeffect.md` 的变体**：那条 feedback 是关于 assert 表达式自身有副作用；这里不同——assert 本身没副作用，但**后续逻辑依赖 assert 守住的不变量**。Release 里同样会出 UB（不一定 crash，可能产出垃圾 paddr 然后 NVMe 去读乱地址）。

**Spec 依据**：spec 没规定具体错误处理方式，但 §10.0 红线之一是"manifest 应精确告诉每个 range 的 slot"——如果 manifest 缺失某 range_base，这是 manifest 构造侧的 bug，应当 hard-fail，不应吞掉。

**Constraint A 违反**：silent fallback（Release 下的 UB 也算 silent，因为没人能从 lookup 失败分辨出 manifest 缺失）。

**修复方向**：换成 `throw std::logic_error("manifest missing range_base")` 或 `if (it == end()) std::abort()`。

---

### F3 — Multi-level lookup descent 缺乏作用域声明（核心 constraint A 命中）

**位置**：`tree/scheduler.hh::lookup_scheduler` 全类，特别是 L255-294 `process_entries`

**事实**：
1. descent 代码本身 depth-agnostic（§4.1 已验证）
2. **没有任何 production 代码构造 tree 页**（grep 证据）
3. tree_sched 写侧未实现，所以 production 路径不会生成深层 tree
4. 所以 descent 在 production 环境下的"已验证深度" ≤ 0（零次执行）；在测试环境的已验证深度 = 用户当年报告的"两层"，本审计无法独立确认这个数字现状

**Constraint A 应当要求**：
- 类名应带限定词（例如 `lookup_scheduler_unverified_depth` 或 `lookup_scheduler_partial`），或
- 类内有显式注释说明"production 已验证深度 = X，超出深度行为 unspecified"，或
- descent 时对 depth 计数，超过 X 时 abort/throw 强制 fail-fast

**当前实际**：类名是普通 `lookup_scheduler`，类内没有任何 scope 注释，没有 depth 计数器。一个新读这份代码的人会假设它是通用 B+ tree lookup，与实际验证范围不符。

**修复方向**（user 必须先回答 §4.3 的两个问题，否则 constraint C 触发）：
- 需要用户告诉我"4 个 test 文件实际构造的最大 tree 深度 = X"
- 然后在 `lookup_scheduler` 类顶部加注释 + descent 中加 `if (depth > X) std::abort("multi-level descent not verified beyond depth X")`，直到 tree_sched 写侧能稳定生成深层树为止

---

### F4 — `tree/scheduler.hh` 内部没有声明 tree_sched 写侧整体缺失

**事实**：tree_sched 写侧（allocator / fold / flush / reclaim）整体缺失。registry.hh L53-55 显式声明了这一点（"Future scheduler slots (placeholder, not implemented yet)"）。但 `tree/scheduler.hh` 本身没有。

**Constraint A 评级**：registry.hh 那行声明覆盖了"global 视角下哪些 sched 不在"，但**模块内部的 scope 声明**仍然缺失。读 `tree/scheduler.hh` 的人看到 `lookup_scheduler` 不会立刻意识到"这个模块只实现了读路径，写路径不存在"。

**修复方向**：在 `tree/scheduler.hh` 文件顶部加 module-level 注释：
```
// Tree module v1 partial implementation:
//   ✓ tree_read_domain + tree_lookup_sched (this file: lookup_scheduler)
//   ✗ tree_sched (write side): allocator, fold, flush, reclaim, manifest construction
//     — placeholder in core/registry.hh; not implemented
//   ✓ shard_partition_map routing (core/shard_partition.hh + core/registry.hh
//     + tree::lookup sender; INC-040 / step 030)
//   This module's lookup descent has no production path that constructs trees;
//   verified depth is determined entirely by test fixtures.
```

---

### F5 — `lookup_scheduler` 持有的 buffer 不是 DMA 内存（constraint A）

**位置**：`tree/scheduler.hh:170, 307`
```cpp
std::vector<std::unique_ptr<char[]>> owned_bufs_;
...
owned_bufs_.push_back(std::make_unique<char[]>(s.page_size));
```

**问题**：`std::make_unique<char[]>` 产生普通堆内存。SPDK NVMe 必须用 `spdk_dma_malloc` 或 `spdk_dma_zmalloc` 分配的 DMA-capable buffer（pinned + iova-mappable）。当前 buffer 直接传给 nvme scheduler 的 read 接口；mock_nvme 接受任何内存，所以现在能跑通；切真实 SPDK nvme 时**会立刻挂**。

**Constraint A 直接违反**：实现只在 mock_nvme 后端工作，未在代码里声明这个限制；切到真 nvme 时不是降级而是直接错。

**修复方向**：要么
- 在类顶加注释明确声明 "v1 mock_nvme only; switch to spdk_dma_malloc when nvme/ lands"，或
- 用一个 `BufferPool` 抽象，mock 模式 std::make_unique，spdk 模式 spdk_dma_malloc，由 build 时切换

注意：feedback memory 里有 `feedback_layered_complete_not_prototype.md`——这种"先用普通内存跑通，后面再换"的状态正是该 feedback 警告的"半成品扩散"。

---

## 6. Significant Findings（设计偏差，constraint B/C 范畴）

### F6 — `tree_manifest` 在 `core/` 而不是 `tree/`，归属不明确

**事实**：
- `code_modules.md` 把 tree_manifest 列入 tree 模块的状态（"manifest 构造" 是 tree_sched 责任）
- spec RSM §4.5 把 tree_manifest 当作 tree_sched 拥有的 immutable snapshot
- 实际代码在 `core/tree_manifest.hh`

**判断**：core/ 包含"被多个 scheduler 引用的 domain object"，tree_manifest 既被 tree_sched（构造）又被 tree_lookup_sched（只读）引用，放 core/ 也说得通。但 spec 没明确划线。

**Constraint C 触发**：spec 不足以唯一决定结构——core/ 还是 tree/ 都说得通。当前实现选了一种但没有任何注释解释为什么。

**修复方向**：要么 (a) 在 `core/tree_manifest.hh` 顶加注释解释为什么放 core/（"多 scheduler 共享"），要么 (b) 移到 `tree/manifest.hh`。在 user 决定之前不要默默选一种。

---

### F7 — ~~`home_tree_lookup_for_front` 路由缺失，`lookup()` sender 让 caller 直接传 sched 指针~~ **已解决（INC-040 / step 030 最终形态）**

**原问题**：spec RSM §4.7 严格要求 "same key → same tree_lookup_sched"（cache 局部性）。当时 `lookup()` 让 caller 传任意 sched 指针，没有 enforcement；registry 的路由 helper 也只是注释掉的 placeholder，写法还是错的 hash routing（`front_owner % K`）。

**收敛方式（INC-040 + INC-003；step 030 重做了路由层并立起 `tree_read_domain`）**：

1. **Routing carrier 换成 `shard_partition_map`**（step 030 §2.1 / §6.4 F2）
   - 前一轮 INC-040 用过的 `leaf_order.find_leaf_for_key(key) % K` 有两个问题：lookup 和 flush fold 决策不一致（fold 用 `leaf_idx * P / leaf_count` range partition），而且 modulo 丢失 range 局部性。step 030 改成全局 `shard_partition_map`：`shards[]` 按 fence_upper 升序 sorted，`route(key)` 一次二分（成本 `log2(shard_count)`），最后一个 shard 带 +∞ sentinel。
   - `core/registry.hh::{install,current}_shard_partitions` 两个 inline helper 负责原子替换 `shared_ptr<const shard_partition_map>`；`builder.hh` 启动时装占位单 shard map（空 leaf_order → `{upper=+∞, idx=0}`）；`tree_sched` 每次 flush 完成后重建并 install（B1 目标设计，step 030 仅落地 API）。
   - `memtable_fold::build_key_partitions` 和 `tree::lookup` 内部都改成调 `current_shard_partitions()->route(key)`，两条路径**共用同一张 map**，保证 "同 key → 同 shard" 不变量。

2. **Scheduler own 结构立 `tree_read_domain<Cache>`**（step 030 §2.3 / §6.5 G1）
   - PUMP runtime tuple 从 `<... tree_lookup_sched<Cache>, tree_worker_sched<Cache>, ...>` 折成 `<... core::tree_read_domain<Cache>, ...>`；read_domain 通过 `advance()` 代驱 lookup 和 worker 两个 arm。
   - `tree_node` `Cache` 成员从 `tree_lookup_sched` 搬到 `tree_read_domain`；lookup 和 worker 共享 read_domain 的 `node_cache` shard，两者都在同一 `Cache` 类型上模板化（零虚调用）。
   - `tree_read_domain_index` 字段上移到 `tree_read_domain_base`；scheduler base 通过 virtual `read_domain_index()` 反向读，仅诊断路径使用。
   - registry 只维护 `tree_read_domains.list` / `by_core`；旧的 `tree_lookup_scheds` / `tree_worker_scheds` / `tree_lookup_at` / `tree_worker_at` 全部移除。

3. **public API 形状不变**：`tree::lookup(keys, manifest)`（无 sched 指针）；caller 永远不知道具体 read_domain。`tree/sender.hh::_lookup_impl::build_route_plan` 改用 `current_shard_partitions()->route(key)`，fan-out 目标从 `tree_lookup_at(idx)` 换成 `tree_read_domain_at(idx)->lookup_sched`。空树 `!manifest->has_root()` 仍然短路成 all-absent，不进入 routing。

4. **验收**：`inconel_test_tree_lookup` / `inconel_test_tree_lookup_multicore` / `inconel_test_runtime` 三个测试全 pass；fixture 改用 `build_initial_shard_partition_map(leaf_order, K)` + `install_shard_partitions` + `tree_read_domain<Cache>(...)` 构造链。

5. **Spec 同步**：`runtime_state_machine.md` §1 路由表 + §4 tree domain 介绍 + §4.7 routing rules + §4.8 worker state + §9.3 flush diagram + §10.3 并发论证；`design_overview.md` §1.7 / §1.8 / §8.1 / §14.2 / §14 pipeline；`read_api_and_pipeline.md` §4 / §5 / §5.4 / §9.2；`code_modules.md` routing helper table + tree 模块表；`INDEX.md` scheduler 总览 + Point GET 关键路径 + Tree-Local Flush 4 段；`cross_doc_contracts.md`（新增 shard_partition_map / tree_read_domain 条目）。

---

### F8 — `internal_record` POD struct 在 spec 有但源码没有

**位置**：spec ODF §4.2 定义了 `struct __attribute__((packed)) internal_record { uint16_t key_len; ... }`；源码 `format/tree_page.hh` 缺失这个 struct。

**当前实现的等价物**：`page_builder.hh::internal_page_builder::add_child` 用 `memcpy(&key_len, ...)` + memcpy key + memcpy paddr 散文写；`page_reader.hh::read_internal_record` 同样散文读。

**问题**：
- spec 定义的字节布局散落在三处（builder / reader / 隐式）。改格式时三处必须同步，没有单一权威定义点
- 没有 `static_assert` 守护（leaf_record_header 有，但 internal_record 没有定义就没法 assert）

**Constraint C / B**：spec 明确说有这个 struct，源码缺失。算"实现没把 spec 的约束承载到代码"。

**修复方向**：加 `internal_record` POD 定义到 `format/tree_page.hh`，加 `static_assert(sizeof(internal_record) == 2)`（fixed 部分），builder/reader 用偏移辅助函数代替散 memcpy。

---

### F9 — `lookup_scheduler::loading_pages_` 用 `std::unordered_set<paddr>`

**位置**：`tree/scheduler.hh:169`

**Spec 偏好**：RSM §4.5 用 `flat_hash_map` 来描述 slot_map；RSM §4.7 暗示 inflight_reads 用 `flat_hash_map`。

**当前**：`std::unordered_set<paddr>`、`std::unordered_map<paddr, uint32_t>`。stdlib unordered 容器节点分配（pointer-chasing），cache 不友好。

**评级**：性能 quality 问题，不是正确性。

**Constraint C**：spec 用了 flat_hash_map 这个具体词，源码用了不同实现。如果是有意决定（比如 abseil 不可用），应在源码注释说明；目前没有解释。

---

### F10 — `tree_manifest::has_root()` 用 `lba != 0` 作为 sentinel

**位置**：`core/tree_manifest.hh:18-21`
```cpp
bool has_root() const {
    return root_slot.lba != 0;
}
```

**问题**：用 lba=0 表示"无 root"。在当前布局下 lba=0 是 superblock A 的位置，永远不会是合法 root，所以这个 sentinel 在 v1 是 OK 的。但：
- 这个不变量没有被任何地方明确写下（"data_area_base_paddr.lba > 0"）
- 如果将来格式变化，sentinel 失效会非常隐蔽

**Constraint B**：`has_root()` 名字读起来像通用判断，实际依赖隐式不变量。

**修复方向**：要么换 `std::optional<paddr> root_slot`，要么显式 `bool has_root_;` 字段，要么 sentinel 比较改成 `root_slot != paddr{0, 0}` + 在 spec 里把 "lba=0 永远不是合法 root" 写成显式约束。

---

## 7. Quality Findings（不阻塞，但应记录）

### F11 — `find_child` / `find` / `lower_bound` 全部 linear scan

**位置**：`tree/page_reader.hh`
- L46-69 `leaf_page_reader::find` (linear, early-terminate)
- L60-69 `leaf_page_reader::lower_bound` (linear)
- L145-157 `internal_page_reader::find_child` (linear, early-terminate)
- L133-141 `internal_page_reader::rightmost_child` (O(N) walk to end)

**Spec 数字**（ODF §4.6, §4.7）：典型 leaf ~268 records，typical internal ~371 children。Linear scan 平均比较 N/2 = 134 / 185。

**对比 B+ tree 文献**：变长记录的 leaf 通常需要"slot directory"（页头加偏移数组）才能用 binary search。spec 没定义 slot directory；当前实现直接用 linear scan。

**Code quality standard §3.1**：要求 hot path 成本"可数、可解释"。当前类内没注释 "linear scan, ~134 cmps avg per leaf, ~185 cmps avg per internal level"。

**Constraint C**：spec 不强制 binary search 也不禁止；当前实现选了 linear scan 但没解释为什么不做 slot directory。算 spec 缺口被 "最容易实现" 填上，需要补 design note。

---

### F12 — `lookup_scheduler::handle::first_call` 分支不可达

**位置**：`tree/scheduler.hh:226-234`
```cpp
if (s.first_call) {
    s.first_call = false;
    if (!s.manifest->has_root()) {
        s.all_done = true;
        r->cb(decision_done{});
        delete r;
        return;
    }
}
```

**追踪**：
- `make_lookup_state` 已经在 `!has_root()` 时把 `s.all_done = true` 设好，并把每个 entry `resolved = true`
- `sender.hh::check_not_done` 协程：`while (!state.all_done) co_yield true; co_return false;`——`all_done` 一开始就是 true 时，coroutine 立刻 `co_return false`，for_each 不进任何迭代
- 所以 process(state) 永远不会被调用 → handle() 永远不会被调用 → `first_call` 分支永远不进

**结论**：dead code。`first_call` 字段也没有其他读取点。

**修复方向**：删 dead code，或者如果有运行时不变量需要 first_call 守住（我没看到），加注释解释。

---

### F13 — `lookup_scheduler` 命名与 spec `tree_lookup_sched` 不一致

**位置**：`tree/scheduler.hh::lookup_scheduler`

**问题**：spec 全文用 `tree_lookup_sched`。源码用 `lookup_scheduler`。在 tree namespace 里去掉 "tree_" 前缀也可以理解，但：
- registry.hh 里的字段名是 `tree_lookup_scheds`（带 tree_ 前缀）
- 全 codebase 没有 `lookup_scheduler` 的 disambiguation——以后如果有 `value::lookup_scheduler` / `front::lookup_scheduler`，名字就撞了

**Constraint B**：通用命名（`lookup_scheduler`）对应的是 module-narrow 语义。如果保留这个名字，至少应该在注释里说"in tree namespace, lookup_scheduler == tree_lookup_sched as defined in spec"。

**修复方向**：要么改名 `tree_lookup_sched`，要么注释。

---

### F14 — `decision_need_cache` 是预留分支但永不产生

**位置**：`tree/lookup.hh:32-34`
```cpp
struct decision_need_cache {};
using batch_decision = std::variant<decision_done, decision_need_read, decision_need_cache>;
```

**追踪**：scheduler 里只产生 `decision_done` 和 `decision_need_read`。`decision_need_cache` 从未在任何地方被构造或返回。`sender.hh::lookup` 的 visit 分支也只判 `decision_need_read`，其余走 `just(true)`——意味着 decision_need_cache 走的是 fall-through 路径，相当于 decision_done。

**Constraint A**：variant 里有一个分支没人产、也没人专门处理。读代码的人看到这个分支会困惑"什么时候用？"

**修复方向**：删 `decision_need_cache`，等真有需要时再加；或在定义处注释 "reserved for future, not currently produced"。

---

### F15 — `process_entries` 不释放未解析 entry 的旧 buffer

非必修，但值得记录：每轮 round 后，已 resolved 的 entry 没有把它们曾用过的 free buffer 归还。当前依赖整个 lookup_state 析构时一起释放（owned_bufs_ 还在 scheduler 上）。这是 cache eviction → free_bufs_ 路径处理的，OK 但隐式。

---

## 8. Verification Needed（必须由 user 回答）

### V1 — Tests 当前构造的 tree 最大深度

我不能读 test 文件。需要 user 告知：
- `test_tree_lookup.cc` 构造的最大 tree 深度
- `test_tree_lookup_multicore.cc` 同上
- `test_tree_value.cc` 同上
- `test_runtime.cc` 同上

这个数字直接决定 §4.3 的结论："multi-level descent 是否在 production 实现层面真的被验证过"。

### V2 — Plan 文件 002/003/004 是否声明过 scope 限制

按本审计的纪律我没读 plan 文件。如果 plan 里已经明确写了"step 003 只实现读侧 + 不超过 N 层"，那 F4 就不是 scope 声明缺失而是 scope 声明分散（spec scope 在 plan，code scope 不在 code）。需要 user 决定是把 plan 内容内联进 tree/scheduler.hh 注释，还是其他做法。

### V3 — `tree_manifest` 应该归属 `core/` 还是 `tree/`

F6 的判断需要 user 拍板。

### V4 — Linear scan 是否接受为 v1 设计

F11 需要 user 决定：保留 linear scan + 在源码注释里写明成本，还是要求实现 slot directory + binary search。spec 没有这个章节。

---

## 9. 总结

### 直接回答用户的问题

**"两层硬编码作弊还在不在 production 代码？"**

在我能不读 tests 的范围内：**production 源码的 lookup descent 不存在硬编码深度限制**。下降循环 / manifest 容器 / page reader 都是 depth-agnostic 的。

但这个结论的可信度被一个事实严重削弱：**production 路径完全没人构造 tree 页**。所有 page 构造都在 4 个 test 文件里。tree_sched 写侧整体不存在。所以 multi-level descent 在 production 环境的"实战验证次数 = 0"，在 test 环境的验证次数 = 取决于 4 个 test 文件构造的最大深度（我不能验证）。

**这本身就是 constraint A 直接命中的情况**：实现的 verified scope 比代码声称的小，但代码没有任何地方声明这个限制——名字、注释、类型、返回状态都没有。

### 现存 tree 模块整体评估

| 维度 | 状态 |
|------|------|
| 格式 POD（leaf header / slot header） | ✓ 与 spec 一致 |
| `internal_record` POD | ✗ 缺失（F8） |
| 单页 builder/reader 原语 | ✓ 存在但只被 tests 用 |
| 多 key batch lookup pipeline | ✓ depth-agnostic 但 unverified（F3） |
| `tree_lookup_sched` 的 PUMP scheduler 框架 | ✓ 6 组件齐全 |
| Cache 集成（page_cache_concept） | ✓ 模板化，clean |
| `current_shard_partitions()->route(key)` routing + sender API | ✓（F7 / INC-040 / step 030 已收敛） |
| `tree_sched` 写侧（allocator/fold/flush/reclaim/manifest construction） | ✗ 整体缺失，但 registry.hh 有 placeholder |
| Data Area 碰撞检测 | ✗ 缺失 |
| `recovery_safe_lsn` 推进 | ✗ 缺失 |
| 错误处理（CRC fail / missing manifest entry） | ✗ 全部 silent fallback / Release UB（F1, F2） |
| 模块顶层 scope 声明 | ✗ 缺失（F4） |
| DMA 内存适配 | ✗ 用 std::make_unique，未声明（F5） |
| 命名一致性（lookup_scheduler vs tree_lookup_sched） | ⚠ 不一致但不致命（F13） |

### 必须先解决再继续的事

**对照 constraint A/B/C 的违反**：F1（silent CRC absent）、F2（Release UB）、F3（unverified depth 不声明）、F4（模块 scope 不声明）、F5（DMA 假设不声明）、F8（spec POD 缺失）、F14（dead variant 分支）——这 7 项是直接命中规则的，必须修。

**等 user 回答**：V1（tests 实际深度）、V2（plan scope 是否已声明）、V3（manifest 归属）、V4（linear scan 是否 acceptable for v1）。

**已经合规的部分**：format 头文件、CRC 函数、descent 循环结构、tree_lookup_sched 的 PUMP 6 组件框架、registry.hh 对未实现 sched 的 placeholder 声明——这些可以保留。

### 下一步建议

1. user 先回答 V1-V4
2. 我用回答补 §4 / §6 / §7 的空白
3. 然后我们决定 F1-F5 / F8 / F14 是 fix-in-place 还是等做 tree_sched 时一起重构
4. tree 审计完成后，按 plan 接着审 value（最大、最复杂的现存模块），再到 format / core / runtime / mock_nvme

---

## 10. 审计纪律记录

| 项目 | 状态 |
|------|------|
| 是否打开过任何 test 文件 | 否 |
| 是否打开过任何 fixture / benchmark 文件 | 否（项目内未发现）|
| 是否打开过 plan 文件 | 否（设计上隔离，避免 plan 把 spec 的 scope 漂移带入审计）|
| 是否读了完整 spec 章节作为信任源 | 是（§1.1 列表）|
| 是否记录"想读测试"的冲动 | 是（§1.3 末尾）|
| 是否对每条 finding 标 spec 依据 | 是 |
| 是否有 finding 的判断纯凭印象（无 spec 依据）| F11 部分（spec 没禁止 linear scan，我标了 quality 而不是 violation）— 已显式标 constraint C |
