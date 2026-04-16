# Step 030 — `tree_read_domain` + `shard_partition_map` 立起来 (INC-040 返工) Review

> Review 对象：step 030 实装交付的所有改动
>
> - **新文件**：`apps/inconel/core/shard_partition.hh`、`apps/inconel/core/tree_read_domain.hh`
> - **重写**：`apps/inconel/core/registry.hh`
> - **重构**：`apps/inconel/tree/lookup_scheduler.hh`、`apps/inconel/tree/worker_scheduler.hh`、`apps/inconel/tree/memtable_fold.hh`、`apps/inconel/tree/sender.hh`、`apps/inconel/tree/owner_scheduler.hh`
> - **启动顺序调整**：`apps/inconel/runtime/builder.hh`
> - **测试 fixture**：`apps/inconel/test/test_tree_lookup.cc`、`test_tree_lookup_multicore.cc`、`test_tree_value.cc`、`test_runtime.cc`
> - **文档同步**：`design_overview.md` / `runtime_state_machine.md` / `read_api_and_pipeline.md` / `runtime_memory_and_cache.md` / `flush_and_frontier_switch.md` / `code_modules.md` / `INDEX.md` / `cross_doc_contracts.md` / `audit/tree.md` / `known_issues.md`
>
> 对照设计文档：`ai_context/inconel/plan/030_tree_read_domain.md`（含 §6 7 条拍板决策 + §7 11 步实施顺序 + §9 验收标准）。
>
> Review 口径：
>
> 1. 评审 production 实装 + 测试 fixture 是否兑现 §6 七条决策（本步 plan 明确允许改 `test_tree_lookup*` / `test_tree_value` / `test_runtime` 四个 fixture，review 同样覆盖它们）。
> 2. "比设计更严或更整洁的实装"如果不破坏 contract 按 ✓ 保留；只有"违反 contract / 破坏自洽语义 / 影响可观测行为"的偏离才记成 finding。
> 3. 其它测试文件（§8 out-of-scope）不读内容，只用 grep 看引用关系——step 030 删 registry 符号后，原本破的几个测试可能失败模式升级，这属于 pre-existing backlog。

---

## 1. 结论

**Step 030 核心实装正确、验收通过**，可以 land。三个验收测试全 pass、构建干净、§6 七条决策全部落地、hot path 零虚调用、concurrency invariant 由 worker 的 `advance` panic 检查兜底。

**本轮 review 发现的 2 条 finding 已全部收敛（2026-04-16 follow-up）**：

- ~~**Significant (S1)**~~ ✅ 已收敛 — 拆出 `core/shard_partition_builder.hh`；`shard_partition.hh` 去掉 `leaf_order.hh` include，只剩 routing 类型 + `route()` + `fence_upper_view()`；`builder.hh` + 4 个测试 fixture 切到新 header；4 个 routing-only 站点（`registry.hh` / `tree_read_domain.hh` / `memtable_fold.hh` / `tree/sender.hh`）保持 include `shard_partition.hh` 不变。plan §2.1 "数据结构自洽"的 header 依赖图承诺物理兑现（详见 §4 S1 收敛记录）。
- ~~**Minor (M1)**~~ ✅ 已收敛 — `test_runtime.cc:287-290` 注释更新成 `current_shard_partitions()->route(key)` + `tree_read_domain_at(shard_idx)->lookup_sched`，并带 plan 锚点 §2.6 / §6.4 F2。

**Pre-existing backlog 2 条**（不在 step 030 范围）：

- **P1** `test_flush_carriers.cc:1474, 1519` 引用被删的 `tree_worker_scheds` → 编译错误升级（该测试本就因 P2 编不过）
- **P2** `test_tree_value.cc` `value_alloc_sched` 5-arg 构造不匹配 builder 的 6-arg 签名

P1 / P2 建议另开 cleanup step；单独看 step 030 scope 已完整 closed，**可以 commit**。

---

## 2. 验收验证

### 2.1 构建

```
cmake --build build_debug --target inconel_test_tree_lookup \
                                   inconel_test_tree_lookup_multicore \
                                   inconel_test_runtime \
                                   -j$(nproc)
→ [100%] Built target inconel_test_tree_lookup
→ [100%] Built target inconel_test_tree_lookup_multicore
→ [100%] Built target inconel_test_runtime
```

三个目标全过，无 warning / error。

### 2.2 测试

| Binary | 结果 |
|---|---|
| `inconel_test_tree_lookup` | all passed — 400 existing keys / missing / mixed / single / empty tree / clock eviction 400/400 745 NVMe reads / slru eviction 400/400 654 NVMe reads |
| `inconel_test_tree_lookup_multicore` | all passed — 400 hits all correct / 100 concurrent misses all absent |
| `inconel_test_runtime` | all passed — clock 400/400 / slru 400/400 e2e multi-core via runtime |

