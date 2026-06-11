# Inconel 详细设计：写路径与 Pipeline

> 依据：`ai_context/inconel/design_overview.md`（唯一概要规范）
>
> 本文细化前台写入的端到端 PUMP pipeline、canonicalization 实现、value 分配流程、WAL 换段交互、多 batch 并发 in-flight 交互、batch 不跨 seal 的协议保证以及异常处理与 orphan 回收。
>
> 与 `runtime_state_machine.md` 的分工：该文档定义各 scheduler 的状态/请求/handle 逻辑（"每个 scheduler 做什么"），本文档定义写路径的 pipeline 编排（"各 scheduler 如何串成一条完整的写链路"）。

## 1. 端到端写路径总览

概要 §7.1 定义了标准写路径。本章把它展开为 PUMP pipeline 形态，标注每一步的执行域和数据流。

```text
client batch
      │
      ▼
 ┌────────────────────────────────────────────────────────────────┐
 │  coord_sched                                                   │
 │  ① canonicalize → ② assign_lsn                                 │
 └────────────────────────┬───────────────────────────────────────┘
                          │
                          ▼
 ┌────────────────────────────────────────────────────────────────┐
 │  value_alloc_sched（leader-follower）                           │
 │  ③ 合并并发 batch 的 PUT entries                                │
 │  ④ 分配 value slots + 填充 DMA frame                           │
 │  ⑤ FUA 写到本核 nvme_sched → value durable                     │
 │  ⑥ 返回 value_ref 给各 batch                                   │
 └────────────────────────┬───────────────────────────────────────┘
                          │  route → per-owner fragment（已携带 value_ref）
          ┌───────────────┼───────────────┐
          ▼               ▼               ▼
  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
  │ front_sched 0│ │ front_sched 1│ │ front_sched N│
  │ ⑦ wal append │ │ ⑦ wal append │ │ ⑦ wal append │
  └──────┬───────┘ └──────┬───────┘ └──────┬───────┘
         │                │                │
         └────── reduce(all WAL ok) ───────┘
                    │
                    ▼
  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
  │ front_sched 0│ │ front_sched 1│ │ front_sched N│
  │ ⑧ mem insert │ │ ⑧ mem insert │ │ ⑧ mem insert │
  └──────┬───────┘ └──────┬───────┘ └──────┬───────┘
         │                │                │
         └────── reduce(all mem ok) ───────┘
                    │
                    ▼
 ┌────────────────────────────────────────────────────────────────┐
 │  coord_sched                                                   │
 │  ⑨ publish_batch(batch_lsn) → ⑩ ACK                           │
 └────────────────────────────────────────────────────────────────┘
```

## 2. PUMP Sender Chain

### 2.1 顶层 Pipeline

```text
write_batch(client_req)
  = on(coord_sched.as_task(),
        canonicalize_and_assign_lsn(client_req))
                                            // → (batch_ctx)
  >> push_result_to_context()               // batch_ctx 入 context 栈
  // ── Phase A: value durable（value_alloc_sched leader-follower）──
  >> on(value_alloc_sched.as_task(),
        persist_put_values(batch_ctx))      // 合并 → 分配 → 填充 DMA → FUA
                                            // FUA 完成后返回 value_refs
  // ── Phase B: route + all-WAL barrier（fan-out 到 front_scheds）──
  >> get_context<batch_ctx>()
  >> then([](batch_ctx& bc) {
         return route_and_build_fragments(bc);  // 按 owner 分组，entries 已携带 value_ref
     })
  >> for_each(fragments)
     >> concurrent(front_sched_count)
     >> flat_map([](fragment&& frag) {
            auto* fs = front_sched_list[frag.owner];
            return fs->write_wal_fragment(std::move(frag));
        })
     >> reduce()
  // ── Phase C: all-memtable barrier ──
  >> get_context<batch_ctx>()
  >> for_each(fragments)
     >> concurrent(front_sched_count)
     >> flat_map([](fragment&& frag) {
            auto* fs = front_sched_list[frag.owner];
            return fs->write_memtable_fragment(std::move(frag));
        })
     >> reduce()
  // ── Phase D: publish + ACK ──
  >> get_context<batch_ctx>()
  >> then([](batch_ctx& bc) {
         return coord_sched->publish_batch(bc.batch_lsn);
     })
  >> flat()
  >> pop_context()
  >> then([]() { /* ACK */ })
```

### 2.2 `batch_ctx` 结构

```cpp
struct canonical_entry {
    uint8_t op_type;                                 // PUT / DELETE
    std::string key;
    std::string value;                               // DELETE 时为空
    value_ref allocated_vr;                          // Phase A 后填入（PUT 时有效）
};

struct fragment {
    uint32_t owner;                                  // 目标 front_sched index
    uint64_t batch_lsn;
    uint32_t entry_count;                            // 整个 batch 的 canonical entry 总数
    small_vector<canonical_entry*, 32> entries;       // 指向 batch_ctx 中的 entries
};

struct batch_ctx {
    uint64_t batch_lsn;
    uint32_t entry_count;
    small_vector<canonical_entry, 64> canonical_entries;  // canonicalization 产出
    small_vector<fragment, 8> fragments;                  // route 后按 owner 分组
};
```

