# 029 — Phase 9 Owner 闭环 Production 实装 Review

> Review 对象：未提交的 8 个改动文件 + 1 个新文件
>
> - `apps/inconel/core/data_area_heads.hh`（新建）
> - `apps/inconel/core/checkpoint_guard.hh`
> - `apps/inconel/core/registry.hh`
> - `apps/inconel/runtime/builder.hh`
> - `apps/inconel/tree/flush_types.hh`
> - `apps/inconel/tree/owner_scheduler.hh`
> - `apps/inconel/tree/sender.hh`
> - `apps/inconel/value/allocator.hh`
> - `apps/inconel/value/scheduler.hh`
>
> 对照设计文档：`ai_context/inconel/plan/029_owner_closure.md`（含 2026-04-16 review 修正 + combined_root 塌陷拍板）。
>
> Review 口径：
>
> 1. 只评审 production 实装是否 ① 兑现 029 设计 contract、② 不引入会产生错误 manifest / 错误 retire / 错误持久化语义的 bug。
> 2. "比设计更严的实装"如果不破坏 contract 也不挤兑 v1 性能锚点，按 ✅ 保留；只有"违反 contract 或破坏可观测语义"的偏离才记成 finding。
> 3. 不读测试文件（按 CLAUDE.md 例外条款，本次是 production code review，不属于"测试维护者"角色）。

---

## 1. 结论

Phase 9 核心算法实装**大方向无误**，substitution_iv 区间模型 / prune 级联 / reverse_topology 全量重编号 / Plan + writer + manifest rebuild 都按设计落到位。

**当前待收敛 2 条 significant、5 条 minor，无 blocking**：

- 原 blocking（B12 `is_root_change` 多余的 slot_index 条件）已在 review 之前由 codex 自行修复；本文 §2.0 保留记录作为历史参考。
- **Significant** 2 条偏离设计文档的拍板，但**调用方契约不变**——倾向更新文档承认实装方案。
- **Minor** 5 条是注释缺漏 / 构造责任错位 / dead path 隐藏 bug 风险，一次性扫掉。

在 significant 定向 + minor 收敛前，029 可以作为 "Phase 9 owner closure 实装草案" 先 land（与 027 `1c5f853` 保留已知失败测试 land 的约定一致），后续 commit 继续扫。

---

## 2. Findings

### ⚪ 历史记录 — 原 blocking B12 已在提交前修复

#### B12 — `is_root_change` 把 root-stable 写新 slot 误判为 root-change（已修）

**位置**

- `apps/inconel/tree/owner_scheduler.hh::_owner::is_root_change`
- 设计参照：`029_owner_closure.md` §2.6、`flush_and_frontier_switch.md` §6.1

**原 review 时看到的 diff 快照**：

```cpp
return !(node->replaces_old_paddrs.size() == 1
         && node->replaces_old_paddrs[0] == base_manifest->root_range_base
         && node->new_range_base == base_manifest->root_range_base
         && node->new_slot_index
             == base_manifest->slot_index(base_manifest->root_range_base));
//          ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
//          BUG: 这一条永远不成立 → 任何重写 root 都被识别成 root-change
```

§4.2 case 1 写新 slot 时 `new_slot_index = cur_slot + 1`，必然 `≠ base.slot_index(...)`；会导致每轮 root-stable flush 都错误触发 superblock FUA 写 + 推迟 `superblock_safe_lsn` / `recovery_safe_lsn` 推进。

**当前代码状态**：codex 在 review 之间的某个 diff 中已经删掉了 `new_slot_index` 条件。当前定义是：

```cpp
return !(node->replaces_old_paddrs.size() == 1
         && node->replaces_old_paddrs[0] == base_manifest->root_range_base
         && node->new_range_base == base_manifest->root_range_base);
```

判定语义正确：root_range_base 不变 → root-stable；root_range_base 变了或 root 是新合成层 → root-change。

保留此记录是因为：(a) review 首发版本基于更早 diff，留痕避免将来 audit 时困惑；(b) 这条 bug 足够隐蔽（只在长跑 flush 时暴露成无谓 FUA 放大）——作为回归测试的必检项写进后续端到端测试 step 的 checklist。

---