### 2.3 §9 验收点逐条

- ✓ `shard_partition_map::route()` 单次二分；空 map 不被调用的前提下 panic 兜底；最后 shard 为 +∞ sentinel 的 contract 由 `build_initial_shard_partition_map` 强制
- ✓ `tree_read_domain` 实例数 K == `registry::tree_read_domains.list.size()`；不再有 `tree_lookup_scheds` / `tree_worker_scheds` list
- ✓ `tree::lookup(keys, manifest)` 公开签名不变
- ✓ fold 产出的 `flush_key_partition.read_domain_index` 来自同一张 `shard_partition_map`；fan-out 走 `tree_read_domain_at(idx)->worker_sched`；读/写同 key 落到同 read_domain（cache 共享不变量成立）
- ✓ 三个验收测试全过
- ✓ spec / audit / known_issues 全部同步（详见 §5）

---

## 3. §6 决策对照

| # | 决策 | 实装验证 |
|---|---|---|
| **6.1 A** | scheduler 模板化 on `Cache`，持 `tree_read_domain<Cache>*` 零虚调用 | ✓ `lookup_scheduler.hh:287` / `worker_scheduler.hh:160` 都是模板指针；`lookup_scheduler.hh:437-438, 483` 的 `pin/contains` 直接调模板成员 `read_domain_->node_cache.xxx`，无虚分派 |
| **6.2 B1** | rebuild 留给 coord_sched；本步只 bootstrap 装一次 | ✓ `builder.hh:306-314` 装占位 map 后不再 install；`install_shard_partitions()` API 已留接口备用 |
| **6.3 C1** | `tree::lookup(keys, manifest)` 保留 manifest | ✓ `sender.hh:254` 签名未变；下降阶段仍用 `manifest->resolve` / `manifest->has_root()` |
| **6.4 F2** | 去掉 `tree_lookup_scheds` / `tree_worker_scheds`，全走 `tree_read_domains.list` | ✓ `registry.hh` 两个 list + `tree_lookup_at` / `tree_worker_at` / `local_tree_lookup` / `local_tree_worker` / `tree_lookup_count` / `tree_worker_count` / `tree_lookup_for_core` / `tree_worker_for_core` / `route_tree_lookup_for_key` 全部删除；`apps/inconel/tree/` 下 grep 零引用；`apps/inconel/test/` 下 4 个 fixture 全部切到 `tree_read_domains` |
| **6.5 G1 + advance** | read_domain own scheduler via `unique_ptr`，暴露 `advance()` | ✓ `tree_read_domain.hh:150-151` unique_ptr 字段；`:213-221` advance() 实现 `lookup->advance() \| worker->advance()`；PUMP tuple 注册 `tree_read_domain<Cache>` 替代两个 scheduler（`runtime_t` 定义 + `get_by_core` 调用点都已切） |
| **6.6 I1** | `read_domain_index` 字段上移到 `tree_read_domain`；scheduler base 不再持 | ✓ `tree_read_domain_base:102`；`lookup_sched::read_domain_index()` / `worker_sched::read_domain_index()` 改成 virtual getter 代理到 `read_domain_->read_domain_index`（`lookup_scheduler.hh:322-325`, `worker_scheduler.hh:172-175`），仅在 panic 诊断路径使用 |
| **6.7 P** | bootstrap 装单 shard 占位 map | ✓ `shard_partition.hh:208-218` empty leaf_order 分支返回单 shard 占位；`builder.hh:306-314` bootstrap 调 `build_initial_shard_partition_map(empty_leaf_order, K)` 后 install；空树 lookup 仍靠 `manifest.has_root()` 短路，占位 map 不被调 route |

---

## 4. Findings

### ✅ S1 (significant, resolved 2026-04-16) — `shard_partition.hh` 违反自己承诺的"自洽"约束

**位置**

- `apps/inconel/core/shard_partition.hh:33` — `#include "./leaf_order.hh"`
- `apps/inconel/core/shard_partition.hh:198-263` — `build_initial_shard_partition_map(const leaf_order_index&, uint32_t)` 定义

**Header 自己的注释承诺**（同文件 L13-L20）：

```
// Separation of concerns (step 030 §2.1 / §2.2):
//   - `shard_partition_map` is self-contained: it only knows its own
//     sorted fence upper bounds and does not import `leaf_order` or
//     `tree_manifest`. Routing decisions never look at leaves.
//   - `shard_partition_builder` is the ONLY site that reads
//     `leaf_order` to derive an initial partition.
```

