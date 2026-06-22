# 060 — Reclaim Pipeline Refactor / Hidden Submit Removal

> 对应：059 发现的 BUG-2（reclaim invalidate completion queue overflow）。
>
> 本文是设计文档。后续实现 production 代码时仍按 CLAUDE.md 规则执行：实现阶段禁止读测试文件；测试维护阶段另行声明。

## 0. 一句话

把当前 tree reclaim 从“tree owner handler 内部 `submit()` 隐藏子 pipeline + completion queue 拼回状态机”重构为一条 first-class PUMP sender pipeline：

```text
tree owner plan
  → read_domain invalidate fan-out
  → tree owner finish-invalidate
  → NVMe TRIM fan-out
  → tree owner finish-trim / recycle
  → value reclaim
  → WAL reclaim
  → release mutation gate
```

目标不是简单调大队列或加局部 inflight counter，而是消除异步逻辑碎片化，让背压、错误传播、owner 边界和生命周期重新回到 sender 编排里。

---

## 1. 背景

### 1.1 当前触发链

reclaim task 的来源仍是正确的：

```text
flush_round_once
  → tree_local_flush 产出 retired
  → coord::frontier_switch 把 retired 挂到旧 checkpoint_guard
  → front::release_gens 释放 flushed gens
  → old guard / old gen 最后引用释放
  → destructor 通过 reclaim_sink post reclaim_task 到 tree_sched.reclaim_q
```

直接消费 reclaim task 的位置不是某条 sender pipeline，而是 `tree_sched::advance()`：

```cpp
progress |= drain_reclaim_ingress();
progress |= drain_reclaim_invalidate_completions();
progress |= drain_reclaim_trim_completions();
progress |= process_pending_reclaim();
```

`process_pending_reclaim()` 拿 mutation gate 后进入 `begin_reclaim_round()`，再调用 `process_reclaim_task()`。

### 1.2 当前实现的关键问题

当前 `process_reclaim_task()` 对每个 retired old slot / old range 直接调用：

```cpp
submit_reclaim_invalidate(round_id, range, recycle_range);
```

`submit_reclaim_invalidate()` 内部创建并 fire-and-forget 一条独立 sender：

```cpp
just()
  >> loop(domains->size())
  >> concurrent()
  >> flat_map(domain.invalidate_range)
  >> all()
  >> then(enqueue_reclaim_invalidate_done)
  >> submit(root_context, null_receiver)
```

invalidate completion 再通过 `reclaim_invalidate_done_q` 回到 tree owner；tree owner drain completion 后又调用：

```cpp
submit_reclaim_trim(round_id, range, recycle_range);
```

`submit_reclaim_trim()` 又 fire-and-forget 一条 NVMe trim sender，TRIM completion 通过 `reclaim_trim_done_q` 回来。

这形成了两层隐藏异步链：

```text
tree_sched::advance()
  → hidden invalidate pipeline
      → reclaim_invalidate_done_q
  → hidden trim pipeline
      → reclaim_trim_done_q
  → owner completion drain
```

这不是 PUMP 期望的结构。PUMP 的目的就是让异步控制流显式组合，而不是在 owner handle 内部再开 root pipeline，再用 completion queue 手写 fan-in。

### 1.3 BUG-2 的直接表现

059 A8 真盘 e2e 已复现：

```text
tree::tree_sched::enqueue_reclaim_invalidate_done: invalidate completion queue full
```

直接原因：

1. `process_reclaim_task()` 一次性 submit 所有 retired slot/range 的 invalidate。
2. 每个 invalidate 完成后从 read_domain 核心并发 enqueue completion。
3. `reclaim_invalidate_done_q` 固定容量 256。
4. tree owner 每次 advance 只 drain 64 个 invalidate completion。
5. 大 flush 退休对象超过 256 时，completion producer 比 owner consumer 快，`try_enqueue` 失败后 panic。

但这只是表面。根因是异步 fan-out 不在同一条 sender 链里，背压不由 `concurrent(N)` 控制，而是落到手写 completion queue 容量上。

---

## 2. 目标与非目标

### 2.1 本步目标

1. 删除 reclaim 路径中的 hidden root `submit()`：
   - 删除 / 停用 `submit_reclaim_invalidate()`
   - 删除 / 停用 `submit_reclaim_trim()`
   - 删除对应 completion queue 驱动状态机

