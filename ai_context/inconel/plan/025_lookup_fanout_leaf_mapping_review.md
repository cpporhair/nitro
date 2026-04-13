# 025 Review — Worker Fanout / Leaf Mapping

> 对 Phase 5（step 025）实现代码的 review。算法测试（`test_leaf_mapping`）全部通过。现有测试（`test_flush_carriers`）编译失败。

## 测试结果

| 测试 | 结果 | 说明 |
|---|---|---|
| `inconel_test_leaf_mapping` | **全过** | 15 个 case（find 3 + mapping 7 + merge 5） |
| `inconel_test_flush_carriers` | **编译失败** | 引用已移除的 `_tree_flush::req`、`schedule_flush`、`flush_lookup_req` |

## P0 — 破坏了现有测试 `test_flush_carriers`

Phase 5 把 `_tree_flush` 拆成 `_flush_fold` + `_flush_merge`，把 `flush_lookup_req` 重命名为 `flush_mapping_req`。但没有同步更新 `test_flush_carriers.cc`，导致编译失败：

```
错误：'_tree_flush' 不是 'apps::inconel::tree' 中的一个类型名
错误：'struct tree_sched' has no member named 'schedule_flush'
```

受影响位置：

| 文件 | 行 | 引用 | 应改为 |
|---|---|---|---|
| `test_flush_carriers.cc:184` | `tree::flush_lookup_req` static_assert | `tree::flush_mapping_req` |
| `test_flush_carriers.cc:186` | 同上 error message | 同上 |
| `test_flush_carriers.cc:1494` | `tree::_tree_flush::req` | `tree::_flush_fold::req` |
| `test_flush_carriers.cc:1505` | `ts.schedule_flush(r)` | `ts.schedule_fold(r)` |
| `test_flush_carriers.cc:1525` | `tree::_tree_flush::req` | `tree::_flush_fold::req` |
| `test_flush_carriers.cc:1539` | `ts.schedule_flush(r)` | `ts.schedule_fold(r)` |

修法：更新这些引用，并调整对应的 `cb` 类型从 `tree_flush_result&&` 到 `flush_fold_result&&`。测试逻辑也要适配——Phase 4 的 handle 测试以前验证 `tree_flush_result`，现在 fold handle 返回的是 `flush_fold_result`，需要验证其字段然后单独测 merge handle。

## P1 — fold 的 empty-workset 路径提前 unpark round

`owner_scheduler.hh:287-303`，fold handle 遇到 empty workset 时直接调 `state.active_rounds.erase(round_id_v)` unpark 了 round。

当 PUMP pipeline 连接后，fold 返回 `partitions = {}` → pipeline 判断 `partitions.empty()` → 调 `submit_flush_merge(round_id)` → merge handle 用 `state.active_rounds.find(round_id_v)` 查找 round → **找不到 → panic**。

同一段还有一行死代码：`build_flushed_gens_by_front(round.pinned_gens)` 的结果赋给局部变量 `gens_by_front`，但 `flush_fold_result` 没有这个字段，计算结果被丢弃。

修法：

```diff
 if (round.workset.empty()) {
-    auto gens_by_front =
-        build_flushed_gens_by_front(round.pinned_gens);
-    auto base_manifest = round.pinned_base_guard->manifest;
-    state.active_rounds.erase(round_id_v);  // unpark
-
     flush_fold_result res{
         .round_id      = flush_round_id{round_id_v},
         .st            = flush_stage_status::ok,
         .partitions    = {},
-        .base_manifest = base_manifest.get(),
+        .base_manifest = round.pinned_base_guard->manifest.get(),
     };
     r->cb(std::move(res));
     delete r;
     progress = true;
     continue;
 }
```

规则：**fold 永远不 unpark round。所有 unpark 由 merge handle 执行。**

## P2 — `tree_local_flush()` pipeline 未实现

`sender.hh` 里只有注释：