这段注释把 `shard_partition_builder` 写成独立概念，但实装把它塞在 `shard_partition.hh` 同一个文件 → 整个 header 被迫 include `leaf_order.hh`。下游凡是只想要"路由 + 类型"的 include site 都连带把 leaf_order 的传递依赖拉了进来。

**问题的实际边界**：

- ✗ `route()` 运行时不依赖 leaf_order —— 这一层是对的。
- ✗ **但 header 依赖图**把 `core/shard_partition.hh` 钉死在 `core/leaf_order.hh` 上。将来想做"热度驱动 rebuild"换 builder 时——plan §2.2 说的 "future heat-driven rebuild will replace the builder without touching the routing code"——换 builder 会触及这个文件，contract 边界已经漂。
- ✗ 下游 6 个 include site（`registry.hh` / `tree_read_domain.hh` / `memtable_fold.hh` / `tree/sender.hh` 只用 route + 类型）被迫多拖一份 leaf_order，违反"dependency-light"注释（L22）。

**修复指引（建议本步顺手做）**：

拆成两个 header，语义和 plan §2.1 / §2.2 设计意图完全对齐：

1. **`core/shard_partition.hh`**（瘦身）
   - 保留：`shard_partition` struct / `shard_partition_map` struct / `route()` / `fence_upper_view()`
   - 删除：`#include "./leaf_order.hh"`、`build_initial_shard_partition_map` 定义
   - include 链：只 `<cstdint> / <string> / <string_view> / <vector> / <cstddef> / <type_traits>` + `./panic.hh`

2. **`core/shard_partition_builder.hh`**（新文件）
   - `#include "./shard_partition.hh"` + `#include "./leaf_order.hh"`
   - 持 `build_initial_shard_partition_map(const leaf_order_index&, uint32_t) -> shard_partition_map`，含 inline 实现

3. **下游 include 调整**：
   - `builder.hh` / 4 个测试 fixture：原 `#include "...shard_partition.hh"` 换成 `#include "...shard_partition_builder.hh"`（它们调用 builder 函数）
   - `registry.hh` / `tree_read_domain.hh` / `memtable_fold.hh` / `tree/sender.hh`：保留 `#include "...shard_partition.hh"` 不变（它们只用 map 类型 + `route()`）

修复量：1 新文件 + 1 源文件瘦身 + 5 个 include 切换。不动语义、不动测试。建议和其它 step 030 改动一起 land，不要留到后续 step。

**收敛记录（2026-04-16）**

- 新建 `apps/inconel/core/shard_partition_builder.hh`：持 `build_initial_shard_partition_map` inline 定义，include `./shard_partition.hh` + `./leaf_order.hh`；header doc L4-L13 明确写 "The ONLY site that reads `leaf_order_index`"
- 瘦身 `apps/inconel/core/shard_partition.hh`：删掉 `#include "./leaf_order.hh"` 和 builder 定义；只剩 `shard_partition` / `shard_partition_map` / `route()` / `fence_upper_view()`。header doc L15-L25 保留"自洽"说明作为 rationale
- **下游 include 切换**（grep 验证）：
  - **包 `shard_partition_builder.hh`**（5 个 builder 调用点）：`runtime/builder.hh:18` / `test/test_tree_lookup.cc:18` / `test/test_tree_lookup_multicore.cc:25` / `test/test_tree_value.cc:34` / `test/test_runtime.cc:33`
  - **包 `shard_partition.hh`**（4 个 routing-only 站点，不再传递依赖 leaf_order）：`core/registry.hh:15` / `core/tree_read_domain.hh:67` / `tree/memtable_fold.hh:41` / `tree/sender.hh:50`
- 构建干净、三个验收测试全 pass（和 S1 修复前完全一致的结果，零 regression）
- plan §2.1 的"自洽"承诺从注释口号变成物理可验证：4 个 routing-only 站点 include `shard_partition.hh` 的传递闭包里**不含** leaf_order.hh

---

### ✅ M1 (minor, resolved 2026-04-16) — `test_runtime.cc:288` 注释过时

**位置**：`apps/inconel/test/test_runtime.cc:288`

代码实际已切到 `tree_read_domain_at` 路径（`:349-365`），但旁边注释仍写着旧的 `tree_lookup_at` 表述。

**修复**：1 行注释更新。建议顺手扫掉，不拦 land。

**收敛记录（2026-04-16）**：`test_runtime.cc:287-290` 改为