2. 新增 first-class reclaim sender pipeline：
   - owner seam 只做 owner-local 状态变更和 plan/finish
   - read_domain invalidate fan-out 显式出现在 sender 链中
   - NVMe TRIM fan-out 显式出现在 sender 链中
   - value reclaim / WAL reclaim 显式接在 pipeline 尾部

3. 用 sender-level bounded concurrency 替代 completion queue 背压：
   - invalidate fan-out bounded
   - TRIM fan-out bounded
   - 单轮处理任务数 bounded
   - 单轮 value refs 数 bounded

4. 保持 mutation gate 语义：
   - reclaim 与 tree flush/merge 互斥
   - 异常路径必须释放 gate

5. 保持已落地 reclaim 语义：
   - old slot：invalidate + TRIM，不 recycle range
   - old range：invalidate + TRIM + `tree_allocator.recycle`
   - old tree values / gen losers：按 `recovery_safe_lsn` gate 后投 value reclaim
   - flush frontier 推进后驱动 WAL reclaim check

### 2.2 本步不做

1. 不做 INC-053 `tree_allocator.free_ranges` coalescing extent map。
   - 本步修异步编排和背压；allocator 数据结构另起 step。

2. 不做 INC-054 tree/value alloc floor 协议。
   - 该项已按 2026-06-16 裁决 blocked。

3. 不做 recovery。
   - 但本步必须继续维护 `recovery_safe_lsn` / `flush_durable_frontier` 现有语义，不能为 recovery 留新债。

4. 不把 reclaim 偷偷并入 `flush_once()`。
   - flush 产出 retired；reclaim 消费已释放 guard/gen 后投递的 task。二者通过生命周期隔离，不能在同一 flush round 内强行同步完成。

5. 不引入 production virtual / override。
   - 遵守 057 后的 no-virtual 规则。

---

## 3. 设计原则

### 3.1 Owner Handler 不启动隐藏 Pipeline

tree owner 的 handle 可以返回 plan、接受 finish、更新 owner 状态；不能在 handle 内部：

```cpp
pump::sender::submit(... root_context ...);
```

尤其不能用 hidden sender + completion queue 表达业务子流程。

允许例外仅限真正的 terminal backend owner 内部机制；tree reclaim 不是这个例外。

### 3.2 Pipeline 编排调度顺序，Scheduler 承载业务状态

重构后的职责分离：

| 层 | 职责 |
|---|---|
| `tree_sched` owner seam | drain reclaim task、分类、构造 work plan、owner cache invalidate、allocator recycle、frontier/deferred value 判定 |
| `tree_read_domain` sender | 在 read_domain owner 上 invalidate tree-node cache |
| `nvme` sender | 执行 TRIM |
| `value` sender | 执行 value reclaim |
| `wal` sender | 执行 WAL segment reclaim check |
| L3 reclaim pipeline | 串起上述阶段、设置 bounded concurrency、处理异常释放 gate |

### 3.3 背压必须显式

本步用 sender-level concurrency 作为背压：

```text
as_stream(invalidate_items) >> concurrent(kInvalidateConcurrency)
as_stream(trim_items)       >> concurrent(kTrimConcurrency)
```

不要再依赖 completion queue 容量承载隐含 inflight 上限。

### 3.4 Reclaim 是 Maintenance Operation

reclaim task 可以由 destructor 异步投递到 `tree_sched.reclaim_q`，但消费它应该是显式 maintenance operation：

```cpp
rt::reclaim_once()
```

或者未来：

```cpp
rt::maintenance_once()
  → tree::reclaim_once()
  → value::drain_trim_pending()
```

`tree_sched::advance()` 可以继续 drain ingress queue，把外部 post 的 task 搬到 owner-local `pending_reclaim`；但它不应自动启动隐藏 reclaim I/O pipeline。

---

## 4. 新 Public / Internal Surface

### 4.1 新增 `tree::reclaim_once(...)`

建议落点：

```text
apps/inconel/tree/sender.hh
```

签名：

```cpp
[[nodiscard]] inline auto reclaim_once();
```

或为测试 / future runtime 显式参数化：

```cpp
[[nodiscard]] inline auto reclaim_once(tree_sched& sched);
```