### 2.3 Front Sched 内部 Pipeline

value 已在 Phase A 中 durable。front_sched 的工作被拆成两个独立 sender：

```text
write_wal_fragment(frag)
  = on(front_sched.as_task(),
        append_wal_entries_fua(frag))       // WAL 编码 + FUA（WAL durable point）

write_memtable_fragment(frag)
  = on(front_sched.as_task(),
        insert_memtable_entries(frag))      // CPU，memtable insert
```

冻结约束：

1. `write_memtable_fragment` 只能在该 batch 的 all-WAL reduce 成功后才启动。
2. 因此 `value` / `WAL` 失败都仍然发生在 memtable phase 之前，可以走 `release_batch(batch_lsn)`。
3. 一旦进入 `write_memtable_fragment`，该 batch 的失败语义就不再是 release，而是 fatal。

## 3. Canonicalization 算法

### 3.1 输入与输出

```text
输入：client_batch = [(op, key, value?), ...] 按客户端提交顺序
输出：canonical_entries = [(op, key, value?), ...] 每个 key 最多一条
```

canonicalization 在 `coord_sched` 内先执行，且先于 `batch_lsn` 分配。只有 canonicalization / 参数校验成功的 batch，才会继续消耗 gap-free `batch_lsn` 并进入 durable path。

### 3.2 折叠规则

概要 §7.1 规则 6：last-op-wins。

```text
canonicalize(client_batch):
    // key → 最后一次出现的操作
    last_op: flat_hash_map<string_view, size_t>  // key → 在 client_batch 中的 index

    for i = 0; i < client_batch.size(); i++:
        last_op[client_batch[i].key] = i

    canonical_entries = []
    // 按 index 升序输出（保持稳定的 key 首次出现顺序，用于路由确定性）
    sorted_indices = sort(last_op.values())
    for idx in sorted_indices:
        op = client_batch[idx]
        canonical_entries.push(canonical_entry {
            op_type = op.op,
            key     = op.key,
            value   = op.value,   // DELETE 时为空
        })

    return canonical_entries
```

**`last_op` 就是 batch cache**：canonicalization 产出的 `last_op` map 天然就是概要 §8.1 定义的 batch cache——它是 batch 内每个 key 的 canonical final image，按 key 索引。v1 只有纯 PUT/DELETE，没有 batch 内 read-your-own-writes 场景，所以 `last_op` 用完即弃，不需要放进 `batch_ctx`。未来若支持 MERGE/INCREMENT（需要先读再写同一 key），只需把 `last_op` 保留在 `batch_ctx` 中并传给读路径即可。

### 3.3 复杂操作折叠

概要 §7.1 规则 3-5：v1 只支持 PUT / DELETE。MERGE / INCREMENT 必须在进入 durable path 之前折叠：

```text
同一 key 的操作序列折叠：
  [PUT(v1), PUT(v2)]           → PUT(v2)         // last-op-wins
  [PUT(v1), DELETE]            → DELETE           // last-op-wins
  [DELETE, PUT(v2)]            → PUT(v2)          // last-op-wins
  [DELETE, DELETE]             → DELETE
  [PUT(v1), INCR(3)]          → PUT(v1 + 3)      // 上层折叠
  [INCR(1), INCR(2), PUT(v3)] → PUT(v3)          // last-op-wins
```

关键约束：折叠只依赖 batch 内操作序列，**不读取** DB / memtable / tree 状态（概要 §7.1 规则 5）。

### 3.4 entry_count 语义

`entry_count` 是 canonical records 总数（概要 §7.1 规则 7），不是原始操作数。它被写入每条 WAL entry 的 header 中，用于 recovery 判定 batch 完整性。

```text
entry_count = canonical_entries.size()
```

同一 batch 的所有 WAL entries（分散在不同 segments / front_scheds 上）都携带相同的 `entry_count`。

## 4. 路由与 Fragment 构造

### 4.1 路由规则

```text
route(canonical_entries, front_sched_count):
    fragments: map<uint32_t, fragment>  // owner → fragment

    for entry in canonical_entries:
        owner = key_hash(entry.key) % front_sched_count
        if owner not in fragments:
            fragments[owner] = fragment { owner, batch_lsn, entry_count, [] }
        fragments[owner].entries.push(entry)

    return fragments.values()
```

### 4.2 空 Fragment

如果某个 front_sched 没有分到任何 entry，它就不出现在 fragments 中。fan-out 只投递到有数据的 front_scheds。

### 4.3 Fragment 的 entry_count 字段

每个 fragment 携带的 `entry_count` 是**整个 batch** 的 canonical record 总数，不是本 fragment 的条数。这是因为 recovery 需要知道一个 `batch_lsn` 全局有多少条 record 才能判定完整性。

## 5. Value 分配流程

