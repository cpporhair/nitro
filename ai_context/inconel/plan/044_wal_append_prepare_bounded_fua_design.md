# 044 - M06 WAL Append Prepare + Bounded FUA Issue

> 本文对应 `ai_context/inconel/plan/front_wal_development_plan.md` 的 M06。
> 目标不是恢复旧分支代码，而是在当前 `inconel.new` 的 M01-M05 基础上，重新定义 front WAL append 的 owner state、sender surface、segment rotation、bounded FUA issue 和失败边界。

## 1. 范围

M06 覆盖：

- `front` 为一个 `front_fragment` 生成 WAL append plan。
- WAL entry/header/trailer 的 v1 byte layout 与 helper surface。
- entry 跨 LBA/page 的 scatter 写入。
- segment full 时的 trailer FUA、向 M04 `wal_space_sched` 申请新 segment、新 segment header FUA。
- WAL page write 的 bounded FUA issue。
- WAL append/FUA 失败时，batch 不进入 memtable，并把失败返回给上层 M08/M09 release 路径。

M06 不覆盖：

- 全链路 `write_batch` 编排。M08/M09 负责把 `coord::release_batch` / `publish_batch` 串起来。
- Value Area 持久化重写。M07 会处理 value persist/read adapter，并且应顺手处理当前 value sender 的 unbounded `concurrent()` 风险。
- recovery parser 的完整实现。M06 只保证新 WAL bytes 与 ODF/recovery contract 相容。

## 2. 已对照输入

- M01 `039_front_wal_phase_a_carrier_inc055_design.md`
- M02 `040_read_handle_prs_memtable_lookup_design.md`
- M03 `041_coord_scheduler_assign_publish_release_design.md`
- M04 `042_wal_space_scheduler_design.md`
- M05 `043_front_scheduler_memtable_owner_design.md`
- `design_doc/impl/write_path_and_pipeline.md`
- `design_doc/impl/runtime_state_machine.md`
- `design_doc/impl/on_disk_formats.md`
- `design_doc/impl/recovery_and_wal_reclaim.md`
- `design_doc/impl/code_modules.md`
- `design_doc/impl/runtime_memory_and_cache.md`
- 当前代码：
  - `apps/inconel/format/wal.hh`
  - `apps/inconel/wal/scheduler.hh`
  - `apps/inconel/front/scheduler.hh`
  - `apps/inconel/nvme/frame_io.hh`
  - `apps/inconel/memory/frame.hh`
- 旧分支证据：
  - Step 18 WAL space / WAL format contract
  - Step 26P prepare/issue 拆分

## 3. 当前冲突与裁决

### 3.1 `front` 不直接依赖 `wal/scheduler.hh`

M04 把 `wal_stream_state` 暂放在 `apps/inconel/wal/scheduler.hh`，M05 明确指出 M06 必须决定 stream state header ownership。`code_modules.md` 要求 L2 scheduler module 不能互相直接依赖。

M06 裁决：

- 把共享的 segment/stream runtime carrier 从 `wal/scheduler.hh` 拆到中性层，例如 `apps/inconel/core/wal_stream.hh`。
- `wal/scheduler.hh` 继续拥有 segment pool 和 allocation policy。
- `front/scheduler.hh` 只包含中性 stream state，不包含 `wal/scheduler.hh`。
- 跨 scheduler 的组合放在 L3 pipeline/helper，而不是塞回 front owner。

推荐移动到 `core/wal_stream.hh` 的类型：

- `segment_id`
- `wal_segment_state`
- `segment_runtime`
- `sealed_segment_info`
- `segment_geometry`
- `wal_stream_state`
- `segment_base_paddr(...)`

命名可以保留 `inconel::wal` namespace，但文件层级必须避免 `front` include L2 `wal/scheduler.hh`。

### 3.2 `write_wal_fragment` 是 pipeline 概念，不是 front 内部偷偷跨模块

迁移计划把目标写成：

- `apps/inconel/front/sender.hh`: `write_wal_fragment` sender

但 M04/M05/code_modules 的组合约束要求：