它返回 sender，执行最多一轮 bounded reclaim。无可处理任务时返回 `reclaim_round_result{ .noop = true }`。

### 4.2 新增 runtime operation

落点：

```text
apps/inconel/runtime/operations.hh
```

新增：

```cpp
[[nodiscard]] inline auto reclaim_once() {
    return tree::reclaim_once(*core::registry::tree_sched_singleton());
}
```

未来可以再加：

```cpp
[[nodiscard]] inline auto maintenance_once();
```

但本步只需要 `reclaim_once()`。

### 4.3 Tree Owner Seam

新增 / 调整 tree owner sender：

```cpp
struct reclaim_round_plan {
    bool noop = false;
    uint64_t round_id = 0;
    tree_mutation_token token{};
    std::vector<reclaim_invalidate_item> invalidates;
    std::vector<format::value_ref> reclaim_now;
    uint64_t flush_durable_frontier = 0;
};

struct reclaim_invalidate_item {
    format::range_ref range{};
    bool recycle_range = false;
};

struct reclaim_trim_item {
    format::range_ref range{};
    bool recycle_range = false;
};

struct reclaim_after_invalidate {
    uint64_t round_id = 0;
    tree_mutation_token token{};
    std::vector<reclaim_trim_item> trims;
    std::vector<format::value_ref> reclaim_now;
    uint64_t flush_durable_frontier = 0;
};

struct reclaim_after_trim {
    uint64_t round_id = 0;
    tree_mutation_token token{};
    std::vector<format::value_ref> reclaim_now;
    uint64_t flush_durable_frontier = 0;
};
```

Owner handles:

```cpp
prepare_reclaim_round(tree_mutation_token token)
  -> reclaim_round_plan

finish_reclaim_invalidates(round_id, invalidated_items)
  -> reclaim_after_invalidate

finish_reclaim_trims(round_id, trimmed_items)
  -> reclaim_after_trim

finish_reclaim_round(round_id)
  -> void

abort_reclaim_round(round_id)
  -> void
```

Notes:

1. `prepare_reclaim_round` assumes caller already holds mutation gate.
2. It drains bounded tasks from `pending_reclaim`.
3. It does **not** submit read_domain or NVMe work.
4. `finish_reclaim_invalidates` does owner-local non-leaf cache invalidation and creates trim plan.
5. `finish_reclaim_trims` recycles ranges and computes value/WAL follow-up.
6. `finish_reclaim_round` releases / clears active owner state after value/WAL follow-up succeeds.
7. `abort_reclaim_round` releases mutation gate and clears active state on any exception before normal finish.

Implementation may combine `finish_reclaim_round` with mutation gate release if the token is carried explicitly, but the sender must show release on both success and exception paths.

### 4.4 Read Domain Invalidate Sender Already Exists

`tree_read_domain_base::submit_invalidate_range(range, page_lbas)` already returns a sender. It should be used directly by the reclaim pipeline fan-out.

No hidden submit is needed.

### 4.5 NVMe TRIM Sender Already Exists

Use:

```cpp
rt::local_nvme()->trim(lba, lbas)
```

inside the reclaim sender chain.

The pipeline must build `trim_desc` / item vectors and run bounded `concurrent(kTrimConcurrency)`.

### 4.6 Value / WAL Sender Already Exist

Use existing sender surfaces:

```cpp
value::reclaim_values(std::span<const value_ref>)
wal::reclaim_check(*core::registry::wal_space_singleton(), flush_durable_frontier)
```

Do not use:

```cpp
core::registry::post_value_reclaim_values(...)
core::registry::post_wal_reclaim_check(...)
```

Those registry post helpers become obsolete after this step and should be removed or made unused.

---

## 5. Pipeline Shape

### 5.1 Top-Level Pseudocode

```cpp
inline auto reclaim_once(tree_sched& owner) {
    return owner.submit_acquire_tree_mutation()
        >> flat_map([&owner](tree_mutation_token token) {
            return owner.submit_prepare_reclaim_round(token)
                >> flat_map([&owner](reclaim_round_plan&& plan) {
                    if (plan.noop) {
                        return owner.submit_release_tree_mutation(plan.token)
                            >> then([] { return reclaim_round_result{.noop = true}; });
                    }
                    return drive_reclaim_plan(owner, std::move(plan));
                })
                >> any_exception([&owner, token](std::exception_ptr ep) {
                    return owner.submit_abort_reclaim_round(token)
                        >> then([ep]() -> reclaim_round_result {
                            std::rethrow_exception(ep);
                        });
                });
        });
}
```