### 5.1 Value Area 分配模型

Value Area 从 `data_area_end_paddr` 向低地址分配（概要 §10.4）。Value 分配与持久化通过单个 `value_alloc_sched` 集中管理（leader-follower 模式）。`value_alloc_sched` 持有分配元数据（bump head、per-class pools）、DMA open frames，并通过本核 `nvme_sched` 提交 value FUA 写入。

**leader-follower 模式**：`value_alloc_sched` 的 `advance()` 合并当前并发到达的多个 batch 的 PUT entries。leader 统一执行：分配 slots → 填充 DMA frame → FUA 写到本核 nvme_sched。FUA 完成后所有 batch 拿到 durable `value_ref`，再各自 fan-out 到 front_scheds 先写 WAL、all-WAL reduce 成功后再进入 memtable phase。

**分层原则**：分配器分为两层——**placement policy**（决定"值放在哪"）和 **write method**（决定"怎么写进去"）。两层独立：placement policy 不依赖具体设备能力；write method 根据设备能力选择最优路径。v1 的 page-based NVMe I/O 只是 write method 的一种 realization。

### 5.2 Placement Policy 配置

```cpp
struct value_placement_config {
    // 当 Data Area 已使用比例超过此水位时，开始复用 sub-LBA 空洞
    // 低于此水位时，只使用 fresh page（bump + whole_page_pool）
    // 默认 0.80（即 80% 使用率后开始消费空洞，为 SSD GC 留余量）
    float hole_reuse_watermark = 0.80f;
};
```

此配置位于 `value_alloc_sched` 上。水位计算由 `value_alloc_sched` 内部完成（bump head 和 tree_alloc_head 均为其自有状态，无需原子操作）。

### 5.3 分配器结构

Value 分配与持久化状态集中在 `value_alloc_sched` 上。完整的 owner state、请求类型和 handle 逻辑见 `runtime_state_machine.md` §6。

`value_alloc_sched` 持有：分配元数据（bump head、per-class pools）、DMA open frames、以及通过本核 `nvme_sched` 的 NVMe 写入能力。front_sched 不参与 value 分配和持久化。

### 5.4 分配与持久化流程

在 `value_alloc_sched` 的 leader-follower advance() 中：

```text
persist_put_values(batch_ctx):
    // leader 合并当前并发 batch 的所有 PUT entries

    for entry in all_put_entries:
        class_idx = find_min_class(sizeof(value_object_header) + entry.value_len)

        // ── open page 快速路径 ──
        if open_frame[class_idx] && open_frame[class_idx]->free_count > 0:
            vr = alloc_slot_from_open_frame(open_frame[class_idx], class_idx, entry.value_len)
        else:
            // ── 换页：从 per-class pools / bump 分配新页 ──
            resp = alloc_page(class_idx)     // 内部操作，不跨 sched
            open_frame[class_idx] = prepare_frame(resp)
            vr = alloc_slot_from_open_frame(open_frame[class_idx], class_idx, entry.value_len)

        entry.allocated_vr = vr
        write_value_object(vr, header, body, open_frame[class_idx])  // memcpy 到 dma_buf

    // 所有 PUT entries 填充完毕，提交 dirty frames + FUA 到本核 nvme_sched
    submit_dirty_frames_fua()
    // FUA 完成 → value durable → 各 batch 拿到 value_ref，继续 fan-out
```

### 5.5 value_alloc_sched 的并发安全

`value_alloc_sched` 是单线程 scheduler，内部状态（bump head、per-class pools、dirty_pages、open frames）无需原子操作或锁。leader-follower 合并发生在 `advance()` 中（drain 请求队列时自然聚合）。

### 5.6 Value 写入（Write Method 层 — v1 Page-Based Realization）

> 统一 frame 模型见 `runtime_memory_and_cache.md` §5。

Write method 操作的是 `value_page_frame` 的 `dma_buf`，不再使用独立的 `slab_page_buf`。

```text
write_value_object(vr, header, body, open_frame):
    // open_frame 是当前 slab.open_frame（dirty_append 或 dirty_hole_fill）

    if class_size >= lba_size && class_size % lba_size == 0:
        // ── LBA-aligned / multi-LBA class ──
        // open_frame->dma_buf 是整个 extent 的 DMA buffer
        memcpy(open_frame->dma_buf, &header, sizeof(header))
        memcpy(open_frame->dma_buf + sizeof(header), body, body_len)
        // 写回时机同下方 §5.7

    else:
        // ── sub-LBA class ──
        // open_frame->dma_buf 是一个 LBA page 的 DMA buffer
        memcpy(open_frame->dma_buf + vr.byte_offset, &header, sizeof(header))
        memcpy(open_frame->dma_buf + vr.byte_offset + sizeof(header), body, body_len)
        // 写回时机同下方 §5.7
```

**关键简化**：dirty_append 和 dirty_hole_fill 使用完全相同的 memcpy 路径。区别只在 frame 的来源：
- `dirty_append`：fresh DMA page，清零后顺序填充
- `dirty_hole_fill`：已有 resident page image（来自 readonly_frame_cache 或 NVMe read），继续填充空洞

