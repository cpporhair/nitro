# 054 — WAL Append Group Commit / Coalesced Plan Design

> 对应 known issue：`INC-057`
>
> 目标：修掉当前 front WAL append 在小 batch 下“每 front 单 pending plan → 每 plan 常只有 1 个 tail-page FUA → NVMe 队列深度塌缩”的 P1 性能问题，同时保持 M06/045 建立的两阶段 commit、WAL failure release 边界和 recovery 契约。

## 1. 背景与问题

当前 WAL append 链路：

```text
write_path::write_wal_fragment
  loop:
    front.prepare_wal_fragment(fragment, cursor)
    ready(plan) -> issue plan.writes bounded FUA -> front.commit_wal_plan(plan_id)
    needs_segment -> wal_space.alloc_segment -> front.install_wal_segment
```

关键代码点：

1. `apps/inconel/front/scheduler.hh::prepare_wal_fragment_now` 在 `stream.has_pending_plan()` 时拒绝新 plan。
2. `apps/inconel/front/scheduler.hh::handle_wal_prepare` 在 `wal_busy()` 时只把 req 放入 `wal_pending_prepares_`。
3. `apps/inconel/write_path/sender.hh::issue_wal_fragment_plan_bounded` 的 `max_fua_inflight` 只作用于单个 plan 的 `writes`。
4. `apps/inconel/front/wal_append.hh::wal_stream_state::begin_pending` 对每个非 trailer plan 都复制一整页 `pending_tail_page_`。

结果：

```text
per-front device queue depth ≈ current_plan.writes.size()
```

小 batch 常只写同一 tail LBA，`writes.size() == 1`，所以单 front WAL durable 点接近：

```text
1 / WAL FUA latency
```

050 §5.2 里“喂满 FUA 管道只需 1-2 个 plan 在飞”的依据不成立：`pending_prepare_capacity` 是 front owner 内存 FIFO 容量，不是在飞 I/O 容量。

## 2. 不变量

必须保持：

1. **value-before-WAL**：value FUA 完成后才开始 WAL append。
2. **all-WAL barrier**：同一 batch 的所有 front fragments WAL durable 后，才进入 memtable phase。
3. **两阶段 WAL commit**：prepare 只生成 plan，不推进 committed cursor；FUA 成功后 commit，失败 abort。
4. **失败边界**：WAL prepare/FUA failure 发生在 memtable 前，上层可 `release_batch(batch_lsn)`。
5. **recovery 契约**：batch 完整性只由 `lsn + entry_count` 重组判定，不依赖同一 batch entries 在 segment 中连续。
6. **owner 归属**：WAL stream cursor、tail image、pending/gated state 只归 `front_sched`；segment pool 仍归 `wal_space_sched`；NVMe I/O 仍通过本 front owner 对应的 `nvme_sched`。
7. **同 LBA 覆写安全**：不能让同一 WAL LBA 的多个 full-page images 并发在设备上重排。

## 3. 非目标

本步不做：

1. 普通 write + NVMe FLUSH barrier。那会改变 durable point，需要单独设计。
2. disjoint-LBA 多 pending plan。它是二阶优化，必须先有 per-LBA ordering / conflict model。
3. header/trailer/segment allocation 的 group participant 扩展。先保留单 participant，降低 rotation 风险。
4. 改 WAL on-disk format。
5. 改 recovery parser。

## 4. 方案概览

采用 **front WAL entry group commit**：

```text
多个同 front 的 WAL prepare req
  -> front owner 按 FIFO 合并 entries
  -> 生成一个 coalesced wal_append_plan
  -> 一组 full-page writes FUA
  -> commit 一次 cursor/tail
  -> 分别唤醒 group 内每个 participant
```

核心变化：

1. `front_sched` 的 WAL gate 从“一个 req 生成一个 plan”改为“idle 时可以 drain 多个 req 合成一个 entry plan”。
2. `wal_append_plan` 仍是唯一在飞物理 write 单元，因此同一 tail LBA 只有一个 full-page image 在飞。
3. 每个 participant 只保存逻辑完成信息：`cursor_after`、`fragment_done`、是否产生错误。
4. group plan FUA 成功后，front commit 一次，再对 group 内所有 participant callback success。
5. group plan FUA 失败后，front abort 一次，再对 group 内所有 participant callback同一 WAL device failure。