```cpp
// ── tree_local_flush — NOT YET IMPLEMENTED ────────────────────
//
// The composed PUMP pipeline (fold → fanout → merge) will be
// implemented when a real caller exists to force full template
// instantiation.
```

025 设计文档 §5 要求在 sender.hh 实现组合 pipeline：`submit_flush_fold >> flat_map(loop >> concurrent >> flat_map(worker->submit_leaf_mapping)) >> to_vector >> flat_map(submit_flush_merge)`。

实现者的理由是 PUMP `connect<ctx_t>()` 是 lazy 的，没有 caller 实例化模板就无法暴露类型错误。这个理由技术上成立——但结果是 fold → fanout → merge 的端到端路径完全没有验证。P1 的 bug 就是一个例子：如果 pipeline 被实例化并测试，empty-workset 路径的 unpark 问题会在测试中暴露。

建议：修完 P0 和 P1 后，在 `test_leaf_mapping.cc`（或新测试文件）里加一个单线程端到端测试——手动驱动 fold → advance → (手动调 worker advance) → merge → advance，验证完整链路。不需要完整 PUMP runtime，只需要验证三个 handle 的串联语义。

## P3 — 注释残留旧名字 `flush_lookup_req`

| 文件 | 行 | 内容 |
|---|---|---|
| `flush_types.hh` | 22 | `flush_lookup_req` / `flush_worker_req` re-gain... |
| `flush_types.hh` | 105 | converting each into a `flush_lookup_req` with |
| `flush_round_state.hh` | 8 | `flush_lookup_req.groups` |
| `flush_round_state.hh` | 25 | Each `flush_lookup_req` / `flush_worker_req`... |
| `flush_round_state.hh` | 98 | `flush_lookup_req.groups`. Phase 3 leaves... |

修法：全局替换注释中的 `flush_lookup_req` → `flush_mapping_req`。

## P4 — `build_key_partitions` 用 `tree_lookup_count()` 做 worker fanout 分区

`owner_scheduler.hh:307`：

```cpp
auto lookup_count = core::registry::tree_lookup_count();
```

Phase 5 的 fanout 目标是 `tree_worker_at(rdi)`，不是 `tree_lookup_at(rdi)`。`lookup_count == worker_count` by design（paired），所以结果正确。但语义上应该用 `tree_worker_count()`。

优先级低。如果改，Phase 4 测试中直接构造 `tree_sched::advance()` 的 test 也要跟着改。

## 通过项

| 文件 | 评价 |
|---|---|
| `tree/leaf_mapping.hh` | `keys_to_leaf_groups` 算法正确：二分 + 顺序 scan + gap panic + shape_change。`merge_lookup_leaf_groups` dedupe + contiguity check 正确。测试全过。 |
| `core/leaf_order.hh` | `find_leaf_for_key` 二分查找正确，空 index / 单 leaf / 多 leaf / boundary 全覆盖 |
| `tree/worker_scheduler.hh` | `_leaf_mapping` PUMP 五件套完整，与 `_build_leaf_candidates` 对称。drain cap / rdi validation / manifest null check 齐全 |
| `tree/owner_scheduler.hh` | `_flush_fold` + `_flush_merge` 五件套正确。fold handle 的 validate → park → fold → partition 逻辑与 Phase 4 一致（除 P1）。merge handle 的 round lookup + merge + unpark 结构合理 |
| `tree/flush_types.hh` | `flush_mapping_req` / `flush_fold_result` / `flush_merge_request` / `flush_leaf_group.keys` carrier 形状符合 025 设计 |

## 修复优先级

| 优先级 | 问题 | 估计工作量 |
|---|---|---|
| **必须修** | P0 旧测试编译失败 | 小：改 6 处引用 + 适配 cb 类型 |
| **必须修** | P1 empty-workset unpark | 小：删 3 行 |
| 建议修 | P2 端到端测试 | 中：写一个手动驱动三 handle 串联的测试 |
| 建议修 | P3 注释残留 | 小：全局替换 |
| 可选 | P4 lookup_count → worker_count | 小但影响旧测试 |
