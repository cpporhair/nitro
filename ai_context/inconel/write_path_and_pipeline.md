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
 │  ① assign_lsn → ② canonicalize                                 │
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
  │ ⑧ mem insert │ │ ⑧ mem insert │ │ ⑧ mem insert │
  └──────┬───────┘ └──────┬───────┘ └──────┬───────┘
         │                │                │
         └────── reduce ──┴────────────────┘
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
        assign_lsn_and_canonicalize(client_req))
                                            // → (batch_ctx)
  >> push_result_to_context()               // batch_ctx 入 context 栈
  // ── Phase A: value durable（value_alloc_sched leader-follower）──
  >> on(value_alloc_sched.as_task(),
        persist_put_values(batch_ctx))      // 合并 → 分配 → 填充 DMA → FUA
                                            // FUA 完成后返回 value_refs
  // ── Phase B: route + WAL + memtable（fan-out 到 front_scheds）──
  >> get_context<batch_ctx>()
  >> then([](batch_ctx& bc) {
         return route_and_build_fragments(bc);  // 按 owner 分组，entries 已携带 value_ref
     })
  >> for_each(fragments)
     >> concurrent(front_sched_count)
     >> flat_map([](fragment&& frag) {
            auto* fs = front_sched_list[frag.owner];
            return fs->write_fragment(std::move(frag));
        })
     >> reduce()
  // ── Phase C: publish + ACK ──
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

### 2.3 `write_fragment` — Front Sched 内部 Pipeline

value 已在 Phase A 中 durable。front_sched 只做 WAL append + memtable insert：

```text
write_fragment(frag)
  = on(front_sched.as_task(),
        append_wal_entries_fua(frag))       // WAL 编码 + FUA（WAL durable point）
  >> insert_memtable_entries(frag)          // CPU，memtable insert
```

两步在同一个 front_sched 上顺序执行。跨 front_sched 的并发由顶层 `for_each >> concurrent` 提供。

## 3. Canonicalization 算法

### 3.1 输入与输出

```text
输入：client_batch = [(op, key, value?), ...] 按客户端提交顺序
输出：canonical_entries = [(op, key, value?), ...] 每个 key 最多一条
```

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

**leader-follower 模式**：`value_alloc_sched` 的 `advance()` 合并当前并发到达的多个 batch 的 PUT entries。leader 统一执行：分配 slots → 填充 DMA frame → FUA 写到本核 nvme_sched。FUA 完成后所有 batch 拿到 durable `value_ref`，再各自 fan-out 到 front_scheds 写 WAL + memtable。

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
2. 提交到本核 nvme_sched（普通写 + 末尾 FUA）
3. FUA 完成后：
   - `free_count == 0` 的 frame → `clean_readonly`
   - 仍有空 slot 的 frame → 保留为 open frame，下一轮继续填充
   - frame 满后归还到 per-class pools（`return_page` 是 value_alloc_sched 内部操作）

value FUA 完成是整个写路径的**第一个 durable point**。之后各 batch 才 fan-out 到 front_scheds 写 WAL。

### 5.8 未来扩展说明

v1 的 write method 层只有 page-based NVMe I/O（通过 `nvme_sched`）。

**非 v1 future capability**：未来若底层 I/O 层支持更细粒度 durable write，可在不改 placement policy、frame 状态机、`value_ref` 语义或 recovery 规则的前提下扩展 write method。具体适配方式（sender 粒度、submit 路径等）留待未来 lower I/O 设计确定。

## 6. Fragment 内两阶段处理

value 已在 §5.4 中由 `value_alloc_sched` 完成 durable。fragment 到达 front_sched 时，每条 PUT entry 的 `allocated_vr` 已填入。front_sched 只做 WAL + memtable。

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
    for (i, page) in wal_pages_to_write:
        if i == wal_pages_to_write.size() - 1:
            // 最后一页 FUA
            nvme_sched->submit_write(page.paddr, page.buf, lba_size,
                                     io_flags = SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS)
        else:
            nvme_sched->submit_write(page.paddr, page.buf, lba_size, io_flags = 0)

    // FUA 完成 → WAL durable
