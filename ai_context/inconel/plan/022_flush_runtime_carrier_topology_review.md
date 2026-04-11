# 022 — Phase 2 Production Code Review

> Review 对象：当前 worktree 上已落地的 Phase 2 production 代码。
>
> 覆盖文件：
>
> 1. `apps/inconel/format/format_profile.hh`
> 2. `apps/inconel/core/tree_geometry.hh`
> 3. `apps/inconel/core/tree_manifest.hh`
> 4. `apps/inconel/core/registry.hh`
> 5. `apps/inconel/tree/flush_types.hh`
> 6. `apps/inconel/tree/lookup_scheduler.hh`
> 7. `apps/inconel/tree/worker_scheduler.hh`
> 8. `apps/inconel/tree/scheduler.hh`
> 9. `apps/inconel/tree/sender.hh`
> 10. `apps/inconel/runtime/builder.hh`
>
> 本轮结论以“当前代码行为”为准，不再复述已经失效的旧 review 结论。

---

## 1. 结论

**本轮 review 未发现新的 blocking bug / API regression。**

上轮挂出的 3 条意见，当前 worktree 都已经收敛：

1. H-1 `tree_lookup_sched` 的 geometry 检查已从 **pointer identity** 改成 **value equality**，`inconel_test_runtime` 当前可正常运行通过。
2. M-2 `tree_manifest::geom` 的 non-null contract 已在 `tree_manifest::empty()` 和 `make_lookup_state()` 入口上做 fail-fast。
3. M-3 `flush_worker_req` 已移除跨异步 queue 的 borrowed `std::span` payload，Phase 2 carrier 不再冻结一个悬空风险接口。

我这轮额外 spot-check 了：

1. `./build/inconel_test_runtime`
2. `./build/inconel_test_tree_value`

这两条都运行通过；与用户提供的“9 个 executable 全部 all passed”状态一致。

---

## 2. 仍需记住的 Residual Risks

以下不是本轮的 open findings，但仍然是后续实现时要明确记住的边界：

1. `tree_manifest::geom` 和 `tree_lookup_sched_base::expected_geom` 仍然是 raw pointer contract。
   当前关键入口已经 fail-fast，但类型层并没有把 “non-null / self-consistent geometry” 封成不可绕过的构造约束。

2. 当前 runtime / executable 覆盖主要跑在 bootstrap geometry：
   `tree_page_size = 4096`、`shadow_slots_per_range = 1`。
   这说明当前 bring-up 路径已经闭环，但**还不能替代**后续对多-slot shadow range、multi-LBA tree page 的专门覆盖。

3. `tree_worker_sched` 仍然是 Phase 2 stub：
   `build_leaf_candidates()` 的 carrier / routing / fail-fast 已冻结，但真实 old-leaf read / candidate build / merge 行为尚未进入 review 范围。

---

## 3. 相邻提醒

1. recovery 文档仍需同步到新的 tree geometry carrier；这不是当前代码 bug，但会直接影响后续 recovery / superblock 路径怎么产出 manifest geometry。
2. cache ownership migration 仍应保持为独立 step，不建议把它混进当前 Phase 2 收尾 commit。