- `front` owner 负责 prepare/commit/abort WAL append plan。
- `wal` owner 负责 segment allocation。
- `nvme` owner 负责 frame write。
- L3 pipeline 负责把三者串起来。

M06 裁决：

- `front/sender.hh` 暴露 front-local handles：
  - `prepare_wal_fragment(...)`
  - `install_wal_segment(...)`
  - `commit_wal_plan(...)`
  - `abort_wal_plan(...)`
- L3 helper 暴露概念上的 `write_wal_fragment(...)`，组合 front/wal/nvme。
- M08/M09 写 `write_batch` 时，只调用这个 L3 helper，不直接操作 WAL owner internals。

这样保留了设计文档中的 `write_wal_fragment` 语义，同时不破坏 L2 ownership。

### 3.3 WAL append 不能使用旧分支的整 segment buffer

旧 Step 18 用 `seg_buf_` 持有整段 WAL bytes，便于 mock decode，但不符合当前 runtime memory/frame 模型：

- WAL tail frame owner 应在 `front_sched`。
- 真 NVMe 路径要写 `memory::segmented_page_frame`。
- 热路径不能为了整个 segment 常驻一个大 vector。

M06 裁决：

- front WAL append 以 WAL LBA/page frame 为单位生成 plan。
- 一个 plan 持有本次需要 FUA 的 `segmented_page_frame` 列表。
- entry 可以跨 LBA/page，append 逻辑把 WAL byte parts scatter 到 touched frames。
- 不恢复整 segment contiguous buffer。

### 3.4 WAL state 必须两阶段提交

旧 Step 18/26P 在 prepare 阶段直接推进 append cursor；这在 FUA 失败后继续运行时不安全。若 cursor 已前进但磁盘中间存在坏/缺页，后续成功 batch 可能写在坏洞之后，recovery 扫描到坏 entry 会停在前面，导致后续已 ack batch 丢失。

M06 裁决：

- `prepare_wal_fragment` 只创建 `wal_append_plan`，记录 proposed cursor/min/max，不推进 committed stream cursor。
- 所有 FUA write 成功后，`commit_wal_plan` 才推进 stream cursor。
- 任一 FUA write 失败时，`abort_wal_plan` 丢弃 pending plan，committed cursor 保持不变。
- 后续 retry/新 batch 可以从相同 committed offset 重写页面，覆盖之前可能落盘的 partial bytes。
- 若进程在失败后、重写前崩溃，recovery 只会在 committed prefix 后看到 partial/corrupt entry，并按 batch-complete 规则丢弃，不会存在后续已提交 entry。

这是 M06 的核心正确性要求。

### 3.5 sealed trailer 位置

sealed trailer 位置:044 初版与 ODF §3.4 /
RW §3 / 042 §11.4 冲突且未记录;045 裁决以 ODF 为准,本节为更正记录。

## 4. WAL Byte Layout

M06 保持 ODF v1 layout，不引入 incompatible layout。

### 4.1 Segment Header

Segment 第一个 byte 开始写：

```text
wal_segment_header {
  u32 magic = 0x57414C53
  u32 format_version = 1
  u32 segment_index
  u16 device_id
  u32 stream_id
  u32 segment_gen
  u32 crc
}
```

大小为 26 bytes。CRC 覆盖 header 内除 `crc` 字段外的 bytes。

### 4.2 Entry

每个 WAL entry：

```text
wal_entry_header {
  u32 total_len
  u32 segment_gen
  u64 lsn
  u32 entry_count
  u8  op_type
  u32 key_len
}
```

PUT bytes：

```text
entry_header | value_ref | key_bytes | entry_crc32
```

DELETE bytes：

```text
entry_header | key_bytes | entry_crc32
```

`entry_count` 是整个 batch 的全局 entry count，不是 fragment 内 entry count，也不是当前 entry index。

### 4.3 Sealed Trailer

Segment full/rotation 时写 trailer：

```text
wal_sealed_trailer {
  u32 magic = 0x5345414C
  u32 segment_gen
  u32 write_end
  u64 min_lsn
  u64 max_lsn
  u8  sealed = 1
  u32 crc
}
```

