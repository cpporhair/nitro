# 041 — Coord Scheduler：Assign / Publish / Release / Read Handle

> 本文是 `front_wal_development_plan.md` 里 M03 的详细设计文档。
> M03 只冻结 coord scheduler 的 `assign_batch_lsn`、`publish_batch`、
> `release_batch`、ready bitmap / publish gate，以及基于 M02 CAT/PRS 的
> `acquire_read_handle` 语义。
>
> 本文不设计 WAL append、front owner memtable、value persist/read、
> `write_batch` pipeline、tree lookup、runtime API、seal round、flush
> frontier switch 或 recovery。

## 1. 范围

M03 把 M01 的 batch carrier 和 M02 的 CAT/PRS/read_handle pin 链接入
`coord_sched` owner 状态，但只覆盖 coord 本地、CPU-only 的协调语义：

1. `assign_batch_lsn(client_batch_buffer&&)`：
   解析并 canonicalize M01 batch input，分配 gap-free `batch_lsn`，构造
   M01 `batch_ctx`。
2. `publish_batch(batch_lsn)`：
   在 all-memtable barrier 完成后，把该 LSN 槽位标记为 published/resolved，
   并按连续前缀推进当前 CAT 的 `durable_lsn`。
3. `release_batch(batch_lsn)`：
   在 value/WAL phase 失败且 memtable phase 尚未启动时，把该 LSN 槽位标记为
   released/resolved-empty，使后续成功 batch 不被永久 hole 卡住。
4. `ready_bitmap` / `publish_gate`：
   管理乱序 terminal completion、fixed in-flight window、gate closed 期间的
   `pending_advance`。
5. `acquire_read_handle()`：
   按 M02 顺序读取 current CAT 和 `durable_lsn`，返回 `read_handle` snapshot。

M03 落点：

1. `apps/inconel/coord/scheduler.hh`：coord owner 状态、请求队列、`advance()`。
2. `apps/inconel/coord/sender.hh`：对外唯一 sender surface。
3. `apps/inconel/core/registry.hh`：后续实现可加入 coord singleton placeholder 到
   真实注册入口；本文不设计 runtime builder / facade API。

M03 不改变 M01/M02 类型：

1. `core::batch_ctx`、`canonical_entry`、`front_fragment`、`write_op_type` 以 039
   和当前 `core/batch_carrier.hh` 为准。
2. `core::publish_catalog`、`published_read_set`、`read_handle`、`catalog_store`
   以 040 和当前 `core/read_catalog.hh` 为准。

## 2. 已检查输入

旧 `inconel` 分支证据：

1. `ai_context/inconel/plan/steps/step_09_design.md`
2. `ai_context/inconel/plan/steps/step_09_test_spec.md`
3. `ai_context/inconel/plan/steps/step_09_review.md`
4. `apps/inconel/runtime/coord/gate.hh`
5. `apps/inconel/test/step_09_publish_gate_contract_test.cc`
6. `ai_context/inconel/plan/steps/step_16_design.md`
7. `ai_context/inconel/plan/steps/step_16_test_spec.md`
8. `ai_context/inconel/plan/steps/step_16_review.md`
9. `apps/inconel/runtime/coord/state.hh`
10. `apps/inconel/runtime/coord/catalog.hh`
11. `apps/inconel/runtime/coord/owner_impl.hh`
12. `apps/inconel/test/step_16_coord_sched_contract_test.cc`

当前 `inconel.new` 证据：

1. `ai_context/inconel/plan/039_front_wal_phase_a_carrier_inc055_design.md`
2. `ai_context/inconel/plan/040_read_handle_prs_memtable_lookup_design.md`
3. `apps/inconel/core/batch_carrier.hh`
4. `apps/inconel/core/memtable.hh`
5. `apps/inconel/core/memtable_lookup.hh`
6. `apps/inconel/core/read_catalog.hh`
7. `apps/inconel/core/registry.hh`
8. `apps/inconel/test/test_m02_read_handle_prs_memtable_lookup.cc`

正式设计依据：

