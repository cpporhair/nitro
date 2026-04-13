# 025 — Worker Fanout / Leaf Mapping

> 实现第二十五步。落地 `flush_development_plan.md` Phase 5：基于 Phase 4 产出的 sorted workset 和 key partitions，实现 tree-local flush 的第二段 sender seam——从 `tree_sched` 扇出到各 `tree_worker_sched`，在每个 worker shard 上执行 `keys_to_leaf_groups()` 映射，再 fan-in 回 `tree_sched` 做跨 partition 去重/合并。
>
> Phase 4（step 024）完成了 `fold_pinned_gens()` + `build_key_partitions()`，round_state 上已有 sorted `workset` 和 `partitions[]`。Phase 4 的非空 round 返回 `unsupported_unimplemented`。Phase 5 取代该 stub，让非空 round 执行真正的 worker fanout，但在 merge 完成后仍然返回 `unsupported_unimplemented`（Phase 6-7 下游未实现）。

## 整体架构：一次 fanout，worker 做完全部 page 工作

flush 的跨 scheduler pipeline 只有**一次 fan-out / fan-in**：

```
tree_sched fold
  → fanout 到 K 个 worker（每个 worker 独立完成自己 partition 的全部 page 级工作）
  → fan-in 回 tree_sched（合并所有 partition 结果，分配 NVMe 地址，写盘，构造 new manifest）
```

最终形态中，worker handle 覆盖从 key-to-leaf mapping 到构造新页、处理分裂的全部 page 级重活。tree_sched 只做轻量协调（分配地址、合并 manifest、发起写盘）。

Phase 5 是第一步：worker handle 先只做 mapping，返回 leaf groups。Phase 6 在**同一个 handle** 里加上读旧页 + merge + 构造 candidate。Phase 8 加上 split 处理。始终是同一次 fanout、同一个 handle、同一次 fan-in——后续 phase 让 handle 做更多事，不另起 fanout。

### 为什么 flush page 工作放 worker 而不是 lookup

原始设计文档把 leaf mapping 放在 `tree_lookup_sched`（RSM §4.7）。经讨论改为放在 `tree_worker_sched`，理由：

1. **代码隔离**。lookup 和 worker 分开是为了维护性，不是功能必须。flush 的 page 级工作（mapping / candidate build / split）统一放 worker，避免散布两个 scheduler。
2. **cache 复用**。worker 和 lookup 在同一个 `tree_read_domain` 共享 cache shard。正常运行时 lookup 处理应用层 point read，已经把 tree page 读进了 cache。worker 在同一个 cache shard 上读 old leaf page 时直接 cache hit。这个收益来自 read_domain 架构，和 mapping 在 lookup 还是 worker 上无关。
3. **一次 fanout**。mapping 和 build 在同一个 worker handle 里，Phase 6 直接扩展，不需要 mapping → fan-in → 再 fanout build 的两次往返。

`tree_lookup_sched` 在本 step 中**零修改**。

## 跨 scheduler 编排走 PUMP sender

与 codebase 已有模式一致：

- `tree_worker_sched` 新增 `_leaf_mapping` PUMP sender surface（req/op/sender + op_pusher + compute_sender_type），与现有 `_build_leaf_candidates` 对称
- `tree_sched` 拆成两个 stage handle：`submit_flush_fold`（fold + partition）和 `submit_flush_merge`（merge mapping results）
- `sender.hh` 的 `tree_local_flush()` pipeline 用 `loop >> concurrent >> flat_map(worker->submit_leaf_mapping)` 编排 fanout

tree_sched 不直接 enqueue 到 worker 的 queue。跨域由 PUMP sender 层自动完成。

## 本 step 覆盖的目标