## 5. 为什么不能简单多 pending plan

WAL page write 是整页覆写，不是 byte-range append。

例子：

```text
committed tail page: [A.........]

plan P1 appends B:
  writes LBA X full image [A B zero...]

plan P2 appends C:
  writes LBA X full image [A B C zero...]
```

若 P2 先落盘、P1 后落盘，最终磁盘是 `[A B zero...]`，C 被旧 full-page image 覆盖。即使 front owner 按 P1→P2 顺序 commit 内存 cursor，也无法控制设备同 LBA write completion/落盘顺序。

因此本步只允许一个物理 WAL plan 在飞，但让这个 plan 聚合多个 logical prepare。

## 6. Public Surface 裁决

### 6.1 保持 L3 write path API 不变

外层 `write_path::write_wal_fragment(...)` 不改调用形态：

```cpp
front::prepare_wal_fragment(...)
  -> ready(plan) / needs_segment
  -> issue plan
  -> front::commit_wal_plan(...) / front::abort_wal_plan(...)
```

理由：

1. `write_batch_wal_phase` 已经按 fragments 并发投递，同 front 多 batch 会自然进入 front owner queue。
2. group commit 应该是 front owner 内部优化，不让 coord/write_path 知道“谁是 leader”。
3. 保持 cross_doc_contracts 里的概念 handle 不漂移。

### 6.2 `wal_prepare_result` 增加 parked 形态

新增：

```cpp
struct wal_prepare_parked {
    uint64_t group_waiter_id = 0;
};

using wal_prepare_result =
    std::variant<
        wal_prepare_ready,
        wal_prepare_needs_segment,
        wal_prepare_parked>;
```

语义：

1. `ready(plan)`：当前 caller 是 group leader，必须 issue plan，然后 commit/abort。
2. `needs_segment`：当前 caller 触发 segment install，语义同现状，单 participant。
3. `parked`：当前 caller 已被 front owner 挂到某个 group，不能 issue I/O；它的 sender 在 front owner 内保存 callback，直到 leader commit/abort 时被唤醒。

注意：`parked` 不会返回到外层 pipeline 后继续执行；它是 front sender 内部等待结果的标记。实现上可以不把 `wal_prepare_parked` 暴露给 `write_path/sender.hh`，而是在 `_front_wal_prepare::req` 的 callback 延后触发。若保留 `wal_prepare_result` variant 纯度，也必须确保 L3 不需要处理 parked 分支。

本文推荐：**不向 L3 返回 parked**。front owner 将 follower req 留在内部，直到 group 完成时直接用原 req callback 返回 synthetic `wal_prepare_ready` completion 等价结果或异常。这样 L3 代码改动更小。

### 6.3 `wal_prepare_ready` 扩展 participant completions

`wal_append_plan` 增加 group 元数据：

```cpp
struct wal_plan_participant {
    uint64_t waiter_id = 0;              // 0 可表示 leader self
    wal_fragment_cursor cursor_before{};
    wal_fragment_cursor cursor_after{};
    bool fragment_done = false;
};

struct wal_append_plan {
    ...
    std::vector<wal_plan_participant> participants;
};
```

约束：

1. participant 顺序 = front owner FIFO 顺序。
2. `plan.cursor_before/cursor_after/fragment_done` 保留为 leader 的兼容字段；新代码使用 `participants`。
3. 单 participant plan 必须仍能按旧字段工作，降低改动面。

## 7. Front Owner 状态

新增：

```cpp
struct wal_group_waiter {
    uint64_t waiter_id = 0;
    borrowed_front_fragment fragment;
    std::span<const core::canonical_entry> canonical_entries;
    wal::wal_fragment_cursor cursor;
    std::move_only_function<void(
        core::owner_outcome<wal::wal_prepare_result>&&)> cb;
};

struct wal_group_plan_state {
    uint64_t plan_id = 0;
    std::vector<wal_group_waiter> waiters;
    std::vector<wal::wal_plan_participant> participants;
};
```

