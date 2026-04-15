# 027 — Worker 内存树提案 Review

> Review 对象：当前 worktree 上的 `027` 相关 production 代码与设计文档。
>
> Review 口径：
>
> 1. `027` 只评审 Phase 7 worker 侧输出模型重构，不把“Phase 9 owner merge 尚未实现”本身记成失败项。
> 2. 但 `027` 已经明确依赖 `flush_development_plan.md` 里 2026-04-15 拍板的 Gap 1 / Gap 2 / Gap 3 契约；凡是这些契约已经写死、代码却继续漂移的，记为 finding。
> 3. 额外检查一条工程约束：即使 `Phase 8` 计划单独做 dead-code/test sweep，当前 worktree 若已引入可观测回归，也要如实记账。

---

## 1. 结论

**027 的 production 主体现在基本成立；剩余问题已经收敛到 Phase 8 sweep。**

已经成立的部分：

1. worker 输出模型已经从 `changed_nodes` 切到 `mem_tree_node + child_ref + worker_tree_proposal`。
2. `build_key_partitions()` 已按 contiguous leaf range 做分配，和 pairwise leaf merge 的真实语义一致。
3. `inconel` 主目标可以正常编译。
4. 第一轮 review 里指出的 P0 + 三条 P1（internal child-ready 门控、Gap 2 `flushed_max_lsn`、Gap 1A `memtable_losers`、027 正文 partition 双 spec）都已经同步修正。

当前仍挡住收口的点：

1. flush 相关旧测试 target 目前整体不 build-clean。

因此更准确的判断是：

- **027 的 worker 输出模型改向可以接受，而且当前 production 实现未再看到新的阻塞性问题**
- **027 仍不能算整个 step 冻结完成，因为紧邻的 Phase 8 test/dead-code sweep 还没做**

---

## 2. 验证执行

本次 review 实际执行：

1. `cmake --build build -j4 --target inconel`
2. `cmake --build build -j4 --target inconel_test_flush_carriers`
3. `cmake --build build -j4 --target inconel_test_candidate_build`
4. `cmake --build build -j4 --target inconel_test_leaf_mapping`
5. 逐文件静态检查：
   - `apps/inconel/tree/candidate_build.hh`
   - `apps/inconel/tree/owner_scheduler.hh`
   - `apps/inconel/tree/flush_types.hh`
   - `apps/inconel/tree/flush_round_state.hh`
   - `apps/inconel/tree/memtable_fold.hh`
   - `apps/inconel/tree/sender.hh`
   - `apps/inconel/tree/worker_scheduler.hh`
   - `ai_context/inconel/plan/027_worker_inmemory_tree_proposal.md`
   - `ai_context/inconel/plan/flush_development_plan.md`

验证结果：

1. `inconel` 主目标编译通过。
2. `inconel_test_flush_carriers` 编译失败：仍引用已删除的 `flush_mapping_req` / `flush_leaf_group` / `mapping_results` / 旧版 `build_key_partitions()` 签名。
3. `inconel_test_candidate_build` 编译失败：仍引用已删除的 `candidate_build_state` / `flush_leaf_candidate` / `merge_and_build_leaf()` / `candidate_need_read` 等旧 Phase 6 符号。
4. `inconel_test_leaf_mapping` 编译失败：仍 `#include "apps/inconel/tree/leaf_mapping.hh"`，而该文件已删除。

---

## 3. Findings

### P2 — 当前 worktree 不是 build-clean；Phase 8 sweep 仍是必要收尾

**位置**：

- `apps/inconel/test/test_flush_carriers.cc:184`
- `apps/inconel/test/test_candidate_build.cc:185`
- `apps/inconel/test/test_leaf_mapping.cc:33`
- 对照计划：`ai_context/inconel/plan/flush_development_plan.md:448-500`

从计划看，Phase 8 本来就明确允许：

1. `candidate_build_state`
2. `candidate_need_read` / `candidate_done`
3. `flush_leaf_candidate`
4. `leaf_mapping.hh`
5. 旧 sender helper 和旧 carrier 引用

因此当前三个测试 target 的失败，本质上仍然是“旧测试还在引用 Phase 5/6 符号”，不是新的 production 回归。

但从 review 角度，这仍然是一个真实回归：

1. 相关测试目标目前无法编译
2. 编译失败还覆盖了 carrier contract、旧 helper、旧头文件等多个层面
3. 这意味着 Phase 7 branch 目前不能以“build-clean”状态收尾

**结论口径**

这条更适合记为 **P2 / release blocker for Phase 7 freeze**，而不是否定 027 设计方向本身。也就是说：027 production 代码方向目前可以接受，但紧邻的 Phase 8 sweep 不是“可有可无的清洁工”，而是当前分支的必要收尾。