| 目标 | 说明 |
|---|---|
| G1 | `flush_leaf_group` 新增 `keys` span 字段 |
| G2 | `leaf_order_index::find_leaf_for_key()` — 二分查找 |
| G3 | `keys_to_leaf_groups()` — sorted merge 映射算法，独立 free function |
| G4 | `tree_worker_sched` 新增 `_leaf_mapping` PUMP sender surface |
| G5 | `tree_sched` 拆成两个 stage：`_flush_fold` + `_flush_merge` |
| G6 | `sender.hh::tree_local_flush()` 用 PUMP sender 组合编排 fanout/fan-in |
| G7 | `merge_lookup_leaf_groups()` — fan-in 合并算法，独立 free function |
| G8 | 新增 carrier：`flush_fold_result`、`flush_merge_request` |
| G9 | `flush_lookup_req` 重命名为 `flush_mapping_req`（目标 scheduler 变了，名字跟上） |

## 本 step 不覆盖

| 不做项 | 归属阶段 | 与架构的关系 |
|---|---|---|
| old leaf read / merge / candidate build | Phase 6 | 扩展同一个 worker handle，同一次 fanout |
| tree delta planning / NVMe 地址分配 / bounded writes | Phase 7 | 扩展 tree_sched merge handle |
| leaf split / parent rewrite / root change | Phase 8 | 扩展同一个 worker handle |
| `leaf_order` builder | Phase 7 | |
| tree_lookup_sched 修改 | 无 | |
| cache ownership migration | 独立 step | |

## 设计决策

| # | 决策点 | 结果 | 说明 |
|---|---|---|---|
| D1 | `flush_leaf_group` 扩展 | 新增 `std::span<const flush_key_group> keys` | Phase 6 worker 做 candidate build 时直接消费，不需重做 mapping |
| D2 | fence upper = empty 语义 | 正无穷 | `fence_lower` empty = 负无穷；标准 B+ tree convention |
| D3 | `leaf_order_index::find_leaf_for_key()` | 新增 `const` 方法，二分查找，O(log L) | 放在 carrier 上——通用 reader 操作 |
| D4 | 算法文件 | `tree/leaf_mapping.hh`，header-only | `keys_to_leaf_groups()` + `merge_lookup_leaf_groups()` 两个 inline free function |
| D5 | `keys_to_leaf_groups()` 算法 | 二分首 key + 顺序 scan，O(log L + N + H) | 不逐 key root descend，不扫全树 leaf |
| D6 | `_leaf_mapping` sender surface 位置 | **`tree_worker_sched`**（不是 `tree_lookup_sched`） | flush page 工作统一 worker。Phase 6 扩展此 handle 加 candidate build，Phase 8 加 split——始终同一 handle、同一 fanout |
| D7 | `_leaf_mapping` queue | `tree_worker_sched::leaf_mapping_q_`（`per_core::queue`） | 与现有 `build_q` 并列 |
| D8 | drain cap | `kMaxLeafMappingOpsPerAdvance = 64` | 与 `kMaxBuildOpsPerAdvance` 对齐 |
| D9 | `tree_sched` 阶段拆分 | `_flush_fold`（fold+partition）+ `_flush_merge`（merge） | 各自一个 queue，PUMP sender pipeline 连接 |
| D10 | `flush_fold_result` | `{ round_id, st, partitions, base_manifest* }` | partitions 的 groups span 借自 round_state.workset |
| D11 | `flush_merge_request` | `{ round_id, mapping_results }` | PUMP pipeline `to_vector` 收集后传入 |
| D12 | pipeline 编排 | `sender.hh::tree_local_flush()` | 外部 API 不变 |
| D13 | fanout 模式 | `loop(P) >> concurrent() >> flat_map(worker->submit_leaf_mapping)` | 与读路径 `loop(n) >> concurrent() >> flat_map(nvme->read())` 同模式 |
| D14 | empty/error 拦截 | fold 直接返回 `{ st, partitions={} }` → pipeline 跳过 fanout → 进 merge | merge 遇 empty/error → unpark round → 返回 result |
| D15 | merge 后下游 | `unsupported_unimplemented` | Phase 6 未实现 |
| D16 | `flush_round_state` 变更 | **不新增异步管理字段** | PUMP context/scope 管理 fanout；round_state 只新增被 merge 填充的 `leaf_groups` |
| D17 | `flush_lookup_req` 重命名 | → `flush_mapping_req` | 目标从 lookup 变成 worker，名字跟上。`flush_leaf_group_result` 不改（描述的是输出内容，不是 scheduler） |
| D18 | merge 前排序 | 按 `read_domain_index` 排序 | `per_core::queue` 到达顺序不保证 = 投递顺序 |

