# Inconel 详细设计：运行时状态机

> 依据：`ai_context/inconel/design_overview.md`（唯一概要规范）
>
> 本文细化六类 scheduler 的内部状态、请求类型、handle 逻辑和交互协议。不重复概要中的系统语义，只在需要时引用章节编号。

## 1. Scheduler 总览

| Scheduler | 实例数 | Owner 状态 | 路由策略 |
|-----------|--------|-----------|---------|
| `coord_sched` | 1 | `next_lsn`、`publish_gate`、`current_publish_catalog` | 固定单点 |
| `front_sched` | N（运行时参数） | WAL stream、active/sealed memtable gens | `key_hash % N` |
| `tree_sched` | 1 | tree allocator、flush 状态、retire 队列 | 固定单点 |
| `wal_space_sched` | 1 | segment free pool、alloc head | 固定单点 |
| `value_alloc_sched` | 1 | bump head、per-class pools、per-class open_frames、dirty_pages、deferred_freed、本地 readonly_frame_cache | 固定单点 |
| `nvme_sched` | 每核心 × 每设备（v1 单盘 = 每核心 1 个） | SPDK qpair | 各 scheduler 使用本核心的实例 |

所有 scheduler 遵循 PUMP 单线程模型：内部状态无锁，跨 scheduler 通过 `per_core::queue` 投递请求。

## 2. `coord_sched`（Batch Scheduler / 协调器）

### 2.1 Owner 状态

```cpp
struct coord_state {
    // ── LSN 分配 ──
    uint64_t next_lsn;                              // 下一个可分配的 batch_lsn，gap-free 递增

    // ── 发布 ──
    publish_gate gate;                               // open / closed
    std::shared_ptr<publish_catalog> current_cat;    // 当前活跃 catalog（atomic store/load）
    ready_bitmap ready_set;                          // 跟踪哪些 batch_lsn 已完成 front-side fan-out

    // ── Seal 重入保护 ──
    bool seal_in_progress = false;                   // seal_round 异步执行期间为 true

    // ── 纪元 ──
    uint64_t cat_epoch;                              // catalog 版本号，单调递增
};
```

### 2.2 请求类型

| 请求 | 输入 | 输出 | 触发者 |
|------|------|------|--------|
| `assign_lsn` | 原始 batch | `batch_lsn` + canonical entries + 路由表 | 前台写入 pipeline |
| `publish_batch` | `batch_lsn` | void（推进 `durable_lsn`） | front fan-out reduce 后 |
| `acquire_read_handle` | — | `read_handle { cat, read_lsn }` | 读路径入口 |
| `install_cat` | 新 `publish_catalog` | void | seal / frontier switch |
| `close_gate` | — | void | seal 发起 |
| `open_gate` | — | void | seal 完成 |

### 2.3 handle 逻辑

#### `handle_assign_lsn`

```text
1. batch_lsn = next_lsn++
2. canonical_image = canonicalize(raw_batch)
   - 同 key 多步操作 → last-op-wins → 最多一条 PUT(value) 或 DELETE
   - MERGE/INCREMENT → 折叠成等价 PUT/DELETE（不读 DB 状态）
3. entry_count = |canonical_image|
4. route_table = { key_hash % front_sched_count → [entries] }
5. cb(batch_lsn, canonical_image, entry_count, route_table)
```

关于 canonicalization 的归属：概要冻结的是 "durable boundary 之后只允许看到 canonical image"。本设计把 canonicalization 放在 `coord_sched` 的 `handle_assign_lsn` 内部完成，而非要求调用方预先折叠。原因：

1. `coord_sched` 是单线程入口，canonicalization 在此完成可以保证和 `batch_lsn` 分配在同一原子步骤。
2. 调用方不需要了解 canonical 规则。

#### `handle_publish_batch`

```text
1. ready_set.mark(batch_lsn)
2. new_prefix = ready_set.advance_contiguous_prefix(current_durable_lsn)
   - 从 current_durable_lsn + 1 开始，连续检查 ready_set 中是否存在
   - 找到最大连续前缀末端
3. if new_prefix > current_durable_lsn:
   if gate.is_open():
       current_cat->durable_lsn.store(new_prefix, release)
   else:
       // gate 关闭中（seal 进行中），将 new_prefix 暂存到 pending_advance
       pending_advance = max(pending_advance, new_prefix)
4. cb()
```

`publish_gate` 的职责（概要 §9.2）：阻止 "旧 topology 上的 publish 越过 seal 边界"。在 gate 关闭期间，`durable_lsn` 不前进，但 `ready_set` 继续累积。gate 重新打开时，`pending_advance` 被应用到新 CAT 上（见 `open_gate`），后续 publish 恢复推进。

#### `handle_acquire_read_handle`

```text
1. cat = atomic_load(current_cat)       // shared_ptr 原子加载
2. read_lsn = cat->durable_lsn.load(acquire)
3. cb(read_handle { cat, read_lsn })
```

**不变量**：reader pin 的是 catalog 实例，不是裸 `durable_lsn`。旧 catalog 被新 catalog 取代后，旧 catalog 的 `durable_lsn` 不再前进（概要 §5.4）。

#### `handle_install_cat`

```text
1. new_cat->epoch = ++cat_epoch
2. atomic_store(current_cat, new_cat)
3. // 旧 cat 的引用通过 shared_ptr 延迟释放
4. cb()
```

#### `handle_open_gate`

```text
1. gate.open()
2. if pending_advance > current_cat->durable_lsn.load(acquire):
       // 把 seal 期间累积的已 ready 前缀应用到新 CAT
       current_cat->durable_lsn.store(pending_advance, release)
3. pending_advance = 0
4. cb()
```

**关键**：`pending_advance` 必须在 gate 打开时被消费。如果只写不读，seal 期间完成 fan-out 的 batch 会永久丢失发布机会，因为对应的 `ready_bitmap` bits 已经在 `advance_contiguous_prefix()` 中被消费了。

### 2.4 `ready_bitmap` 实现

```cpp
struct ready_bitmap {
    // 固定窗口：[base, base + WINDOW_SIZE)
    // base = 当前 durable_lsn + 1
    // WINDOW_SIZE = 最大允许同时在飞 batch 数（如 65536）
    uint64_t base;
    std::bitset<WINDOW_SIZE> bits;

    void mark(uint64_t lsn) {
        assert(lsn >= base && lsn < base + WINDOW_SIZE);
        bits.set(lsn - base);
    }

    uint64_t advance_contiguous_prefix(uint64_t current) {
        uint64_t i = current + 1 - base;
        while (i < WINDOW_SIZE && bits.test(i)) {
            bits.reset(i);
            ++i;
        }
        base = current + 1 + (i - (current + 1 - base));
        // 实际实现用 word-level scan 加速连续位检测
        return base - 1;  // 新的 durable_lsn
    }
};
```