```
// Main thread submits lookups, splits across the tree_read_domains via
// `current_shard_partitions()->route(key)` and
// `core::registry::tree_read_domain_at(shard_idx)->lookup_sched`
// (step 030 §2.6 / §6.4 F2), and verifies all 400 keys resolve.
```

注释完整反映 step 030 的路由语义 + 带锚点指向 plan 条款。

---

### 🔵 P1 (pre-existing，step 030 升级了失败模式) — `test_flush_carriers.cc` 引用被删符号

**位置**：

- `apps/inconel/test/test_flush_carriers.cc:1474` — `core::registry::tree_worker_scheds.list.push_back(...)`
- `apps/inconel/test/test_flush_carriers.cc:1519` — `core::registry::tree_worker_scheds.list.clear()`

**背景**：该测试原本就在 pre-existing broken 列表里（`value_alloc_sched` 构造参数不匹配，plan §8 明确列出）。step 030 删 `tree_worker_scheds` 后，编译错误从"构造参数不匹配"**升级**成"符号完全不存在"——但测试结果依然是编译失败，没有 regression，只是失败原因变了。

**建议**：

- 如果一并清：改 `:1474` / `:1519` 成 `tree_read_domains.list` 的等价 API（例如构造 `tree_read_domain_base` mock 再 push 进 list；或者绕开 registry 直接用 scheduler 指针，看 fixture 原本意图）。但该测试还有 `value_alloc_sched` 6-arg 问题（P2），单修 tree_worker_scheds 它仍然构建失败。
- 如果不一并清：确认该测试继续挂在 pre-existing broken 列表，不影响 step 030 validate。

单独看 step 030 scope，本项**不是 blocker**。

---

### 🔵 P2 (pre-existing) — `test_tree_value.cc` value_sched 构造参数不匹配

**位置**：`apps/inconel/test/test_tree_value.cc:117-121` vs `apps/inconel/runtime/builder.hh:346-352`

fixture 用 5-arg 构造：

```cpp
value_sched(std::span<const uint32_t>(CLASS_SIZES),
            LBA_SIZE,
            paddr{0, DATA_AREA_BASE_LBA},
            paddr{0, DATA_AREA_END_LBA},
            clock_cache(value_cache_cap))
```

builder 里已经是 6-arg（多一个 `shared_heads.get()` 参数）。纯 pre-existing，step 030 无关。

**注意**：该文件 fixture **已经按 step 030 正确迁移** —— `leaf_order`、`build_initial_shard_partition_map`、`install_shard_partitions`、`tree_read_domain<Cache>` 构造链都已到位（`test_tree_value.cc:113-131`）。但 value_sched 构造错仍然让它跑不了。

---

## 5. 文档同步检查

对照 plan §7 第 11 步 + §9 "spec / audit / known_issues 同步" 要求，逐文件验证：

| 文档 | 关键章节 | 状态 |
|---|---|---|
| `design_doc/runtime_state_machine.md` | §1 表（L13 路由公式）、§4.7 routing rules | ✓ R1 的 `leaf_order % K` 被替换为 `shard_partition_map::route(key)` |
| `design_doc/design_overview.md` | §1.7 / §1.8 / §8.1 / §14.2 / §14.4 | ✓ §14.2 point_get 伪码 L1855-1859 使用 `current_shard_partitions()->route(key)` + `tree_read_domain_at(shard_idx)`；无 `home_tree_lookup` / `route_tree_lookup(owner)` 残留 |
| `design_doc/read_api_and_pipeline.md` | §4 / §5 / §5.4 / §6.1 / §6.2 / §9.2 | ✓ §4 步骤 6 用 `current_shard_partitions()->route(key)` 单次二分；§5.4 tree batch lookup 段落改成 shard-local 下降 + 公开 sender 的 fan-out 说明 |
| `design_doc/runtime_memory_and_cache.md` | §7.1 / §7.2 / §7.3 / §9.1 / §12 | ✓ cache ownership 归 `tree_read_domain`；lookup + worker 共享 per-domain cache shard |
| `design_doc/flush_and_frontier_switch.md` | §1 表、§3.2、§3.4 | ✓ fold 描述改用 `current_shard_partitions()->route(key)`；无 `leaf_idx * P / leaf_count` 残留 |
| `design_doc/code_modules.md` | 路由表 + tree 模块 + runtime deploy params | ✓ 域对象表新增 `tree_read_domain` / `shard_partition_map`；registry 路由表新增 `current_shard_partitions()` / `install_shard_partitions()` |
| `design_doc/INDEX.md` | scheduler 总览、Point GET、Tree-Local Flush | ✓ 路由列用 `current_shard_partitions()->route(key)`；无 ordinal / hash 公式 |
| `design_doc/cross_doc_contracts.md` | handle 签名 + struct 字段 + 数据源断言 | ✓ 新增 4 个 handle 签名（`current_shard_partitions` / `install_shard_partitions` / `shard_partition_map::route` / `tree_read_domain::advance`）+ 4 个 struct 字段（`shard_partition` / `shard_partition_map` / `tree_read_domain<Cache>` / `tree_read_domain_base`）+ 读路径 pipeline |
| `audit/tree.md` | §2.2 读侧 / §3 现存实现表 / F4 注释 / F7 resolved | ✓ F7 resolved 描述重写为 step 030 最终形态（不是 R1 ordinal-mod）；§2.2 routing 段引用 `shard_partition_map::route` |
| `known_issues.md` | Resolved 区 INC-003 / INC-040 | ✓ INC-003 说明公开 API 不变 + 内部换算法；INC-040 说明 leaf-ordinal → shard_partition_map 二分 + 引入 tree_read_domain 运行对象 |