1. `ai_context/inconel/design_doc/design_overview.md`
2. `ai_context/inconel/design_doc/runtime_state_machine.md`
3. `ai_context/inconel/design_doc/write_path_and_pipeline.md`
4. `ai_context/inconel/design_doc/read_api_and_pipeline.md`
5. `ai_context/inconel/design_doc/runtime_memory_and_cache.md`
6. `ai_context/inconel/design_doc/recovery_and_wal_reclaim.md`
7. `ai_context/inconel/design_doc/cross_doc_contracts.md`
8. `ai_context/inconel/design_doc/code_modules.md`
9. `ai_context/inconel/design_doc/code_quality_standard.md`

## 3. 语义来源对照表

| 项目 | 旧 `inconel` Step 9 / 16 证据 | 当前 `inconel.new` 状态 | 正式设计依据 | 041 决议 |
|---|---|---|---|---|
| 模块归属 | 旧代码在 `runtime/coord/*`。Step 16 仍是 CPU-only owner 壳。 | 当前没有 `apps/inconel/coord/`，`registry.hh` 只有 future placeholder。 | `code_modules.md` 要求 L2 coord 模块，其他模块只能 include `coord/sender.hh`。 | 新 coord 落到 `apps/inconel/coord/{scheduler.hh,sender.hh}`；旧 `runtime/coord` 目录形态不迁移。 |
| `assign_batch_lsn` 输入 | Step 16 接受旧 `raw_batch_op` / `client_batch_view`，调用旧 `build_batch_ctx` / `build_batch_plan`。 | M01 已冻结 `client_batch_buffer` owner + view-based `batch_ctx`，fragments 用 stable indices。 | WP §3/§4；RSM §2.3；039。 | 生产 sender 接受 move-only `core::client_batch_buffer`，返回 M01 `core::batch_ctx`。raw op overload 只能作为 test adapter。 |
| canonicalization | Step 16 复用 Step 10：same-key last writer wins，winning original position 升序。 | `core::build_batch_ctx` 已实现同规则。 | OV §7.1，WP §3，039 §4。 | 精确保留 M01 规则；coord 不重新发明 canonicalization。 |
| LSN 分配点 | Step 16 `next_lsn_++`；review 要求 gap-free。 | 当前无 coord。M01 builder 需要传入 `batch_lsn`。 | OV §6：`batch_lsn` 无间隙单调递增；只有进入 durable path 的 batch 消耗 LSN。 | `assign_batch_lsn` 必须先完成所有 recoverable validation / materialization，再提交 `next_lsn++`；构造失败不消耗 LSN。 |
| empty canonical batch | 旧代码会给空 input 分配 LSN。当前 M01 builder 也能返回 0-entry ctx。 | 当前行为只是 helper 形态，不是 coord contract。 | “真正进入 durable path 的 batch”才消耗 LSN；0-entry batch 没有 WAL/memtable terminal action。 | M03 把 0-entry canonical batch 视为 pre-LSN no-op/invalid，不消耗 `batch_lsn`。若实现需要对客户端返回成功空操作，也必须发生在 coord assign 之前。 |
| front routing | Step 16 使用固定 `front_sched_count` 和 `key_hash`。旧 fragments 保存 pointers。 | M01 使用 `key_hash(entry.key) % front_count`，fragments 保存 indices。 | WP §4；039。 | coord 构造时固定 `front_count > 0`；assign 调用 M01 builder，输出 index fragments，不恢复 pointer fragments。 |
| ready tracking | Step 9 `ready_bitmap` fixed window，`ready_base` 随已消费前缀前移。 | 当前无 coord ready state。 | OV §6/§7.4；WP §8.4/§8.5；RSM §2.4。 | 实现 owner-local fixed-window ready bitmap。resolved bit 消费后必须清零并推进 `ready_base`，不能永久以 initial base 取模。 |
| publish vs release | Step 9/16 二者走同一 `resolve_slot`；release 只补洞。 | 当前无 coord。 | OV §6/§7.4；WP §10.2-§10.5。 | 二者共享前缀推进算法。差异只在调用前提和上层可见语义：publish 代表数据已进入 memtable，release 代表 resolved-empty。 |
| gate closed pending | Step 9 gate closed 消费 ready bits，不推进 visible durable；`pending_advance` 记录最大连续前缀；open 消费 pending。 | 当前无 coord gate。 | RSM §2.5；OV §9.2；WP §9.5。 | 保留。gate 不是 mutex，也不阻塞 read；它只决定 resolved prefix 是否写进 current CAT。 |
| pending 跨 CAT replacement | Step 16 测试要求 closed gate 下 install 新 CAT 后，open 把 pending 应用到新 CAT。 | M02 `catalog_store` 支持 install，但不是 coord。 | OV §9.2：CAT1 继承 D0，open 后 post-seal publish 进入 CAT1。 | M03 不设计 install sender，但 gate 状态必须与 current CAT 解耦：`open_gate` 总是读取“当时”的 current CAT 并写 pending。 |
| `acquire_read_handle` | Step 16 复用 Step 11 `catalog_store`。 | M02 已实现 acquire-load CAT，再 acquire-load durable_lsn。 | RAP §2；OV §8；RMC §3；040。 | Coord sender 直接复用 `core::catalog_store` 或完全等价顺序；不能读取裸 durable_lsn 后再找 CAT。 |
| old handle pin | Step 16 测试用 weak_ptr 验证旧 CAT 存活。 | M02 测试进一步覆盖 PRS、guard、front gens、manifest pin。 | RMC §3；RAP §2。 | M03 不改变 M02 pin 链。install/future CAT replacement 只影响新 reader，旧 handle 的 `cat/read_lsn` 不变。 |
| current branch authority | 旧 owner_impl 后续混入 capture/frontier switch sender。 | 当前没有 coord 实现。 | front_wal plan 规定当前现状不是自动权威。 | 041 只取 Step 9/16 的 M03 子集；旧后续 sender surface 不进入本文范围。 |