### 🟡 P1 / Significant — 文档与实装方向偏离，建议讨论后定向

#### B9 — `build_leaf_order_full` 全量重建，未走 §6.3 window slicing

**位置**

- `apps/inconel/tree/owner_scheduler.hh::_owner::collect_leaf_items` + `build_leaf_order_full`
- 设计参照：`029_owner_closure.md` §6.3 Step A-D（window slicing + base 字节复用）

`collect_leaf_items` 把整个 new tree 的所有 leaf（含 paddr ref 子树展开 + 新 leaf）按 in-order 全列出；`build_leaf_order_full` 一次性重建整条 `leaf_order` + `fence_pool`，不复用 `base_lo.fence_pool` 字节。

实际复杂度仍是 O(L)（与 §6.3.5 论证的输出下界一致），结果正确，但偏离 §6.3 的 "window slicing + 未改 spans 字节继承" 设计意图。失去：

1. fence_pool 字节复用机会（base_lo 里未变 leaf 的 fence 字节本可不复制）
2. 后续做 sliced rebuild 优化的 carrier 形态。

**为什么不算 blocking**

按 §6.3.5 论证，O(L) ~3.87M leaves × 一次 hashmap probe ≈ 50-80ms CPU，远低于一次 NVMe flush 总时长（>1s 量级）。v1 性能锚点不卡这条。

**两种修法**

- **(a) 文档对齐 [推荐]**：更新 §6.3 拍板 "v1 全量重建是合法实装"，把 window slicing 降级为 post-v1 优化。理由：实装更简单、bug 面更小、性能 §6.3.5 已论证可接受。
- **(b) 代码对齐**：让 codex 改成 §6.3 写的 window slicing。

#### D — `_flush_merge` handle 输出变 variant，违反 §10.2 选项 X 拍板

**位置**

- `apps/inconel/tree/flush_types.hh::flush_merge_result`（新加的 `variant<done, root_stable, root_change>`）
- `apps/inconel/tree/owner_scheduler.hh::_flush_merge` cb 出 `flush_merge_result`
- `apps/inconel/tree/sender.hh::flush_pipeline::continue_after_merge`
- 设计参照：`029_owner_closure.md` §10.2（拍板"选项 X"：merge handle 内部异步发 sender，cb 末端直接返 `tree_flush_result`）

实装做法：merge handle 同步完成 §2-§4 + §6 + 启动 writes pipeline 后，writes 完成回调把"下一步该做什么"封装成 variant 通过 cb 回传给外层 sender；外层 sender visit variant 后 dispatch 到 `submit_finalize_flush_round` 或 `submit_update_superblock + submit_finalize_flush_round`。

这和 §10.2 拍板的"选项 X"不同——更接近"选项 Y"的精神（把 update_superblock + finalize 拆成独立 handle，dispatch 暴露给外层 sender）。

**但**：`tree_local_flush` 顶层 sender 把 dispatch 封装在内部 `flush_pipeline::continue_after_merge` 里，**调用方契约不变**——依然是"一个 sender，回 `tree_flush_result`"，符合 flush_module_guide.md §2.4 的整体形态。

唯一的语义违反：`submit_flush_merge` 这个**单独**入口的"通用语义"应该是"做完合并到 result"，按构造 B 应该自包含；现在它返回 variant 要求 caller 进一步 dispatch。

**两种修法**

- **(a) 文档对齐 [推荐]**：更新 §10.2 拍板 codex 方案，按构造 B 把 `submit_flush_merge` 重命名带限制词（如 `submit_flush_merge_decision` / `submit_flush_merge_with_followup`），明确语义是"返回后续步骤决策而非完整结果"。
- **(b) 代码对齐**：把 update_superblock + finalize 内联回 merge handle，merge handle cb 直接出 `tree_flush_result`，不暴露 variant。

倾向 (a)。理由：codex 拆分让"等 update_superblock 完成才推 superblock_safe_lsn"的语义更清晰——finalize handle 自然在 sender 顺序里被触发，merge handle 内部不需要额外维护 inflight 状态机。`pending_writes` 表的设计也更干净。

---

### 🟢 P2 / Minor — 一次性扫掉