**整体评价**：文档层面没有 R1 残留，对 step 030 设计边界的描述一致。唯一 nit 是 `test_runtime.cc:288` 的注释（见 M1），属代码旁注释而非 spec，不影响文档 contract。

---

## 6. Review 方法

并行启动 4 个 Explore agent 分工 + 主会话跑构建测试：

| Agent | 范围 |
|---|---|
| **Agent 1** | 新文件 `core/shard_partition.hh` + `core/tree_read_domain.hh` —— 对照 §2.1 / §2.2 / §2.3，8 项检查 |
| **Agent 2** | scheduler / sender / fold 改造 —— 5 个文件，对照 §2.4 / §2.5 / §2.6；grep 旧 API |
| **Agent 3** | registry / builder / 4 个 test fixture —— 对照 §2.7 / §2.8 / §7 第 8-9 步；grep 删除 API 引用面 |
| **Agent 4** | 10 份 spec / audit / known_issues —— 对照 §6 七条决策的文字表述 |
| **主会话** | 构建三个测试 + 跑三次测试验证验收 §9 |

Agent 1 发现 S1；Agent 3 发现 P1（`test_flush_carriers.cc` 引用被删符号）；其余 agent 清单全过。

---

## 7. Land 建议

**已按推荐方案 land：S1 + M1 收敛 + 核心改动一起 commit**。

- S1 修复后 plan §2.1 的"数据结构自洽"承诺物理兑现：4 个 routing-only 站点 include `shard_partition.hh` 的传递闭包里不含 leaf_order.hh
- M1 注释更新，代码和描述文字一致
- 三次验收测试（零 regression）：S1 修复前 → 修复后结果完全一致（clock 745 NVMe reads / slru 654 NVMe reads / multicore 400 hits + 100 misses / runtime 400/400 both caches）
- step 030 可以 closed

**P1 / P2 独立处理**（不在本 step）：

- P1 (`test_flush_carriers.cc` broken) 建议和其他 pre-existing 坏测一起开单独的 cleanup step
- P2 (`test_tree_value.cc` value_sched 构造) 属 `value_alloc_sched` signature evolution 的 debt，等 value 模块下一次改动一起收

---

## 8. Checklist

- [x] 三个验收测试全过（§2.2）
- [x] §6 七条决策全部落地（§3）
- [x] hot path 零虚调用（§3 决策 A）
- [x] concurrency invariant 由 worker advance panic 兜底（`worker_scheduler.hh:193-199`）
- [x] 旧 API (`tree_lookup_at` / `tree_worker_at` / `tree_lookup_scheds` / `tree_worker_scheds` / `route_tree_lookup_for_key` / `local_tree_lookup` / `local_tree_worker`) 在 `apps/inconel/tree/` 零引用；`apps/inconel/test/` 中 4 个 step 030 fixture 零引用
- [x] 10 份 spec / audit / known_issues 同步完（§5）
- [x] **S1：`shard_partition.hh` 拆 builder 到独立 header**（2026-04-16 已完成；见 §4 S1 收敛记录）
- [x] **M1：`test_runtime.cc:288` 注释更新**（2026-04-16 已完成；见 §4 M1 收敛记录）
- [ ] P1：`test_flush_carriers.cc` tree_worker_scheds 引用（pre-existing，另开 cleanup step）
- [ ] P2：`test_tree_value.cc` value_sched 构造（pre-existing，另开 cleanup step）

---

## 9. 最终评级

**Step 030 完整通过 review，可以 commit**。所有 significant 和 minor finding 均已收敛；pre-existing backlog 记录在案，与 step 030 scope 解耦。