## 4. Coord Owner State

M03 `coord_state` 概念字段：

```cpp
struct coord_state {
    uint64_t next_lsn;                         // next assignable batch_lsn
    uint32_t front_count;                      // fixed at construction

    core::catalog_store cats;                  // current CAT atomic holder
    uint64_t cat_epoch;                        // mirrors current CAT epoch

    ready_window ready;                        // fixed-window resolved slots
    publish_gate gate;                         // open / closed + pending_advance

    pending_assign_fifo pending_assigns;        // request nodes waiting for ready capacity
};
```

说明：

1. `next_lsn`、`ready`、`gate`、`cat_epoch` 是 coord owner-local 状态；只在
   coord scheduler 线程读写，不需要内部 atomic。
2. `current CAT` 使用 M02 `catalog_store` 或等价 atomic shared_ptr wrapper，
   因为 read handle 是跨 scheduler 传播的 correctness owner。
3. `seal_in_progress`、`capture_flush_frontier`、`frontier_switch`、`install_cat`
   sender 不属于 041 scope。未来 seal/frontier work 可在同一 owner state 上扩展，
   但不能改变本文已冻结的 assign/publish/release/acquire 语义。

### 4.1 Construction Preconditions

Coord 初始化必须 fail-fast 检查：

1. initial CAT 非空。
2. initial CAT 的 PRS、tree_guard、manifest、fronts 非空；这些由
   `core::publish_catalog` constructor 已检查。
3. `front_count > 0`，且必须等于 `initial_cat->prs->fronts->size()`。
4. `next_lsn > initial_cat->durable_lsn`。clean boot 通常是
   `recovered_max_lsn + 1`。
5. ready window size 非 0，且足以覆盖配置的 maximum in-flight writes。

这些是配置 / recovery install 错误，不是可恢复客户端错误；实现必须抛
`std::invalid_argument` 或 `panic_inconsistency`，不能只依赖 release build 会消失的
`assert`。

## 5. `assign_batch_lsn`

### 5.1 Sender Contract

生产 sender 语义：

```text
coord::assign_batch_lsn(client_batch_buffer&& input)
  -> core::batch_ctx
```