#### A1 — `tree_allocator::recycle` 缺 §3.4 PHASE 9 NARROWING 注释

**位置**：`apps/inconel/tree/owner_scheduler.hh::tree_allocator::recycle`

代码逻辑没问题（直接 `free_ranges.try_enqueue`），但 §3.4 明确要求加显式 narrowing 注释，说明：

1. 本 step 内 `recycle()` 没有 producer，永远不被调用；
2. RSM §4.4 / cross_doc §6.2 要求的 tree_node invalidate barrier **不实装**——frontier_switch step 会同时补 producer + barrier。

否则未来读到此函数的人会以为 `recycle()` 已经接通而错误地调用它。

#### B14 — `succeed_pending_round` 推 `superblock_safe_lsn` 缺隐式依赖注释

**位置**：`apps/inconel/tree/owner_scheduler.hh::tree_sched::succeed_pending_round`

```cpp
state.superblock_safe_lsn = std::max(
    state.superblock_safe_lsn, round->flushed_max_lsn);
```

正确性依赖 sender pipeline 顺序：root-change 路径下 `submit_finalize_flush_round` 一定在 `submit_update_superblock` cb 之后被触发，所以 finalize 内推 `superblock_safe_lsn` 时 superblock 必然已 durable。

**但代码里没有任何注释说明这条依赖**。未来重构 `flush_pipeline::continue_after_merge` 顺序时容易破坏，且不会被任何编译期检查捕获。

修法：在 `succeed_pending_round` 加注释明确"依赖 caller（finalize sender）保证 root-change case 下 update_superblock 已完成；调用顺序见 `tree/sender.hh::flush_pipeline::finalize_root_change`"。

#### B15 — `finalize_q` `ok=false` 路径用 `rollback=false`，dead path with hidden bug

**位置**：`apps/inconel/tree/owner_scheduler.hh::tree_sched::advance` 的 finalize 分支

```cpp
tree_flush_result res = r->args.ok
    ? succeed_pending_round(round_id, committed_slot)
    : fail_pending_round(round_id, /*rollback_allocated_ranges=*/false);
//                                                              ^^^^^
```

当前没有 caller 走 `ok=false` 进 finalize：

- write 失败 → `complete_pending_tree_writes(round_id, false)` → `fail_pending_round(rollback=true)`，**不**进 finalize_q
- update_superblock 失败 → `flush_pipeline::panic_update_superblock_failure` 直接 panic，**不**调 `submit_finalize_flush_round`

所以 `ok=false` 是 dead path。但代码留着这条分支 + `rollback=false` 的选项，未来如果有人加了路径调用 `submit_finalize_flush_round({ok=false})`，会**默默泄漏**已分配 ranges。

修法二选一：

- 删 `ok=false` 分支：finalize handle 强制要求 `r->args.ok == true`，否则 `panic_inconsistency`。
- 改 `rollback=true`：与 write-failure 路径行为统一。

#### E1 — `tree_sched` 构造函数未对称 store `shared_heads->tree_head_lba`

**位置**：

- `apps/inconel/tree/owner_scheduler.hh::tree_sched::tree_sched`（构造函数没 store）
- `apps/inconel/runtime/builder.hh`（builder 显式 store 兜底）
- 对照：`apps/inconel/value/allocator.hh::per_device_value_state` 构造函数（已对称 store `value_head_lba`）

`tree_sched` 构造时 `state.alloc.head = data_area_base`，但**没有**:

```cpp
shared_heads->tree_head_lba.store(state.alloc.head.lba, std::memory_order_relaxed);
```

依赖 `runtime/builder.hh` 在外面手动补一行：

```cpp
shared_heads->tree_head_lba.store(tsched->state.alloc.head.lba, std::memory_order_relaxed);
```

这是**构造责任错位**——`value_allocator` 构造里自己负责 store，对称地 `tree_allocator` / `tree_sched` 构造也应该自己负责。否则未来 builder 改了就会忘 store，留下"shared_heads.tree_head_lba 仍是 0 / value 侧误判 tree 没占任何空间"的 bug。

修法：把 store 移进 `tree_sched` 构造函数尾部（与 `value_allocator` 对称），builder 那两行可以删一行（tree_head_lba 那行）。