## 详细设计

### 0. `tree/flush_types.hh` — carrier 变更

#### 0.1 `flush_leaf_group` 扩展（D1）

```cpp
struct flush_leaf_group {
    paddr leaf_range_base;
    paddr old_slot_paddr;
    std::span<const flush_key_group> keys;  // borrows from flush_round_state.workset
};
```

#### 0.2 `flush_lookup_req` → `flush_mapping_req`（D17）

```cpp
struct flush_mapping_req {   // was flush_lookup_req
    flush_round_id                    round_id;
    uint32_t                          read_domain_index;
    const core::tree_manifest*        base_manifest;
    std::span<const flush_key_group>  groups;
};
```

#### 0.3 新增 `flush_fold_result`（D10）

```cpp
struct flush_fold_result {
    flush_round_id                     round_id;
    flush_stage_status                 st;
    std::vector<flush_key_partition>   partitions;
    const core::tree_manifest*         base_manifest;
};
```

#### 0.4 新增 `flush_merge_request`（D11）

```cpp
struct flush_merge_request {
    flush_round_id                            round_id;
    std::vector<flush_leaf_group_result>      mapping_results;
};
```

### 1. `core/leaf_order.hh` — `find_leaf_for_key()`（D3）

```cpp
std::size_t
find_leaf_for_key(std::string_view key) const {
    if (spans.empty()) return spans.size();
    std::size_t lo = 0, hi = spans.size();
    while (lo < hi) {
        std::size_t mid = lo + (hi - lo) / 2;
        if (fence_lower(spans[mid]) <= key) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return spans.size();
    std::size_t idx = lo - 1;
    auto upper = fence_upper(spans[idx]);
    if (!upper.empty() && key >= upper) return spans.size();
    return idx;
}
```

### 2. `tree/leaf_mapping.hh` — 映射算法（D4, D5）

新文件，header-only。`keys_to_leaf_groups()` 和 `merge_lookup_leaf_groups()` 两个 inline free function。

#### 2.1 `keys_to_leaf_groups()`

```text
keys_to_leaf_groups(req, result):
  lo = req.base_manifest->leaf_order
  groups = req.groups

  if groups.empty(): return ok
  if lo.empty(): return unsupported_shape_change

  li = lo.find_leaf_for_key(groups[0].key)
  if li >= lo.size(): return unsupported_shape_change

  ki = 0
  while ki < groups.size():
    span = lo.spans[li]
    upper = lo.fence_upper(span)

    key_start = ki
    while ki < groups.size():
      if !upper.empty() && groups[ki].key >= upper: break
      ki++

    if ki > key_start:
      result.leaf_groups.push_back({
        .leaf_range_base = span.leaf_range_base,
        .old_slot_paddr  = manifest->resolve(span.leaf_range_base),
        .keys            = groups.subspan(key_start, ki - key_start),
      })

    if ki >= groups.size(): break
    li++
    if li >= lo.size(): return unsupported_shape_change
    next_lower = lo.fence_lower(lo.spans[li])
    if !next_lower.empty() && groups[ki].key < next_lower:
      panic_inconsistency(...)  // gap = manifest corruption
```

#### 2.2 `merge_lookup_leaf_groups()`

```text
merge_lookup_leaf_groups(mapping_results, leaf_groups_out):
  sort(mapping_results, by read_domain_index)
  for result in mapping_results:
    if result.st != ok: return result.st
    for lg in result.leaf_groups:
      leaf_groups_out.push_back(move(lg))
  // adjacent same-leaf dedupe
  // verify contiguity, extend span
```

### 3. `tree/worker_scheduler.hh` — `_leaf_mapping` PUMP sender surface（D6–D8）

与现有 `_build_leaf_candidates` 对称的五件套。Phase 6 将扩展此 handle 加入 candidate build（读旧页 + merge + 构造新页），Phase 8 加入 split 处理。扩展时 handle 做更多工作，返回类型可能从 `flush_leaf_group_result` 变为更丰富的 carrier——但 pipeline 结构（一次 fanout、一次 fan-in）不变。