`batch_ctx` 的字段和 lifetime 完全沿用 M01：

1. `ctx.input` owns ingress bytes。
2. `ctx.canonical_entries[*].key/value` view into `ctx.input.bytes`。
3. `ctx.fragments[*].entry_indices` index into `ctx.canonical_entries`。
4. `ctx.put_entry_indices` contains canonical indices for PUT entries only。

### 5.2 Algorithm

```text
assign_batch_lsn(input):
  1. Validate coord construction invariants still hold.
  2. Parse / validate input using M01 `client_batch_view`.
  3. Build the canonical positions and detect canonical_entry_count.
  4. If canonical_entry_count == 0:
       return pre-LSN no-op / invalid request; do not mutate coord state.
  5. If ready window has no assign capacity:
       move the request node to coord owner `pending_assigns`;
       do not call the callback and do not mutate `next_lsn`.
  6. candidate_lsn = next_lsn.
  7. Build `batch_ctx(input, candidate_lsn, front_count)`.
  8. Commit: next_lsn = candidate_lsn + 1.
  9. Return ctx.
```

关键要求：

1. recoverable parse / validation failure must happen before `next_lsn` changes。
2. `std::bad_alloc` during `batch_ctx` materialization is process/resource failure；
   it must not be hidden by issuing a fake release slot. If implementation wants to
   recover from allocation failure, it must prove `next_lsn` was not committed yet。
3. `front_count` is coord owner configuration, not per-call input。
4. `assign_batch_lsn` does not write value_ref, WAL bytes, memtable entries or CAT
   durable_lsn。

### 5.3 Assign Capacity

`ready_window` tracks unresolved assigned slots. M03 must prevent ring overwrite:

```text
assign_capacity_available =
    (next_lsn - ready.next_unresolved_lsn()) < ready.window_size()
```

where `ready.next_unresolved_lsn()` is the moving base after consumed resolved prefix.
When gate is closed, visible `CAT.durable_lsn` may intentionally lag behind
`ready.next_unresolved_lsn() - 1`; using visible durable alone would falsely block
or conflate publish-gate delay with unresolved-slot pressure.

Future seal/write pipeline work may impose a stronger no-new-write rule while a seal is
closing the topology. That rule is separate from M03 ready bitmap capacity and belongs to
M12/M08/M09, not 041.

When capacity becomes available after `publish_batch` / `release_batch` consumes prefix,
coord owner must drain `pending_assigns` FIFO while capacity remains. This preserves
client arrival order among requests that reached coord but could not yet receive an LSN.
No pending request has a `batch_lsn`; cancellation or caller failure before callback is a
pre-LSN concern and cannot create an LSN hole.

## 6. Ready Window

M03 uses a fixed ring window:

```text
ready_window:
  base_lsn: first unresolved LSN not yet consumed by prefix scan
  bits:     window_size resolved flags, indexed by lsn % window_size
```

`mark_resolved(lsn)` preconditions:

1. `lsn >= base_lsn`
2. `lsn < base_lsn + window_size`
3. bit for `lsn` is currently clear
4. `lsn < next_lsn` if the caller has access to coord owner state

Violation is an internal correctness error:

1. `lsn < base_lsn`: old or duplicate terminal completion.
2. bit already set: duplicate publish/release for an unresolved slot.
3. `lsn >= base_lsn + window_size`: assign capacity bug or untrusted caller.
4. `lsn >= next_lsn`: publish/release of an unassigned LSN.

Do not silently ignore any of these. A dropped or duplicated terminal signal can create
permanent holes or false visibility.

`advance_contiguous_prefix(scan_from)`:

```text
result = scan_from
while bit(result + 1) is set:
    clear bit(result + 1)
    result++
base_lsn = max(base_lsn, result + 1)
return result
```

When gate is open, `scan_from = current_cat->durable_lsn`.
When gate is closed, `scan_from = max(current_cat->durable_lsn, gate.pending_advance)`.

## 7. Publish Gate

Gate state:

```text
open:
  publish/release writes new resolved prefix into current CAT.durable_lsn

closed:
  publish/release still marks and consumes ready bits
  visible current CAT.durable_lsn does not advance
  gate.pending_advance records max consumed resolved prefix
```

Operations:

```text
close_gate():
  gate.state = closed
  gate.pending_advance = 0

open_gate():
  pending = gate.pending_advance
  gate.state = open
  gate.pending_advance = 0
  if pending > current_cat->durable_lsn.load(acquire):
      current_cat->durable_lsn.store(pending, release)
```

M03 does not expose `close_gate` / `open_gate` as user-facing runtime API. They are coord
owner operations needed for publish gate correctness and for future seal tests. If sender
surface includes them for whitebox tests, the document must label them internal/testing
until M12 designs seal.

Important boundary:

1. gate is not a lock and not a reader pause.
2. gate does not itself prove “batch does not cross memtable generation”. Future seal
   pipeline ordering must prove that separately.
3. `open_gate` applies pending to whichever CAT is current at open time. This preserves
   Step 16’s “pending survives CAT replacement” semantics without making 041 design the
   CAT replacement API.

## 8. `publish_batch`

Precondition:

1. `batch_lsn` was returned by `assign_batch_lsn`.
2. All value writes for the batch have completed durably.
3. All WAL fragments for the batch have completed durably.
4. All memtable fragment inserts for the batch have completed.
5. Current CAT/PRS covers the memtable generation into which those inserts landed.

Algorithm:

```text
publish_batch(batch_lsn):
  1. ready.mark_resolved(batch_lsn)
  2. scan_from =
       gate.open
         ? current_cat->durable_lsn.load(acquire)
         : max(current_cat->durable_lsn.load(acquire), gate.pending_advance)
  3. new_prefix = ready.advance_contiguous_prefix(scan_from)
  4. if new_prefix == scan_from: return
  5. if gate.open:
       current_cat->durable_lsn.store(new_prefix, release)
     else:
       gate.pending_advance = max(gate.pending_advance, new_prefix)
```

`publish_batch` completion means coord accepted the terminal signal. It does not mean a
closed gate has made the batch visible; visibility changes only when the release-store to
current CAT happens.

## 9. `release_batch`

Precondition:

1. `batch_lsn` was returned by `assign_batch_lsn`.
2. The batch failed before memtable phase started.
3. No `memtable_entry.data_ver == batch_lsn` exists in any front memtable.

Algorithm is identical to `publish_batch` except the semantic label of the resolved slot
is released-empty.

```text
release_batch(batch_lsn):
  resolve_slot(batch_lsn)
```

Consequences:

1. `durable_lsn` may advance across released-empty slots.
2. readers with `read_lsn >= batch_lsn` see no data for that released batch because no
   memtable/tree record exists for it.
3. pre-WAL or partial-WAL orphan values are outside M03; WP §10 and recovery docs define
   how later phases clean them.

Calling `release_batch` after memtable phase starts is a fatal write pipeline bug. Coord
cannot infer that phase from an LSN alone; M08/M09 must carry batch phase state and call
the correct terminal path.

## 10. `acquire_read_handle`

Sender contract:

```text
coord::acquire_read_handle() -> core::read_handle
```

Algorithm is exactly M02:

```text
cat = atomic_load(current_cat, acquire)
read_lsn = cat->durable_lsn.load(acquire)
return read_handle{cat, read_lsn}
```

Rules:

1. `read_handle.cat` pins the CAT instance.
2. `read_handle.read_lsn` is immutable after acquire.
3. Later `durable_lsn` stores to the same CAT do not change old handle snapshots.
4. Later CAT replacement does not affect old handles.
5. The returned handle is the only allowed source for read path topology:
   `cat->prs->fronts[owner]` and `cat->prs->tree_guard->manifest`.

M03 does not design point_get, multiget, scan, tree lookup or value read. It only provides
the handle those later pipelines must carry.

## 11. 状态机

### 11.1 LSN Slot State

