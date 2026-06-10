# 042 — WAL Space Scheduler / Stream State

> 本文是 `front_wal_development_plan.md` 里 M04 的详细设计文档。
> M04 只冻结 `wal_space_sched` 的 segment allocation / reclaim 语义，以及
> future front WAL stream 所需的 runtime state shape。
>
> 本文不设计 WAL entry append / prepare / FUA issue、front owner memtable、
> `write_batch` pipeline、runtime builder/API、recovery scanner、seal/frontier
> switch，或任何 NVMe I/O 提交路径。旧 Step 18 覆盖的 append mock success path
> 在当前迁移计划中被拆到 M06。

## 1. 范围

M04 把旧 `inconel` 分支 Step 18 的 WAL segment 空间管理语义迁移到当前
`inconel.new` 架构，但按当前总计划收窄为两个部分：

1. `wal_space_sched`：
   - 全局单实例 owner。
   - 管理 WAL segment pool、free/reuse generation、sealed segment metadata、
     pending allocation backpressure。
   - 暴露 `alloc_segment(stream_id, sealed_info?)` 和
     `reclaim_check(recovery_safe_lsn)` sender surface。
2. `wal_stream_state`：
   - front owner 私有的 WAL stream cursor / active segment token / segment geometry
     carrier。
   - 冻结 header/trailer offset、usable range、generation token 与 later append
     需要维护的 `min_lsn/max_lsn/write_offset` 位置。
   - 不编码 WAL entries，不写 header/trailer bytes，不提交 NVMe FUA。

M04 落点：

1. `apps/inconel/wal/scheduler.hh`
2. `apps/inconel/wal/sender.hh`
3. `apps/inconel/core/registry.hh` 后续实现可加入 wal singleton placeholder；042
   只定义需要的注册形状，不实现 runtime builder。
4. M04 合约测试：
   - segment alloc/free/generation。
   - sealed metadata + reclaim threshold。
   - pending alloc FIFO / backpressure。
   - stream state geometry / token stale checks。
   - PUMP sender facade smoke。

M04 明确不做：

1. 不调用 `format::encode_wal_put_entry` / `encode_wal_delete_entry`；只引用
   `format/wal.hh` 的 POD size 和常量做 geometry。
2. 不构造 WAL page image、tail DMA frame、header bytes、sealed trailer bytes。
3. 不提交 `nvme::write_frame`、不定义 FUA plan、不定义 write batching budget。
4. 不实现 `front_sched::write_wal_fragment`、`insert_memtable_entries`、seal owner。
5. 不实现 recovery WAL segment scan，也不重置 WAL pool 的 boot recovery 流程。
6. 不实现 top-level `write_batch` pipeline 或 public runtime API。

## 2. 已检查输入

旧 `inconel` 分支证据：

1. `ai_context/inconel/plan/steps/step_18_design.md`
2. `ai_context/inconel/plan/steps/step_18_test_spec.md`
3. `ai_context/inconel/plan/steps/step_18_review.md`
4. `apps/inconel/runtime/wal/space.hh`
5. `apps/inconel/test/step_18_wal_space_sched_contract_test.cc`
6. `apps/inconel/format/wal.hh`

当前 `inconel.new` 证据：

1. `ai_context/inconel/plan/039_front_wal_phase_a_carrier_inc055_design.md`
2. `ai_context/inconel/plan/040_read_handle_prs_memtable_lookup_design.md`
3. `ai_context/inconel/plan/041_coord_scheduler_assign_publish_release_design.md`
4. `apps/inconel/core/batch_carrier.hh`
5. `apps/inconel/core/registry.hh`
6. `apps/inconel/coord/scheduler.hh`
7. `apps/inconel/coord/sender.hh`
8. `apps/inconel/format/wal.hh`
9. `apps/inconel/format/superblock.hh`
10. 当前 `apps/inconel/wal/` 只有 `.gitkeep`，没有 production WAL scheduler。

正式设计依据：

1. `ai_context/inconel/design_doc/design_overview.md`
2. `ai_context/inconel/design_doc/runtime_state_machine.md`
3. `ai_context/inconel/design_doc/write_path_and_pipeline.md`
4. `ai_context/inconel/design_doc/recovery_and_wal_reclaim.md`
5. `ai_context/inconel/design_doc/on_disk_formats.md`
6. `ai_context/inconel/design_doc/runtime_memory_and_cache.md`
7. `ai_context/inconel/design_doc/code_modules.md`
8. `ai_context/inconel/design_doc/cross_doc_contracts.md`

## 3. 语义来源对照表