大小为 33 bytes。Trailer 是 reclaim/recovery hint；已有 committed entries 的 durability 依赖每次 entry page FUA，不依赖 trailer 才变 durable。

## 5. Format Helper Surface

当前 `format/wal.hh` 的 contiguous encoder/decoder 与 ODF v1 基本一致，可以保留用于单 entry decode/test。但 M06 append 需要 scatter 到 frame，不应先拼整 segment buffer。

M06 需要补齐以下 helper：

```c++
wal_segment_header make_wal_segment_header(
    uint32_t segment_index,
    uint16_t device_id,
    uint32_t stream_id,
    uint32_t segment_gen,
    uint32_t format_version = SUPERBLOCK_FORMAT_VERSION_V1);

wal_sealed_trailer make_wal_sealed_trailer(
    uint32_t segment_gen,
    uint32_t write_end,
    uint64_t min_lsn,
    uint64_t max_lsn);

struct wal_entry_parts {
    wal_entry_header header;
    std::span<const char> value_ref_bytes; // PUT only
    std::span<const char> key_bytes;
    uint32_t crc;
    uint32_t total_len;
};

wal_entry_encode_status build_wal_put_entry_parts(
    uint32_t segment_gen,
    uint64_t lsn,
    uint32_t entry_count,
    std::string_view key,
    const value_ref& value,
    wal_entry_parts* out);

wal_entry_encode_status build_wal_delete_entry_parts(
    uint32_t segment_gen,
    uint64_t lsn,
    uint32_t entry_count,
    std::string_view key,
    wal_entry_parts* out);
```

细节：

- `build_*_parts` 负责 size/CRC/layout，不知道 frame。
- front append 负责把 parts 逐段 copy 到 WAL frames。
- 现有 `encode_wal_put_entry` / `encode_wal_delete_entry` 可以改成调用 parts helper 后 copy 到 contiguous span。
- `decode_wal_entry` 继续用于 test/recovery parser 的 contiguous window。跨页 decode 由 recovery 层聚合 window 后调用，或后续补 stream decoder。

禁止：

- 不允许在 front append 中手写另一套 WAL header/CRC 规则。
- 不允许用 ad-hoc string 拼 entry。

## 6. Front WAL Runtime State

`front_sched` 增加 WAL owner state，但只拥有本 stream 的 append cursor 和 frame plan；segment pool 仍归 M04 `wal_space_sched`。

推荐结构：

```c++
struct wal_append_config {
    uint32_t max_fua_inflight;
    uint32_t max_pages_per_plan;
};

struct wal_fragment_cursor {
    uint32_t next_fragment_entry; // index into front_fragment.entry_indices
};

struct wal_frame_write {
    memory::pooled_frame_ptr<memory::segmented_page_frame> frame;
    memory::frame_write_desc desc;
};

enum class wal_plan_kind {
    header,
    entries,
    trailer,
};

struct wal_append_plan {
    uint64_t plan_id;
    wal_plan_kind kind;
    uint32_t stream_id;
    segment_id segment;
    uint32_t segment_gen;
    uint32_t start_offset;
    uint32_t end_offset;
    uint64_t min_lsn;
    uint64_t max_lsn;
    wal_fragment_cursor cursor_before;
    wal_fragment_cursor cursor_after;
    bool fragment_done;
    wal_append_config config;
    std::vector<wal_frame_write> writes;

    std::optional<sealed_segment_info> sealed_on_commit;
};
```

实现可用 `small_vector` 或固定容量 vector，只要容量来自 `max_pages_per_plan`，不得随 entry count 无界增长。

`wal_stream_state` 需要区分：

- committed active segment
- committed write offset
- committed min/max LSN in active segment
- optional pending plan id

front owner 同一时间只允许一个 pending WAL plan。front scheduler 本身串行处理 owner mutation；NVMe FUA 在 owner 外执行时，plan 持有被写 frames 的 ownership，防止被复用。

## 7. Sender/Handle Surface

### 7.1 Front-local sender

`apps/inconel/front/sender.hh` 增加 front-local handles：