### 2.5 `publish_gate`

```cpp
struct publish_gate {
    enum class state : uint8_t { open, closed };
    state st = state::open;
    uint64_t pending_advance = 0;  // gate 关闭期间累积的最大连续前缀

    bool is_open() const { return st == state::open; }
    void close() { st = state::closed; pending_advance = 0; }
    void open()  { st = state::open; }
};
```

gate 不是 mutex，不涉及线程阻塞。它只是 `coord_sched` 单线程内部的一个状态位，用于控制 `handle_publish_batch` 是否推进 `durable_lsn`。如果 publish_batch 请求在 gate 关闭时到达，它仍然正常完成（更新 ready_set、cb()），只是 `durable_lsn` 暂不前进。

### 2.6 Seal 触发策略

Seal 是全局操作（所有 front_scheds 同时切换，概要 §9.2），因此触发条件也必须是全局的。

```cpp
struct seal_trigger_config {
    uint64_t seal_memory_threshold;                  // 所有 front active gen 内存总和阈值（如 256 MiB）
    float    wal_seal_threshold;                     // WAL segment pool 使用率阈值（如 0.70）
    uint64_t total_memtable_limit;                   // active + sealed gen 总内存上限（如 1 GiB）
    uint32_t max_sealed_gens_per_front;              // 单个 front 最大 sealed gen 数（如 4）
};
```

触发条件（任一满足即由 coord_sched 发起 seal_round）：

```text
1. sum(front[i].active.memory_usage) > seal_memory_threshold
2. wal_segment_pool.used_ratio > wal_seal_threshold
3. sum(所有 front 的 active + sealed gen memory) > total_memtable_limit
```

参��� RocksDB：不使用时间驱��（seal 半满 memtable 浪费 tree page 空间，增加写放大）。

**监控机制**：`coord_sched` 在 `handle_assign_lsn` 内联检查，不使用独立监控线程或跨 sched 消息。每个 `front_sched` 维护 `std::atomic<uint64_t> active_memory_usage`（memtable insert 时 relaxed store），`wal_space_sched` 维护 `std::atomic<uint32_t> used_segment_count`（分配/回收时 relaxed store）。`coord_sched` 在 `assign_lsn` 时 relaxed load 这些 atomic 本地求和判断。数据略旧不影响正确性（seal 触发是运行时调优，不是正确性约束）。

### 2.7 写入反压

反压挡在 coord_sched 的 `handle_assign_lsn` 之前（pre-lsn），不挡在 post-lsn：

```text
handle_assign_lsn(raw_batch):
    // ── seal 触发检查（内联，relaxed load atomic counters）──
    if !seal_in_progress && seal_conditions_met():
        seal_in_progress = true
        initiate_seal_round()
        // seal_round 异步完成后回调 coord_sched: seal_in_progress = false

    // ── 反压检查（relaxed load 各 front 的 sealed_gen_count atomic）──
    if any front_sched 的 sealed gen count >= max_sealed_gens_per_front:
        // 拒绝分配 lsn，客户端等待
        // 等 flush 完成后 sealed gen 释放，再恢复分配
        enqueue_to_pending(raw_batch)
        return

    // 正常分配 lsn ...
```

为什么不挡在 post-lsn：batch 一旦拿到 `batch_lsn`，如果某个 front 卡住而其他 front 完成了，reduce 等不到 → durable_lsn 永远推不过该 batch → 等同于永久 hole（概要 §7.1 rule 7）。

## 3. `front_sched`（前台 Scheduler）

### 3.1 Owner 状态

```cpp
struct front_state {
    uint32_t owner_id;                              // 该 front scheduler 的编号

    // ── Memtable ──
    intrusive_ptr<memtable_gen> active;              // 当前写入目标
    small_vector<intrusive_ptr<memtable_gen>, 8> imms;  // sealed gens（newest → oldest）

    // ── WAL Stream ──
    wal_stream_state wal;                            // WAL 追加状态

    // value 分配通过 value_alloc_sched 请求，front_sched 不再持有本地分配器状态
};
```

### 3.2 `memtable_gen` 结构

```cpp
struct memtable_gen {
    uint64_t gen_id;                                 // 代际 ID，全局唯一递增
    enum class state : uint8_t { active, sealed } st;
    uint64_t min_lsn = UINT64_MAX;                   // 该 gen 中最小 batch_lsn
    uint64_t max_lsn = 0;                            // 该 gen 中最大 batch_lsn

    // ── 索引结构 ──
    // btree_map：cache 友好的 B-tree 有序容器（类似 absl::btree_map）
    // 选择理由：单线程 scheduler 不需要 skip list 的无锁并发；
    //   btree 宽节点 cache 局部性好，兼顾 O(log n) 写入和 O(k) range scan
    // 语义约束：
    //   - 按 logical key 有序
    //   - 同 key 可存多个版本，按 data_ver 降序排列
    btree_map<logical_key, small_vector<memtable_entry, 1>> table;

    // ── 回收 ──
    retire_list<retired_value_ref> loser_durable_refs; // fold 输家的 {value_ref, data_ver}
    std::atomic<uint32_t> ref_count;                 // 被 published_read_set 引用计数
};
```

### 3.3 `memtable_entry`

```cpp
struct memtable_entry {
    uint64_t data_ver;                              // 语义等价于 batch_lsn

    enum class kind : uint8_t { value, tombstone } k;

    // kind == value 时有效：
    value_handle vh;
};

struct value_handle {
    value_ref durable;                              // 稳定的盘上定位
    intrusive_ptr<hot_blob> hot;                    // 内存值
};

struct hot_blob {
    std::atomic<uint32_t> ref_count;
    uint32_t len;
    char data[];                                    // flexible array member
};

struct retired_value_ref {
    value_ref vr;
    uint64_t data_ver;                              // 用于和 recovery_safe_lsn 比较
};
```

### 3.4 请求类型