| 项目 | 旧 Step 18 证据 | 当前 `inconel.new` 状态 | 正式设计依据 | 042 决议 |
|---|---|---|---|---|
| 模块归属 | 旧代码在 `runtime/wal/space.hh`，同文件包含 space 和 stream mock append。 | `apps/inconel/wal/` 为空；`registry.hh` 只有 wal singleton future 注释。 | `code_modules.md` 要求 L2 `wal/` 模块，scheduler 模块只对外暴露 `sender.hh`。 | 新 WAL space 落到 `apps/inconel/wal/{scheduler.hh,sender.hh}`。旧 `runtime/wal` 路径不迁移。 |
| M04 范围 | Step 18 包含 `append_entries_fua()`、mock block device、segment image decode。 | 当前总计划把 WAL append prepare/FUA 放到 M06。 | `front_wal_development_plan.md`：M04 = WAL space scheduler；M06 = WAL append prepare + FUA issue。 | 042 只设计 segment metadata / stream state。append bytes、header/trailer writing、mock/real NVMe I/O 全部排除。 |
| `segment_id` | `{device_id, index}`，v1 device 0。 | 当前没有 WAL id 类型；`format::paddr` 已 device-aware。 | OV §13：WAL segment id 从一开始就是 `(device_id,index)`。 | 定义 `wal::segment_id { uint16_t device_id; uint32_t index; }`；v1 allocator 只产生 device 0，但类型不省略 device。 |
| segment base 地址 | 旧 helper `base.lba = index * segment_lbas`，没有 `wal_base_paddr`。 | 当前 `format/superblock.hh` / ODF 有 `wal_base_paddr`。 | ODF §3.1：`wal_base_paddr.lba + index * segment_lbas`。 | 新 helper 必须接受 `wal_base_paddr`，计算 `base = wal_base_paddr + index * segment_lbas`。旧 helper 只适用于 mock layout，不能迁移。 |
| `segment_gen` | 初次分配 gen=1；reclaim 后 next_gen = old + 1。entry/header/trailer 都写 gen。 | 当前 `format/wal.hh` 已把 gen 放进 header/entry/trailer。 | OV §11.2：gen 是同一物理 segment 复用代数，recovery 依赖它区分旧残留。 | gen 是 allocation token 的必含字段。任何 sealed/reclaim 请求必须校验 `{id,segment_gen}`，stale gen 直接 fail-fast。 |
| segment state | 旧 Step 18 有 `FREE -> ACTIVE -> SEALED -> FREE`。 | 当前无 WAL space。 | OV §11.5 / RSM §5。 | 保留状态机。ACTIVE lease 只允许一个 front stream 持有；SEALED 只被 wal_space reclaim；FREE 只在 free_pool。 |
| `segment_runtime*` | 旧 `wal_space_state` owns vector slots，alloc 返回 `segment_runtime*`。front 直接读写 min/max/st。 | 当前无实现；M03 coord sender 已使用 scheduler-owned request + callback 模式。 | Cross-doc `alloc_segment` 签名仍写 `segment_runtime*`。 | 042 保留 stable pointer token，但明确 ownership：wal_space owns storage；ACTIVE 期间 front stream 独占该 slot 的 runtime lease；wal_space 不读取该 active slot 的 mutable append fields，直到 front 通过 sealed_info 归还 metadata。 |
| `sealed_segment_info` | `{id, segment_gen, min_lsn, max_lsn}`。 | 当前无类型。 | RSM §5.1 / RW §12.4 使用同形态；reclaim 条件看 `max_lsn`。 | 保留四字段。`write_end` 属于 sealed trailer / stream append state，不进入 wal_space reclaim contract。 |
| sealed metadata push 时机 | 旧 Step 18 把 sealed_info 搭在下一次 `alloc_segment` 请求上。 | 当前无 WAL。 | RSM §5.3 / RW §12.4：换段时随 alloc 请求 push，零额外消息。 | 保留。实现必须保证同一个 request 的 sealed_info 只记录一次；若该 alloc 因无空段而 pending，重试时不能重复 push sealed_info。 |
| free pool 数据结构 | 旧 `std::vector<segment_alloc_entry>`；RSM 伪码写 `local::queue<...,256>`。 | 当前没有实现。 | 容量硬约束：不能用小量级固定上限冒充 segment pool。 | 用 owner-local scalable container，至少 reserve `wal_segment_count` capacity。固定 256 slot free_pool 不可接受。 |
| sealed list 数据结构 | 旧 `std::vector<sealed_segment_info>`；RSM 伪码写 small_vector<64>。 | 当前没有实现。 | RW §12.4 sealed segments 可很多；reclaim 本地筛选。 | 用 `std::vector<sealed_segment_info>` 或等价 scalable owner-local container，容量按 segment count 预留或增长；不能固定 64。 |
| alloc 耗尽行为 | 旧 Step 18 允许 `alloc_segment()==nullptr`；总计划 M04 要求 backpressure 或 fail-fast。 | 当前 M03 assign 已有 pending FIFO，不制造假成功。 | OV §11.4 / WP §7.4：已分配 LSN 后 WAL pressure 只能等待/排队或终止，不能留下 hole。 | 生产 sender 不返回假成功/null。无 segment 时请求进入 `pending_alloc_queue`，直到 reclaim 唤醒；测试 helper 可提供 non-blocking `try_alloc_for_testing()` 返回 null 来检查 empty 状态。 |
| pending alloc queue | 旧 Step 18 明确不做。 | 当前无 WAL；M03 已证明 pending FIFO 形态。 | RSM §5.3/§5.4、WP §7.4 要求 pending alloc queue。 | M04 必须设计并实现 pending FIFO 语义。若实现 agent认为要延后，必须申请人工裁决。 |
| `wal_stream_state` | 旧 Step 18 持 `active_seg/write_offset/segment_bytes/lba_size/seg_buf` 并写 mock device。 | 当前无 front/wal stream。`runtime_memory_and_cache.md` 把 WAL tail frame 归 front。 | OV §11.3 / RMC §7.1：WAL append state 在 front_sched，tail frame 是 front owner 本地 frame。 | M04 定义 front-local stream state shape，但不放 `seg_buf` mock image；tail frame / segmented DMA frame 由 M06/front 实现。 |
| format helper | 旧 `format/wal.hh` 使用 bool encode/decode、`WAL_SEALED_TRAILER_MAGIC`。 | 当前 `format/wal.hh` 已是 reason-aware status、`WAL_SEAL_MAGIC`、`std::span<char>`。 | ODF §3；plan 015 已冻结 current format PODs。 | M04 不改格式。后续 append 必须用当前 `apps/inconel/format/wal.hh`，不能照搬旧 helper 名称或 byte layout。 |
| registry | 旧 runtime 直接构造 runtime/wal state。 | 当前 `registry.hh` 有 future `wal::scheduler*` 注释，没有 concrete type。 | `code_modules.md` 要求 singleton access。 | M04 实现可把 wal singleton 加入 registry，占位与 accessor 模式仿 M03 coord。042 不实现 builder。 |