`front_sched` 新增字段：

```cpp
uint64_t next_wal_waiter_id_ = 1;
std::optional<wal_group_plan_state> wal_pending_group_;
```

已有字段保留：

```cpp
bool wal_awaiting_segment_;
std::deque<_front_wal_prepare::req*> wal_pending_prepares_;
std::size_t wal_pending_prepare_capacity_;
```

owner 状态机：

```text
IDLE
  prepare -> build single/group entry plan -> PLAN_PENDING
  prepare needing segment -> AWAITING_SEGMENT

PLAN_PENDING
  further prepare -> wal_pending_prepares_ FIFO
  commit/abort -> wake group -> IDLE -> drain next group

AWAITING_SEGMENT
  further prepare -> wal_pending_prepares_ FIFO
  install -> IDLE -> drain next group
```

## 8. Group 构造规则

### 8.1 入口

`handle_wal_prepare` 逻辑改为：

```text
if wal_busy():
    enqueue req to wal_pending_prepares_
    return

build_or_start_wal_group(req)
```

`drain_wal_pending_prepares()` 改为：

```text
while !wal_busy() && !wal_pending_prepares_.empty():
    pop one seed req
    build_or_start_wal_group(seed)
```

### 8.2 Candidate drain

`build_or_start_wal_group(seed)`：

```text
1. 尝试用 seed 生成 plan。
2. 若 seed 需要 header/trailer/segment：
     按旧单 participant 路径返回 ready/needs_segment，不 group。
3. 若 seed 可生成 entries plan：
     从 wal_pending_prepares_ FIFO 继续 peek/pop 后续 req；
     逐个尝试把 req 的下一个 entry range 追加进同一个 plan；
     直到达到任一边界：
       - plan page count 达到 max_pages_per_plan
       - participant count 达到 max_participants_per_group
       - 下一个 entry 放不进 segment usable end
       - 下一个 req 需要 header/trailer/segment
       - 下一个 req 校验失败
```

推荐新增配置：

```cpp
struct wal_append_config {
    uint32_t max_fua_inflight = 16;
    uint32_t max_pages_per_plan = 16;
    uint32_t pending_prepare_capacity = 0;
    uint32_t max_participants_per_group = 16;
};
```

默认 `16`：与 page budget 同阶，限制一次 commit 唤醒 fan-out 规模，也避免单 plan 把某个 front 的 later batch 全吞掉导致 latency 抖动。

### 8.3 不 peek 失败污染 FIFO

对 FIFO 中后续 req 的处理分两类：

1. **可合并**：pop 进 group。
2. **不可合并但不是错误**：停止 drain，保留在 FIFO 头，等待当前 plan commit 后继续。
3. **校验错误**：pop，fail callback，继续尝试下一个，直到遇到可合并/不可合并/容量边界。

这样避免一个坏 request 卡住整个 front WAL FIFO。

### 8.4 每个 participant 至少推进一条 entry

只有当某个 req 能在当前 group plan 中至少写入一条 WAL entry 时，才加入 group。

若 req 的下一步是 trailer 或 needs_segment：

1. 如果 group 当前已有 entry：停止 drain，保留该 req。
2. 如果 group 为空且 seed 触发 rotation：走旧单 participant trailer/needs_segment 路径。

### 8.5 header/trailer 单 participant

本步不 group：

1. segment header plan
2. sealed trailer plan
3. `needs_segment` install window

原因：

1. header/trailer 不是小 batch 高频项。
2. rotation 状态涉及 `sealed_segment_info` 和 wal_space lease，先不扩大 failure fan-out。
3. entry group commit 已解决小 batch 稳态瓶颈。

## 9. Plan Builder 重构

当前 `prepare_wal_entry_plan(...)` 同时负责：

1. 校验 fragment/cursor
2. 从 stream committed cursor 构建 pages
3. scatter entries
4. finalize suffix
5. `stream.begin_pending(plan, tail)`