两者的 write method 都是"memcpy 到 dma_buf + 后续写回"，不需要 slot_source 分支。

### 5.7 Dirty Frame 写回

在 `value_alloc_sched` 的 `persist_put_values()` 末尾统一提交：

1. 收集本轮所有写入过的 dirty value frames
2. 提交到本核 nvme_sched（v1 当前方案：每个相关 write command 都带 FUA）
3. 所有 FUA 完成后：
   - `free_count == 0` 的 frame → `clean_readonly`，直接留在 readonly cache；**不**回 free pool，也**不**调用 `return_page`
   - 仍有空 slot 的 frame → 保留为 open frame，下一轮继续填充

value page 的 FUA completion 是整个写路径的**第一个 durable point**。之后各 batch 才 fan-out 到 front_scheds 写 WAL。

### 5.8 未来扩展说明

v1 的 write method 层只有 page-based NVMe I/O（通过 `nvme_sched`），并采用**每条相关 write command 都带 FUA**的保守实现。

**非 v1 future capability**：未来若 PUMP / lower I/O 层支持把一批普通写和最终 `NVMe FLUSH` 组合成明确的 durable barrier，可在不改 placement policy、frame 状态机、`value_ref` 语义或 recovery 规则的前提下，把当前“每页 FUA”演进为“write + flush”模式。具体 sender 粒度和 submit 路径留待未来接口设计确定。

## 6. Fragment 内两阶段处理

value 已在 §5.4 中由 `value_alloc_sched` 完成 durable。fragment 到达 front_sched 时，每条 PUT entry 的 `allocated_vr` 已填入。front-side 剩余工作被拆成 batch-global barrier 约束下的两步：先 WAL，后 memtable。

### 6.1 Phase 1：WAL 批量追加 + FUA

```text
append_wal_entries_fua(frag):
    // ── 1a. 确保有 active segment ──
    if wal.active_seg == nullptr:
        wal.active_seg = wal_space_sched->alloc_segment(stream_id)
        write_segment_header(wal.active_seg)
        wal.write_offset = HEADER_SIZE

    // ── 1b. 逐条编码到 tail_frame->dma_buf ──
    // tail_frame 是 page_frame { dom = wal_page, st = dirty_append }
    wal_pages_to_write = []
    for entry in frag.entries:
        encoded = encode_wal_entry(entry)   // PUT 编码 value_ref，不编码 value bytes

        // 检查是否需要换段
        if wal.write_offset + encoded.len > segment_usable_size:
            flush_current_tail_frame(wal_pages_to_write)
            seal_current_segment()
            wal.active_seg = wal_space_sched->alloc_segment(stream_id)
            write_segment_header(wal.active_seg)
            wal.write_offset = HEADER_SIZE
            memset(wal.tail_frame->dma_buf, 0, LBA_SIZE)

        append_to_wal_frames(encoded, wal_pages_to_write)

    // ── 1c. 提交 WAL pages 到本核 nvme_sched ──
    // v1 当前方案：每个页写 command 都带 FUA；不使用“前几页普通写、最后一页 FUA”。
    for page in wal_pages_to_write:
        nvme_sched->submit_write(page.paddr, page.buf, lba_size,
                                 io_flags = SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS)

    // 所有页写的 FUA 完成 → WAL durable
```

PUMP pipeline 形态：

```text
append_wal_entries_fua(frag)
  = on(front_sched.as_task(),
        encode_wal_entries(frag))            // CPU: WAL 编码
  >> submit_wal_pages_fua(...)               // NVMe: WAL pages（每页 FUA）
```

### 6.2 Phase 2：Memtable 批量插入（CPU only）

```text
insert_memtable_entries(frag):
    for entry in frag.entries:
        // Probe-then-allocate for key: 避免同 gen 重复 key 浪费 arena
        string_view incoming_key{entry.key}
        it = active->table.find(incoming_key)
        if (it == active->table.end()):
            string_view arena_key =
                active->kv_arena.allocate(entry.key.data(), entry.key.size())
            it = active->table.try_emplace(arena_key).first

        if entry.op_type == PUT:
            it->second.push_back(memtable_entry {
                data_ver = frag.batch_lsn,
                k        = memtable_entry::kind::value,
                vh       = value_handle {
                    durable = entry.allocated_vr,
                },
            })
        else:
            it->second.push_back(memtable_entry {
                data_ver = frag.batch_lsn,
                k        = memtable_entry::kind::tombstone,
                vh       = {},
            })

    active->max_lsn = max(active->max_lsn, frag.batch_lsn)
    active->min_lsn = min(active->min_lsn, frag.batch_lsn)
```

## 7. WAL 换段交互

### 7.1 触发时机

Phase 2 中，如果当前 entry 放不进 active segment 的剩余空间，触发换段。

### 7.2 换段 Pipeline

换段需要和 `wal_space_sched` 交互，这是一次跨 scheduler 调用：