```cpp
namespace _leaf_mapping {
    struct req {
        flush_mapping_req args;
        std::move_only_function<void(flush_leaf_group_result&&)> cb;
    };
    struct op {
        constexpr static bool leaf_mapping_op = true;
        tree_worker_sched* sched;
        flush_mapping_req args;
        template<uint32_t pos, typename ctx_t, typename scope_t>
        void start(ctx_t& ctx, scope_t& scope) {
            sched->schedule_leaf_mapping(new req{
                std::move(args),
                [ctx = ctx, scope = scope](flush_leaf_group_result&& r) mutable {
                    pump::core::op_pusher<pos + 1, scope_t>::push_value(
                        ctx, scope, std::move(r));
                },
            });
        }
    };
    struct sender {
        tree_worker_sched* sched;
        flush_mapping_req args;
        auto make_op() { return op{.sched = sched, .args = std::move(args)}; }
        template<typename ctx_t>
        auto connect() {
            return pump::core::builder::op_list_builder<0>().push_back(make_op());
        }
    };
}
```

PUMP 特化（`pump::core` namespace）：

```cpp
// op_pusher for leaf_mapping_op
template<uint32_t pos, typename scope_t>
requires (...) && (get_current_op_type_t<pos, scope_t>::leaf_mapping_op)
struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> { ... };

// compute_sender_type → flush_leaf_group_result
template<typename ctx_t>
struct compute_sender_type<ctx_t, apps::inconel::tree::_leaf_mapping::sender> { ... };
```

`tree_worker_sched` 新增成员：

```cpp
struct tree_worker_sched {
    // existing
    pump::core::per_core::queue<_build_leaf_candidates::req*> build_q;

    // new (Phase 5)
    pump::core::per_core::queue<_leaf_mapping::req*>          leaf_mapping_q;

    void schedule_leaf_mapping(_leaf_mapping::req* r) {
        leaf_mapping_q.try_enqueue(r);
    }
    auto submit_leaf_mapping(flush_mapping_req args) {
        return _leaf_mapping::sender{this, std::move(args)};
    }

    bool advance() {
        bool progress = false;
        // existing: drain build_q
        // new: drain leaf_mapping_q (validate rdi + manifest, run algorithm, cb)
        return progress;
    }
};
```

### 4. `tree/owner_scheduler.hh` — `tree_sched` 拆阶段（D9）

Phase 4 的 `_tree_flush` 拆成 `_flush_fold` + `_flush_merge`。

**`_flush_fold`** — validate + fold + partition → `flush_fold_result`

**`_flush_merge`** — merge mapping results → `tree_flush_result`

```cpp
struct tree_sched {
    tree_state state;
    pump::core::per_core::queue<_flush_fold::req*>   fold_q;
    pump::core::per_core::queue<_flush_merge::req*>  merge_q;

    auto submit_flush_fold(tree_flush_request args);
    auto submit_flush_merge(flush_merge_request args);

    bool advance() {
        bool progress = false;
        // drain fold_q: validate → park round → fold → partition → cb(fold_result)
        // drain merge_q: lookup round → merge → unpark → cb(flush_result)
        return progress;
    }
};
```

### 5. `tree/sender.hh` — PUMP pipeline（D12, D13）