需要拆成两层：

```cpp
struct wal_entry_group_builder {
    wal_append_plan plan;
    std::vector<front_sched::wal_page_builder> pages;
    uint32_t offset;
    uint64_t proposed_min;
    uint64_t proposed_max;
};

try_append_fragment_to_wal_group(builder, req)
    -> wal_group_append_result
```

返回：

```cpp
enum class wal_group_append_status {
    appended,             // 写入 >=1 entry
    would_exceed_budget,  // page/participant budget
    needs_rotation,       // 下一 entry 放不进 usable end
    validation_error,
};
```

`validation_error` 携带 `exception_ptr`。

### 9.1 单 participant 兼容

`prepare_wal_entry_plan(...)` 可以先改为：

```text
make builder from current stream cursor
try_append_fragment_to_wal_group(builder, req)
finalize builder
stream.begin_pending(...)
return ready(plan)
```

随后 group path 复用同一 builder。

## 10. Tail Image 与 Snapshot 成本

group commit 下，同一 plan 只有一个 `begin_pending(...)`：

```text
N 个小 batch append 到同一 tail page
  旧：N 次 full-page pending_tail_page_ memcpy + N 次 tail LBA FUA
  新：1 次 full-page pending_tail_page_ memcpy + 1 次 tail LBA FUA
```

`wal_page_for(...)` 铺底规则不变：

1. 命中 committed tail image → memcpy 一整页作为 base。
2. scatter group 内所有 entries 覆盖对应 byte ranges。
3. `zero_wal_plan_suffix(...)` 对最终 group end offset 后缀清零一次。
4. `begin_pending(...)` 快照最终 tail image 一次。

## 11. Commit / Abort 语义

### 11.1 Commit

leader issue FUA 成功后：

```text
front.commit_wal_plan(plan_id, writes)
  -> stream.commit_pending(plan_id)
  -> req 析构归还 frames 到 front thread pool
  -> wake participants
  -> drain_wal_pending_prepares()
```

participant wake：

```text
for p in participants:
    cb(owner_outcome<wal_prepare_result>{
        wal_prepare_ready{
            .plan = logical_completion_without_writes(p)
        }
    })
```

但不要真的给 follower 返回一个可 issue 的 `wal_append_plan`。推荐新增 internal-only completion callback：

```cpp
struct wal_prepare_committed {
    wal_fragment_cursor cursor_after{};
    bool fragment_done = false;
    std::optional<wal::sealed_segment_info> sealed;
};
```

更干净的 surface 是把 front prepare sender 的返回值从“physical plan”拆成：

```cpp
using wal_prepare_result =
    std::variant<wal_prepare_issue_plan, wal_prepare_committed, wal_prepare_needs_segment>;
```

其中：

1. `issue_plan` 只给 leader。
2. `committed` 给 followers，表示 WAL durable 已经完成，L3 只更新 `state.cursor/state.done`。

本文推荐采用这个拆分，避免“ready 但不准 issue”的反直觉状态。

### 11.2 Abort

leader issue FUA false/exception 后：

```text
front.abort_wal_plan(plan_id, writes)
  -> stream.abort_pending(plan_id)
  -> req 析构归还 frames
  -> wake all participants with wal_append_error{device_failure}
  -> drain_wal_pending_prepares()
```

所有 participant 都停在 memtable 前，上层 `write_batch` 走 release。

### 11.3 Partial durable bytes

若 group plan 部分 pages 已落盘但 FUA group 失败：

1. committed cursor 未推进。
2. 后续 retry/new group 从同一 committed cursor 重写。
3. crash 在重写前发生时，recovery 看到 committed prefix 后的 partial/corrupt/incomplete entries，按 CRC 和 `entry_count` 丢弃不完整 batch。

与 M06 单 plan failure 语义一致。

## 12. L3 Write Path 调整

`wal_plan_completion` 保持：

```cpp
struct wal_plan_completion {
    wal::wal_fragment_cursor cursor_after{};
    bool fragment_done = false;
    std::optional<wal::sealed_segment_info> sealed;
};
```