## 4. 类型与模块边界

### 4.1 `segment_id`

```cpp
struct segment_id {
    uint16_t device_id = 0;
    uint32_t index = 0;
};
```

规则：

1. v1 allocator 只产生 `device_id == 0`。
2. `index < wal_segment_count`。
3. 类型必须保留 `device_id`，不能以 v1 单盘为理由把 token 简化为裸 index。

### 4.2 `wal_segment_state`

```cpp
enum class wal_segment_state : uint8_t {
    free = 1,
    active = 2,
    sealed = 3,
};
```

这只是 runtime state。磁盘上没有独立 persisted state 字段；recovery 通过 header /
entry / optional trailer 判断 surviving data。

### 4.3 `segment_runtime`

```cpp
struct segment_runtime {
    segment_id id;
    uint32_t owner_stream;
    uint32_t segment_gen;
    wal_segment_state st;

    uint64_t min_lsn;
    uint64_t max_lsn;
};
```

字段语义：

1. `id` 和 `segment_gen` 组成 stale-use token。
2. `owner_stream` 是当前 ACTIVE lease 的 front owner index。
3. `st` 的 owner 转移规则：
   - `FREE` / `SEALED` 状态由 `wal_space_sched` 独占维护。
   - `ACTIVE` lease 发给 front stream 后，front stream 是该 segment 的 append owner。
   - front stream sealing 时构造 `sealed_segment_info` 并随下一次 alloc 请求归还给
     wal_space；wal_space 记录后才重新拥有该 segment 的 reclaim metadata。
4. `min_lsn/max_lsn` 仅描述该 segment 当前 generation 已 append 的 lsn 范围。
   初始 ACTIVE 使用 `min_lsn = UINT64_MAX`、`max_lsn = 0` 表示尚无 entry。
5. `segment_runtime` 不保存 `write_offset`。`write_offset` 是 per-stream append
   cursor，属于 `wal_stream_state`。

实现可以把 `segment_runtime` 存在 `std::vector<segment_runtime> slots_` 中并返回
stable pointer。该 vector 必须在构造后不再 reallocate；最简单做法是按
`wal_segment_count` 一次性构造完整 `slots_`。

### 4.4 `segment_alloc_entry`

```cpp
struct segment_alloc_entry {
    segment_id id;
    uint32_t next_gen;
};
```