```c++
auto prepare_wal_fragment(
    front_sched* sched,
    core::front_fragment fragment,
    std::span<const core::canonical_entry> entries,
    wal_fragment_cursor cursor,
    wal_append_config config);

auto install_wal_segment(
    front_sched* sched,
    segment_runtime* segment);

auto commit_wal_plan(
    front_sched* sched,
    uint64_t plan_id);

auto abort_wal_plan(
    front_sched* sched,
    uint64_t plan_id);
```

`prepare_wal_fragment` 返回：

```c++
struct wal_prepare_ready {
    wal_append_plan plan;
};

struct wal_prepare_needs_segment {
    uint32_t stream_id;
    std::optional<sealed_segment_info> sealed;
};

using wal_prepare_result =
    std::variant<wal_prepare_ready, wal_prepare_needs_segment>;
```

含义：

- `ready`：caller 必须 issue `plan.writes`，随后 commit 或 abort。
- `needs_segment`：front 没有可 append active segment，或者刚完成 trailer seal 后需要新 segment；caller 必须通过 M04 `wal_space_sched` 申请 segment，再调用 `install_wal_segment`。
- `prepare` 不得直接调用 wal_space，不得直接调用 nvme。

### 7.2 L3 `write_wal_fragment`

L3 helper 负责组合：

```text
loop until fragment_done:
  front.prepare_wal_fragment
  if needs_segment:
    wal.alloc_segment
    front.install_wal_segment
    continue

  issue plan.writes through nvme::write_frame(..., IO_FLAGS_FUA)
  if all succeed:
    front.commit_wal_plan
  else:
    front.abort_wal_plan
    raise wal_append_error
```

这个 L3 helper 是设计文档里概念上的 `write_wal_fragment`。

M08/M09 集成时：

- `wal_append_error` 在 memtable 之前返回。
- 上层 catch 后调用 `coord::release_batch`。
- 不调用 `front::insert_memtable_entries`。

## 8. Prepare Algorithm

### 8.1 Common validation

`prepare_wal_fragment` 在 owner 线程内做：

- `fragment.entry_count` 必须等于 canonical batch 的全局 entry count。
- 每个 `entry_indices[i]` 必须在 `entries` span 内。
- entry op 必须是 PUT 或 DELETE。
- key length 必须满足 M06 WAL format 上限。
- PUT 必须有 valid `value_ref`。
- DELETE 不引用 Value Area。

失败时返回/抛出 `wal_append_error{reason}`。因为仍在 memtable 前，M08/M09 只 release batch。

### 8.2 Header plan

当 stream 安装了新 active segment，但 header 尚未 committed：

- prepare 生成一个 header plan。
- header 写入 offset 0。
- FUA 成功并 commit 后，committed write offset 变为 `WAL_SEGMENT_HEADER_SIZE`。
- 后续 entry 从 header 后开始。

实现允许把 header 与第一批 entries 合并在同一个 plan 中，但必须满足：

- header bytes 与第一批 entry bytes 在同一批 FUA writes 中。
- FUA 失败时 header/entry 都不 commit。
- committed cursor 仍保持旧状态或新 segment 的 uncommitted header 状态。

为了测试和可读性，M06 首版建议使用显式 header plan。

### 8.3 Entry plan

Entry plan 从 `cursor.next_fragment_entry` 开始，最多准备 `config.max_pages_per_plan` 个 WAL page write。若一个 entry 自身触达的 page 数超过 budget，仍允许该 single entry 形成 plan；这个上限由 `kMaxSupportedWalEntrySize` 保证可控。

每个 entry：

1. 用 `format::build_wal_*_entry_parts` 得到 byte parts 和 `total_len`。
2. 若 `total_len + WAL_SEALED_TRAILER_SIZE` 无法放入一个空 segment，返回 `entry_too_large_for_segment`。
3. 若当前 active segment 剩余空间不足以容纳 entry 和 trailer reserved：
   - 当前 entry 不写入本 plan。
   - prepare 返回 trailer plan 或 `needs_segment` 状态。