| 请求 | 输入 | 输出 | 说明 |
|------|------|------|------|
| `write_entries` | `(batch_lsn, entry_count, entries[])` | void | WAL FUA + memtable insert（value 已由 value_alloc_sched durable） |
| `seal_active` | — | `front_read_set` | seal 时由 coord_sched 投递 |
| `lookup_memtable` | `(key, read_lsn, front_read_set)` | `variant<value_handle, tombstone, miss>` | point read（搜索 PRS snapshot 的 active/imms，概要 §8.1 step 3） |
| `scan_memtable` | `(begin, end, read_lsn, front_read_set)` | `scan_result_set` | range scan（同上） |
| `release_gens` | `gen_id_list` | void | frontier switch 后移除 imms |

### 3.5 `handle_write_entries`（前台写两阶段）

> 完整的 pipeline 编排见 `write_path_and_pipeline.md` §6。本节只给出 scheduler handle 级的高层摘要。

value 已由 `value_alloc_sched` 完成 durable（leader-follower FUA）。fragment 到达 front_sched 时，每条 PUT entry 的 `allocated_vr` 已填入。front_sched 只做 WAL + memtable：

```text
// ── Phase 1: WAL 编码 + FUA（WAL durable point）──
// 编码 WAL entries 到 tail_frame->dma_buf
// 提交 WAL pages 到本核 nvme_sched，最后一页 FUA
// FUA 完成 → WAL durable

// ── Phase 2: memtable insert（CPU only）──
for each canonical entry in entries[]:
    if entry.op == PUT:
        hot = make_hot_blob(entry.value_bytes, entry.value_len)
        vh = value_handle { durable = entry.allocated_vr, hot = hot }
        active->insert(entry.key, memtable_entry {
            data_ver = batch_lsn, kind = value, vh = vh,
        })
    else:  // DELETE
        active->insert(entry.key, memtable_entry {
            data_ver = batch_lsn, kind = tombstone,
        })
    active->max_lsn = max(active->max_lsn, batch_lsn)
    active->min_lsn = min(active->min_lsn, batch_lsn)
```

**Value-before-WAL 保证**：value FUA 在 `value_alloc_sched` 上已完成，是 front_sched 开始写 WAL 的因果前置条件。不需要 value 和 WAL 在同一 qpair 上。

### 3.6 `handle_seal_active`

```text
1. F = active                           // 旧 active → 将成为 sealed
2. F.st = sealed
3. N = new memtable_gen { gen_id = next_gen_id++, st = active }
4. active = N
5. imms.push_front(F)                   // F 进入 imms 最前面（最新）
6. cb(front_read_set { active = N, imms = [F] ++ old_imms })
```

**不变量**（概要 §7.1 补充）：同一 batch 不会跨这次 seal 边界裂成两代。这由 `coord_sched` 的单线程顺序保证：seal_active 请求和 write_entries 请求在同一个 `front_sched` 队列中按顺序执行。

### 3.7 `handle_lookup_memtable`

搜索的是调用方传入的 PRS snapshot 中的 active/imms（概要 §8.1 step 3："查 `cat->prs` 对应的 active + imms"），**不是** front_sched 当前的 active/imms。原因：frontier switch 后 `release_gens` 会从 front_sched 本地 imms 移除已 flush 的 gens，但旧 reader pin 的 PRS 仍引用这些 gens，其 tree_guard 也不含这些 gens 的数据。如果搜索 front_sched 当前 imms，旧 reader 会丢数据。

```text
handle_lookup_memtable(key, read_lsn, frs):
    // frs = 调用方 read_handle 的 cat->prs->fronts[owner]
    // frs.active / frs.imms 由 PRS snapshot 的 intrusive_ptr 保活

1. 在 frs.active 中查找 key，收集所有 data_ver <= read_lsn 的 entries
2. 在 frs.imms 中从新到旧查找 key，收集所有 data_ver <= read_lsn 的 entries
3. winner = 所有命中中 data_ver 最大的那条
4. if winner 不存在:
       cb(miss)
   else if winner.kind == value:
       cb(winner.vh)          // 直接返回 value_handle（含 hot_blob）
   else:
       cb(tombstone)          // 上层返回 not found
```

**线程安全**：dispatch 到 front_sched 执行是为了保证 btree_map 读操作的线程安全。sealed gen 是 immutable 的；PRS 中的 active gen 可能仍在被 front_sched 写入，但 lookup 在 front_sched 单线程上执行，与 write_entries 串行，不存在竞争。

**优化**：因为 data_ver 在同一 gen 内不重复（概要 §1.6 `same key → same front scheduler`），可以从 frs.active → frs.imms[0] → frs.imms[1] → ... 依次查找，找到第一条 `data_ver <= read_lsn` 的即为 winner，无需遍历所有 gens。

### 3.8 `handle_release_gens`

```text
for gen_id in gen_id_list:
    imms.remove_if(g -> g.gen_id == gen_id)
    // gen 引用计数由 published_read_set 的 shared_ptr 管理
    // 这里只是从本 front_sched 的 imms 列表中移除
```

### 3.9 WAL Stream 状态

```cpp
struct wal_stream_state {
    uint32_t stream_id;                             // == owner_id (v1)
    segment_runtime* active_seg;                    // 当前 active segment
    uint32_t write_offset;                          // 当前追加位置（相对 segment 起始）
    page_frame* tail_frame;                          // 当前 tail page 的 DMA frame
    // 统一模型中 = page_frame { dom = wal_page, st = dirty_append }
    // tail_frame->dma_buf 即旧 tail_buf（见 runtime_memory_and_cache.md §5.3/§8.5）
    uint64_t seg_max_lsn;                           // 当前 active segment 的最大 lsn
    small_vector<segment_runtime*, 16> sealed_segs; // 已 seal 待回收
};
```

#### `append_entry_fua` 流程

```text
// tail_frame->dma_buf 即 WAL 当前 tail page 的 DMA buffer
1. encoded = encode_wal_entry(entry)
2. if write_offset + encoded.len > segment_size - TRAILER_RESERVED:
       // 换段
       seal_current_segment()
       active_seg = wal_space_sched->alloc_segment(stream_id)
       write_segment_header(active_seg)
       write_offset = HEADER_SIZE
       memset(tail_frame->dma_buf, 0, LBA_SIZE)
3. memcpy(tail_frame->dma_buf + (write_offset % LBA_SIZE), encoded.data, encoded.len)
4. write_offset += encoded.len
5. // FUA 写当前 tail page（可能包含本次 entry 和同页的旧 entries）
   fua_write(active_seg.base_paddr + floor(write_offset - 1, LBA_SIZE), tail_frame->dma_buf)
   // 如果 entry 跨页，先非 FUA 写前面的完整页，最后一页 FUA
6. seg_max_lsn = max(seg_max_lsn, entry.lsn)
```