The exact code should avoid capturing stale references into async lambdas unless lifetime is obvious. `owner` is singleton and stable for runtime lifetime; `plan` should be carried by context / shared holder as needed.

### 5.2 Drive Plan

```text
plan.invalidates
  → fan-out each item to all read_domains
  → reduce/all
  → owner.finish_reclaim_invalidates(...)
  → fan-out TRIMs
  → owner.finish_reclaim_trims(...)
  → value::reclaim_values(...)
  → wal::reclaim_check(...)
  → owner.finish_reclaim_round(...)
  → release mutation gate
```

More detailed:

```cpp
drive_reclaim_plan(owner, plan):
  with_context(plan)
    loop(plan.invalidates.size())
      >> concurrent(kReclaimInvalidateConcurrency)
      >> flat_map(invalidate_one_item_on_all_domains)
      >> all()
    >> owner.finish_reclaim_invalidates(...)
    >> flat_map(trim_items_bounded)
    >> owner.finish_reclaim_trims(...)
    >> flat_map(reclaim_values_if_any)
    >> flat_map(reclaim_wal_if_needed)
    >> owner.finish_reclaim_round(...)
    >> owner.release_mutation_gate(...)
```

### 5.3 Invalidate One Item

Each item must invalidate on every read_domain:

```cpp
invalidate_one_item_on_all_domains(item):
  loop(read_domains.size())
    >> concurrent(kReadDomainFanoutConcurrency)
    >> flat_map([item](i) {
        return read_domains[i]->submit_invalidate_range(
            item.range, page_lbas);
    })
    >> all()
    >> then([item](bool ok) {
        if (!ok) panic_inconsistency(...);
        return item;
    })
```

Concurrency decisions:

1. Outer item concurrency should be bounded.
2. Inner per-domain fan-out can be `concurrent(read_domains.size())`, because K is runtime core count and each item needs a full barrier across read domains.
3. If item count is large, outer concurrency is the main backpressure. Reasonable initial value: 32 or 64, matching existing owner queue drain scale.

### 5.4 TRIM One Item

After invalidates complete, tree owner creates trim items. Each item maps to one NVMe TRIM:

```cpp
trim_one_item(item):
  lbas = item.range.slot_count * geom.page_lbas()
  return rt::local_nvme()->trim(item.range.base.lba, lbas)
      >> then([item](bool ok) {
          if (!ok) panic_inconsistency(...);
          return item;
      });
```

TRIM concurrency must be bounded:

```cpp
as_stream(trim_items)
  >> concurrent(kReclaimTrimConcurrency)
  >> flat_map(trim_one_item)
  >> to_vector<reclaim_trim_item>()
```

### 5.5 Value and WAL Tail

After owner finishes TRIM:

```cpp
if (!after_trim.reclaim_now.empty())
    value::reclaim_values(after_trim.reclaim_now)

wal::reclaim_check(wal_sched, after_trim.flush_durable_frontier)
```

Ordering:

1. value reclaim can happen after tree slot/range TRIM; no reader can still use old tree refs after guard/gen release and invalidate barrier.
2. WAL reclaim uses `flush_durable_frontier`, not `recovery_safe_lsn`.
3. If value reclaim fails, panic / propagate through pipeline; mutation gate must still release.
4. If WAL reclaim fails, panic / propagate through pipeline; mutation gate must still release.

---

## 6. Tree Owner State Changes

### 6.1 Remove Completion Queues

Remove from `tree_state`:

```cpp
reclaim_invalidate_done_q
reclaim_trim_done_q
```

Remove:

```cpp
reclaim_invalidate_completion
reclaim_trim_completion
drain_reclaim_invalidate_completions()
drain_reclaim_trim_completions()
enqueue_reclaim_invalidate_done()
complete_reclaim_trim()
submit_reclaim_invalidate()
submit_reclaim_trim()
```

After this step `tree_sched::advance()` should no longer drive reclaim I/O completion. It may still:

```cpp
drain_reclaim_ingress();
```