```cpp
inline auto
tree_local_flush(tree_sched* owner, tree_flush_request req) {
    return owner->submit_flush_fold(std::move(req))
        >> flat_map([owner](flush_fold_result&& fr) {
            if (fr.st != flush_stage_status::ok || fr.partitions.empty()) {
                return just(flush_merge_request{
                           .round_id        = fr.round_id,
                           .mapping_results = {},
                       })
                    >> flat_map([owner](flush_merge_request&& mr) {
                           return owner->submit_flush_merge(std::move(mr));
                       });
            }

            auto n = fr.partitions.size();
            auto round_id = fr.round_id;

            return just(std::move(fr))
                >> push_result_to_context()
                >> loop(n)
                >> concurrent()
                >> get_context<flush_fold_result>()
                >> flat_map([](flush_fold_result& ctx, size_t i) {
                       auto& part = ctx.partitions[i];
                       auto* worker = registry::tree_worker_at(
                           part.read_domain_index);
                       return worker->submit_leaf_mapping(flush_mapping_req{
                           .round_id          = ctx.round_id,
                           .read_domain_index = part.read_domain_index,
                           .base_manifest     = ctx.base_manifest,
                           .groups            = part.groups,
                       });
                   })
                >> to_vector<flush_leaf_group_result>()
                >> pop_context()
                >> flat_map([owner, round_id](
                       std::vector<flush_leaf_group_result>&& results) {
                       return owner->submit_flush_merge(flush_merge_request{
                           .round_id        = round_id,
                           .mapping_results = std::move(results),
                       });
                   });
        }) >> flat();
}
```

### 6. tree_lookup_sched

**本 step 零修改。**

## Fail-Fast 矩阵

| 检查点 | 条件 | 动作 |
|---|---|---|
| `tree_worker_sched::advance(leaf_mapping)` | `req.rdi != self.rdi` | `panic_inconsistency` |
| `tree_worker_sched::advance(leaf_mapping)` | `base_manifest == nullptr` | `panic_inconsistency` |
| `keys_to_leaf_groups` | `leaf_order.empty() && !groups.empty()` | `unsupported_shape_change` |
| `keys_to_leaf_groups` | key 超出 tree range | `unsupported_shape_change` |
| `keys_to_leaf_groups` | key 落入 leaf 间 gap | `panic_inconsistency` |
| `merge_lookup_leaf_groups` | span 不连续 | `panic_inconsistency` |
| `handle_flush_merge` | `round_id` 不在 active_rounds | `panic_inconsistency` |

## 偏差记录

| # | 偏差 | 说明 |
|---|---|---|
| Δ-3 | `flush_leaf_group` 新增 `keys` span | RSM §4.7 语义对齐 |
| Δ-4 | merge 前按 `read_domain_index` 排序 | 到达顺序不保证 = 投递顺序 |
| Δ-5 | `tree_sched` 拆 `_flush_fold` + `_flush_merge` | Phase 4 `_tree_flush` 被取代 |
| Δ-6 | leaf mapping 从 `tree_lookup_sched` 移到 `tree_worker_sched` | RSM §4.7 原设计放 lookup；经讨论改为 worker——flush page 级工作统一 worker，代码隔离 |
| Δ-7 | `flush_lookup_req` 重命名 `flush_mapping_req` | 目标 scheduler 变了 |

## 新增文件 / 修改文件

| 文件 | 操作 | 说明 |
|---|---|---|
| `tree/leaf_mapping.hh` | **新建** | `keys_to_leaf_groups()` + `merge_lookup_leaf_groups()` |
| `tree/flush_types.hh` | 修改 | `flush_leaf_group` + rename + `flush_fold_result` + `flush_merge_request` |
| `core/leaf_order.hh` | 修改 | `find_leaf_for_key()` |
| `tree/worker_scheduler.hh` | 修改 | `_leaf_mapping` PUMP 五件套 + queue + drain |
| `tree/owner_scheduler.hh` | **重写** | `_flush_fold` + `_flush_merge` 取代 `_tree_flush` |
| `tree/sender.hh` | 修改 | `tree_local_flush()` 组合 pipeline |
| `tree/lookup_scheduler.hh` | **不动** | |

## 验证计划

### 算法层

1. `find_leaf_for_key`：空 index / 单 leaf / 三 leaf / 边界 key
2. `keys_to_leaf_groups`：happy path / empty tree / key 超出 range / gap panic
3. `merge_lookup_leaf_groups`：dedupe / error propagation

### PUMP pipeline 层

4. P=1：fold → 1 worker mapping → merge → result
5. P=4：fold → 4 concurrent mapping → to_vector → merge → result
6. empty round：skip fanout → merge → ok
7. error：一个 shard 返回 `unsupported_shape_change` → merge 传播

### 生命周期层

8. round 跨 tick：fold park → pipeline → merge unpark
9. span 有效性：workset 不 mutate → keys span 全程有效
