# 019 — Value Resident Frame State Review

> reviewer 视角结论。基于 step 019 plan、production diff、测试补充和本地动态验证；不采信实现者自述作为结论。
> 当前 review 只记录阻塞问题、修复边界和已验证范围，不把测试输出反推成 spec。

## 结论

主体 blocker 已修复，但当前 **还不能提交**：只剩两处 production 注释仍在描述错误状态流转。

首轮 review 的 blocker 是：`open_frames_` 声明为 dirty active resident tier，但没有正常生产者，导致 `handle_read()` 的 open-frame tier 是死分支。Claude 的第二轮修复已经把这个状态机补上：

- `acquire_round_page()` 会把从 `allocatable_frames_` 或 allocator 获取的 frame 安装到 `open_frames_[ci]`。
- `publish_round()` 把 round pages 标记为 `writeback_inflight`，`open_frames_[ci]` 在 finalize 前仍可读。
- `commit_pages()` / `rollback_pages()` 在 round 结束时清理对应 `open_frames_` 引用。
- 被更新 round 替换掉的 inflight/dirty frame 继续由所属 `round_page` 持有，避免 double free。

reviewer 已新增 `case_13_open_frame_visible_until_finalize()`，直接覆盖 `prepare_persist` 后、`finalize_persist` 前的 open-frame 可读窗口。该测试证明 `open_frames_` 现在有真实 producer，且 `read_value()` 能在 finalize 前 0-NVMe 命中它。

## Findings

### 1. 已修复：`open_frames_` read tier 原先无正常生产者

原问题：

- `open_frames_` 被定义为每 class 当前 dirty active frame。
- `handle_read()` 声称优先查询 `open_frames_[ci]`。
- 但旧实现没有正常路径设置 `open_frames_[ci] = frame`。

为什么这是 blocker：

- step 019 的目标不是简单 rename `writable_pages_`，而是把 resident state 拆成明确的 dirty open state 和 clean allocatable state。
- 一个没有 producer 的 `open_frames_` read tier 会让代码声明一套状态机、实际维护另一套 owner 模型。

修复后行为：

- 新分配 / reopen 的 active frame 会进入 `open_frames_[ci]`。
- `publish_round()` 后 frame 进入 `writeback_inflight`，但仍在 `open_frames_[ci]` 可读。
- 当前 class 需要新 active frame 时，旧 dirty/inflight frame 会从 `open_frames_` displacement，继续由原 `round_page` 负责 finalize。
- finalize 时只在 `open_frames_[ci] == frame` 时清理引用，避免误清新一轮的 open frame。

reviewer-owned test：

- `case_13_open_frame_visible_until_finalize()` 直接调用 `prepare_persist()`，不走完整 `persist_values()`。
- 在 finalize 前断言 `open_frames_[0] != nullptr`、状态为 `writeback_inflight`、frame id 匹配 `value_ref`。
- finalize 前执行 `read_value()`，断言返回 body 且 NVMe read count 为 0。
- 最后 `finalize_persist(round_id, false)`，断言 `open_frames_[0]` 清空且 fresh page 被 rollback。

### 2. 待修：两处 production 注释仍描述错误流转

位置：

- `apps/inconel/value/scheduler.hh`：`acquire_round_page()` 中 full open frame 的 displacement 注释。
- `apps/inconel/value/scheduler.hh`：`rollback_pages()` 中 `writable` source 的注释。

具体问题：

- `acquire_round_page()` 注释写 “a full frame is displaced below so it lands in allocatable_frames_ on finalize”。这不对：full page finalize 后应该进入 `clean_readonly` readonly cache，或 multi-LBA drop；不应进入 `allocatable_frames_`。
- `rollback_pages()` 注释写 `writable` source “if still in open_frames_ it stays there, otherwise -> allocatable_frames_”。这也不对：当前代码会先清掉 matching `open_frames_[ci]`，然后把 resident source 恢复为 `clean_allocatable` 并推入 `allocatable_frames_[ci]`。

期望修复：

- 只改注释，不改行为。
- `acquire_round_page()` 注释应说明：full dirty frame 不能继续写，replacement 会清掉 `open_frames_` 槽位；旧 frame 仍由当前/原 `round_page` 持有，finalize 时按 full-page 分支进入 readonly cache 或 drop。
- `rollback_pages()` 注释应说明：resident source rollback 总是恢复 original `free_mask`，转回 `clean_allocatable`，并进入 `allocatable_frames_[ci]`；如果它仍是当前 `open_frames_` occupant，会先清掉 open 引用。

## Reviewer-Owned Test

reviewer 已在 `apps/inconel/test/test_value.cc` 增加 step 019 用例：

- `case_9_value_page_frame_type_contract`
- `case_10_partial_page_becomes_clean_allocatable`
- `case_11_next_round_reopens_clean_allocatable_frame`
- `case_12_full_page_enters_readonly_cache_only`
- `case_13_open_frame_visible_until_finalize`

这些测试覆盖：

- `value_page_frame` 类型表面和 allocator-visible 字段。
- partial sub-LBA page 写回后进入 `clean_allocatable`，不进入 readonly cache。
- 下一轮 persist 复用 `allocatable_frames_` 中的 clean resident frame。
- full 1-LBA page 写回后进入 `clean_readonly` readonly cache，不留在 open/allocatable resident lists。
- `prepare_persist` 后、`finalize_persist` 前，`open_frames_` 作为真实 resident read tier 可命中，且读路径不触发 NVMe。

## 已做验证

修复后已执行：

- `cmake --build build --target inconel_test_value inconel_test_runtime inconel_test_tree_value`
- `./build/inconel_test_value`
- `./build/inconel_test_runtime`
- `./build/inconel_test_tree_value`
- `git diff --check -- apps/inconel/memory/frame.hh apps/inconel/value/scheduler.hh apps/inconel/value/allocator.hh apps/inconel/test/test_value.cc ai_context/inconel/plan/019_value_resident_frame_state_review.md`

结果：全部通过。`inconel_test_value` 当前包含 13 个 case。

## 给实现修复的约束

修复 agent 只允许改 production 注释，不允许读、搜索或修改测试文件。

禁止范围：

- 不提交。
- 不碰 `CLAUDE.md`。
- 不更新 `known_issues.md`，除非 reviewer 另行要求。
- 不处理无关工作树项。
- 不改 production 行为。
- 不引入 `hole_page_list`、`freed_slots`、`recycle_whole`、`install_recovered_state`。

必须完成：

1. 修正 `acquire_round_page()` 中 full open frame displacement 的注释。
2. 修正 `rollback_pages()` 中 `writable` source rollback 的注释。
3. 修复后至少重建并运行 `inconel_test_value`、`inconel_test_runtime`、`inconel_test_tree_value`；实现 agent 不允许打开测试源码，只能运行目标。

## 收口判断

当前功能语义已经基本满足 step 019：dirty active frame、clean allocatable frame、clean readonly frame 三类 resident state 都有可达路径和测试覆盖。

两处错误 production 注释清掉后，本 step 可以收。