**幂等性**：tail page 可能被多次整页重写（概要 §11.3）。已经在同一页内的旧 entry bytes 是同一 page image 的前缀，重写是幂等的。`tail_frame` 是 `page_frame { dom = wal_page, st = dirty_append }` 的实例。

#### `seal_current_segment` 流程

```text
1. active_seg->st = SEALED
2. active_seg->max_lsn = seg_max_lsn
3. 写 sealed trailer（如有剩余空间）：
   trailer = {
       segment_gen = active_seg->segment_gen,
       write_end   = write_offset,
       min_lsn     = active_seg->min_lsn,
       max_lsn     = seg_max_lsn,
       sealed      = true,
       crc,
   }
   // trailer 不需要 FUA，它只是加速 recovery 的 hint
4. sealed_segs.push_back(active_seg)
```

## 4. `tree_sched`（Tree Scheduler）

### 4.1 Owner 状态

```cpp
struct tree_state {
    // ── Allocator ──
    tree_allocator alloc;                            // Data Area 低端分配器
    // alloc.head = 当前已分配的最高地址 + 1

    // ── Flush 状态 ──
    uint64_t flush_max_lsn;                          // 已 flush 进 tree 的最大 batch_lsn
    uint64_t recovery_safe_lsn;                      // 对 WAL/value 回收安全的下界

    // ── Retire 队列 ──
    // 由旧 checkpoint_guard 的 destructor 投递过来的回收任务
    per_core::queue<reclaim_task*> reclaim_q;
};
```

### 4.2 请求类型

| 请求 | 输入 | 输出 | 说明 |
|------|------|------|------|
| `flush` | eligible sealed gens | `checkpoint_guard` + new manifest | 详见 flush_and_frontier_switch.md |
| `reclaim` | `reclaim_task` | void | TRIM old slots/ranges, recycle value extents |
| `update_superblock` | `root_base_paddr` | void | root 变化时异步更新 |

注：`tree_lookup` 和 `tree_scan` 不是 tree_sched 的请求。它们就地在调用方 scheduler 上执行（见 §4.7）。

### 4.3 Data Area 碰撞检测

概要 §10.4：tree 向高地址增长，value 向低地址增长，两端在中间相遇表示 Data Area 已满。碰撞检测通过 per-device 的共享 atomic 实现：

```cpp
// 全局共享（per-device，v1 单设备只有一份）
struct data_area_heads {
    std::atomic<uint64_t> tree_head_lba;             // tree_sched relaxed store, value_alloc_sched relaxed load
    std::atomic<uint64_t> value_head_lba;            // value_alloc_sched relaxed store, tree_sched relaxed load
};
// 两个 head 都是单调的（tree 只增，value 只减），读到略旧的值只会导致
// 少分配一点后重试，不破坏正确性。不需要 acquire/release。
```

### 4.4 `tree_allocator`

```cpp
struct tree_allocator {
    paddr head;                                      // 下一个可分配的 range base
    // head 从 data_area_base_paddr 开始，向高地址增长
    // 分配单位 = tree_page_size * shadow_slots_per_range
    local::queue<range_ref, 4096> free_ranges;       // 已回收可重用的 range
    data_area_heads* shared_heads;                   // 碰撞检测共享结构

    range_ref allocate() {
        if (auto r = free_ranges.try_dequeue(); r)
            return *r;
        // 碰撞检测：确保 bump 不会撞到 value 端
        uint64_t next_end = head.lba + tree_page_size * shadow_slots_per_range / lba_size;
        if (next_end > shared_heads->value_head_lba.load(std::memory_order_relaxed))
            return {};  // Data Area 已满
        range_ref r = { .base = head, .slot_count = shadow_slots_per_range };
        head.lba = next_end;
        shared_heads->tree_head_lba.store(head.lba, std::memory_order_relaxed);
        return r;
    }

    void recycle(range_ref r) {
        free_ranges.try_enqueue(r);
    }
};
```

### 4.5 `tree_manifest`

```cpp
struct tree_manifest {
    paddr root_slot;                                 // 当前 root 的精确 slot 地址

    // 每个 range 在该 snapshot 下应读哪个 slot
    // 实现选择 slot_index（0-based offset within range）
    // exact_paddr = range_base + slot_index * tree_page_size / lba_size
    flat_hash_map<paddr/*range_base*/, uint32_t/*slot_index*/> slot_map;

    paddr resolve(paddr range_base) const {
        auto it = slot_map.find(range_base);
        assert(it != slot_map.end());
        uint32_t idx = it->second;
        return { range_base.device_id,
                 range_base.lba + idx * (tree_page_size / lba_size) };
    }
};
```

`tree_manifest` 是 immutable snapshot（概要 §4.4，详见 §4.5）。新 flush 创建新 manifest，旧 manifest 跟着旧 guard 活到最后一个 reader 释放。

### 4.6 `checkpoint_guard`

```cpp
struct checkpoint_guard {
    std::shared_ptr<const tree_manifest> manifest;

    // ── 回收对象 ──
    struct retired_objects {
        small_vector<paddr, 32> old_slots;           // 被新版本取代的旧 slot
        small_vector<range_ref, 8> old_ranges;       // consolidation 后的旧 range
        small_vector<retired_value_ref, 64> old_tree_values; // 被覆盖/删除的旧 tree-visible {value_ref, data_ver}
    } retired;

    ~checkpoint_guard() {
        if (!retired.empty()) {
            // 投递 reclaim_task 到 tree_sched
            // 注意：destructor 不直接发 NVMe 命令（概要 §3.1 约束 5）
            tree_sched->enqueue_reclaim(std::move(retired));
        }
    }
};
```

### 4.7 Tree Lookup（就地执行，不经过 tree_sched）

tree_lookup **不在 tree_sched 上执行**。它就地在调用方所在的 scheduler（如 front_sched）上执行，通过 nvme_sched 异步读 page。

原因：tree_lookup 的依赖（immutable manifest、NVMe read、readonly_frame_cache）全部不需要 tree_sched 的可变状态。CoW tree 的核心设计就是读写分离——reader 拿着旧 manifest 走旧结构，writer 在 tree_sched 上写新 slot。把 tree_lookup 放到 tree_sched 上会破坏这一核心属性，使读路径被 flush/reclaim 阻塞。

