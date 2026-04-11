# 023 — Phase 3 Production Code Review

> Review 对象：当前 worktree 上已落地的 step 023 production 代码 + 设计文档 + 边界测试。
>
> 覆盖文件：
>
> 1. `ai_context/inconel/plan/023_flush_manifest_round_state.md`
> 2. `apps/inconel/core/leaf_order.hh`
> 3. `apps/inconel/core/tree_manifest.hh`
> 4. `apps/inconel/core/checkpoint_guard.hh`
> 5. `apps/inconel/core/retired_objects.hh`
> 6. `apps/inconel/core/registry.hh`
> 7. `apps/inconel/tree/flush_types.hh`
> 8. `apps/inconel/tree/flush_round_state.hh`
> 9. `apps/inconel/tree/owner_scheduler.hh`
> 10. `apps/inconel/tree/scheduler.hh`
> 11. `apps/inconel/tree/sender.hh`
> 12. `apps/inconel/runtime/builder.hh`
> 13. `apps/inconel/test/test_flush_carriers.cc`
>
> 本轮结论以"当前 commit 落地内容"为准。前一轮 review 挂出的 3 条 finding 都已经收敛在本轮 worktree 中。

---

## 1. 结论

**本轮 review 未发现新的 blocking bug / API regression。**

上一轮 review 挂出的 1 条 high + 2 条 medium 都已经收敛：

1. **H-1（已修复）** — `flush_round_state` 现在 mirror `tree_flush_result` 的全部 6 个字段，包含原本缺失的 `st: flush_stage_status` 与 `new_manifest: shared_ptr<const tree_manifest>`。`test_flush_carriers.cc` 用 SFINAE 在编译期锁住字段名 + 字段类型，并在 `test_flush_round_state_defaults` 里把 round_state 默认值与 `tree_flush_result` 默认值逐字段对比，任何后续 phase 的字段漂移都会立即露馅。
2. **M-1（已修复）** — `tree_sched::advance()` 在 `base_guard != nullptr` 之后追加 `base_guard->manifest != nullptr` 的 `core::panic_inconsistency`。注释明确写明这是 023 review M-1：carrier-contract 违约，不属于 Phase 4 范畴。新的 forked panic 测试 `in_child_provoke_null_inner_manifest` 端到端验证 SIGABRT。
3. **M-2（已修复）** — `leaf_order_index::fence_lower / fence_upper` 改走私有 helper `fence_slice("site", off, len)`，在 `off > pool_size` 或 `len > pool_size - off` 时显式 `panic_inconsistency` 并打印 `site / off / len / pool_size`。`std::string_view::substr` 的 silent truncate / `noexcept`-throw 这两条都被堵死。新的 forked panic 测试 `in_child_provoke_leaf_order_out_of_bounds` 端到端验证 SIGABRT。

`test_flush_carriers` 单元测试在每条修复路径上都新增了对应断言；现有 `inconel_test_runtime` / `inconel_test_tree_value` 不退化。

### 1.1 验证执行

本轮 review 实际跑过：

1. `cmake --build build -j$(nproc)` — 干净，无 warning / error
2. `./build/inconel_test_flush_carriers` — 18 项 + 4 个 forked panic（含 M-1 / M-2），全部 OK
3. `./build/inconel_test_runtime` — clock + slru registry / fail-fast / 400-key e2e，全部 OK
4. `./build/inconel_test_tree_value` — 5 个 case 全部 OK
5. 对照 step 023 设计文档、`runtime_state_machine.md`、`flush_and_frontier_switch.md`、`cross_doc_contracts.md` 做 carrier / owner / result 字段一致性检查

panic 测试通过 `fork() + waitpid()` 在子进程内 trigger，父进程检查 `WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT`，避免污染主测试进程。

---

## 2. 仍需记住的 Residual Risks

以下不是本轮的 open finding，但仍是后续实现要明确记住的边界：

1. `flush_round_state` 现在虽然 mirror 了 `tree_flush_result` 的全部字段，但 Phase 3 没有任何 production 代码会真正实例化它（D22）。Phase 4 落 fold 时是第一次 round_state 真正进入 `tree_state.active_rounds`；那一步必须用本 step 已冻结的 mirror 字段，**不允许**再"加一个 result 装一份" 类的旁路存储。
2. `tree_sched::advance()` 现在的 panic 风格是 Phase 3-only。Phase 4 一旦引入合法的"empty round → fast path success" 语义，必须在同一处把 `sealed_gens.empty()` 从 panic 降级为 structured status，而不是再加一个新的 advance() 路径。`base_guard == nullptr` 与 `base_guard->manifest == nullptr` 应该仍然保留为 panic。
3. `leaf_order_index::fence_lower / fence_upper` 现在走 helper + panic，accessor 不再带 `noexcept`。这是有意的——`panic_inconsistency` 是 `[[noreturn]]`，它不抛异常，但 helper 调用图里没必要再做 noexcept 承诺。Phase 7 root-stable writer 在构造 leaf_order 时应当**只**经由"先确认 fence_pool 已 reserve 足够空间，再 push leaf_span" 这种顺序写入路径，不要在 builder 里把 accessor 当 round-trip self-test 用——那样会让构造期 panic 与 round-trip panic 缠在一起难以定位。
4. recovery 文档仍未同步 tree-domain 新拓扑。022 review 已经提醒过一次，本步骤再次提醒。如果 Phase 4 之前不补一个独立的 recovery sync step，后面进入 `leaf_order` rebuild / frontier switch / reclaim 时还会再撞一次。
5. 设计文档的 follow-up doc-sync（Δ-1 / Δ-2 同步回 RSM §4.2、cross_doc §1、FF §3.1）仍未完成。本 step 自己的设计文档明确说"不可跳过"。这件事应该作为 Phase 3 整体收尾里 commit 之后的下一个 step，而不是丢给 Phase 4。

---

## 3. 相邻提醒

1. step 023 设计文档已经把 doc-sync follow-up 标成"必须紧接，不可跳过"。当前状态：尚未完成。
2. cache ownership migration 仍应保留为独立 step，不要混进 Phase 3 收尾或 Phase 4 fold 里。
3. `tree_state.active_rounds` / `next_round_id` 的引入应该是 Phase 4 fold 的第一笔 production 改动；Phase 3 测试用 SFINAE 已经断言这两个字段当前不存在，Phase 4 加上它们时本测试会自动 fail，那就是开始 Phase 4 的明确信号。