```text
switch_segment()
  = on(front_sched.as_task(), seal_current_segment())
  >> flat_map([stream_id]() {
         return wal_space_sched->alloc_segment(stream_id);
     })                                     // 跨域：切到 wal_space_sched
  >> on(front_sched.as_task(),
        install_new_segment(new_seg))       // 切回 front_sched
```

### 7.3 换段期间的 Pipeline 阻塞

换段是**同步阻塞**的（在 pipeline 意义上）：当前 fragment 的后续 entries 必须等待新 segment 就绪。

这是可接受的：
1. 换段频率低（每 segment_size / avg_entry_size 次追加才换一次）
2. `wal_space_sched` 的 alloc 操作是轻量的（dequeue from free pool 或 bump head）
3. 换段开销主要是 seal trailer 写入（一次非 FUA write）

front WAL append 按 plan 粒度串行(见 044/045)。同一 front 上不同 batch
的 entries 可能按 plan 交错出现在同一 segment 内;这不改变 recovery 契约
——重组只按 `lsn + entry_count`(概要 §11.2 约束 4),段内顺序不承载语义。

### 7.4 Backpressure

如果 `wal_space_sched` 无法分配新 segment（free pool 空、alloc head 到顶）：

```text
wal_space_sched 将 alloc 请求放入 pending_alloc_queue
→ front_sched 的 write_wal_fragment pipeline 挂起（sender 未完成）
→ coord_sched 的整个 write_batch pipeline 在 WAL reduce() 处等待该 front
→ 该 batch 不会 publish（因为 reduce 未完成）
→ 后续 batch 的 assign_lsn 不受影响（coord_sched 可以继续分配）
→ 但后续 batch 如果也路由到同一个 front_sched，会排在队列里等待

恢复：
→ flush round 完成 → WAL segments 可回收
→ wal_space_sched 回收 segments → 唤醒 pending_alloc_queue
→ front_sched 的 pipeline 恢复
```

概要 §11.4 规则 5：backpressure 不能留下永久 LSN hole。已分配 `batch_lsn` 的 batch 只能等待或触发运行时终止，不能丢弃。

## 8. 多 Batch 并发 In-Flight

### 8.1 并发模型

```text
时间线（coord_sched 视角）：

  t0: canonicalize_and_assign_lsn(batch_A) → lsn=1, dispatch fan-out
  t1: canonicalize_and_assign_lsn(batch_B) → lsn=2, dispatch fan-out
  t2: canonicalize_and_assign_lsn(batch_C) → lsn=3, dispatch fan-out
  ...
  t5: batch_A 的 all-WAL reduce 完成 → dispatch memtable phase
  t6: batch_C 的 all-WAL reduce 完成 → dispatch memtable phase
  t7: batch_A 的 all-memtable reduce 完成 → publish_batch(1)
  t8: batch_C 的 all-memtable reduce 完成 → publish_batch(3)  // C 先于 B 完成
  t9: batch_B 的 all-memtable reduce 完成 → publish_batch(2)
```

`coord_sched` 是单线程的，因此成功进入 durable path 的 batch 的 `assign_lsn` 是严格顺序的。但 fan-out 之后，各 batch 在 front_scheds 上的执行可以交错。

### 8.2 Front Sched 队列内交错

同一个 front_sched 可能同时收到 batch_A 和 batch_B 的 `write_wal_fragment` / `write_memtable_fragment`。但因为 front_sched 是单线程的：

```text
front_sched[0] 队列：
  [write_wal_fragment(batch_A, frag_0)]
  → [write_wal_fragment(batch_B, frag_0)]
  → [write_memtable_fragment(batch_A, frag_0)]
  → ...

执行顺序：
  每个消息都是原子的：
  - `write_wal_fragment` 内部只做 WAL
  - `write_memtable_fragment` 内部只做 memtable
```

**不变量**：同一 front_sched 上，单个 sender（`write_wal_fragment` 或 `write_memtable_fragment`）不会被另一条消息插入中间。

### 8.3 WAL 交错

不同 batch 的 WAL entries 在同一 segment 中是**顺序排列**的（不交错），因为同一 front_sched 顺序处理 fragments。

但同一 batch 的 entries 可以散落在不同 front_scheds 的不同 segments 中（概要 §11.2 约束 4）。Recovery 按 `lsn + entry_count` 重组，不依赖 segment 内连续性。

### 8.4 durable_lsn 推进

publish_batch / release_batch 到达 coord_sched 的顺序可能乱序（batch_C 先于 batch_B 完成）。ready_bitmap 处理这种乱序：

```text
state: durable_lsn = 0
  publish(1) → ready[1]=1, advance: durable_lsn = 1
  publish(3) → ready[3]=1, advance: durable_lsn = 1 (2 还没 ready)
  publish(2) → ready[2]=1, advance: durable_lsn = 3 (1,2,3 连续)
```

如果中间某个 batch 在 value/WAL phase 失败，则它走 `release_batch(X)`，同样会把 `ready[X]` 置 1，从而填平前缀而不是留下永久 hole。