```text
// 在调用方 scheduler 上就地执行（如 front_sched、coord_sched）
tree_lookup(key, manifest):
1. slot_paddr = manifest->resolve(root_range_base)
2. root_page = readonly_frame_cache.get_or_read(slot_paddr)
   // cache miss → nvme_sched->read → 回填 cache
3. 从 root 开始 B+ tree 下降：
   for each internal level:
       child_range_base = find_child(page, key)
       child_slot_paddr = manifest->resolve(child_range_base)
       child_page = readonly_frame_cache.get_or_read(child_slot_paddr)
4. leaf_page = 最终叶子
5. record = leaf_page.find(key)
6. if record 不存在:
       return absent
   else if record.kind == value:
       return leaf_value_record { data_ver, value_ref }
   else:
       return leaf_tombstone { data_ver }
```

**node frame cache**：每个可能执行 tree_lookup 的 scheduler 拥有自己的 `readonly_frame_cache` shard（tree_node domain）。tree page 以 `page_frame { dom = tree_node, st = clean_readonly }` 形式驻留在 DMA 内存中。遍历期间通过 `pin_count` 防止驱逐；pin 释放后按 LRU 可淘汰。cache 驱逐不影响正确性，因为 manifest 持有结构信息，page 可以随时从 NVMe 重新读取。同一个 tree page 可以在多个 shard 中各自驻留（`runtime_memory_and_cache.md` §7.3）。

**tree_sched 只保留写侧操作**：flush、reclaim、allocator、superblock 更新。flush 期间读取旧 leaf pages 也在 tree_sched 上执行（这是写侧 fold 的一部分），不影响其他 scheduler 上的读路径 tree_lookup。

### 4.8 `recovery_safe_lsn` 推进

`recovery_safe_lsn` 表达的是：所有 `data_ver <= recovery_safe_lsn` 的历史旧版本，如果不是当前 durable winner，就不可能再从 recovery 输入中出现（概要 §6 额外约束 3）。

推进条件：

```text
if 所有 sealed WAL segments 都已回收（或无 sealed segments）:
    wal_frontier = flush_max_lsn
else:
    wal_frontier = 所有 sealed WAL segments 中最小的 min_lsn - 1

recovery_safe_lsn = min(
    flush_max_lsn,               // tree 已经物化到这里
    superblock.generation 对应的那次 flush 的 flush_max_lsn,
                                  // superblock 已经更新到这里
    wal_frontier,                // 即使崩溃，比这更早的 WAL 已不会出现
)
```

简化理解：只有当 tree flush 完成 + superblock 更新 + 对应 WAL segments 回收完毕后，`recovery_safe_lsn` 才能安全推进。

## 5. `wal_space_sched`（WAL 空间 Scheduler）

### 5.1 Owner 状态

```cpp
struct segment_alloc_entry {
    segment_id id;
    uint32_t next_gen;                               // 下次分配时应使用的 segment_gen
};

struct sealed_segment_info {
    segment_id id;
    uint32_t segment_gen;
    uint64_t min_lsn;
    uint64_t max_lsn;
};

struct wal_space_state {
    // ── 分配 ──
    uint32_t alloc_head;                             // 下一个可从初始池分配的 segment index
    local::queue<segment_alloc_entry, 256> free_pool; // 已回收可重用（携带 next_gen）

    // ── Sealed 追踪（由 front_sched 换段时 push）──
    small_vector<sealed_segment_info, 64> sealed_segments;

    // ── 格式常量 ──
    uint32_t wal_segment_count;                      // 来自 superblock
};
```

### 5.2 请求类型

| 请求 | 输入 | 输出 |
|------|------|------|
| `alloc_segment` | `stream_id`, `sealed_info?`（可选，换段时携带刚 seal 的 segment 元数据） | `segment_runtime*` 或 backpressure |
| `reclaim_check` | `recovery_safe_lsn` | void（内部筛选并回收） |

### 5.3 `handle_alloc_segment`

```text
handle_alloc_segment(stream_id, sealed_info?):
    // ── 记录刚 seal 的 segment（搭在 alloc 请求上，零额外消息）──
    if sealed_info 存在:
        sealed_segments.push(sealed_info)

    // ── 分配新 segment ──
    1. if free_pool.try_dequeue() → entry:
           // 重用 segment，使用 entry 中携带的 next_gen
           seg = new segment_runtime {
               id = entry.id,
               segment_gen = entry.next_gen,
               owner_stream = stream_id,
               st = ACTIVE,
           }
           cb(seg)
    2. else if alloc_head < wal_segment_count:
           seg_id = { device_id = 0, index = alloc_head++ }
           seg = new segment_runtime {
               id = seg_id,
               segment_gen = 1,              // 首次使用
               owner_stream = stream_id,
               st = ACTIVE,
           }
           cb(seg)
    3. else:
           // WAL backpressure
           // 概要 §11.4：不能留下永久 LSN hole
           // 将请求排入 pending_alloc_queue
           // 等 reclaim_check 归还后重新 dequeue
           pending_alloc_queue.push(req)
```

### 5.4 `handle_reclaim_check`

```text
handle_reclaim_check(recovery_safe_lsn):
    // tree_sched 推进 recovery_safe_lsn 后投递此请求
    // wal_space_sched 本地筛选可回收的 sealed segments
    reclaimable = []
    remaining = []
    for info in sealed_segments:
        if info.max_lsn <= recovery_safe_lsn:
            reclaimable.push(info)
        else:
            remaining.push(info)
    sealed_segments = remaining

    for info in reclaimable:
        entry = segment_alloc_entry { id = info.id, next_gen = info.segment_gen + 1 }
        free_pool.try_enqueue(entry)
        // 如有 pending_alloc_queue 中的等待者，立即唤醒
        if pending_alloc_queue.not_empty():
            dequeue → fulfill with recycled entry
```

## 6. `value_alloc_sched`（Value 分配 Scheduler）

### 6.1 定位

集中管理 value page 的分配、DMA frame 填充与 NVMe FUA 写入（leader-follower 模式）。通过本核 `nvme_sched` 提交 value FUA。