Optionally it may not call `process_pending_reclaim()` at all, because reclaim consumption is now explicit through `tree::reclaim_once()`.

### 6.2 Active Reclaim State

Current:

```cpp
struct active_reclaim_round {
    tree_mutation_token token;
    uint64_t round_id;
    uint32_t pending_invalidations;
    uint32_t pending_trims;
    uint32_t processed_tasks;
    std::vector<value_ref> reclaim_now;
};
```

New shape should not track pending async completions. It should track ownership and validation:

```cpp
struct active_reclaim_round {
    tree_mutation_token token{};
    uint64_t round_id = 0;
    uint32_t processed_tasks = 0;
    bool finishing = false;
};
```

The work vectors live in pipeline carriers, not in owner state.

### 6.3 Pending Reclaim

Keep:

```cpp
mpmc::queue<core::reclaim_task*> reclaim_q;
std::deque<core::reclaim_task*> pending_reclaim;
```

`drain_reclaim_ingress()` remains in `advance()` because destructor post can happen from arbitrary threads. Moving ingress into `reclaim_once()` is possible, but then maintenance callers would need to pump tree owner before seeing new tasks. Keeping ingress drain in `advance()` is simpler and still synchronous owner-local work.

### 6.4 `process_pending_reclaim()` Replacement

Current `process_pending_reclaim()` requests mutation gate and starts hidden reclaim. After this step:

1. Delete it, or make it only report “work available”.
2. `reclaim_once()` should be the only place that starts a reclaim round.

Suggested helper:

```cpp
bool has_pending_reclaim() const {
    return !state.pending_reclaim.empty();
}
```

But avoid adding public polling unless a caller needs it. `reclaim_once()` can simply return noop if no work.

---

## 7. Mutation Gate Handling

### 7.1 Success Path

The pipeline owns the token:

```text
acquire token
  → prepare_reclaim_round(token)
  → ...
  → finish_reclaim_round(round_id)
  → release token
```

`finish_reclaim_round` should clear `active_reclaim` before release.

### 7.2 Exception Path

Any exception after acquiring the token must:

1. call `abort_reclaim_round(round_id/token)` on tree owner
2. release mutation gate
3. rethrow

This mirrors `tree_local_flush()` already releasing mutation gate in `any_exception`.

Important: if `prepare_reclaim_round()` returns noop, it must either not install `active_reclaim`, or it must clear it before returning. The token still must be released.

### 7.3 Abort Semantics

Abort should:

1. Clear `active_reclaim`.
2. Keep unprocessed `pending_reclaim` tasks in `pending_reclaim`.
3. For tasks already moved into the failed plan, choose one of two explicit policies:

   - **Fatal policy**: reclaim I/O failure is process-fatal via `panic_inconsistency`; no retry needed.
   - **Retry policy**: keep task ownership until all external side effects complete, and on failure requeue remaining safe work.

For v1, use fatal policy for device/invalidate failures. This matches current behavior (`panic_inconsistency` on TRIM/invalidate failure) and avoids inventing partial retry semantics.

Even with fatal policy, the sender should release mutation gate before rethrowing if it is going to propagate a normal exception rather than immediately panicking.

---

## 8. Data Carriers and Ownership

### 8.1 `reclaim_round_plan`

Owner seam returns owning vectors. This is acceptable because reclaim is background / maintenance path, not per-KV foreground hot path.

Still apply bounded sizing:

```cpp
kMaxReclaimTasksPerRound
kMaxReclaimInvalidateItemsPerRound
kMaxValueRefsPerReclaimBatch
```

If pending work exceeds limits, leave the rest in `pending_reclaim` for next `reclaim_once()`.

### 8.2 Task Ownership

`prepare_reclaim_round()` should move `core::reclaim_task*` from `pending_reclaim` into local `unique_ptr`s while building plan. Once converted into plan vectors:

- old slot/range work is represented by `reclaim_invalidate_item`
- value work is represented either in `reclaim_now` or `deferred_value_reclaim`

No raw task pointer should outlive `prepare_reclaim_round()`.

### 8.3 Invalidated / Trimmed Item Echo

The pipeline should carry successful items forward:

```text
invalidate item -> trim item -> finish trim item
```

This avoids relying on “same order as input” after concurrent fan-out unless explicitly preserved by collection logic.