它只存在于 `free_pool`，表示一个 whole segment 已经通过 recovery-safety 条件回收，
下次分配该物理 segment 时应使用的 generation。

### 4.5 `sealed_segment_info`

```cpp
struct sealed_segment_info {
    segment_id id;
    uint32_t segment_gen;
    uint64_t min_lsn;
    uint64_t max_lsn;
};
```

校验规则：

1. `id.index < wal_segment_count`。
2. `segment_gen != 0`。
3. `min_lsn <= max_lsn`。如果实现允许 empty segment 被 seal，需要单独定义
   `empty` marker；M04 不允许 empty sealed segment 进入 reclaim list。
4. 该 `{id,segment_gen}` 必须对应一个当前 ACTIVE lease，且 owner 与请求
   `stream_id` 一致。
5. 同一个 `{id,segment_gen}` 只能被 sealed 一次。

任何 violation 都是 internal correctness bug，必须 fail-fast。不能 silently drop。

### 4.6 `segment_geometry`

M04 必须把 disk format geometry 放成显式结构，而不是散落在 helper 参数中：

```cpp
struct segment_geometry {
    format::paddr wal_base_paddr;
    uint32_t wal_segment_size;
    uint32_t lba_size;
    uint32_t wal_segment_count;
    uint32_t expected_format_version;
};
```

构造前提：

1. `wal_base_paddr.device_id == 0` in v1。
2. `wal_segment_count > 0`。
3. `lba_size > 0`。
4. `wal_segment_size % lba_size == 0`。
5. `format::WAL_SEGMENT_HEADER_SIZE +
   align_up(format::WAL_SEALED_TRAILER_SIZE, lba_size) < wal_segment_size`。
6. `wal_segment_size - header - trailer_reserved > max_supported_wal_entry_size`。
   M04 不定义 key policy，但不能让 geometry 明显无法容纳 v1 entry。

`segment_base_paddr(geometry, id)`：

```text
segment_lbas = geometry.wal_segment_size / geometry.lba_size
return {
  .device_id = id.device_id,
  .lba = geometry.wal_base_paddr.lba + id.index * segment_lbas,
}
```

旧 Step 18 的 `id.index * segment_lbas` helper 不得迁移到 production。

## 5. `wal_space_sched` Owner State

M04 `wal_space_sched` 概念字段：

```cpp
struct wal_space_state {
    segment_geometry geometry;

    std::vector<segment_runtime> slots;              // size == wal_segment_count
    uint32_t alloc_head;                             // first never-allocated index

    std::vector<segment_alloc_entry> free_pool;       // reusable segments
    std::vector<sealed_segment_info> sealed_segments; // not yet recovery-safe

    pending_alloc_fifo pending_allocs;               // reqs waiting for a segment

    std::atomic<uint32_t> used_segment_count;         // heuristic / seal trigger only
};
```

Ownership:

1. `wal_space_sched` is the only owner of `slots`, `alloc_head`, `free_pool`,
   `sealed_segments`, and `pending_allocs`.
2. ACTIVE segment append state belongs to the owning front stream. `wal_space_sched`
   must not read an active segment's `min_lsn/max_lsn` to decide reclaim; it only trusts
   the later `sealed_segment_info`.
3. `used_segment_count` is diagnostic / seal-trigger metadata. It is not a correctness
   gate.

Capacity and memory:

1. `slots.size() == wal_segment_count` is required. This makes returned
   `segment_runtime*` stable and avoids per-allocation heap work.
2. `free_pool` and `sealed_segments` must be able to hold O(`wal_segment_count`) entries.
   Fixed 64/256 slot containers are not acceptable for production.
3. Normal `alloc_segment` / `reclaim_check` hot path must not allocate after construction
   if enough capacity was reserved. If vector growth remains possible, it must be documented
   as cold-path-only and never on per-entry append.

For scale calibration: with 4 MiB segments, 1 TiB of WAL area has 262,144 segments.
A 32-byte `sealed_segment_info` vector for every segment is about 8 MiB; a fixed 64-entry
small vector would be wrong by four orders of magnitude.

## 6. Sender Surface

Production sender API:

```cpp
namespace apps::inconel::wal {

[[nodiscard]] auto alloc_segment(wal_space_sched& sched,
                                 uint32_t stream_id,
                                 std::optional<sealed_segment_info> sealed = {});

[[nodiscard]] auto reclaim_check(wal_space_sched& sched,
                                 uint64_t recovery_safe_lsn);

}
```

Value types:

1. `alloc_segment(...) -> segment_runtime*`
2. `reclaim_check(...) -> void`

Implementation pattern must follow current M03 coord scheduler:

1. Request nodes are allocated by sender `start`.
2. `schedule_*` enqueues request into owner queue.
3. `advance()` drains bounded batches from alloc/reclaim queues.
4. Callback exceptions are not converted into scheduler semantic failures after state has
   been committed.
5. `compute_sender_type` and `op_pusher` are specialized for both sender types.

M04 sender functions take an explicit scheduler reference, matching M03 coord's current
`coord::assign_batch_lsn(sched, ...)` facade. Later runtime API may wrap this through
`rt::wal_space()`, but 042 does not design runtime builder.

## 7. `alloc_segment`

### 7.1 Contract

```text
wal::alloc_segment(stream_id, sealed_info?) -> segment_runtime*
```

Semantics:

1. If `sealed_info` is present, first record that sealed segment exactly once.
2. Then try to allocate a new ACTIVE segment for `stream_id`.
3. If a segment is immediately available, callback with a stable `segment_runtime*`.
4. If no segment is available, keep the request pending. Do not callback with `nullptr`
   and do not report success.
5. Pending requests are served FIFO as `reclaim_check` returns segments to `free_pool`.

### 7.2 Algorithm

```text
handle_alloc_segment(req):
  if !req.sealed_consumed and req.sealed_info:
      record_sealed_segment(req.stream_id, req.sealed_info)
      req.sealed_consumed = true

  if try_allocate_now(req.stream_id) -> seg:
      cb(seg)
      delete req
      drain_pending_allocs()
      return

  pending_allocs.push_back(req)
```

`try_allocate_now(stream_id)`:

```text
if !free_pool.empty():
    entry = free_pool.pop_back()
    slot = slots[entry.id.index]
    slot.id = entry.id
    slot.owner_stream = stream_id
    slot.segment_gen = entry.next_gen
    slot.min_lsn = UINT64_MAX
    slot.max_lsn = 0
    slot.st = ACTIVE
    used_segment_count++
    return &slot

if alloc_head < wal_segment_count:
    idx = alloc_head++
    slot = slots[idx]
    slot.id = {device_id = geometry.wal_base_paddr.device_id, index = idx}
    slot.owner_stream = stream_id
    slot.segment_gen = 1
    slot.min_lsn = UINT64_MAX
    slot.max_lsn = 0
    slot.st = ACTIVE
    used_segment_count++
    return &slot

return null
```

### 7.3 Recording `sealed_info`

```text
record_sealed_segment(stream_id, info):
  validate id / generation / owner / current ACTIVE state
  slot = slots[info.id.index]
  slot.st = SEALED
  slot.min_lsn = info.min_lsn
  slot.max_lsn = info.max_lsn
  sealed_segments.push_back(info)
```

Important:

1. Recording sealed metadata and allocating a replacement segment are in the same
   wal_space owner turn.
2. If replacement allocation cannot complete immediately, the old sealed segment remains
   tracked in `sealed_segments`; retrying the pending request must not push it again.
3. If `sealed_segments.push_back` can allocate and throw, it must happen before any
   irreversible state change to the old slot. Preferred implementation reserves capacity
   for `wal_segment_count` to make this path non-throwing in practice.

### 7.4 Pending FIFO

Pending FIFO rules:

1. Requests wait in arrival order.
2. A request with already consumed `sealed_info` stays pending only for a replacement
   allocation; it must not be revalidated as a new seal on wakeup.
3. `reclaim_check` drains pending while both pending requests and free segments exist.
4. New bump allocation after `alloc_head` exhaustion is not retried unless `wal_segment_count`
   changes, which M04 does not support. Pending wakeups are driven by reclaim.
5. Queue full in `pending_allocs` is a runtime configuration failure; fail-fast rather
   than dropping the request.

## 8. `reclaim_check`

### 8.1 Contract

```text
wal::reclaim_check(recovery_safe_lsn) -> void
```

`recovery_safe_lsn` comes from tree/flush logic. M04 does not compute it.

Reclaim condition:

```text
sealed segment is reclaimable iff info.max_lsn <= recovery_safe_lsn
```

This is intentionally whole-segment reclaim. There is no partial segment reclaim.

### 8.2 Algorithm

```text
handle_reclaim_check(recovery_safe_lsn):
  write = 0
  for each info in sealed_segments:
      if info.max_lsn <= recovery_safe_lsn:
          free_pool.push_back({info.id, info.segment_gen + 1})
          slot = slots[info.id.index]
          slot.st = FREE
          slot.owner_stream = invalid
          slot.min_lsn = UINT64_MAX
          slot.max_lsn = 0
          used_segment_count--
      else:
          sealed_segments[write++] = info
  sealed_segments.resize(write)

  drain_pending_allocs()
  cb()
```

Ordering:

1. Move reclaimable segments to `free_pool` before draining pending.
2. Drain pending in FIFO order.
3. `reclaim_check` callback should run after pending drain attempts so tests and future
   maintenance code observe a stable post-reclaim state.

`reclaim_check` must not inspect disk bytes. It trusts the recovery-safety frontier and
its own sealed metadata list.

## 9. `wal_stream_state`

M04 stream state is front-local runtime state. It is designed here because future M05/M06
front owner must own it, but M04 does not implement front owner.

```cpp
class wal_stream_state {
public:
    uint32_t stream_id() const;
    segment_runtime* active_segment() const;
    uint32_t write_offset() const;
    uint32_t usable_end_offset() const;
    uint32_t segment_bytes() const;
    uint32_t lba_size() const;

    void install_segment(segment_runtime* seg);
    bool can_fit_entry(uint32_t encoded_len) const;
    void note_appended(uint64_t batch_lsn, uint32_t encoded_len);
    sealed_segment_info make_sealed_info() const;
};
```

Required fields:

```cpp
struct wal_stream_state {
    uint32_t stream_id;
    segment_runtime* active_seg;
    segment_geometry geometry;
    uint32_t write_offset;
    uint32_t trailer_reserved_bytes;
    uint64_t seg_min_lsn;
    uint64_t seg_max_lsn;
};
```

Construction / install:

1. `active_seg` may be null only before the first allocation. Production front append will
   request an active segment before writing header/entry bytes.
2. `install_segment(seg)` requires non-null `seg`, `seg->st == active`,
   `seg->owner_stream == stream_id`, and `seg->segment_gen != 0`.
3. After install:
   - `write_offset = format::WAL_SEGMENT_HEADER_SIZE`
   - `trailer_reserved_bytes = align_up(format::WAL_SEALED_TRAILER_SIZE, lba_size)`
   - `seg_min_lsn = UINT64_MAX`
   - `seg_max_lsn = 0`
4. M04 does not write the header bytes. M06 must write header bytes before the first entry
   reaches disk.

Append geometry helpers:

```text
usable_end = wal_segment_size - trailer_reserved_bytes
can_fit_entry(len) = active_seg != null
                  && len > 0
                  && len <= usable_end - WAL_SEGMENT_HEADER_SIZE
                  && write_offset + len <= usable_end
```

`note_appended(batch_lsn, encoded_len)`:

1. Requires `can_fit_entry(encoded_len)`.
2. Advances `write_offset += encoded_len`.
3. Updates stream-local min/max and active segment min/max.
4. Does not write bytes and does not submit I/O.

`make_sealed_info()`:

1. Requires `active_seg != nullptr`.
2. Requires at least one appended entry (`seg_min_lsn <= seg_max_lsn`).
3. Returns `{active_seg->id, active_seg->segment_gen, seg_min_lsn, seg_max_lsn}`.
4. Does not mutate wal_space state; caller passes the result to
   `wal::alloc_segment(stream_id, sealed_info)` during rotation.

M04 tests may call these helpers to lock geometry, but production append code remains M06.

## 10. State Machines

### 10.1 Segment Lifecycle

```text
UNALLOCATED(index >= alloc_head)
  -- alloc from bump -->
ACTIVE(id=index, gen=1, owner_stream=S)
  -- front stream seals; wal_space records sealed_info -->
SEALED(id, gen, min_lsn, max_lsn)
  -- reclaim_check(max_lsn <= recovery_safe_lsn) -->
FREE(id, next_gen=gen+1)
  -- alloc from free_pool -->
ACTIVE(id, gen+1, owner_stream=S2)
```

Invalid transitions:

1. `ACTIVE -> ACTIVE` without seal/reclaim.
2. `SEALED -> ACTIVE` without `reclaim_check`.
3. `FREE -> SEALED`.
4. Reclaiming the same `{id,gen}` twice.
5. Sealing an old generation after the segment has already been reallocated.

### 10.2 Alloc Request Lifecycle

```text
NEW
  -- sealed_info recorded? -->
SEALED_CONSUMED
  -- segment available -->
COMPLETED(callback with segment_runtime*)
  -- no segment available -->
PENDING
  -- reclaim creates free entry -->
COMPLETED(callback with segment_runtime*)
```

`PENDING` is a request state, not a segment state.

### 10.3 Stream State Lifecycle

```text
NO_ACTIVE
  -- install_segment(active token) -->
ACTIVE_EMPTY(write_offset=HEADER_SIZE)
  -- note_appended -->
ACTIVE_NONEMPTY
  -- make_sealed_info + alloc replacement pending -->
ROTATING(no writes until replacement installed)
  -- install_segment(new token) -->
ACTIVE_EMPTY
```