| 属性 | 值 |
|------|---|
| 实例数 | 1（全局唯一） |
| Owner 状态 | bump head（per-device）、per-class pools（whole_page_pool / hole_page_list / extent_free_pool）、dirty_pages、deferred_freed、per-class open frames |
| NVMe I/O | 通过本核 nvme_sched 提交 value FUA 写入 |
| 路由 | 固定单点 |

### 6.2 请求类型

| 请求 | 输入 | 输出 | 调用者 |
|------|------|------|--------|
| `persist_put_values` | batch PUT entries | durable value_refs | coord_sched（leader-follower：多 batch 合并） |
| `freed_slots` | page_base, class_idx, freed_mask | void | tree_sched（sub-LBA value 回收） |
| `recycle_whole` | class_idx, page_base | void | tree_sched（LBA-aligned value 回收，TRIM 由 tree_sched 先完成） |

注：`alloc_page` 和 `return_page` 不再是跨 scheduler 请求——它们是 `persist_put_values` 内部的本地操作（value_alloc_sched 自己持有 open frames 和 per-class pools）。

### 6.3 Owner 状态

```cpp
struct per_device_value_state {
    uint64_t bump_head_lba;                          // 从 data_area_end 向低地址递减
    data_area_heads* shared_heads;                   // 碰撞检测共享结构（同 tree_allocator）

    // bump 分配一个 page（返回 null 表示 Data Area 已满）
    paddr bump_next_page(uint32_t span_lbas) {
        uint64_t next = bump_head_lba - span_lbas;
        if (next < shared_heads->tree_head_lba.load(std::memory_order_relaxed))
            return {};  // 撞到 tree 端
        bump_head_lba = next;
        shared_heads->value_head_lba.store(bump_head_lba, std::memory_order_relaxed);
        return { .device_id = 0, .lba = next };
    }
};

struct value_alloc_state {
    // ── 分配 ──
    per_device_value_state dev_state;                // bump head（per-device，v1 单设备）

    // ── Per-class pools ──
    struct per_class_state {
        flat_hash_map<paddr, hole_page_descriptor> hole_pages;  // 带空洞页
        local::queue<paddr, 256> whole_page_pool;               // 整页空闲
        local::queue<paddr, 64>  extent_free_pool;              // multi-LBA extent
    };
    small_vector<per_class_state, 16> classes;

    // ── Per-class open frames（当前正在填充的 DMA page）──
    // persist_put_values 中 leader 统一分配 slot、memcpy 到 open_frame，
    // 本轮结束后提交 FUA。满页转 clean_readonly，未满页保留到下一轮继续填充。
    small_vector<value_page_frame*, 16> open_frames; // index by class_idx，null = 无 open page

    // ── Dirty tracking ──
    // 跟踪 value_alloc_sched 当前持有的 active open pages 的地址集合。
    // 用于在 tree_sched 投递 freed_slots 时判断该页是否正被写入，
    // 若是则暂存到 deferred_freed，等 return_page 时合并。
    flat_hash_set<paddr> dirty_pages;                // 当前 value_alloc_sched 的 active open pages
    flat_hash_map<paddr, bitset> deferred_freed;     // dirty 期间暂存的回收 mask

    // ── Placement policy ──
    value_placement_config config;                   // hole_reuse_watermark 等
};
```

不变量：`dirty_pages` 中的 page 不会同时出现在 `hole_pages` 中。`deferred_freed` 只在 dirty 期间累积，`return_page` 时一次性合并清空。

### 6.4 `handle_alloc_page`

```text
handle_alloc_page(class_idx):
    cls = classes[class_idx]

    if !hole_reuse_enabled:
        // fresh_first 模式（Data Area 使用率低于 watermark）
        if try_alloc_whole_page(class_idx) → result: return result
        if try_alloc_bump(class_idx) → result: return result
        if try_alloc_hole_page(class_idx) → result: return result
        → space_exhausted

    else:
        // hole_first 模式（使用率超过 watermark）
        if try_alloc_hole_page(class_idx) → result: return result
        if try_alloc_whole_page(class_idx) → result: return result
        if try_alloc_bump(class_idx) → result: return result
        → space_exhausted

try_alloc_hole_page(class_idx):
    if (page_base, desc) = classes[class_idx].hole_pages.take_any():
        dirty_pages.insert(page_base)
        return alloc_result { page_base, class_idx, desc.free_mask, source=non_resident_hole }
    return null

try_alloc_whole_page(class_idx):
    if cls.whole_page_pool.try_dequeue() → page_addr:
        dirty_pages.insert(page_addr)
        return alloc_result { page_addr, class_idx, all_free_mask, source=whole_page }
    if cls.extent_free_pool.try_dequeue() → extent_addr:
        dirty_pages.insert(extent_addr)
        return alloc_result { extent_addr, class_idx, all_free_mask, source=whole_page }
    return null

try_alloc_bump(class_idx):
    page_addr = bump_next_page(class_idx)
    if page_addr == null: return null
    dirty_pages.insert(page_addr)
    return alloc_result { page_addr, class_idx, all_free_mask, source=fresh }
```

`persist_put_values` 内部根据 `source` 准备 DMA frame：
- `fresh` / `whole_page` → 分配 DMA frame，清零
- `non_resident_hole` → 先查本地 `readonly_frame_cache`；cache hit 则零 NVMe read，cache miss 则通过本核 nvme_sched 读整页

### 6.5 `handle_return_page`

```text
handle_return_page(page_base, class_idx, free_bitmap):
    dirty_pages.erase(page_base)

    // 合并借出期间积累的 deferred freed_mask
    if deferred_freed.contains(page_base):
        free_bitmap |= deferred_freed[page_base]
        deferred_freed.erase(page_base)

    if free_bitmap.none():
        // 全满 → value_alloc_sched 不管该页
        pass
    elif free_bitmap.all():
        // 全空 → 回收整页
        recycle_whole_page(class_idx, page_base)
    else:
        // 部分空 → 记入 hole_pages
        classes[class_idx].hole_pages.insert(page_base,
            hole_page_descriptor { page_base, class_idx, free_bitmap })
```

### 6.6 `handle_freed_slots`（sub-LBA 回收核心）