### 8.4 Owner Cache Invalidate Location

Read_domain invalidate removes read-domain tree-node frames. Owner non-leaf cache invalidation must remain on tree owner.

Therefore:

```text
after all read_domain invalidates complete
  → tree owner finish_reclaim_invalidates()
      → invalidate_reclaim_range_locally()
      → build trim items
```

Do not let read_domain or NVMe stages touch tree owner cache or allocator.

---

## 9. Backpressure and Limits

Initial constants:

```cpp
static constexpr uint32_t kMaxReclaimTasksPerRound = 16;
static constexpr uint32_t kMaxReclaimInvalidateItemsPerRound = 256;
static constexpr uint32_t kReclaimInvalidateItemConcurrency = 32;
static constexpr uint32_t kReclaimTrimConcurrency = 32;
static constexpr uint32_t kMaxValueRefsPerReclaimBatch = 256;
```

Rationale:

1. `kMaxReclaimTasksPerRound=16` preserves current per-round task bound.
2. `kMaxReclaimInvalidateItemsPerRound=256` gives bounded vector size without relying on a completion queue.
3. Inflight invalidate work is now bounded by `kReclaimInvalidateItemConcurrency × read_domain_count`, explicit in the sender chain.
4. TRIM inflight is bounded by `kReclaimTrimConcurrency`.

These are tuning constants, not correctness limits. If more work remains, next `reclaim_once()` continues.

---

## 10. Scheduling Model

### 10.1 Who Calls `reclaim_once()`

Tests / maintenance driver can call:

```cpp
rt::reclaim_once()
```

Production runtime should eventually have a maintenance loop / cadence that submits:

```text
seal_once / flush_once / reclaim_once / value::drain_trim_pending
```

This document does not design the full maintenance scheduler; it only exposes the reclaim unit as a real sender operation.

### 10.2 What Happens If Nobody Calls It

Then reclaim tasks accumulate in `pending_reclaim`, and disk space is not physically reclaimed. That is already true if the old hidden pipeline is not driven by `advance()`. For production, the next step after this refactor should ensure the steady-state maintenance loop calls `rt::reclaim_once()` regularly.

For existing tests that currently rely on `advance()` eventually doing reclaim automatically, update the harness to call `rt::reclaim_once()` / maintenance once. That is a test-maintenance change, not a production semantic workaround.

### 10.3 Should `advance()` Auto-Submit `reclaim_once()`?

No. That would reintroduce hidden root pipeline from inside a scheduler tick.

If production needs automatic background cadence, it should be a top-level maintenance sender submitted by the runtime owner / application maintenance loop, not by `tree_sched::advance()` internally.

---

## 11. Cross-Doc Updates

After implementation, update:

1. `runtime_state_machine.md`
   - §4 tree domain request table: clarify `reclaim` is a first-class sender pipeline, not internal fire-and-forget.
   - §4.1 state: remove completion queues.

2. `flush_and_frontier_switch.md`
   - §7 reclaim chain: make explicit that old guard/gen destructors only enqueue tasks; `reclaim_once()` consumes tasks.

3. `code_quality_standard.md`
   - Add rule: scheduler handlers must not start hidden root sender pipelines with `submit(root_context)` for business subflows; async subflows must be returned as first-class senders from L3 composition.

4. `known_issues.md`
   - Add BUG-2 / resolve after implementation.
   - Mark INC-057 progress text as landed if desired; not required for this step.

---

## 12. Migration Plan

### Patch A — Design / Docs

1. Add this doc.
2. Add known issue entry for BUG-2 if not already present.

### Patch B — Owner Seam

1. Add `reclaim_round_plan` and result carriers.
2. Add sender ops:
   - `prepare_reclaim_round`
   - `finish_reclaim_invalidates`
   - `finish_reclaim_trims`
   - `finish_reclaim_round`
   - `abort_reclaim_round`
3. Keep old hidden submit path temporarily unused or behind compile removal.

### Patch C — Pipeline

1. Implement `tree::reclaim_once(tree_sched&)`.
2. Add `rt::reclaim_once()`.
3. Replace registry post value/WAL calls with explicit sender tail.

### Patch D — Delete Hidden Submit State

1. Delete `submit_reclaim_invalidate`.
2. Delete `submit_reclaim_trim`.
3. Delete completion structs / queues / drain functions.
4. Remove `process_pending_reclaim()` auto-start from `advance()`.