---

## 4. 上一轮 Findings 的修复确认

下面这些问题在第一轮 review 里是 P0/P1；当前已核对为**已修复**，不再保留为 active finding。

### R-1 — internal cascade child-ready 门控已补齐

**位置**：

- `apps/inconel/tree/candidate_build.hh:162-178`
- `apps/inconel/tree/candidate_build.hh:1057-1066`
- `apps/inconel/tree/candidate_build.hh:1368-1402`

`internal_work` 新增了 `affected_child_internals`，`initialize_cascade()` 会把受影响的 internal child 建立到 parent 的 gating 列表里，`process_flush_round()` 在 internal build 阶段也明确检查 `all_children_built(iw)` 后才允许 build。

这把“ancestor cache-hit，但 changed child internal 仍在 NVMe 路上”那条 panic 路径关掉了。

### R-2 — Gap 2 的 `flushed_max_lsn` / empty-round 语义已同步

**位置**：

- `apps/inconel/tree/owner_scheduler.hh:277-289`
- `apps/inconel/tree/owner_scheduler.hh:367-431`
- `apps/inconel/tree/flush_round_state.hh:114-126`

当前实现已经：

1. 在 round 分配时计算 `flushed_max_lsn = max(pinned_gens[*].max_lsn)`
2. case 1（empty gens）返回 `ok, flushed_max_lsn=0`
3. case 2（empty workset）返回 `ok`
4. case 3（non-empty workset）才保留 `unsupported_unimplemented`
5. merge 返回始终带 `round.flushed_max_lsn`

这和 `flush_development_plan.md` 的 Gap 2 决议已经对上。

### R-3 — Gap 1A 的 `memtable_losers` 错误挂点已删除

**位置**：

- `apps/inconel/tree/flush_types.hh:277-290`
- `apps/inconel/tree/flush_round_state.hh:108-126`

`tree_flush_result` 和 `flush_round_state` 里的 `memtable_losers` 字段都已经删除，注释也明确写成“memtable-only loser 只挂 `memtable_gen.loser_durable_refs`”。

这部分 carrier API 现在不会再向后续实现暴露错误生命周期挂点。

### R-4 — 027 主文档的 partition 正文已改成 range-based

**位置**：

- `ai_context/inconel/plan/027_worker_inmemory_tree_proposal.md:229-245`
- `ai_context/inconel/plan/027_worker_inmemory_tree_proposal.md:299-300`
- `apps/inconel/tree/memtable_fold.hh:173-245`

027 正文现在已经直接写成 range-based，并补了“为什么不能用 `% worker_count`”的说明；实现与文档已重新对齐。

---

## 5. 不计入 027 失败的项

以下点存在，但按当前阶段划分，不记为本次 review finding：

1. owner 侧真正的 hybrid-tree merge / paddr allocation / CRC patch / writes / new manifest 构造尚未实现。这是 Phase 9 的职责，不把“merge 仍是 stub”本身记成 027 失败。
2. `checkpoint_guard.retired` 还没扩字段。这在 Gap 1B 里已经明确归到 Phase 9 / frontier_switch 组。
3. `tree_local_flush()` 顶层 sender 组合仍未落地。这同样跟 Phase 9 merge sender 是否能产生真实 `tree_flush_result` 绑定。

---

## 6. 建议顺序

如果按“现在什么最顺手、最不返工”的顺序推进，我的建议是：

1. **先做 Phase 8 sweep**：把旧测试 / 旧注释 / 旧 carrier / 旧 helper 清干净，至少恢复 flush 相关 target 的 build-clean。
2. **再开 Phase 9 owner merge**：当前 production 契约已经基本稳定，再往下推进不需要先补新的 worker/owner contract 修复。

第 1 条就是 027 完成后最直接、也最该顺手做的相邻项。

---

## 7. 当前判断

如果只问“027 有没有把 worker 输出模型从 overlay 改成内存混合树”，答案是 **有**。  
如果问“027 的 production 实现按当前设计约束能不能算 review 通过”，我当前判断是 **基本可以**。当前没有再看到新的 production 阻塞性问题；第一轮 review 抓到的 P0/P1 也都已经修掉。

但如果问“027 整个 step 能不能算彻底冻结结束”，我当前判断仍然是 **还差 Phase 8**，原因只有一条：

1. flush 相关旧测试 target 还没做 sweep，当前 worktree 不是 build-clean。

所以当前最准确的结论是：

- **027 production 代码：review 通过**
- **027 整体收口：还差 Phase 8 test/dead-code sweep**