```text
handle_freed_slots(page_base, class_idx, freed_mask):

    // ── 情况 1：该页当前是 value_alloc_sched 的 active open page ──
    if dirty_pages.contains(page_base):
        // 暂存 freed_mask，等 return_page 时合并
        deferred_freed[page_base] |= freed_mask
        return

    // ── 情况 2：该页在 hole_page_list 中 ──
    if desc = classes[class_idx].hole_pages.find(page_base):
        desc->free_mask |= freed_mask
        if desc->free_mask.all():
            classes[class_idx].hole_pages.erase(page_base)
            recycle_whole_page(class_idx, page_base)
        return

    // ── 情况 3：该页不在任何结构中 ──
    if freed_mask.all():
        recycle_whole_page(class_idx, page_base)
    else:
        classes[class_idx].hole_pages.insert(page_base,
            hole_page_descriptor { page_base, class_idx, freed_mask })
```

**幂等性**：全部用 `|=` + `bitmap.count()`，不用 `++`。

### 6.7 `recycle_whole_page` 统一路径

```text
recycle_whole_page(class_idx, page_base):
    1. stale cache invalidation
    2. 放回 classes[class_idx].whole_page_pool
    // TRIM 由调用方（tree_sched）在投递 recycle 之前完成（等待 TRIM 回调后再投递）
```

multi-LBA extent 的 `handle_recycle_whole` 同理：放回 `classes[class_idx].extent_free_pool`。

### 6.8 TRIM 顺序协议

`recycle_whole_page` 路径中，TRIM 必须在回收之前完成。因为 per-core 模型下 TRIM（tree_sched 核心的 qpair）和后续写入（value_alloc_sched 核心的 qpair）在不同 qpair 上，没有隐式顺序保证：

```text
tree_sched: submit_trim(page_base) → 等待 TRIM 完成回调
tree_sched: freed_slots / recycle_whole → value_alloc_sched
// TRIM 已完成 → 页可安全重分配
// value_alloc_sched 拿到页后写入，不会被迟到的 TRIM 覆盖
```

TRIM 在回收路径上（非写关键路径），等待完成不影响前台延迟。

### 6.9 `dirty_pages` / `deferred_freed` 生命周期

| 事件 | 动作 |
|------|------|
| `persist_put_values` 内部 `alloc_page` | `dirty_pages.insert(page_base)` |
| `persist_put_values` 完成 FUA 后 `return_page` | `dirty_pages.erase(page_base)` + 合并 `deferred_freed` |
| `handle_freed_slots` 命中 dirty page | `deferred_freed[page_base] \|= freed_mask`（暂存） |
| value 写入失败 / abort | 归还 → 同 return_page |

## 7. `nvme_sched`（NVMe Scheduler）

每核心 × 每设备各一个实例，每个实例拥有独立的 SPDK qpair。各 scheduler 使用本核心的 `nvme_sched`，不做跨核心 NVMe I/O 投递。v1 单盘场景下每核心 1 个 `nvme_sched`。

Inconel 复用 PUMP 框架已有的 NVMe scheduler（`src/env/scheduler/nvme/`）。需要扩展的操作：

| 操作 | 现有 | 需新增 |
|------|------|--------|
| read page | 已有 | — |
| write page | 已有 | — |
| write page FUA | — | 需要（WAL tail page） |
| NVMe FLUSH | — | 需要（flush 后固定 tree durable frontier） |
| TRIM | — | 需要（回收旧 slot/range/value） |

### 7.1 FUA Write

在现有 `put_page` 基础上，增加 `io_flags = SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS`。框架 NVMe scheduler 的 `req` 已有 `io_flags` 字段，直接设置即可。

### 7.2 NVMe FLUSH

```cpp
// 新增 sender：nvme::flush(scheduler)
// 底层：spdk_nvme_ns_cmd_flush(ns, qpair, cb, ctx)
// 完成后 cb → op_pusher push_value
```

FLUSH 保证此前所有写入（含普通写和 FUA 写）都已 durable（概要 §10.8）。

### 7.3 TRIM

```cpp
// 新增 sender：nvme::trim(scheduler, paddr, lba_count)
// 底层：spdk_nvme_ns_cmd_dataset_management(ns, qpair, DEALLOCATE, ranges, ...)
// TRIM 不在写关键路径上，可以批量异步执行
```

## 8. 对象生命周期总览

### 8.1 `publish_catalog` 生命周期

```text
创建：
  - 初始化时（boot / recovery）
  - seal 完成后（CAT1）
  - frontier switch 后（CAT2）

持有：
  - coord_sched.current_cat（当前活跃）
  - reader 通过 read_handle.cat（pin 住）

释放：
  - 当新 CAT 安装后，旧 CAT 只剩 reader 的引用
  - 最后一个 reader 释放 read_handle → shared_ptr 引用归零
  → 级联释放 published_read_set
  → 级联释放 memtable_gen（引用计数减少）
  → 级联释放 checkpoint_guard
```

### 8.2 `published_read_set` 生命周期

```cpp
struct published_read_set {
    std::shared_ptr<checkpoint_guard> tree_guard;
    std::shared_ptr<const std::vector<front_read_set>> fronts;
    uint64_t epoch;
};
```

`fronts` 中的每个 `front_read_set` 持有 `memtable_gen` 的 `intrusive_ptr`。当 prs 释放时，这些引用一起释放。

### 8.3 `value_handle` / `hot_blob` 生命周期

```text
创建：
  1. 前台 PUT 时创建 hot_blob（内存 value bytes 的 copy）
  2. value_handle = { value_ref（盘上位置）, hot_blob }
  3. memtable_entry 持有 value_handle

存活期间：
  - memtable entry live → hot_blob 有 ref → 不可释放
  - hot_blob 独立于 page/frame cache（见 runtime_memory_and_cache.md §9.3）
  - tree-path value read 使用 value_view（frame_pin 引用 DMA page），不复用 hot_blob

释放触发：
  - memtable_gen 从所有 published_read_set 中摘除
  - memtable_gen.ref_count → 0
  - memtable_gen 析构 → memtable_entry 析构 → value_handle 析构
    → hot_blob.ref_count--（可能归零释放）
    → value_ref 进入回收判定（见下）

durable value_ref 回收：
  - 如果该 value_ref 已经被 tree flush 覆盖（更新版本进入 tree）：
    → 挂在旧 checkpoint_guard.retired.old_tree_values 上
    → 最后 reader 释放旧 guard 后回收
  - 如果该 value_ref 从未进入 tree（memtable-only loser）：
    → 挂在 owning memtable_gen.loser_durable_refs 上
    → gen 释放且 data_ver <= recovery_safe_lsn 后回收
```

### 8.4 `memtable_gen` 引用模型