4. 把 entry parts 按 current offset scatter 到 WAL frames。
5. 更新 plan 内 proposed offset/min/max/cursor。

Entry 可以跨 LBA/page，但不能跨 segment。

### 8.4 Trailer + rotation

当 active segment 没有空间容纳下一 entry 加 trailer reserved：

1. front prepare 一个 trailer plan。
2. trailer 写入段尾固定 `TRAILER_RESERVED` 区(起始 `usable_end =
   wal_segment_size - TRAILER_RESERVED`);`trailer.write_end` 字段记录最后一条
   committed entry 之后的字节偏移。位置裁决见 045 §4.1。
3. issue trailer page FUA。
4. commit trailer plan：
   - 旧 active segment 在 front 侧变为 sealed/inactive。
   - 返回 `sealed_segment_info`，min/max 只覆盖已经 committed 的 entries。
5. L3 调 M04 `wal_space_sched::alloc_segment(stream_id, sealed_info)`。
6. `front.install_wal_segment(new_segment)`。
7. prepare/issue/commit new segment header plan。
8. 继续当前 entry。

如果 trailer FUA 失败：

- abort trailer plan。
- active segment 保持 committed cursor 不变。
- batch 不进入 memtable。
- 上层 release batch。

Trailer 是 hint；旧 committed entries 在之前 entry plans 的 FUA 成功后已经 durable。Trailer 失败不会让旧 entries 失效，但当前 batch 必须失败，因为本次 WAL append 没有完成。

### 8.5 Segment allocation pending

M04 `alloc_segment` 在无可用 segment 时挂起 pending FIFO，不应返回 null 给生产 sender。

M06 语义：

- `needs_segment` 后，L3 helper 等待 `wal_space_sched::alloc_segment`。
- 等待 segment 不是 WAL failure，不触发 `release_batch`。
- 这会在 WAL phase 自然形成 backpressure，符合 `write_path_and_pipeline.md`。

## 9. Bounded FUA Issue

M06 禁止：

```c++
loop(writes.size()) >> concurrent()
```

必须使用显式上限：

```text
for each plan:
  issue writes through bounded concurrency <= config.max_fua_inflight
```

推荐默认：

- `max_fua_inflight` 来自 runtime/config，首版可设为每 front stream 16 或 32。
- `max_pages_per_plan` 与 `max_fua_inflight` 同阶，避免 prepare 阶段积累大量 frames。

NVMe call：

```c++
nvme::write_frame(runtime_scheduler, frame, desc, nvme::IO_FLAGS_FUA)
```

规则：

- 每个 touched WAL page/frame 都用 FUA。
- 不使用 “普通写一批 + 最后一页 FUA” 的简化模型。
- `write_frame` false 或 exception 都视为 WAL append failure。

## 10. Failure Semantics

### 10.1 Encode/validation failure

发生在 owner prepare 阶段：

- 不生成 memtable mutation。
- 不 commit WAL cursor。
- 上层 release batch。

### 10.2 Segment unavailable

不是 failure：

- `alloc_segment` pending。
- batch LSN hole 已由 M03 assign 创建，但不 release。
- 等待有 segment 后继续 WAL phase。

### 10.3 FUA failure

任一 WAL page FUA false/exception：

- L3 调 `front.abort_wal_plan(plan_id)`。
- `wal_stream_state` committed cursor 不变。
- plan 持有的 dirty frames 释放或标记不可复用。
- 不调用 memtable insert。
- `write_wal_fragment` 返回/抛出 `wal_append_error{device_failure}`。
- M08/M09 catch 后调用 `coord::release_batch`。

如果部分 bytes 已经落盘：

- 它们位于 committed cursor 之后。
- 后续 retry/新 batch 从同一 committed cursor 重写。
- 若 crash 发生在重写前，recovery 在 committed prefix 后看到 partial/corrupt entry，并按 `entry_count` / CRC 规则丢弃，不会影响已 committed prefix。

### 10.4 Commit failure

`commit_wal_plan` 是 front owner 内存状态 mutation，不应失败。若 invariant 破坏，例如 unknown `plan_id`、active segment 不匹配、cursor 不匹配，视为 runtime bug，走 fail-fast。