### 8.5 最大 In-Flight 限制

`ready_bitmap` 有固定窗口 `WINDOW_SIZE`（如 65536）。如果 in-flight batch 数接近此窗口，新 batch 的 assign_lsn 应等待旧 batch publish。

```text
if next_lsn - durable_lsn >= WINDOW_SIZE:
    // 前台写反压：暂不分配新 lsn
    // 等待旧 batch publish 后恢复
```

这是少见的极端情况，通常意味着某些 front_sched 严重滞后。

## 9. Batch 不跨 Seal 边界

### 9.1 不变量

概要 §7.1 补充不变量：同一 batch 的所有 memtable inserts 必须全部落在同一套 active memtable gens 上。不允许一部分 entries 落进 pre-seal 的 `A*`，另一部分落进 post-seal 的 `N*`。

### 9.2 保证机制

保证来自两层顺序性：

**层 1：coord_sched 的单线程顺序**

```text
coord_sched 队列中的操作：
  ... write_batch(A).WAL fan-out ... write_batch(A).memtable fan-out ... write_batch(B)... seal_round ...

场景 1: batch_X 的 WAL / memtable 两轮 fan-out 都在 seal_round 之前入队
  → 各 front_sched 先收到 `write_wal_fragment(X)` 与 `write_memtable_fragment(X)`，再收到 seal_active
  → X 的所有 entries 写入 pre-seal 的 A*

场景 2: batch_Y 的 WAL / memtable 两轮 fan-out 都在 seal_round 之后入队
  → 各 front_sched 先收到 seal_active，再收到该 batch 的两轮 fragment
  → Y 的所有 entries 写入 post-seal 的 N*
```

**层 2：每个 front_sched 的队列顺序**

同一 front_sched 上，消息按入队顺序执行。所以如果 batch_X 的 `write_memtable_fragment` 在 seal_active 之前入队到某个 front_sched，那么它一定在该 front_sched 执行 seal_active 之前完成。

### 9.3 Seal 发起约束

Seal 发起必须由 `coord_sched` 发起（不能由 front_sched 自行决定），因为只有 coord_sched 能保证"不会把 seal_active 插到同一 batch 的 WAL phase 和 memtable phase 之间"。

```text
seal_round()
  = on(coord_sched, close_publish_gate())
  // 此时 coord_sched 不再接受新的 write_batch fan-out
  // 但已经投递的 fan-out 消息会继续在各 front_sched 上执行
  >> for_each(front_sched_list)
     >> concurrent(N)
     >> flat_map([](auto* fs) { return fs->seal_active(); })
     >> reduce()
  >> on(coord_sched, build_prs1_and_install_cat1())
  >> on(coord_sched, open_publish_gate())
```

### 9.4 Seal 与 In-Flight Writes 的时序

```text
时间线：

  coord_sched:
    ┌─ fan-out(batch_X) ─┐
    │                     │ → front_sched 队列: [write_frag(X)]
    ├─ close_gate ────────┤
    │                     │
    ├─ fan-out seal_active ┤ → front_sched 队列: [write_frag(X), seal_active]
    │                     │
    ...

  front_sched:
    执行 write_frag(X)    → X 写入 A*
    执行 seal_active      → A* 变成 F*, 安装 N*
    后续 write_frag(Y)    → Y 写入 N*
```

**关键观察**：close_gate 只影响 publish，不影响 fan-out 投递。seal_active 投递到 front_sched 队列的时间点，严格在所有已完成 fan-out 之后。

### 9.5 publish_gate 与 seal 的配合

```text
场景：batch_X fan-out 完成，reduce 完成，准备 publish。
     同时 seal_round 刚 close_gate。

  coord_sched 队列:
    [..., publish_batch(X), close_gate, seal_active fan-out, ...]

  因为 coord_sched 单线程，publish_batch(X) 在 close_gate 之前执行：
    → X 正常 publish 到旧 CAT0

  或者 publish_batch(X) 在 close_gate 之后到达：
    → gate 关闭，gate.pending_advance 记录 X
    → seal 完成后 open_gate → CAT1 安装
    → X 的 lsn 在 CAT1 上继续 publish
```

无论哪种情况，X 都不会丢失发布机会。

## 10. 异常处理

### 10.1 异常分类

写路径按 §2.1 的三阶段划分异常：

| 异常类型 | 发生阶段 | 发生位置 | 影响范围 |
|---------|---------|---------|---------|
| Value 分配失败（空间不足） | Phase A | `value_alloc_sched` | 该 batch（fan-out 之前） |
| Value DMA 填充 / FUA 写失败 | Phase A | `value_alloc_sched` | 该 batch（fan-out 之前） |
| WAL append 失败 | Phase B | `front_sched` | 该 fragment → 该 batch |
| WAL 换段失败（backpressure） | Phase B | `front_sched` | 该 fragment（暂停） |
| Memtable insert 失败 | Phase C | `front_sched` | 该 batch（fatal） |
| Data Area 空间不足 | Phase A | `value_alloc_sched` | 该 batch + 后续 batch |