```text
引用来源：
  1. front_sched.active（当前写入目标，1 个）
  2. front_sched.imms（sealed gens，多个）
  3. published_read_set.fronts[owner].active / imms（reader 可见集合）

何时可释放：
  - gen 已从 front_sched.imms 中移除（frontier switch 后）
  - gen 已从所有仍存活的 published_read_set 中消失
  - 即：最后一个 pin 旧 CAT 的 reader 释放后
```

## 9. 跨 Scheduler 交互时序

### 9.1 前台写入完整时序

```text
client
  │
  ▼
coord_sched ── assign_lsn(raw_batch) ──────────────────────┐
  │  cb: (batch_lsn, canonical_entries, route_table)        │
  │                                                         │
  ├── fan-out ──────────────────────────────────────────────┤
  │                                                         │
  │  ┌─────────────────┐  ┌─────────────────┐              │
  │  │ front_sched[0]  │  │ front_sched[1]  │  ...         │
  │  │ write_entries() │  │ write_entries() │              │
  │  │  (value 已durable)│ │  (value 已durable)│            │
  │  │  1. WAL FUA     │  │  1. WAL FUA     │              │
  │  │  2. memtable ins│  │  2. memtable ins│              │
  │  └────────┬────────┘  └────────┬────────┘              │
  │           │                    │                        │
  │           └────── reduce ──────┘                        │
  │                    │                                    │
  ▼                    ▼                                    │
coord_sched ── publish_batch(batch_lsn) ───────────────────┘
  │  ready_set.mark → advance durable_lsn
  │
  ▼
ACK to client
```

### 9.2 Seal / Rotate 完整时序

```text
coord_sched ── close_publish_gate() ─────────────────────────┐
  │                                                           │
  ├── fan-out ────────────────────────────────────────────────┤
  │                                                           │
  │  ┌──────────────────┐  ┌──────────────────┐              │
  │  │ front_sched[0]   │  │ front_sched[1]   │  ...         │
  │  │ seal_active()    │  │ seal_active()    │              │
  │  │  A→F, install N  │  │  A→F, install N  │              │
  │  │  cb(front_read_set)  cb(front_read_set)│              │
  │  └────────┬─────────┘  └────────┬─────────┘              │
  │           │                     │                         │
  │           └────── reduce ───────┘                         │
  │                    │                                      │
  ▼                    ▼                                      │
coord_sched ── build PRS1 from front_read_sets ──────────────┘
  │  PRS1 = { tree_guard = G0（不变）,
  │           fronts = [ {active=N0, imms=[F0]+old_imms0},
  │                      {active=N1, imms=[F1]+old_imms1}, ... ] }
  │  CAT1 = { prs = PRS1, durable_lsn = D0（旧值继承）}
  │  install_cat(CAT1)
  │
  ▼
coord_sched ── open_publish_gate() ─────────────────────────
  │  // post-seal batch 现在可以推进 CAT1.durable_lsn
```

### 9.3 Flush / Frontier Switch 完整时序

详见 `flush_and_frontier_switch.md`，这里只画骨架：

```text
tree_sched ── choose eligible gens ──────────────────────────┐
  │                                                           │
  ├── fan-out to front_scheds: pin imms ─── reduce ──────────┤
  │                                                           │
  ▼                                                           │
tree_sched ── fold visible state ─────────────────────────────┤
  │  生成 winner records（复用已有 value_ref）                 │
  │                                                           │
  ▼                                                           │
tree_sched ── write new tree slots ── nvme_sched(s) ─────────┤
  │  构造新 tree_manifest (M1)                                 │
  │                                                           │
  ▼                                                           │
nvme_sched ── NVMe FLUSH ────────────────────────────────────┤
  │                                                           │
  ▼                                                           │
coord_sched ── build G1, PRS2, install CAT2 ─────────────────┘
  │  CAT2.durable_lsn = 安装瞬间旧 CAT 的 durable_lsn
  │  PRS2 中摘掉已被 flush 覆盖的 gens
  │
  ▼
tree_sched ── enqueue reclaim ─── maybe update superblock
```

## 10. 并发安全论证

### 10.1 `same key → same front_sched`

同一运行期内，key hash 路由到唯一 front_sched。因此：

1. 同 key 的 memtable writes 不会跨 front_sched 竞争。
2. point GET 只需查一个 front_sched 的 memtable。
3. flush fold 时同 key 的前台版本裁决不需要跨 scheduler。

### 10.2 `coord_sched` 单线程顺序

`coord_sched` 是单线程的，因此：

1. `batch_lsn` 分配是无间隙的。
2. seal 发起和 write batch fan-out 有确定的先后关系。
3. `publish_gate` 的开/关和 `durable_lsn` 推进不会并发。

### 10.3 `tree_sched` 单线程

1. tree allocator 不需要锁。
2. `tree_manifest` 构造不需要并发保护。
3. `recovery_safe_lsn` 推进不会并发。

### 10.4 跨 scheduler 的 read_handle 安全

reader pin `publish_catalog` 后：
1. catalog 内的 prs（shared_ptr）保证 memtable_gen 和 checkpoint_guard 不会被提前释放。
2. reader 使用的 manifest 是 immutable snapshot，不受新 flush 影响。
3. reader 的 `read_lsn` 固定，不受后续 publish 影响。

## 11. 异常与错误处理

### 11.1 NVMe 写失败（post-LSN）

batch 已拿到 `batch_lsn` 后的任何写入失败都必须触发运行时终止，交给 recovery 收敛（概要 §7.1 规则 7：不允许留下永久 LSN hole 后继续服务）。

1. value object 写失败 → 该 entry 的 WAL 和 memtable insert 不执行 → batch 在 reduce 时观察到异常 → 不 publish → **运行时终止**。
2. WAL FUA 写失败 → 同上。
3. 概要 §7.1 规则 9：如果 value object 已 durable 但 WAL 最终没 durable，该 value object 可立即回收或留给 recovery 当垃圾清理。

注：pre-LSN 阶段（如 canonicalization 失败、参数校验错误）可以安全返回错误给客户端，因为此时尚未分配 `batch_lsn`。

### 11.2 WAL backpressure

segment 分配失败 → 请求排队等待 → flush + reclaim 释放 sealed segments → 恢复分配。
不允许跳过 WAL 写入继续 memtable insert（概要 §11.4 规则 5）。

### 11.3 Data Area 空间不足

tree allocator 和 value allocator 在中间相遇 → 拒绝新 PUT → 返回空间不足错误。
不影响已 commit 数据的安全性。