## 11. Interaction With M05 Memtable

M06 不改变 M05 的 memtable contract：

- `front::insert_memtable_entries(fragment, entries)` 仍只在 WAL phase 成功后调用。
- WAL failure/release path 不进入 memtable。
- Memtable insert failure 不是可 release failure；这是 M05 已定义的 fatal/programming error 类。

M06 测试可以用 fake memtable counter 或 front owner 状态断言 “FUA failure 后没有 memtable insertion”。真正的 release 串接测试留给 M08/M09。

## 12. Test Plan

M06 至少补以下测试。测试可以读/复用旧 Step 18/26P 的行为证据，但 production implementation 不应依赖旧代码。

### 12.1 Format tests

- PUT entry encode/decode round trip。
- DELETE entry encode/decode round trip。
- `entry_count` 在每个 entry 中都是全局 batch entry count。
- CRC 覆盖 header/value_ref/key，不接受 corrupted key/value_ref/header。

### 12.2 Prepare tests

- 新 segment header 写入 offset 0，header FUA 成功后 cursor 从 `WAL_SEGMENT_HEADER_SIZE` 开始。
- PUT fragment 生成 decodable WAL bytes，value_ref 与 M01/M07 carrier 一致。
- DELETE-only fragment 不要求 Value Area bytes，但必须生成 WAL DELETE entry。
- 一个 entry 跨 LBA/page，decode 后 key/value_ref/lsn/entry_count 正确。
- plan page 数不超过 `max_pages_per_plan`，单 oversized-but-supported entry 例外要显式断言。

### 12.3 Rotation tests

- active segment 空间不足时：
  - 当前 entry 不跨 segment。
  - 旧 segment 写 sealed trailer。
  - trailer FUA 后向 M04 申请新 segment。
  - 新 segment 写 header。
  - 当前 entry 写入新 segment。
- `sealed_segment_info.min_lsn/max_lsn` 只覆盖旧 segment 已 committed entries。
- segment physical base 使用 M04 修正后的 `wal_base_paddr + segment_index * segment_lbas`，不得退回旧 bug。

### 12.4 Bounded issue tests

- fake NVMe 统计最大 simultaneous writes，永远不超过 `max_fua_inflight`。
- 多页 plan 中任一 write false，整体 sender 失败。
- write false 后必须调用 abort，不能调用 commit。

### 12.5 Failure/no-memtable tests

- FUA failure 后：
  - `wal_stream_state` committed offset 不变。
  - front memtable entry count 不变。
  - 后续 retry 从相同 offset 重写，并可成功 decode。
- encode validation failure 后不触发 NVMe write，也不触发 memtable。
- segment unavailable pending 时不 release、不 memtable，只 pending。

## 13. Implementation Order

建议按以下顺序实现 M06：

1. 拆 `core/wal_stream.hh`，让 `wal/scheduler.hh` 与 `front/scheduler.hh` 共享中性 carrier。
2. 扩展 `format/wal.hh` helper surface，保持现有 contiguous encode/decode 测试通过。
3. 在 `front_sched` 增加 WAL stream state、pending plan table、frame budget。
4. 实现 header/entry/trailer prepare。
5. 实现 commit/abort。
6. 实现 L3 bounded `write_wal_fragment` helper。
7. 补 fake NVMe/bounded/failure tests。

## 14. 相邻事项提醒

- M07 紧接着会碰 Value Area 持久化 sender。当前 `apps/inconel/value/sender.hh` 仍有 unbounded `concurrent()`，最好在 M07 顺手改为显式 bounded issue，否则 M06 只修 WAL 会留下同类背压问题。
- M08/M09 集成 `write_batch` 时，必须把 M06 的 `wal_append_error` 接到 M03 `coord::release_batch`。M06 本身不应偷偷调用 coord。
- 若后续要优化 header 与第一批 entries 合并 FUA，必须保留两阶段 commit invariant；不能为了少一个 plan 又在 prepare 阶段推进 stream cursor。