```

PUMP pipeline 形态：

```text
append_wal_entries_fua(frag)
  = on(front_sched.as_task(),
        encode_wal_entries(frag))            // CPU: WAL 编码
  >> submit_wal_pages_fua(...)               // NVMe: WAL pages + 末页 FUA
```

### 6.2 Phase 2：Memtable 批量插入（CPU only）

```text
insert_memtable_entries(frag):
    for entry in frag.entries:
        if entry.op_type == PUT:
            hot = make_hot_blob(entry.value_data, entry.value_len)
            vh = value_handle { durable = entry.allocated_vr, hot = hot }
            active->insert(entry.key, memtable_entry {
                data_ver = frag.batch_lsn,
                kind     = value,
                vh       = vh,
            })
        else:
            active->insert(entry.key, memtable_entry {
                data_ver = frag.batch_lsn,
                kind     = tombstone,
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

### 7.4 Backpressure

如果 `wal_space_sched` 无法分配新 segment（free pool 空、alloc head 到顶）：

```text
wal_space_sched 将 alloc 请求放入 pending_alloc_queue
→ front_sched 的 write_fragment pipeline 挂起（sender 未完成）
→ coord_sched 的整个 write_batch pipeline 在 reduce() 处等待该 front
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

  t0: assign_lsn(batch_A) → lsn=1, dispatch fan-out
  t1: assign_lsn(batch_B) → lsn=2, dispatch fan-out
  t2: assign_lsn(batch_C) → lsn=3, dispatch fan-out
  ...
  t5: batch_A reduce 完成 → publish_batch(1)
  t6: batch_C reduce 完成 → publish_batch(3)  // C 先于 B 完成
  t7: batch_B reduce 完成 → publish_batch(2)
```

`coord_sched` 是单线程的，因此 `assign_lsn` 是严格顺序的。但 fan-out 之后，各 batch 在 front_scheds 上的执行可以交错。

### 8.2 Front Sched 队列内交错

同一个 front_sched 可能同时收到 batch_A 和 batch_B 的 fragment。但因为 front_sched 是单线程的：

```text
front_sched[0] 队列：
  [write_fragment(batch_A, frag_0)] → [write_fragment(batch_B, frag_0)] → ...

执行顺序：
  先完整执行 batch_A 的 frag_0（value write + WAL + memtable）
  再完整执行 batch_B 的 frag_0
```

**不变量**：同一 front_sched 上，一个 fragment 的四阶段处理是原子的（在单线程意义上）。batch_B 的 fragment 不会插入到 batch_A 的 fragment 中间。

### 8.3 WAL 交错

不同 batch 的 WAL entries 在同一 segment 中是**顺序排列**的（不交错），因为同一 front_sched 顺序处理 fragments。

但同一 batch 的 entries 可以散落在不同 front_scheds 的不同 segments 中（概要 §11.2 约束 4）。Recovery 按 `lsn + entry_count` 重组，不依赖 segment 内连续性。

### 8.4 durable_lsn 推进

publish_batch 到达 coord_sched 的顺序可能乱序（batch_C 先于 batch_B 完成）。ready_bitmap 处理这种乱序：

```text
state: durable_lsn = 0
  publish(1) → ready[1]=1, advance: durable_lsn = 1
  publish(3) → ready[3]=1, advance: durable_lsn = 1 (2 还没 ready)
  publish(2) → ready[2]=1, advance: durable_lsn = 3 (1,2,3 连续)
```

一次推进可以跨过多个已 ready 的 batch（概要 §7.4）。

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
  ... write_batch(A).fan-out ... write_batch(B).fan-out ... seal_round ...

场景 1: batch_X 的 fan-out 在 seal_round 之前入队
  → 各 front_sched 先收到 write_fragment(X)，再收到 seal_active
  → X 的所有 entries 写入 pre-seal 的 A*

场景 2: batch_Y 的 fan-out 在 seal_round 之后入队
  → 各 front_sched 先收到 seal_active，再收到 write_fragment(Y)
  → Y 的所有 entries 写入 post-seal 的 N*
```

**层 2：每个 front_sched 的队列顺序**

同一 front_sched 上，消息按入队顺序执行。所以如果 batch_X 的 write_fragment 在 seal_active 之前入队到某个 front_sched，那么 batch_X 的 write_fragment 一定在该 front_sched 执行 seal_active 之前完成。

### 9.3 Seal 发起约束

Seal 发起必须由 `coord_sched` 发起（不能由 front_sched 自行决定），因为只有 coord_sched 能保证"所有正在进行的 write batch fan-out 都已投递到 front_scheds 之后，再投递 seal_active"。

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
    → gate 关闭，pending_advance 记录 X
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
| Memtable insert 失败 | Phase B | `front_sched` | 不应发生（纯内存） |
| Data Area 空间不足 | Phase A | `value_alloc_sched` | 该 batch + 后续 batch |

### 10.2 Phase A 失败 → 整 Batch 立即失败

`value_alloc_sched` 的 `persist_put_values` 失败时，fan-out 尚未发生：

```text
1. persist_put_values sender 产生异常
2. 整个 write_batch pipeline 异常终止（不进入 Phase B fan-out）
3. batch 不会 publish
4. ready_bitmap 中该 lsn 永远不 ready
```

此时不产生 orphan value——value FUA 要么未提交，要么未成功。DMA frame 中的 slot 可重用。

### 10.3 Phase B 失败 → 整 Batch 失败 + 可能 Orphan

value 已在 Phase A durable。某个 `front_sched` 的 WAL FUA 失败：

```text
1. 该 fragment 的 sender 产生异常
2. 顶层 reduce() 收到异常
3. 整个 write_batch pipeline 异常终止
4. batch 不会 publish
5. ready_bitmap 中该 lsn 永远不 ready
```

此时 Phase A 已为整个 batch 的所有 PUT entries 写入了 durable value objects。这些 value objects 没有对应的 WAL/memtable 引用 → **orphan values**。

### 10.4 异常对 durable_lsn 的影响

如果 `batch_lsn = X` 永远不 ready：

```text
durable_lsn 永远无法推进到 >= X
→ 所有后续 batch (X+1, X+2, ...) 即使 ready 也无法 publish
→ 系统实际上停止服务
```

这是概要 §7.1 规则 7（v1 不定义永久 hole 语义）的体现。运行时应当在 batch 失败后立即终止并进入 recovery。

### 10.5 Orphan Value 处理

Orphan 只在 Phase B 失败时产生（Phase A 成功 → value durable，Phase B 失败 → 无 WAL 引用）。

Orphan 的 scope 是 `value_alloc_sched` 本轮 leader-follower 合并写入的**整个 batch 的所有 PUT value objects**，不是单个 fragment 的 value。

处理方式（概要 §7.1 规则 9）：

```text
运行时路径：
  Phase B 失败触发运行时终止
  orphan value 交给 recovery 处理（v1 推荐，最简单）

recovery 路径：
  recovery 扫描 WAL → 该 batch 不完整 → entries 丢弃
  orphan value_ref 进入 dead_value_refs 集合
  → LBA-aligned/multi-LBA class: Step 8.3 整段 TRIM
  → sub-LBA class: 不逐 slot TRIM，Step 9.2 按整页判断
    整页全 dead → TRIM + value_alloc_sched whole_page_pool
    部分 dead → dead slots 进 hole_page_descriptor
```

### 10.6 Pipeline 异常处理编排

```text
write_fragment(frag)
  // value 已由 value_alloc_sched 在 Phase A 中 durable（见 §2.1/§5.4）
  // write_fragment 只做 WAL + memtable
  = on(front_sched.as_task(),
        append_wal_entries_fua(frag))       // WAL 编码 + FUA（WAL durable point）
  >> insert_memtable_entries(frag)          // CPU，memtable insert
  >> any_exception([&frag](auto e) {
         // v1 推荐：不在运行时回收 orphan，交给 recovery 处理
         // post-LSN 失败会触发运行时终止，recovery 自然清理
         return just_exception(e);  // 重新抛出
     })
```

顶层：

```text
write_batch(client_req)
  = on(coord_sched, assign_lsn_and_canonicalize(client_req))
  >> push_result_to_context()
  // ── Phase A: value durable ──
  >> on(value_alloc_sched, persist_put_values(batch_ctx))
  // ── Phase B: fan-out WAL + memtable ──
  >> ...for_each(fragments) >> concurrent >> flat_map(write_fragment) >> reduce()
  // ���─ Phase C: publish ──
  >> on(coord_sched, publish_batch(batch_lsn))
  >> pop_context()
  >> any_exception([batch_lsn](auto e) {
         // batch 失败，不 publish
         // 日志记录
         // 触发运行时终止或上层 abort 逻辑
         return just_exception(e);
     })
```

## 11. 持久化顺序论证

### 11.1 Value-Before-WAL 保证

概要 §10.5 要求：`value object write → value object durable → WAL FUA`。

在 leader-follower 模型下，value-before-WAL 由**因果顺序**保证，而非 qpair 内命令顺序：

```text
value_alloc_sched（leader-follower）:
  合并 PUT entries → 分配 slots → 填充 DMA frame → FUA 写到本核 nvme_sched
  → FUA 完成 → value 已 durable → 返回 value_ref 给各 batch

各 front_sched:
  收到 value_ref 后才开始 WAL append + FUA
```

value FUA 完成是 front_sched 开始写 WAL 的**前置条件**（因果依赖）。因此 value 一定先于 WAL durable，即使两者在不同核心的不同 qpair 上执行。

**冻结约束**：value durable → WAL 开始写，是系统语义要求的因果顺序。每个 scheduler 使用本核心的 `nvme_sched`（per-core × per-device），不需要 value 和 WAL 共享同一 qpair。

### 11.2 WAL Entry 之间的顺序

同一 fragment 的多条 WAL entries 在 segment 中按写入顺序排列。FUA 只在最后一页发出，覆盖所有 entries。

如果 FUA 之前崩溃：
- 部分 entries 可能写入，部分未写入
- Recovery 通过 CRC 逐条检查，只接受完整 entry
- 该 batch 的 `actual_count < entry_count` → 不完整 → 整个 batch 丢弃

这是安全的：要么全部 WAL entries durable（FUA 后），要么 batch 在 recovery 中被判定为不完整。

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

  coord_sched assign_lsn:     ~1μs（CPU only）
  coord_sched route:           ~1μs（CPU only）
  fan-out dispatch:            ~1μs（per_core queue）
  front_sched alloc values:    ~1μs（CPU only）
  NVMe value writes:           ~10-30μs（取决于 value size 和并发度）
  NVMe WAL FUA:                ~10-30μs（单页 FUA）
  memtable insert:             ~1μs（CPU only）
  reduce + publish:            ~2μs（CPU + per_core queue）

  总计：~30-70μs（value + WAL FUA 主导）
```

### 12.2 吞吐量

```text
瓶颈在 NVMe FUA 延迟。

单 front_sched 吞吐 ≈ 1 / WAL_FUA_latency ≈ 30K-100K batches/s
N 个 front_scheds 并行 → N × 单 front 吞吐

但实际瓶颈可能在 NVMe IOPS（value writes + WAL writes）
或 coord_sched 的 assign_lsn 序列化点。
```

### 12.3 优化方向

1. **Group commit**：多个 batch 的 WAL entries 合并到同一次 FUA。
   - 需要在 front_sched 上引入 batch 窗口或 timer
   - 显著减少 FUA 次数
   - 增加单 batch 延迟（等待窗口）
   - v1 可暂不实现

2. **Value write pipelining**：下一个 batch 的 value writes 和上一个 batch 的 WAL FUA 重叠。
   - 需要 front_sched 级别的异步 pipeline
   - 增加复杂度但可显著提升吞吐
   - v1 可暂不实现

3. **Memtable batch insert**：积累多条 entry 后一次性 bulk insert。
   - 取决于 memtable 数据结构是否支持 bulk 操作
   - skip-list 不太适合，sorted array / B+ tree 更适合

v1 先实现 baseline（per-fragment 顺序四阶段），确保正确性后再考虑这些优化。