`write_wal_fragment_step` 的 visit 分支变为：

```text
front.prepare_wal_fragment(...)
  -> needs_segment:
       allocate_and_install_wal_segment(...)
  -> issue_plan:
       issue_and_finish_wal_plan(...)
       -> completion from leader participant
  -> committed:
       completion directly from front owner
```

这样 follower pipeline 不碰 NVMe，不调用 commit/abort，只等 front owner 告知 WAL durable。

## 13. Queue / Fairness / Latency

### 13.1 FIFO

Group participants 必须按 front owner 接收 prepare req 的 FIFO 顺序合并。允许同一 front 上不同 batch entries 在 segment 中按 plan/group 粒度交错；recovery 不依赖段内 batch 连续性。

### 13.2 Group 边界

Group 结束条件：

1. `writes.size() >= max_pages_per_plan`
2. `participants.size() >= max_participants_per_group`
3. 下一个 req 会触发 rotation/header/trailer/needs_segment
4. 下一个 req 的下一条 entry 自身会让 page budget 超出，且当前 group 已非空
5. FIFO 空

### 13.3 避免永久等待

不引入 timer。只要第一个 req 到达且 front idle，就立即形成 group；group 只吸收当时已经排队的 req，不等待未来请求。

这避免延迟型 group commit 引入新的 tail latency 参数。吞吐收益来自已有并发 batch 在 FIFO 中积累，而不是人为 sleep。

## 14. Error Semantics

| 场景 | 结果 |
|---|---|
| seed validation failure | fail seed callback；继续 drain FIFO |
| follower validation failure | fail follower callback；继续尝试后续 FIFO |
| group FUA false/exception | abort plan；所有 participants 返回 `device_failure` |
| commit invariant failure | runtime bug，fail-fast / exception propagation，同现有 |
| prepare FIFO full | `prepare_queue_full`，不进入 group |
| segment unavailable | 单 participant `needs_segment` pending，不 release |

## 15. 文档更新

实现时必须同步：

1. `ai_context/inconel/design_doc/write_path_and_pipeline.md` §8.3 / §12.2：说明 front WAL group commit 后，小 batch FUA 数按 group pages 而非 batch 数计。
2. `ai_context/inconel/design_doc/runtime_state_machine.md` §3.9：补充 group plan 是 WAL stream 的唯一在飞 physical plan，participants 是 logical waiters。
3. `ai_context/inconel/design_doc/cross_doc_contracts.md` §1：刷新 M06 handle 备注，加入 `issue_plan/committed/needs_segment` 的 front prepare result。
4. `ai_context/inconel/plan/050_runtime_topology_operation_surface_design.md` §5.2：删除“1-2 个 plan 在飞”的错误依据；明确 `pending_prepare_capacity` 是 backpressure capacity，不是 I/O concurrency。
5. `ai_context/inconel/known_issues.md` `INC-057`：落地后记录提交和剩余二阶优化。

## 16. 测试计划

实现 agent 如需读/改测试文件，必须以测试维护者角色显式声明。

### 16.1 新增

1. `m14_wal_group_commit_single_lba_many_batches`
   - N 个 DELETE-only 或 small PUT batch，同一 front。
   - 并发提交 WAL phase。
   - fake NVMe 统计 WAL tail LBA FUA write 次数，应从 N 收敛为 1 或 `ceil(total_bytes/lba_size)`。
   - 解码 WAL bytes，按 `lsn + entry_count` 重组全部 batch。

2. `m14_wal_group_commit_followers_do_not_issue_nvme`
   - 多个 participant group。
   - 断言只有 leader path 调用 `write_frame_range_bounded_fua`。
   - followers 只收到 `committed` completion。

3. `m14_wal_group_commit_fua_failure_releases_all_participants`
   - fake NVMe 注入 group plan 某页 false/exception。
   - 所有 participants WAL phase 失败。
   - 没有 memtable insert。
   - 后续 retry 从同一 committed offset 成功。