M04 only defines the CPU state transitions. Header/trailer byte writes and FUA completion
belong to M06.

## 11. Invariants

### 11.1 Segment Pool

1. At any time, a physical segment id is in exactly one of:
   - unallocated suffix `[alloc_head, wal_segment_count)`
   - ACTIVE lease
   - SEALED list
   - FREE pool
2. `alloc_head` is monotonically non-decreasing and never exceeds
   `wal_segment_count`.
3. `free_pool` entries have `next_gen > slots[id.index].segment_gen` or correspond to
   slots reset to FREE after reclaim.
4. `sealed_segments` contains no duplicate `{id,segment_gen}`.
5. No two ACTIVE leases share the same `{id,segment_gen}`.
6. No ACTIVE lease exists for `id.index >= alloc_head`.

### 11.2 Generation

1. First activation of a physical segment uses `segment_gen = 1`.
2. Every reclaim of a sealed generation produces exactly one future free entry with
   `next_gen = old_gen + 1`.
3. `segment_gen` never wraps silently. If `old_gen == UINT32_MAX`, reclaim must fail-fast
   and require recovery/reformat/human handling; wrapping to 0 would make recovery accept
   stale bytes.

### 11.3 Backpressure

1. Production `alloc_segment` never returns fake success.
2. If all segments are active/sealed and none reclaimable, alloc remains pending.
3. Pending alloc does not consume a new LSN by itself; it only blocks WAL rotation inside
   a write pipeline that already has a batch LSN. This matches WP §7.4: already assigned
   batches wait rather than creating holes.
4. Dropping pending requests is forbidden.

### 11.4 Stream Geometry

1. `write_offset` always satisfies:
   ```text
   WAL_SEGMENT_HEADER_SIZE <= write_offset <= usable_end_offset
   ```
2. An entry may cross LBA boundary but must satisfy `write_offset + encoded_len <= usable_end`.
3. `usable_end = wal_segment_size - align_up(WAL_SEALED_TRAILER_SIZE, lba_size)`.
4. Segment header is at offset 0; sealed trailer starts at `usable_end`.
5. M04 does not allow entry bytes to occupy trailer-reserved space.

## 12. Memory Ordering

Most M04 state is single-owner and needs no atomics:

1. `slots`, `alloc_head`, `free_pool`, `sealed_segments`, `pending_allocs` are only
   mutated on `wal_space_sched`'s owner thread.
2. `wal_stream_state` is only mutated on the owning `front_sched` thread.
3. Cross-scheduler transfer happens through PUMP sender queues and callbacks; do not add
   ad hoc locks.

`used_segment_count`:

1. Optional but recommended because RSM §2.6 seal trigger observes WAL pool pressure from
   coord.
2. `wal_space_sched` updates it with `store(memory_order_relaxed)` or
   `fetch_add/sub(memory_order_relaxed)`.
3. `coord_sched` may `load(memory_order_relaxed)` for heuristic seal triggering.
4. It is not a correctness predicate; stale reads only affect when seal is triggered, not
   data visibility or recovery safety.

If implementation later exposes current `sealed_segments` count across schedulers, it must
use the same heuristic-only relaxed model or route through a wal_space sender. Other modules
must not read wal_space owner containers directly.

## 13. Error / Failure Semantics

### 13.1 Construction Errors

Throw `std::invalid_argument` or panic during construction for:

1. `wal_segment_count == 0`
2. `lba_size == 0`
3. `wal_segment_size % lba_size != 0`
4. header + trailer reserved does not leave room for a v1 entry
5. `wal_base_paddr.device_id != 0` in v1

These are runtime configuration / format profile errors, not client request failures.

### 13.2 Request Errors

Fail-fast for:

1. `stream_id` outside configured front stream count, if scheduler is constructed with that
   bound.
2. invalid `sealed_info` id / gen / owner / state.
3. duplicate sealed info.
4. stale generation token.
5. segment generation overflow.

Implementation may report these as `std::logic_error` through the sender exception path or
`panic_inconsistency` depending on local precedent. It must not silently ignore them.

### 13.3 Allocation Exhaustion

Production:

1. No available segment means pending/backpressure.
2. The `alloc_segment` sender remains incomplete until a future `reclaim_check` releases a
   segment.
3. If pending queue capacity is exhausted, fail-fast; do not drop the request.

Testing:

1. A non-sender `try_alloc_segment_for_testing()` may return `nullptr` so tests can assert
   empty-pool behavior without hanging.
2. That helper is not the production contract.

### 13.4 Callback Exceptions

Follow the M03 fix:

1. Catch only scheduler semantic failures before state commit.
2. Once allocation/reclaim state is committed and callback is invoked, callback exceptions
   propagate out of `advance()`.