```text
unassigned
  -- assign_batch_lsn success -->
assigned_inflight
  -- publish_batch, after all-memtable barrier -->
resolved_published
  -- release_batch, before memtable phase -->
resolved_released_empty
  -- prefix scan consumes -->
consumed_prefix
```

`resolved_published` and `resolved_released_empty` are both represented by ready bits in
M03. The distinction is semantic and belongs to the caller / logs / future observability;
prefix advancement treats both as resolved.

Invalid transitions:

1. `unassigned -> publish/release`
2. `assigned_inflight -> release` after memtable phase has started
3. `resolved_* -> publish/release` again
4. `consumed_prefix -> publish/release` again

### 11.2 Gate State

```text
open
  -- close_gate -->
closed(pending_advance = 0)
  -- publish/release resolved prefix -->
closed(pending_advance = max prefix)
  -- open_gate -->
open, current_cat.durable_lsn >= pending_advance
```

### 11.3 CAT Read Snapshot

```text
current_cat = CAT0, CAT0.durable_lsn = D0
  acquire_read_handle -> {CAT0, D0}
  publish/release may store CAT0.durable_lsn = D1
  acquire_read_handle -> {CAT0, D1}
  future CAT replacement -> current_cat = CAT1
  old handle remains {CAT0, D0}; new handle sees CAT1
```

## 12. 不变量

1. `next_lsn` is strictly greater than every assigned LSN.
2. Each successful assign consumes exactly one LSN and returns that exact value in
   `batch_ctx.batch_lsn`.
3. Recoverable invalid input never consumes an LSN.
4. `ready.base_lsn` is the first unresolved LSN not already consumed by prefix scan.
5. No ready bit may remain set below `ready.base_lsn`.
6. `durable_lsn = X` means every LSN `<= X` has reached a terminal state:
   published or released-empty.
7. `durable_lsn` never skips an unresolved lower LSN.
8. Gate closed never loses resolved prefix: consumed prefix is either visible in
   CAT.durable_lsn or recorded in `pending_advance`.
9. `open_gate` applies pending to current CAT, not to a cached old CAT pointer.
10. `current_cat->durable_lsn` is the single visible publish frontier. Coord must not keep
    a second durable frontier that can diverge.
11. `read_handle` pins CAT/PRS/guard/front gens through shared_ptr; coord never returns a
    handle with null CAT.
12. M03 never makes value bytes visible and never stores value body in memtable or read
    handle.

## 13. 内存序

Coord owner-local state:

1. `next_lsn`, `ready`, `gate`, `cat_epoch` are single-thread scheduler state.
2. These fields use ordinary loads/stores inside coord `advance()`.
3. No locks or CAS are needed for these fields.

CAT publication:

1. `current_cat` must be an atomic `shared_ptr<const publish_catalog>` or M02
   `catalog_store`.
2. CAT install, when future M12 designs it, must store the new shared_ptr with
   `memory_order_release`.
3. `acquire_read_handle` must load current CAT with `memory_order_acquire`.

Durable frontier:

1. `publish_batch` / `release_batch` write `cat->durable_lsn.store(new_prefix,
   memory_order_release)` when gate is open or when open_gate consumes pending.
2. `acquire_read_handle` reads `cat->durable_lsn.load(memory_order_acquire)`.
3. This release/acquire pair is the publish fence for memtable entries that causally
   completed before `publish_batch` was enqueued.

Cross-scheduler causality:

1. all-memtable reduce must enqueue `publish_batch` only after every target front has
   completed its insert callback.
2. PUMP/per-core queues provide the message-ordering edge from front completion to reduce
   and from reduce to coord publish.
3. Coord’s release-store to `durable_lsn` is the final edge that lets later readers acquire
   a `read_lsn` and safely query PRS-pinned memtable generations.

Release path:

1. `release_batch` uses the same release-store because readers may observe later published
   batches whose prefix includes the released slot.
2. The released slot itself has no memtable record; visibility remains correct because
   reads select actual records by `data_ver <= read_lsn`.

## 14. 错误 / 失败语义

### 14.1 Assign Errors