#### G — `checkpoint_guard` 缺 §9.2 PHASE 9 NARROWING 注释

**位置**：`apps/inconel/core/checkpoint_guard.hh`

```cpp
struct checkpoint_guard {
    std::shared_ptr<const tree_manifest> manifest;
    retired_objects                      retired;       // ← 新增
};
```

字段加上去了，但 §9.2 要求的 narrowing 注释缺失：

```cpp
// PHASE 9 NARROWING: ~checkpoint_guard does NOT post a reclaim_task.
// Section §1.2: that destructor side-effect lands in the
// frontier_switch step, together with the matching reclaim_q
// consumer in tree_sched. Adding the destructor here without the
// consumer would let `retired` silently leak when guards drop.
```

否则未来读到此结构体的人不知道为什么 `retired` 字段进来了 destructor 却没投递 `reclaim_task`，可能"补完整"加上 destructor 副作用，引入与不存在的 consumer 之间的 silent 泄漏。

---

## 3. ✅ 比设计文档更严 / 更鲁棒的实装（保留）

以下几条是 codex 实装中比 029 文档要求**更严或更鲁棒**的部分，没破坏 contract，按 ✅ 保留：

1. **`merge_internal_old_paddr` 的 `source_kind::merged_child` 递归 dispatch**（owner_scheduler.hh 内 substitution_iv 处理）：
   当 worker 单 contributor 给的 internal child 实际上是 owner 视角的 shared ancestor（多 contrib），自动切到 `merged_child` 路径递归 `merge_old_paddr`。这处理了 §2.4.E 没显式覆盖的"嵌套 shared ancestor"边界。

2. **`prune_child_ref` 父级联时 retire 父自身 range**：
   §6.3.2 要点 4 要求父压扁时 internal 自身的 `replaces_old_paddrs` 也进 `retired.old_ranges`，codex 正确实装（包括"父只剩单子上提"的子情况）。

3. **`make_empty_root_leaf` 完整实装 §6.3.2 要点 5 塌陷规则**：
   `leaf_page_builder.init + finalize` 写出 magic + record_count=0 + CRC，`replaces_old_paddrs={}` 走 §4.2 case 0 fresh range，与文档要点 5 完全对齐。

4. **`collect_worker_proposals` 用 `reduce` 显式重抛 `exception_ptr`**：
   设计文档 §11 写的是 `to_vector<worker_tree_proposal>()`，但 PUMP `reduce` 默认不因流元素异常终止流（参见 SENDERS_DETAIL.md §reduce），如果 worker 抛异常而 `to_vector` 不重抛会丢异常 + 进死循环。codex 用 `reduce` 显式 `std::rethrow_exception(item)`，**比文档更鲁棒**。

5. **`build_reverse_topology_full` 严格按 §6.3.4 实装**：
   按 combined_root children list 精确决定每张 leaf new parent；unchanged_subtree DFS 全量重新分配 idx；承认全量重编号；leaf_parent_idx 按 new_lo.spans 顺序填，未改 leaf 通过 base_topo→range_base→new_idx 映射。完全对齐 review P0-1 收敛后的算法。

6. **`assign_planned_paddrs` 后序遍历 + Plan case 0/1/2+ 全覆盖**：
   `reformat_internal_node` 在 Plan 之后 patch child_base + 重算 CRC（leaf 也重算 CRC，冗余但无害）；case 2+ pairwise leaf merge 的 carrier + P_other 退役处理正确。

---

## 4. 推荐处理顺序

当前 commit 是 "Phase 9 owner closure 实装草案" land（无 blocking；B12 已提交前修复）。后续 commit 收敛：

1. **第一步**：定向 B9 / D。倾向都走文档对齐——更新 029 §6.3 / §10.2 承认 codex 的实装方案，给 §10.2 的 `submit_flush_merge` 重命名带限制词（如 `submit_flush_merge_with_followup`）。
2. **第二步**：批量补 minor（A1 / B14 / G 注释 + E1 构造对称 + B15 dead path 决议），单次 commit 扫掉。

这两轮做完后 029 进入 "production-green 可合入" 状态。frontier_switch / reclaim consumer / INC-040 / 端到端测试按 029 §1.2 留给后续 step。