### Patch E — Harness / Verification Adjustments

1. Update maintenance/e2e harnesses to call `rt::reclaim_once()` where they currently only drive scheduler advance waiting for reclaim.
2. Do not weaken coverage. A8 should now pass through the former BUG-2 point.

---

## 13. Verification

### 13.1 Build / Static Gates

1. Full build.
2. No `virtual` / `override` production hits:

```bash
rg -n '\bvirtual\b|\boverride\b' apps/inconel --glob '!**/test*' --glob '!**/*test*'
```

3. No hidden reclaim submit remains:

```bash
rg -n 'submit_reclaim_invalidate|submit_reclaim_trim|reclaim_invalidate_done_q|reclaim_trim_done_q' apps/inconel
```

Expected: no production hits except deleted-history references in docs/tests if intentionally retained.

4. No new `pump::sender::submit(...)` inside tree owner reclaim helpers.

### 13.2 Functional Tests

Core targets:

1. `inconel_test_multishard_split_e2e`
   - Must no longer panic at A8 round 3.
   - Must keep shadow-CoW readback correctness.

2. `inconel_test_steady_e2e`
   - Must keep multi-round steady correctness.

3. Existing reclaim / flush e2e targets if present in current build.

4. WAL group commit / write path tests should remain unaffected.

### 13.3 Runtime Assertions

Add or preserve assertions:

1. `finish_reclaim_invalidates` round_id matches active round.
2. `finish_reclaim_trims` round_id matches active round.
3. `finish_reclaim_round` only runs after active round exists.
4. `abort_reclaim_round` handles active/noop states deterministically.
5. `tree_allocator.recycle` still fail-fast on allocator free queue overflow until INC-053.

### 13.4 Behavioral Evidence

The final report should state:

1. Whether `inconel_test_multishard_split_e2e` reaches past former BUG-2.
2. Number / type of hidden reclaim completion queues removed.
3. Whether value reclaim and WAL reclaim are now explicit sender stages.
4. Any remaining deferred work:
   - INC-053 allocator coalescing
   - production maintenance cadence if not yet added

---

## 14. Risks and Open Decisions

### R1 — Maintenance Caller Gap

If reclaim stops auto-starting in `advance()`, production code needs a top-level maintenance caller. For tests and current harnesses this is easy: explicitly call `rt::reclaim_once()` in the existing drive loop.

For real service runtime, we need a follow-up maintenance cadence design. This is adjacent and should be called out, but it should not justify keeping hidden submit in tree owner.

### R2 — Partial Side Effects on Failure

If invalidate succeeds for some ranges and TRIM fails later, current behavior is fatal. This step can keep fatal semantics. Do not invent retry unless recovery and idempotence policy are fully specified.

### R3 — More Work Than One Round Budget

`prepare_reclaim_round()` must leave unprocessed work in `pending_reclaim`. `reclaim_once()` returns after one bounded round. Callers that want quiescence loop until noop / idle.

### R4 — `value::reclaim_values` Input Lifetime

`value::reclaim_values(std::span<const value_ref>)` must not receive a span to a vector that dies before the sender consumes it. The reclaim pipeline should carry the vector in context / owning holder until the value sender has started and copied/moved as required by its op. Review this carefully during implementation.

### R5 — Read Domain Fan-Out Cost

Invalidating each retired range across all read domains is required by RSM §4.4/§4.7. Bounded fan-out may make reclaim take more ticks, but it prevents unbounded completion pressure and keeps owner work schedulable.

---

## 15. Final Shape

After this step, the reclaim control flow should read like this:

```text
rt::reclaim_once()
  → tree_sched.acquire_mutation_gate
  → tree_sched.prepare_reclaim_round
  → for each invalidate item, bounded:
        all read_domain.submit_invalidate_range
  → tree_sched.finish_reclaim_invalidates
  → for each trim item, bounded:
        nvme.trim
  → tree_sched.finish_reclaim_trims
  → value.reclaim_values
  → wal.reclaim_check
  → tree_sched.finish_reclaim_round
  → tree_sched.release_mutation_gate
```

No hidden `submit()` inside tree reclaim owner code; no reclaim completion queues; no queue-capacity-dependent liveness.