### 10.2 Phase A 失败 → `release_batch(batch_lsn)` + 返回错误

`value_alloc_sched` 的 `persist_put_values` 失败时，fan-out 尚未发生：

```text
1. persist_put_values sender 产生异常
2. coord_sched 调用 release_batch(batch_lsn)
3. 客户端收到错误
4. 后续 batch 可以继续推进
```

此时可能出现 **pre-WAL orphan values**：如果某些 value page 已先 durable、后续 value write 失败，它们没有对应 WAL/memtable 引用，留给 recovery 按 orphan 清理。DMA frame 中未 durable 的 slot 仍可重用。

### 10.3 Phase B 失败 → `release_batch(batch_lsn)` + 返回错误 + 可能 Orphan

value 已在 Phase A durable。某个 `front_sched` 的 WAL FUA 失败：

```text
1. 该 fragment 的 sender 产生异常
2. 顶层 all-WAL reduce() 收到异常
3. coord_sched 调用 release_batch(batch_lsn)
4. memtable phase 根本不会启动
5. 客户端收到错误
```

此时 Phase A 已为整个 batch 的所有 PUT entries 写入了 durable value objects；同时可能已有部分 WAL fragment durable，但因为 all-WAL barrier 失败，整个 batch 仍视为未完成。对应的 value objects / partial WAL 都交给 recovery 清理。

### 10.4 Phase C 失败 → 运行时终止

只有进入 memtable phase 后失败，运行时才必须主动崩溃：

```text
1. all-WAL barrier 已成功
2. 某个 front_sched 的 insert_memtable_entries 失败
3. batch 不再允许 release
4. 运行时终止，交给 recovery 收敛
```

原因：此时一整批 WAL 已 durable，但 runtime memtable 状态不完整；继续服务会让 live runtime 与 recovery 结果分叉。

### 10.5 `release_batch` 对 durable_lsn 的影响

`release_batch(X)` 与 `publish_batch(X)` 一样，都会把 `ready_bitmap[X]` 置 1，用来补齐连续前缀；区别只是：

1. `publish_batch`：该 batch 的数据变为可见，客户端 ACK
2. `release_batch`：该 batch 不产生任何可见数据，客户端收到错误

例子：

```text
state: durable_lsn = 9
  release(10) → ready[10]=1, durable_lsn = 10
  publish(11) → ready[11]=1, durable_lsn = 11
```

因此 v1 仍保持“连续前缀推进”，只是前缀中的某些 batch_lsn 槽位可以是 released-empty slot，而不是 committed batch。

### 10.6 Orphan Value 处理

Orphan 可能出现在两类场景：

1. **Phase A 失败**：partial durable value，但还没进入 WAL
2. **Phase B 失败**：value 已 durable，all-WAL barrier 没通过

处理方式（概要 §7.1 规则 9）：

```text
运行时路径：
  value/WAL 失败 → release_batch(batch_lsn) → 返回错误
  orphan value 留给 recovery 处理（v1 推荐，最简单）

recovery 路径：
  recovery 扫描 WAL → 未完成 batch 丢弃
  orphan value_ref 进入 dead_value_refs / occupied 补集清理路径
```

### 10.7 Pipeline 异常处理编排

```text
write_wal_fragment(frag)
  = on(front_sched.as_task(),
        append_wal_entries_fua(frag))       // WAL 编码 + FUA（WAL durable point）
  >> any_exception([&frag](auto e) {
         // 交给顶层 all-WAL reduce；顶层会 release_batch(batch_lsn)
         return just_exception(e);  // 重新抛出
     })

write_memtable_fragment(frag)
  = on(front_sched.as_task(),
        insert_memtable_entries(frag))
  >> any_exception([&frag](auto e) {
         // 一旦进入 memtable phase，失败语义就是 fatal
         return just_exception(e);  // 重新抛出
     })
```

顶层：

```text
write_batch(client_req)
  = on(coord_sched, canonicalize_and_assign_lsn(client_req))
  >> push_result_to_context()
  // ── Phase A: value durable ──
  >> on(value_alloc_sched, persist_put_values(batch_ctx))
  // ── Phase B: all-WAL barrier ──
  >> ...for_each(fragments) >> concurrent >> flat_map(write_wal_fragment) >> reduce()
  // ── Phase C: all-memtable barrier ──
  >> ...for_each(fragments) >> concurrent >> flat_map(write_memtable_fragment) >> reduce()
  // ── Phase D: publish ──
  >> on(coord_sched, publish_batch(batch_lsn))
  >> pop_context()
  >> any_exception([batch_lsn](auto e) {
         if current_phase in {value_phase, wal_phase}:
             return on(coord_sched, release_batch(batch_lsn))
                    >> then(return_error_to_client(e))
         // memtable phase 失败：fatal
         return just_exception(e);
     })
```

## 11. 持久化顺序论证