| Condition | Semantics |
|---|---|
| malformed `client_batch_buffer` | return/throw client-visible invalid request; no LSN consumed |
| `front_count == 0` / CAT fronts mismatch | configuration failure; fail-fast |
| empty canonical batch | pre-LSN no-op/invalid; no LSN consumed |
| ready window full | owner-local FIFO deferral; no LSN consumed until capacity returns |
| allocation failure while materializing `batch_ctx` | fatal resource failure unless implementation proves no owner mutation occurred |

### 14.2 Terminal Errors

| Condition | Semantics |
|---|---|
| publish/release unassigned LSN | fail-fast internal bug |
| duplicate publish/release | fail-fast internal bug |
| old LSN below ready base | fail-fast internal bug |
| LSN outside ready window | fail-fast; assign capacity invariant was violated |
| request queue enqueue failure for publish/release | fail-fast; losing a post-LSN terminal signal can create permanent hole |

`publish_batch` and `release_batch` should not silently return `false`. A terminal signal
is required to close a gap-free LSN slot.

### 14.3 Write Phase Failure Boundary

1. Validation/canonicalization failure: no LSN, no release.
2. Value phase failure after LSN: call `release_batch`.
3. WAL phase failure before memtable phase: call `release_batch`.
4. Memtable phase failure after any insert begins: runtime fatal, not release.
5. Publish failure inside coord is fatal; otherwise a durable, memtable-visible batch may
   remain permanently invisible in live runtime.

## 15. 测试计划

M03 implementation should add a focused contract test target, for example
`inconel_test_m03_coord_scheduler_assign_publish_release`.

Required tests:

1. `assign_batch_lsn_is_gap_free_and_consumes_m01_carrier`  
   Build from `client_batch_buffer`; assert returned `batch_lsn` values are 1, 2, 3 and
   `batch_ctx` retains M01 view/fragment/index invariants.

2. `assign_batch_lsn_canonicalizes_and_routes_by_fixed_front_count`  
   Same-key last writer wins; fragments route by `key_hash % front_count`; fragment entries
   remain stable indices, not pointers.

3. `assign_batch_lsn_rejects_malformed_input_without_lsn_gap`  
   Bad op, truncated input, DELETE with value bytes, or empty canonical batch must not
   advance `next_lsn`.

4. `assign_batch_lsn_defers_fifo_when_ready_window_full`  
   With small ready window, assign up to capacity; next assign remains pending with no
   callback and no LSN; resolving the oldest slot drains pending requests in FIFO order.

5. `publish_batch_advances_only_contiguous_prefix`  
   Publish 1, publish 3, publish 2; durable_lsn observes 1, 1, 3.

6. `release_batch_fills_gap_without_visible_data`  
   Publish 1, publish 3, release 2; durable_lsn advances to 3. Test only coord prefix
   state; visible data is covered by later write/read pipeline tests.

7. `ready_base_moves_with_consumed_prefix`  
   Preserve old Step 9 regression: base starts at 1, moves to 2 after resolving 1, stays
   at 2 while 3 is pending, then moves to 4 after resolving 2.

8. `closed_gate_accumulates_pending_and_open_applies_to_current_cat`  
   Close gate, resolve 1/3/2, assert current CAT durable_lsn unchanged, pending is 3,
   open gate stores 3 to current CAT and clears pending.

9. `gate_pending_survives_future_cat_replacement_contract`  
   If a test-only current CAT swap exists, close gate, resolve 1, swap CAT with same
   durable_lsn, open gate, assert pending applies to new CAT and old handle remains old.
   If M03 implementation does not expose CAT swap, record this as a future M12 regression
   derived from Step 16.

10. `acquire_read_handle_snapshots_cat_and_read_lsn`  
    Reuse M02 behavior through coord sender: old handle read_lsn does not change after
    later durable_lsn store; new handle sees the new value.

11. `old_handle_pins_old_prs_guard_manifest_and_front_gens`  
    Reuse M02 weak_ptr pattern through coord acquire path.