4. `m14_wal_group_commit_rotation_boundary_stops_group`
   - FIFO 中 A 可写到 segment 尾，B 下一条需要 trailer。
   - group 只包含 A；B 留在 FIFO，A commit 后 B 触发 trailer/needs_segment 单 participant 路径。

5. `m14_wal_group_commit_bad_follower_does_not_block_fifo`
   - FIFO 中 seed OK，follower bad index，next OK。
   - bad follower callback error；seed 和 next 仍 group 成功。

6. `m14_wal_group_commit_pool_return_on_front_commit`
   - FUA 完成后、front commit 前 pool free count 仍低。
   - front commit 后 group writes 在 front thread 析构归池。

### 16.2 回归

必须保留并跑通：

1. M06 WAL header/entry/trailer/rotation/failure tests。
2. M08/M09 write_batch release/fatal phase tests。
3. M11 runtime topology pending_prepare_capacity decoupling test。
4. M13 live read/write e2e。

## 17. Implementation Order

### Phase A — Type surface

1. 扩展 `wal_append_config.max_participants_per_group`。
2. 引入 `wal_prepare_issue_plan` / `wal_prepare_committed` / `wal_prepare_needs_segment`，或等价拆分。
3. 给 `wal_append_plan` 加 participants 元数据，旧字段保持兼容。

### Phase B — Plan builder split

1. 从 `prepare_wal_entry_plan` 拆出 group builder。
2. 单 participant 路径先走 builder，确保行为不变。
3. 保持 header/trailer/needs_segment 旧路径。

### Phase C — Front WAL group state

1. 新增 waiter/group state。
2. `handle_wal_prepare` / `drain_wal_pending_prepares` 改为 seed + drain group。
3. 校验错误 follower 不阻塞 FIFO。

### Phase D — L3 branch

1. `write_wal_fragment_step` 支持 `committed` completion。
2. leader 分支继续 issue/commit/abort。
3. follower 分支只更新 cursor/done。

### Phase E — Failure fan-out

1. commit 后唤醒 participants success。
2. abort 后唤醒 participants `device_failure`。
3. 验证 frames 仍在 front thread 回收。

### Phase F — Docs and tests

1. 更新 §15 所列文档。
2. 增加 §16 测试。
3. Release + ASAN 跑 M06-M14 相关目标和 e2e。

## 18. 热路径预算

新增：

| 项 | 成本 | 说明 |
|---|---|---|
| group participants vector | 每个 group 最多 16 个小 POD | request-local，随 plan 生命周期释放 |
| waiter state | 每个 parked prepare 一个 req/waiter | 取代原 FIFO req 等待，不新增队列级别 |
| group drain loop | O(participants + entries in group) | 原本这些 req 也会逐个 prepare；合并后少了多次 begin_pending / FUA / callback cycle |

减少：

| 项 | 改前 | 改后 |
|---|---|---|
| 小 batch tail FUA | N 次 | `ceil(total_group_wal_bytes / lba_size)` 次 |
| tail snapshot memcpy | N 次 4K | 每 group 1 次 4K |
| commit/abort owner round-trip | N 次 | 每 group 1 次 physical commit/abort + N 次 callback |

风险：

1. group 内 participant callback fan-out 可能让一次 front advance 做更多 work；用 `max_participants_per_group` 限制。
2. 单 group 更大时，一个 failed FUA 会让多个 batch release；这是 WAL durable point 共享的必然语义，仍在 memtable 前。

## 19. 相邻事项

1. **050 §5.2 必须顺手修**：当前容量依据误把 prepare FIFO 当在飞 I/O。这个文档错误会误导后续调参。
2. **普通 write + FLUSH 不要混进本步**：它和 group commit 都能降低 FUA 压力，但 durable barrier 语义不同，应单独设计。
3. **disjoint-LBA 多 pending plan 暂缓**：等 group commit 后若仍看到大 batch 下单 front 不够，再基于 per-LBA conflict graph 设计。
4. **INC-057 落地后再做真实 NVMe 小 batch microbench**：按仓库性能方法论，需要先定义 workload、client 压力和验证方式，不能只看工具吞吐。