### 11.1 Value-Before-WAL 保证

概要 §10.5 要求：`value object write → value object durable → WAL FUA`。

在 leader-follower 模型下，value-before-WAL 由**因果顺序**保证，而非 qpair 内命令顺序：

```text
value_alloc_sched（leader-follower）:
  合并 PUT entries → 分配 slots → 填充 DMA frame → 按页 FUA 写到本核 nvme_sched
  → 所有相关页的 FUA 完成 → value 已 durable → 返回 value_ref 给各 batch

各 front_sched:
  收到 value_ref 后才开始 WAL append + FUA
```

value FUA 完成是 front_sched 开始写 WAL 的**前置条件**（因果依赖）。因此 value 一定先于 WAL durable，即使两者在不同核心的不同 qpair 上执行。

**冻结约束**：value durable → WAL 开始写，是系统语义要求的因果顺序。每个 scheduler 使用本核心的 `nvme_sched`（per-core × per-device），不需要 value 和 WAL 共享同一 qpair。

### 11.2 WAL Entry 之间的顺序

同一 fragment 的多条 WAL entries 在 segment 中按写入顺序排列。v1 当前实现对该 fragment 涉及的**每个 WAL page write command** 都设置 FUA。

如果崩溃发生在所有相关页的 FUA 完成之前：
- 部分 entries 可能写入，部分未写入
- Recovery 通过 CRC 逐条检查，只接受完整 entry
- 该 batch 的 `actual_count < entry_count` → 不完整 → 整个 batch 丢弃

这是安全的：只有已经完成 FUA 的页才算 durable；若整 batch 的 WAL image 未收齐，recovery 仍会把它判成不完整并丢弃。

### 11.3 跨 Front Sched 的 Ordering

不同 front_scheds 的 WAL FUA 可能在不同时刻完成。但这不影响正确性：

```text
batch_lsn = X，有 3 个 fragments：

  front_sched[0]: WAL FUA 完成 at t=100
  front_sched[1]: WAL FUA 完成 at t=105
  front_sched[2]: WAL FUA 完成 at t=103

  reduce() 在 t=105 完成（最慢的 front 决定）
  publish_batch(X) 在 t=105 之后执行
  → durable_lsn 推进到 X

  如果在 t=102 崩溃：
  → front[0] 的 entries durable
  → front[1] 的 entries 不确定
  → front[2] 的 entries 可能 durable
  → recovery: actual_count < entry_count → batch X 不完整 → 丢弃
```

结论：跨 front 不需要额外 ordering 保证。Batch 完整性由 `entry_count` 在 recovery 时判定。

## 12. 写路径延迟分析

### 12.1 关键路径

```text
单 batch 端到端延迟（最坏情况）：

  coord_sched canonicalize+assign_lsn: ~1μs（CPU only）
  coord_sched route:           ~1μs（CPU only）
  fan-out dispatch:            ~1μs（per_core queue）
  front_sched alloc values:    ~1μs（CPU only）
  NVMe value page writes:      ~10-30μs / page（当前每页 FUA）
  NVMe WAL page writes:        ~10-30μs / page（当前每页 FUA）
  memtable insert:             ~1μs（CPU only）
  reduce + publish:            ~2μs（CPU + per_core queue）

  总计：取决于该 batch 触达的 value/WAL 页数；小 batch 常由 1-2 次 value page FUA 和 1-2 次 WAL page FUA 主导
```

### 12.2 吞吐量

```text
瓶颈在按页 FUA 的次数和 NVMe FUA 延迟。

对当前 v1 实现，单 front_sched 的上限更接近：

  1 / (avg_wal_pages_per_batch × FUA_latency)

value 路径同理也会受 `avg_value_pages_per_batch × FUA_latency` 影响。

N 个 front_scheds 并行后，整体吞吐仍可线性扩展，但会更早打到设备侧 FUA IOPS 上限。

但实际瓶颈可能在 NVMe IOPS（value writes + WAL writes）
或 coord_sched 的 canonicalize+assign_lsn 序列化点。
```

### 12.3 优化方向

1. **Write + Flush 模式**：多个 page write 不带 FUA，最后用一次 `NVMe FLUSH` 固定 durable 点。
   - 需要 PUMP / lower I/O 层支持“写批次 + flush barrier”的明确编排
   - 显著减少 FUA 次数
   - durable 点从“每页 write completion”变成“flush completion”
   - v1 先不实现，当前保守方案仍是每页 FUA

2. **Value write pipelining**：下一个 batch 的 value writes 和上一个 batch 的 WAL FUA 重叠。
   - 需要 front_sched 级别的异步 pipeline
   - 增加复杂度但可显著提升吞吐
   - v1 可暂不实现

3. **Memtable batch insert**：积累多条 entry 后一次性 bulk insert。
   - 取决于 memtable 数据结构是否支持 bulk 操作
   - skip-list 不太适合，sorted array / B+ tree 更适合

v1 先实现 baseline（per-fragment 顺序四阶段），确保正确性后再考虑这些优化。