12. `terminal_duplicate_old_and_out_of_window_lsn_fail_fast`  
    Duplicate publish/release, unassigned LSN, old consumed LSN and out-of-window LSN are
    death/error tests, not no-ops.

13. `publish_release_hot_path_has_no_payload_copy`  
    Whitebox or allocation-count test proving terminal operations do not allocate and do
    not touch batch value bytes.

Old Step 9 / Step 16 tests remain the semantic seed, but they must be rewritten to current
M01/M02 names and value_ref-only contracts.

## 16. 排除范围

041 明确排除：

1. WAL segment allocation, WAL byte layout, WAL append, FUA batching, WAL reclaim.
2. front scheduler owner methods: WAL stream, memtable insert, lookup, scan, seal,
   collect/release gens.
3. value persistence, value read, value cache policy, orphan cleanup implementation.
4. `write_batch` sender pipeline, all-WAL/all-memtable fan-out/reduce, batch phase carrier.
5. point_get, multiget, scan, tree lookup, tree/value result merge.
6. runtime builder/facade/API surface and application operation wrappers.
7. seal trigger, `close_gate` orchestration, CAT1 construction, frontier switch, CAT2
   construction, flush frontier capture.
8. recovery implementation and allocator rebuild.

If implementation work for 041 needs any item above to compile, it must introduce only a
test-local shim or narrow placeholder with explicit name and fail-fast behavior. It must
not use a generic production name that implies the excluded semantics are implemented.

## 17. 冲突与决议

1. **旧 pointer fragments vs M01 index fragments**  
   Old Step 16 tests assert pointer fragments. Current M01 intentionally replaced them
   with stable indices. 041 follows M01; old pointer-specific assertions must be rewritten.

2. **旧 empty batch consumed LSN vs production durable-path rule**  
   Old helper and current M01 builder can produce 0-entry `batch_ctx` after receiving an
   LSN. 041 resolves this at coord level: empty canonical batch is pre-LSN no-op/invalid,
   because it has no WAL/memtable terminal path and should not create a release obligation.

3. **formal WP §8.5 uses `next_lsn - durable_lsn`, old Step 9 moves `ready_base` while
   gate closed**  
   041 separates the two concepts. Ready bitmap ring safety is bounded by unresolved slots
   (`next_lsn - ready_base`). Future seal/write backpressure may additionally stop assign
   while visible durable is held behind a closed gate.

4. **old `coord_state` includes capture/frontier switch in later branch state**  
   Those methods are outside M03 and are not migrated by 041. Future M12/flush work must
   write separate design before exposing them.

5. **M02 `catalog_store` vs coord-owned current CAT**  
   040 explicitly says `catalog_store` is not coord scheduler. 041 may embed it or copy
   its exact atomic semantics inside coord owner; either choice is allowed if external
   acquire behavior stays identical.

## 18. 需要人工判断的点

当前 041 没有必须阻塞的人工裁决点。上面的冲突均可由旧 Step 9/16、039/040 和正式
design_doc 唯一裁决。

若 reviewer 不接受“empty canonical batch 不消耗 LSN”，需要在实现前改本文和 M01
tests；不能在代码里悄悄采用旧 helper 行为。

## 19. 相邻事项

1. M04 (`wal_space_sched`) 是 M03 后最直接的相邻设计；它可以依赖
   `assign_batch_lsn` 产出的 `batch_ctx`，但不能把 WAL allocation 或 append 回填进
   coord。
2. M05 front owner 才能实现 memtable insert / lookup sender，并承担
   `release_batch` 前提中的“memtable phase 未开始”边界。
3. M08/M09 write pipeline 必须显式携带 batch phase，确保 value/WAL 失败走
   `release_batch`，memtable 失败 fatal。这个前置不适合提前放进 M03。
4. M12 seal/CAT replacement 必须复用本文 gate pending 规则，并补设计
   `close_gate -> front seal -> CAT1 install -> open_gate`；041 不提前设计该 pipeline。
5. 后续 value/WAL fan-out 设计必须回看 `known_issues.md` 中 INC-048，显式 bounded
   concurrency，不能使用无界 `concurrent()`。