3. Request memory must still be released if callback throws. Use `std::unique_ptr` or
   equivalent ownership.
4. Do not call both success callback and fail callback for the same request.

## 14. Tests Required For M04 Implementation

M04 implementation should add a focused test target, e.g.
`inconel_test_m04_wal_space_scheduler`.

### 14.1 Segment Geometry

1. `segment_base_paddr` includes `wal_base_paddr`.
2. segment size / lba size validation rejects invalid geometry.
3. `trailer_reserved = align_up(WAL_SEALED_TRAILER_SIZE, lba_size)`.
4. `usable_end` and `can_fit_entry` reject crossing trailer reserved space while allowing
   LBA crossing.

### 14.2 Allocation / Generation

1. First allocations use indices `0..N-1` and `segment_gen == 1`.
2. Each allocated segment has unique `{id,gen}` and correct owner stream.
3. `alloc_head` advances monotonically and stops at `wal_segment_count`.
4. No fake success when pool is empty.

### 14.3 Sealed Reclaim

1. `alloc_segment(stream, sealed_info)` records sealed metadata before allocating
   replacement.
2. `reclaim_check(recovery_safe_lsn < max_lsn)` does not reclaim.
3. `reclaim_check(recovery_safe_lsn >= max_lsn)` moves the segment to free pool.
4. Reallocation of that segment uses `segment_gen + 1`.
5. Duplicate or stale `sealed_info` fails fast.

### 14.4 Pending Backpressure

1. Allocate all segments.
2. Submit another production `alloc_segment` sender and drive `advance()`; callback must not
   run.
3. Reclaim a sealed segment; pending callback runs with reclaimed segment.
4. Multiple pending allocs wake FIFO.
5. A pending request carrying `sealed_info` records it once, not once per wake attempt.

### 14.5 Stream State

1. `install_segment` sets offset to `WAL_SEGMENT_HEADER_SIZE` and resets stream min/max.
2. `note_appended` advances offset and updates min/max.
3. `make_sealed_info` returns the current token and lsn range.
4. Installing a null / wrong owner / non-active / stale segment fails fast.
5. Empty segment seal is rejected unless implementation explicitly models empty seal; 042
   does not require empty seal support.

### 14.6 Sender Facade

1. Static asserts for `compute_sender_type`:
   - `alloc_segment` has one value, `segment_runtime*`.
   - `reclaim_check` has zero values.
2. Runtime smoke through `submit + then + advance`, not only direct test helpers.
3. Callback throw propagation does not masquerade as scheduler failure.

### 14.7 Exclusion Tests

Do not test WAL entry decode/encode in M04 except via existing `format/wal.hh` tests.
Do not introduce mock block device or NVMe FUA tests in M04; those belong to M06.

## 15. 排除范围

M04 explicitly excludes:

1. WAL append bytes, `append_entries_fua`, `prepare_append_entries`, page write plans,
   mock block devices, SPDK/NVMe FUA.
2. Segment header/trailer byte encoding. M04 only defines where they belong.
3. `front_sched` owner implementation, memtable insert, seal active, release gens.
4. `value_alloc_sched` persist/read adapter.
5. `coord` changes beyond future registry singleton placeholder.
6. `write_batch` baseline / production pipeline.
7. `point_get` / read path integration.
8. Recovery WAL scanner and reset-wal-pool boot flow.
9. Runtime builder / public API.

## 16. Conflict Decisions And Manual Judgment

Resolved conflicts:

1. **Old Step 18 append mock path vs current M04 scope**  
   Old Step 18 included `append_entries_fua()` and mock block device decode tests. Current
   migration plan deliberately split that into M06. 042 follows the current plan and keeps
   M04 to metadata/stream state.

2. **Old `alloc_segment()==nullptr` vs formal backpressure**  
   Old Step 18 allowed nullptr as success-path boundary. Formal OV/WP/RSM require pending
   backpressure once a batch has entered the gap-free LSN sequence. 042 chooses production
   pending sender semantics; nullptr is allowed only for test-only nonblocking helpers.

3. **Old `segment_base_paddr` missing `wal_base_paddr`**  
   ODF §3.1 is authoritative. Current M04 helper must include `wal_base_paddr`.

4. **RSM small fixed containers vs capacity requirement**  
   RSM uses `local::queue<...,256>` / `small_vector<...,64>` as sketch. Capacity hard
   requirements override these illustrative sizes. Implementation must use containers sized
   for `wal_segment_count`.

No unresolved design conflict currently requires human judgment. If implementation finds
that pending allocation cannot be represented cleanly with the current PUMP sender model,
that is not grounds to fall back to nullptr; it must be recorded and escalated for manual
decision.

