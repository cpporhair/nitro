# Inconel 详细设计：运行时状态机

> 依据：`ai_context/inconel/design_overview.md`（唯一概要规范）
>
> 本文细化八类 scheduler 的内部状态、请求类型、handle 逻辑和交互协议。不重复概要中的系统语义，只在需要时引用章节编号。

## 1. Scheduler 总览

| Scheduler | 实例数 | Owner 状态 | 路由策略 |
|-----------|--------|-----------|---------|
| `coord_sched` | 1 | `next_lsn`、`publish_gate`、`current_publish_catalog` | 固定单点 |
| `front_sched` | N（运行时参数） | WAL stream、active/sealed memtable gens | `key_hash % N` |
| `tree_lookup_sched` | K（运行时参数，通常 `< front_sched_count`） | `tree_node` `readonly_frame_cache`、可选 miss-coalescing 表 | `front_owner -> stable home lookup shard` |
| `tree_worker_sched` | K（运行时参数，与 `tree_lookup_sched` 成对） | `read_domain` 引用、candidate-build 临时状态 | `read_domain_index` 对应同 shard `tree_lookup_sched` |
| `tree_sched` | 1 | tree allocator、flush 状态、retire 队列 | 固定单点 |
| `wal_space_sched` | 1 | segment free pool、alloc head | 固定单点 |
| `value_alloc_sched` | 1 | bump head、per-class pools、per-class open_frames、dirty_pages、deferred_freed、value_page `readonly_frame_cache` | 固定单点 |
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
    ready_bitmap ready_set;                          // 跟踪哪些 batch_lsn 已达到终态（publish 或 release）

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
| `release_batch` | `batch_lsn` | void（推进 `durable_lsn`，但该 batch 不产生可见数据） | value/WAL 阶段失败后的异常路径 |
| `acquire_read_handle` | — | `read_handle { cat, read_lsn }` | 读路径入口 |
| `capture_flush_frontier` | — | `flush_frontier { durable_lsn, old_guard }` | flush Phase 1 发起 |
| `frontier_switch` | `(old_guard, new_manifest, retired, flushed_gens_by_front)` | void | flush Phase 3 安装 CAT2 |
| `install_cat` | 新 `publish_catalog` | void | seal / frontier switch |
| `close_gate` | — | void | seal 发起 |
| `open_gate` | — | void | seal 完成 |

### 2.3 handle 逻辑

#### `handle_assign_lsn`

```text
1. canonical_image = canonicalize(raw_batch)
   - 同 key 多步操作 → last-op-wins → 最多一条 PUT(value) 或 DELETE
   - MERGE/INCREMENT → 折叠成等价 PUT/DELETE（不读 DB 状态）
2. batch_lsn = next_lsn++
3. entry_count = |canonical_image|
4. route_table = { key_hash % front_sched_count → [entries] }
5. cb(batch_lsn, canonical_image, entry_count, route_table)
```

关于 canonicalization 的归属：概要冻结的是 "durable boundary 之后只允许看到 canonical image"。本设计把 canonicalization 放在 `coord_sched` 的 `handle_assign_lsn` 内部完成，而非要求调用方预先折叠。原因：

1. `coord_sched` 是单线程入口；先 canonicalize、再分配 `batch_lsn`，可以保证只有成功进入 durable path 的 batch 才消耗 gap-free LSN。
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
       // gate 关闭中（seal 进行中），将 new_prefix 暂存到 gate.pending_advance
       gate.pending_advance = max(gate.pending_advance, new_prefix)
4. cb()
```

`publish_gate` 的职责（概要 §9.2）：阻止 "旧 topology 上的 front-side terminal action（publish / release）越过 seal 边界"。在 gate 关闭期间，`durable_lsn` 不前进，但 `ready_set` 继续累积。gate 重新打开时，`gate.pending_advance` 被应用到新 CAT 上（见 `open_gate`），后续 publish / release 恢复推进。

#### `handle_release_batch`

`release_batch(batch_lsn)` 表示：该 batch 已拿到 `batch_lsn`，但在 memtable phase 之前失败；`coord_sched` 需要把这个 LSN 槽位标记为“已终态解决”，允许连续前缀继续前进，但该 batch 不产生任何可见数据、客户端收到错误而不是 ACK。

```text
1. ready_set.mark(batch_lsn)
2. new_prefix = ready_set.advance_contiguous_prefix(current_durable_lsn)
3. if new_prefix > current_durable_lsn:
   if gate.is_open():
       current_cat->durable_lsn.store(new_prefix, release)
   else:
       gate.pending_advance = max(gate.pending_advance, new_prefix)
4. cb()
```

成立前提：

1. `release_batch` 只允许发生在 memtable phase 之前；
2. 因而 runtime 中不会留下 `data_ver = batch_lsn` 的 memtable entry；
3. 盘上即使留下 partial value / partial WAL 残影，recovery 也会把它们当作未完成 batch 或 orphan 清理。

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

#### `handle_capture_flush_frontier`

```text
1. cat = atomic_load(current_cat)
2. frontier = flush_frontier {
       durable_lsn = cat->durable_lsn.load(acquire),
       old_guard   = cat->prs->tree_guard,
   }
3. cb(frontier)
```

`old_guard` 用 shared_ptr pin 住当前 tree snapshot，使 flush round 后续拿到的 `frontier.old_guard->manifest`
在整轮 fold / write tree / frontier switch 期间都不会被提前释放。

#### `handle_frontier_switch`

```text
输入：
  old_guard            // 本轮 flush 基于的旧 G0
  new_manifest         // 本轮 flush 产出的 immutable manifest
  retired              // 本轮覆盖掉的 old_slots / old_ranges / old_tree_values
  flushed_gens_by_front

1. G0 = old_guard
2. G0.retired.old_slots.append(retired.old_slots)
   G0.retired.old_ranges.append(retired.old_ranges)
   G0.retired.old_tree_values.append(retired.old_tree_values)
3. G1 = checkpoint_guard {
       manifest = new_manifest,
       retired = {},
   }
4. cat = atomic_load(current_cat)
5. D1 = cat->durable_lsn.load(acquire)
6. new_epoch = cat_epoch + 1
7. PRS2 = {
       tree_guard = G1,
       fronts = subtract_flushed_gens(cat->prs->fronts, flushed_gens_by_front),
       epoch = new_epoch,
   }
8. CAT2 = {
       prs = PRS2,
       durable_lsn = D1,
       epoch = new_epoch,
   }
9. install_cat(CAT2)
10. cb()
```

`frontier_switch` 只负责 reader 可见拓扑切换：把本轮 retired 挂到旧 G0，构造新 G1/PRS2/CAT2 并安装。它**不**直接修改各
`front_sched` 的本地 `imms` 链表；本地移除仍由后续 fan-out 的 `release_gens` 完成。

#### `handle_open_gate`

```text
1. gate.open()
2. if gate.pending_advance > current_cat->durable_lsn.load(acquire):
       // 把 seal 期间累积的已 resolved 前缀应用到新 CAT
       current_cat->durable_lsn.store(gate.pending_advance, release)
3. gate.pending_advance = 0
4. cb()
```

**关键**：`gate.pending_advance` 必须在 gate 打开时被消费。如果只写不读，seal 期间完成 fan-out 的 batch 会永久丢失发布机会，因为对应的 `ready_bitmap` bits 已经在 `advance_contiguous_prefix()` 中被消费了。

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

gate 不是 mutex，不涉及线程阻塞。它只是 `coord_sched` 单线程内部的一个状态位，用于控制 `handle_publish_batch` / `handle_release_batch` 是否推进 `durable_lsn`。如果请求在 gate 关闭时到达，它仍然正常完成（更新 ready_set、cb()），只是 `durable_lsn` 暂不前进。

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

    // 正常执行 canonicalize + 分配 lsn ...
```

为什么不挡在 post-lsn：batch 一旦拿到 `batch_lsn`，如果某个 front 卡住而其他 front 完成了，reduce 等不到 → durable_lsn 永远推不过该 batch → 等同于永久 hole（概要 §7.1 rule 7）。

## 3. `front_sched`（前台 Scheduler）

### 3.1 Owner 状态

```cpp
struct front_state {
    uint32_t owner_id;                              // 该 front scheduler 的编号

    // ── Memtable ──
    std::shared_ptr<memtable_gen> active;             // 当前写入目标
    small_vector<std::shared_ptr<memtable_gen>, 8> imms;  // sealed gens（newest → oldest）

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

    // ── Per-gen bump arena ──
    // kv_arena 同时持有 key bytes 与 value bytes。
    //   - key 通过 table 的 std::string_view 指向 arena 切片
    //   - value 通过 memtable_entry.vh.hot (value_view) 指向 arena 切片
    // 单 writer（owning front_sched）、随 gen shared_ptr 一起生灭。
    gen_arena kv_arena;

    // ── 索引结构 ──
    // key 类型是 std::string_view，指向 kv_arena。
    //   - 按 key 字节序有序
    //   - 同 key 可存多个版本，按 data_ver 降序排列
    //   - 声明顺序在 kv_arena 之后，保证反向析构时 table 先于 arena 析构
    absl::btree_map<std::string_view,
                    absl::InlinedVector<memtable_entry, 1>> table;

    // ── 回收 ──
    retire_list<retired_value_ref> loser_durable_refs; // fold 输家的 {value_ref, data_ver}
};

// Per-gen bump allocator. 64KB chunk，超长 allocation 独占 chunk。
struct gen_arena {
    static constexpr std::size_t kChunkBytes = 64 * 1024;
    std::vector<std::unique_ptr<char[]>> chunks;
    char* bump_next = nullptr;
    char* bump_end  = nullptr;

    // Copy len bytes from src into arena, return view over slice.
    // View validity = this arena's validity (= owning gen's validity).
    std::string_view allocate(const char* src, std::size_t len);
};
```

**生命周期管理**：`memtable_gen` 本身**不**嵌 refcount。所有跨域引用（PRS、`front_state.active/imms`、flush round state）都通过 `std::shared_ptr<memtable_gen>` 持有，control block 里的 atomic 就是整条 pin 链的唯一 gate。构造走 `std::make_shared<memtable_gen>(...)`（object + control block 一次分配）。

**kv_arena 的存在理由**：
1. **消灭每 entry 的 per-key heap 分配**：insert 热路径从 `malloc + memcpy` 变成 `bump + memcpy`。
2. **消灭每 PUT 的 `hot_blob` 独立分配**：value bytes 和 key bytes 住同一个 arena，无独立对象、无 deleter、无 `unique_ptr`。
3. **批量释放**：gen 析构一次 sweep 释放所有 chunk，不是百万次 delete。
4. **POD-friendly**：`memtable_entry` 与 `value_handle` 都变成 trivially copyable POD，`absl::InlinedVector` 的 relocate 走 trivial `memcpy` 路径。

**为什么 shared_ptr 而不是 intrusive_ptr**：gen 量级小（每 front 几到十几个），shared_ptr 的 16B pointer + 融合 control block 总占用远小于写一套 intrusive_ptr glue 的维护成本；`memtable_gen` 既不需要 `shared_from_this` 也不需要 `weak_ptr`，切 shared_ptr 没有 corner case。

### 3.3 `memtable_entry`

```cpp
struct memtable_entry {
    uint64_t data_ver;                              // 语义等价于 batch_lsn

    enum class kind : uint8_t { value, tombstone } k;

    // kind == value 时有效；memtable_entry 整体是 trivially copyable POD
    value_handle vh;
};

// POD：durable 是盘上位置；hot 是 value bytes 的视图，指向 owning
// memtable_gen 的 kv_arena 切片。
struct value_handle {
    value_ref  durable;
    value_view hot;
};

// {pointer, length} 视图。本身是 POD；不 own 任何内存。
// 生命周期与 owning memtable_gen 的 shared_ptr 绑定。
struct value_view {
    const char* data;
    uint32_t    len;
};

struct retired_value_ref {
    value_ref vr;
    uint64_t data_ver;                              // 用于和 recovery_safe_lsn 比较
};
```

**value 内存的 owner 模型（冻结）**：

1. value bytes 住在 owning `memtable_gen` 的 `kv_arena` 切片里；`value_handle.hot` 是指向这段切片的只读 view。没有独立的 `hot_blob` 对象。
2. 跨核心读通过 `read_handle → publish_catalog → prs → front_read_set → std::shared_ptr<memtable_gen>` 的 pin 链间接保活，真正的 atomic gate 在 `std::shared_ptr<memtable_gen>` 的 control block——gen 活，arena 活，所有 value_view 有效。
3. 因此 value 模块**不**维护 `value_ref -> hot_blob` 的 materialized 索引；memtable 被 flush、gen 被释放之后，同 key 后续读走 `tree_lookup → value_ref → value_alloc_sched.read_value()`，命中的是 value_page `readonly_frame_cache`。
4. `memtable_entry` 与 `value_handle` 都是 trivially copyable POD；`absl::InlinedVector<memtable_entry, 1>` 的 relocate 是 trivial `memcpy`。

### 3.4 请求类型

| 请求 | 输入 | 输出 | 说明 |
|------|------|------|------|
| `write_wal_entries` | `(batch_lsn, entry_count, entries[])` | void | WAL FUA；value 已由 value_alloc_sched durable |
| `insert_memtable_entries` | `(batch_lsn, entries[])` | void | all-WAL barrier 成功后的 memtable insert |
| `seal_active` | — | `front_read_set` | seal 时由 coord_sched 投递 |
| `batch_lookup` | `(keys[], read_lsn, front_read_set)` | `batch_lookup_results[]` | MultiGet 的 front-local 批量 memtable 查找；只是 `lookup_memtable` 的顺序包装 |
| `lookup_memtable` | `(key, read_lsn, front_read_set)` | `variant<value_view, tombstone, miss>` | point read（搜索 PRS snapshot 的 active/imms，概要 §8.1 step 3）；命中 value 时返回 `value_view` 指向 owning gen 的 `kv_arena` 切片，调用方必须在 read_handle 作用域内使用 |
| `scan_memtable` | `(begin, end, read_lsn, front_read_set)` | `scan_result_set` | range scan（同上） |
| `collect_eligible_gens` | `durable_lsn` | `eligible_gens[]` | flush Phase 1：收集 `max_lsn <= durable_lsn` 的 sealed gens |
| `release_gens` | `gen_id_list` | void | frontier switch 后移除 imms |

### 3.5 前台写入 Handles

> 完整的 pipeline 编排见 `write_path_and_pipeline.md` §6。本节只给出 scheduler handle 级的高层摘要。

#### `handle_write_wal_entries`

value 已由 `value_alloc_sched` 完成 durable（leader-follower FUA）。`coord_sched` 先对所有目标 `front_sched` 并发投递 `write_wal_entries`，并等待 all-WAL reduce 成功；在这一步完成之前，任何 `front_sched` 都**不**允许执行该 batch 的 memtable insert。

```text
// 编码 WAL entries 到 tail_frame->dma_buf
// 提交 WAL pages 到本核 nvme_sched，v1 当前方案为每页 write command 都带 FUA
// 所有相关页的 FUA 完成 → WAL durable
```

**Value-before-WAL 保证**：value FUA 在 `value_alloc_sched` 上已完成，是 front_sched 开始写 WAL 的因果前置条件。不需要 value 和 WAL 在同一 qpair 上。

#### `handle_insert_memtable_entries`

只有当同一 batch 的所有 `write_wal_entries` 都成功后，`coord_sched` 才会启动第二轮 fan-out，并发投递 `insert_memtable_entries`：

```text
for each canonical entry in entries[]:
    // Probe-then-allocate 避免同 gen 重复 key 浪费 arena 空间
    string_view incoming_key{entry.key}
    it = active->table.find(incoming_key)
    if (it == active->table.end()):
        string_view arena_key =
            active->kv_arena.allocate(entry.key.data(), entry.key.size())
        it = active->table.try_emplace(arena_key).first

    if entry.op == PUT:
        // value bytes 走同一个 arena，得到 value_view
        string_view val_slice =
            active->kv_arena.allocate(entry.value_bytes, entry.value_len)
        value_view hot = { val_slice.data(), val_slice.size() }

        it->second.push_back(memtable_entry {
            data_ver = batch_lsn,
            k        = memtable_entry::kind::value,
            vh       = value_handle { .durable = entry.allocated_vr, .hot = hot },
        })
    else:  // DELETE
        it->second.push_back(memtable_entry {
            data_ver = batch_lsn,
            k        = memtable_entry::kind::tombstone,
            vh       = {},                                  // hot = {nullptr, 0}
        })

    active->max_lsn = max(active->max_lsn, batch_lsn)
    active->min_lsn = min(active->min_lsn, batch_lsn)
```

**arena allocate 的两个优化点**：
1. **probe-then-allocate**：先 `find(incoming_key)`，不命中才 `allocate` 到 arena 里。跨 batch 写同一 key 时只有第一次占 arena，之后只在 bucket 里 push 新版本。
2. **tombstone 不占 arena**：只 key 要 allocate，value 侧直接 `{nullptr, 0}`，一个 byte 都不占用。

关键约束：

1. `insert_memtable_entries` 之前必须已有 all-WAL barrier 成功；
2. 一旦开始 memtable phase，该 batch 的失败语义不再是 `release_batch`，而是运行时终止；
3. `coord_sched` 不得让 `seal_active` 插在同一 batch 的 `write_wal_entries` 和 `insert_memtable_entries` 两轮 fan-out 之间。

### 3.6 `handle_seal_active`

```text
1. F = active                           // 旧 active → shared_ptr<memtable_gen>
2. F->st = sealed
3. N = std::make_shared<memtable_gen>(gen_id = next_gen_id++, st = active)
4. active = N                           // 旧 F 仍被 imms 和 PRS 共享持有
5. imms.push_front(F)                   // F 进入 imms 最前面（最新）
6. cb(front_read_set { active = N, imms = [F] ++ old_imms })
```

**不变量**（概要 §7.1 补充）：同一 batch 不会跨这次 seal 边界裂成两代。这由 `coord_sched` 的单线程顺序保证：seal_active 请求不会插入到同一 batch 的 `write_wal_entries` fan-out 与 `insert_memtable_entries` fan-out 之间；在单个 `front_sched` 上，这两类请求与 `seal_active` 也都按入队顺序执行。

### 3.7 `handle_lookup_memtable`

搜索的是调用方传入的 PRS snapshot 中的 active/imms（概要 §8.1 step 3："查 `cat->prs` 对应的 active + imms"），**不是** front_sched 当前的 active/imms。原因：frontier switch 后 `release_gens` 会从 front_sched 本地 imms 移除已 flush 的 gens，但旧 reader pin 的 PRS 仍引用这些 gens，其 tree_guard 也不含这些 gens 的数据。如果搜索 front_sched 当前 imms，旧 reader 会丢数据。

```text
handle_lookup_memtable(key, read_lsn, frs):
    // frs = 调用方 read_handle 的 cat->prs->fronts[owner]
    // frs.active / frs.imms 由 PRS snapshot 的 std::shared_ptr<memtable_gen> 保活

1. 在 frs.active 中查找 key，收集所有 data_ver <= read_lsn 的 entries
2. 在 frs.imms 中从新到旧查找 key，收集所有 data_ver <= read_lsn 的 entries
3. winner = 所有命中中 data_ver 最大的那条
4. if winner 不存在:
       cb(miss)
   else if winner.kind == value:
       cb(winner.vh.hot)      // winner.vh.hot 本身就是 value_view，直接转发
   else:
       cb(tombstone)          // 上层返回 not found
```

**Zero-copy view 的生命周期契约**：`cb` 回传的 `value_view` 指向 owning gen 的 `kv_arena` 切片，其有效期与调用方当前持有的 `read_handle` 绑定。调用链是 `read_handle → cat → prs → front_read_set → std::shared_ptr<memtable_gen> → memtable_entry → value_handle.hot → kv_arena chunk`——只要 `read_handle` 未释放，`shared_ptr<memtable_gen>` 的 `use_count > 0`，arena 的所有 chunk 都活着，view 就是稳定可读的。调用方必须在 `read_handle` 作用域内完成 view 的消费（例如写到 RESP/RPC 响应 buffer）；不允许把 view 保存在比 read_handle 更长寿的对象里。

**线程安全**：dispatch 到 front_sched 执行是为了保证 btree_map 读操作的线程安全。sealed gen 是 immutable 的；PRS 中的 active gen 可能仍在被 front_sched 写入，但 lookup 在 front_sched 单线程上执行，与 `insert_memtable_entries` 串行，不存在竞争。

**约束**：不能因为在 `frs.active` 中命中 key 就提前返回。`front_read_set` 中各 gen 的拓扑顺序不保证同一 key 的 `data_ver` 单调递减；`lookup_memtable` 的唯一正确 winner 规则仍然是“在 `active + imms` 的所有命中中取最大 `data_ver`”。

#### `handle_batch_lookup`

`batch_lookup(keys[], read_lsn, front_read_set)` 是 front-side 的正式 handle，用于 MultiGet；它不引入新的可见性语义，只是把同一个 `front_read_set` 上的多个 `lookup_memtable()` 串行打包：

```text
results = []
for key in keys[]:
    results.push({ key, lookup_memtable(key, read_lsn, front_read_set) })
cb(results)
```

它存在的意义只是：

1. 明确这是一个跨文档可引用的 scheduler handle，而不是 RAP 里的临时 helper 名字；
2. 让 MultiGet 可以按 owner 做一次 front-local fan-out，而不是对每个 key 单独发消息。

### 3.8 Flush 辅助 Handles

#### `handle_collect_eligible_gens`

```text
1. eligible_gens = [ gen in imms where gen.max_lsn <= durable_lsn ]
2. cb(eligible_gens)
```

返回的是 `std::shared_ptr<memtable_gen>` snapshot。flush round 在 reduce 后持有这些引用，因此即使后续有新的
seal/frontier switch，本轮选中的 gens 也不会在 fold 结束前被提前释放。

#### `handle_release_gens`

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
5. // v1 当前方案：本次触达的每个 WAL page write command 都带 FUA
   // 当前 tail page（可能包含本次 entry 和同页的旧 entries）写回时使用 FUA
   fua_write(active_seg.base_paddr + floor(write_offset - 1, LBA_SIZE), tail_frame->dma_buf)
   // 如果 entry 跨页，则每个被写回的页都各自带 FUA
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

## 4. Tree Domain（`tree_sched` / `tree_lookup_sched` / `tree_worker_sched`）

tree 域拆成三类角色：

1. `tree_sched`
   - 单实例
   - 负责 flush round owner 状态、tree allocator、manifest delta、bounded writes、reclaim
2. `tree_lookup_sched`
   - `K` 实例
   - 负责普通读路径 tree traversal，以及 flush 中的 `keys_to_leaf_groups()`
3. `tree_worker_sched`
   - `K` 实例
   - 负责 flush 中的 old leaf read / decode / candidate materialization

启动期按 read-domain 成对部署：

```cpp
struct tree_read_domain {
    uint32_t read_domain_index;
    readonly_frame_cache node_cache;                // tree_node domain clean frames
    tree_lookup_sched* lookup;
    tree_worker_sched* worker;
};
```

`tree_lookup_sched` 与 `tree_worker_sched` 共享各自 `tree_read_domain` 的 `node_cache` shard；`tree_sched` 不再持有 tree-node read cache。

### 4.1 Owner 状态

```cpp
struct tree_state {
    // ── Allocator ──
    tree_allocator alloc;                            // Data Area 低端分配器
    // alloc.head = 当前已分配的最高地址 + 1

    // ── Flush 状态 ──
    uint64_t flush_max_lsn;                          // 已 flush 进 tree 的最大 batch_lsn
    uint64_t superblock_safe_lsn;                    // 当前 on-disk superblock root 已能覆盖到的最大 flush frontier
    uint64_t recovery_safe_lsn;                      // 对 WAL/value 回收安全的下界

    // ── Retire 队列 ──
    // 由旧 checkpoint_guard 的 destructor 投递过来的回收任务
    per_core::queue<reclaim_task*> reclaim_q;
};
```

### 4.2 Tree Domain 请求类型

```cpp
struct retired_objects {
    small_vector<paddr, 32> old_slots;              // 被新版本取代的旧 slot
    small_vector<range_ref, 8> old_ranges;          // consolidation 后的旧 range
    small_vector<retired_value_ref, 64> old_tree_values; // 被覆盖/删除的旧 tree-visible {value_ref, data_ver}
};

struct tree_flush_request {
    std::shared_ptr<const checkpoint_guard> base_guard;
    span<std::shared_ptr<memtable_gen>> sealed_gens;
    uint64_t recovery_safe_lsn;
};

struct tree_flush_result {
    bool ok;
    std::shared_ptr<const tree_manifest> new_manifest;
    retired_objects retired;
    flat_hash_map<uint32_t, small_vector<std::shared_ptr<memtable_gen>, 8>> flushed_gens_by_front;
    small_vector<retired_value_ref, 64> memtable_losers;
    uint64_t flushed_max_lsn;
    flush_error error;
};
```

| Scheduler | 请求 | 输入 | 输出 | 说明 |
|-----------|------|------|------|------|
| `tree_sched` | `tree_flush` | `tree_flush_request` | `tree_flush_result` | 只覆盖 tree-local flush pipeline；frontier switch / gen release 在外层流程消费 result |
| `tree_sched` | `reclaim` | `reclaim_task` | void | TRIM old slots/ranges, recycle value extents |
| `tree_sched` | `update_superblock` | `(root_base_paddr, covered_lsn)` | void | root-change flush 后异步更新；完成后推进 `superblock_safe_lsn` |
| `tree_lookup_sched` | `tree_lookup` | `(key, manifest)` | `variant<leaf_value, leaf_tombstone, absent>` | 普通读路径的 tree traversal |
| `tree_lookup_sched` | `keys_to_leaf_groups` | `flush_lookup_req` | `flush_leaf_group_result[]` | 基于 `manifest->leaf_order` 做 key-group 到 affected leaf 的映射 |
| `tree_worker_sched` | `build_leaf_candidates` | `flush_worker_req` | `flush_candidate_batch` | 读 old leaf / merge / compact，返回 candidate proposals |

注：

1. `tree_lookup` / `tree_scan` 不是 `tree_sched` 的请求。读路径绝不访问 tree_sched 的当前可变状态。
2. `keys_to_leaf_groups` 与 `build_leaf_candidates` 是 tree-local flush 的内部 sender seam，不对外暴露成前台 API。

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
    local::queue<range_ref, 4096> free_ranges;       // 已回收可重用的 range（入队前必须完成 tree_node invalidate barrier）
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
        invalidate_tree_node_range_on_all_shards(r); // wait-all-acks；发现 pinned frame = 生命周期 bug
        free_ranges.try_enqueue(r);
    }
};
```

### 4.5 `tree_manifest`

```cpp
struct leaf_span {
    key_fence lower_bound_inclusive;
    key_fence upper_bound_exclusive;                // last leaf may use +inf sentinel
    paddr     leaf_range_base;
};

struct tree_manifest {
    paddr root_slot;                                 // 当前 root 的精确 slot 地址

    // 每个 range 在该 snapshot 下应读哪个 slot
    // 实现选择 slot_index（0-based offset within range）
    // exact_paddr = range_base + slot_index * tree_page_size / lba_size
    flat_hash_map<paddr/*range_base*/, uint32_t/*slot_index*/> slot_map;

    // runtime-only immutable metadata
    // 按 key range 有序，覆盖当前 snapshot 下全部可达 leaf
    small_vector<leaf_span, 0> leaf_order;

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

`leaf_order` 的语义冻结为：

1. 只属于 runtime manifest，不单独持久化；
2. 跟 manifest / guard 同生命周期；
3. 覆盖当前 manifest 下全部 leaf，按 key range 有序且区间不重叠；
4. 只保存稳定的 key fence / range base，不保存 frame 指针；
5. flush 中的 `keys_to_leaf_groups()` 必须依赖它，不能退化成“逐 key 独立从 root descend”。

### 4.6 `checkpoint_guard`

```cpp
struct checkpoint_guard {
    std::shared_ptr<const tree_manifest> manifest;

    // ── 回收对象 ──
    retired_objects retired;

    ~checkpoint_guard() {
        if (!retired.empty()) {
            // 投递 reclaim_task 到 tree_sched
            // 注意：destructor 不直接发 NVMe 命令（概要 §3.1 约束 5）
            tree_sched->enqueue_reclaim(std::move(retired));
        }
    }
};
```

### 4.7 `tree_lookup_sched` 与 Tree Lookup

为避免把 `tree_node` read cache 在每个 `front_sched` 上重复一份，同时又不把共享锁带进 front hot path，读侧引入少量 `tree_lookup_sched`。它们是 tree traversal 的执行者，并访问所属 `tree_read_domain` 的 read-only cache shard。

```cpp
struct tree_lookup_state {
    uint32_t lookup_id;
    tree_read_domain* read_domain;                  // node_cache 位于 read-domain shard 上

    // 可选：对同一 slot 的并发 miss 做 single-flight 合并
    flat_hash_map<frame_id, pending_read*> inflight_reads;
};
```

路由规则：

1. 启动期建立稳定映射 `home_tree_lookup(front_owner)`。
2. point GET / MultiGet 的 tree miss 路由到 owner `front_sched` 对应的 home `tree_lookup_sched`。
3. `same key -> same front_sched` 因而也推出 `same key -> same tree_lookup_sched`（在同一次运行期间）。
4. `tree_lookup_sched` 的实例数 `K` 通常小于 `front_sched_count`，并优先按 NUMA 分组放置；默认优先选择与 owner `front_sched` 同 NUMA 的 shard。
5. range scan 的 tree-side 遍历整体路由到一个选定的 `tree_lookup_sched` shard；默认选择调用方 NUMA 上的本地 shard，而不是把 leaf frames 借回多个 `front_sched`。

tree_lookup **不在 tree_sched 上执行**。它在路由得到的 `tree_lookup_sched` 上执行，通过该 scheduler 本核的 `nvme_sched` 异步读 page。

原因：tree_lookup 的依赖（immutable manifest、NVMe read、`tree_read_domain.node_cache`）全部不需要 tree_sched 的可变状态；但如果仍把 tree traversal 留在 `front_sched`，又会引入跨 scheduler 借 frame / 归还 frame 的生命周期问题。把 tree traversal 与 tree-node cache 一起放进 `tree_lookup_sched`，可以保持 owner-local、无锁、无借页。

```text
// 在 home tree_lookup_sched 上执行
tree_lookup(key, manifest):
1. if manifest->root_slot == null:
       return absent
2. slot_paddr = manifest->root_slot
3. root_page = read_domain->node_cache.get_or_read(slot_paddr)
   // cache miss → 本核 nvme_sched->read → 回填 cache
4. page = root_page
5. 从 root 开始 B+ tree 下降：
   for each internal level:
       child_range_base = find_child(page, key)
       child_slot_paddr = manifest->resolve(child_range_base)
       page = read_domain->node_cache.get_or_read(child_slot_paddr)
6. leaf_page = page
7. record = leaf_page.find(key)
8. if record 不存在:
       return absent
   else if record.kind == value:
       return leaf_value_record { data_ver, value_ref }
   else:
       return leaf_tombstone { data_ver }
```

**node frame cache**：每个 `tree_read_domain` 拥有一个 `readonly_frame_cache` shard（tree_node domain），由该 domain 的 `tree_lookup_sched` / `tree_worker_sched` 共享。tree page 以 `page_frame { dom = tree_node, st = clean_readonly }` 形式驻留在 DMA 内存中。遍历期间通过 `pin_count` 防止驱逐；pin 释放后按 LRU / clock 可淘汰。cache 驱逐不影响正确性，因为 manifest 持有结构信息，page 可以随时从 NVMe 重新读取。

v1 的 tree cache key 仍是裸 `frame_id { slot_paddr, span_lbas, domain::tree_node }`。它之所以安全，不是因为 slot 地址永不复用，而是因为 old range 在进入 `tree_allocator.free_ranges` 前必须先完成跨 shards 的 `tree_node` invalidate barrier（见 `runtime_memory_and_cache.md` §10.2）。

flush 中还有一个 lookup-only handle：

```text
keys_to_leaf_groups(sorted_flush_key_groups, manifest):
1. assert(manifest->leaf_order 覆盖全空间、严格有序、无重叠)
2. 用 key-sorted groups 与 manifest->leaf_order 做顺序 merge
3. 为每个命中的 leaf 产出：
   { leaf_range_base, old_slot_paddr = manifest.resolve(leaf_range_base), touched_key_groups[] }
4. 相邻 key groups 若命中同一 leaf，可在本 shard 先局部合并
5. 返回 flush_leaf_group_result[]
```

约束：

1. 只能依赖 `manifest->leaf_order` 做 batch leaf mapping；
2. 不允许退化成“每个 key 独立从 root descend”；
3. 不允许扫全树 leaf；
4. 最终跨 shard 的 dedupe / merge 必须回到 `tree_sched` 完成。

### 4.8 `tree_worker_sched` 与 Candidate Build

```cpp
struct tree_worker_state {
    uint32_t worker_id;
    tree_read_domain* read_domain;                  // 与 lookup shard 共享 node_cache
};
```

`tree_worker_sched` 只做 page materialization，不拥有 tree allocator、manifest mutation 或 retire list。

```text
build_leaf_candidates(req):
1. for each affected leaf in req.leaf_groups:
   a. old_slot = req.manifest.resolve(leaf_range_base)
   b. old_page = read_domain->node_cache.get_or_read(old_slot)
   c. decode old leaf records
   d. merge(old leaf records, memtable winners for this leaf)
   e. compact tombstones:
      if record.kind == tombstone
         && record.data_ver <= req.recovery_safe_lsn:
           omit(record)   // tombstone -> absent
   f. if merged image == old image:
         return zero-write candidate
   g. if merge 后需要 split / merge / parent rewrite / root change:
         return unsupported_shape_change
   h. return flush_leaf_candidate { leaf_range_base, old_slot, candidate_image }
2. collect all candidates and return to tree_sched
```

冻结约束：

1. worker 只负责 old leaf read / decode / merge / compact / candidate image materialization；
2. worker 不分配 slot/range，不修改 manifest，不挂接 retired；
3. `shape change` 路径可以返回 `unsupported_*`，由后续步骤扩展；
4. `tree_sched` 在收齐 candidates 后，才允许进入 tree delta planning 与 bounded writes。

### 4.9 `recovery_safe_lsn` 推进

`recovery_safe_lsn` 表达的是：所有 `data_ver <= recovery_safe_lsn` 的历史旧版本，如果不是当前 durable winner，就不可能再从 recovery 输入中出现（概要 §6 额外约束 3）。

推进条件：

```text
if 所有 sealed WAL segments 都已回收（或无 sealed segments）:
    wal_frontier = flush_max_lsn
else:
    wal_frontier = 所有 sealed WAL segments 中最小的 min_lsn - 1

recovery_safe_lsn = min(
    flush_max_lsn,               // tree 已经物化到这里
    superblock_safe_lsn,         // 当前 on-disk superblock root 已能覆盖到这里
    wal_frontier,                // 即使崩溃，比这更早的 WAL 已不会出现
)
```

`superblock_safe_lsn` 的语义是：

1. **root-stable flush**
   - 当前 on-disk superblock 记录的 `root_base_paddr` 与本轮 flush 后的 root range base 相同。
   - recovery 即使不更新 superblock，也能从该 root range 扫描到最新 root slot。
   - 因此在 `nvme_flush` 完成后，`superblock_safe_lsn` 可以直接推进到本轮 `flush_max_lsn`，不需要额外 metadata IO。

2. **root-change flush**
   - 本轮 flush 改变了 `root_base_paddr`。
   - 在异步 superblock 更新完成前，recovery 仍只会从旧 root base 出发，因此必须依赖 WAL 补齐这轮及其后的变更。
   - 因此在 superblock update completion 之前，`superblock_safe_lsn` 保持旧值；完成后再推进到该次更新覆盖的 `covered_lsn`。

简化理解：`recovery_safe_lsn` 取决于三条边界同时满足：

1. tree 已 durable（`flush_max_lsn`）
2. 当前 on-disk superblock root 已经“看得到”这些 flushed 结果（`superblock_safe_lsn`）
3. 比它更老的 WAL 已经不再需要作为 recovery 输入（`wal_frontier`）

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

集中管理 value page 的分配、DMA frame 填充、NVMe FUA 写入（leader-follower 模式），以及 **value read 服务**（tree-path value 读取的唯一执行域）。通过本核 `nvme_sched` 提交 value FUA 写入和 value page 读取。

最佳实践是把它部署在独占核心上；但语义上只要求它保持单实例 owner。读写共享同一份 `readonly_frame_cache`（value_page domain），避免跨 shard 缓存重复和 invalidate 开销。

| 属性 | 值 |
|------|---|
| 实例数 | 1（全局唯一） |
| Owner 状态 | bump head（per-device）、per-class pools（whole_page_pool / hole_page_list / extent_free_pool）、owner-local `generic_free_spans`、dirty_pages、deferred_freed、per-class open frames、本地 `readonly_frame_cache`（value_page domain，读写共享） |
| NVMe I/O | 通过本核 nvme_sched 提交 value FUA 写入和 value page 读取 |
| 路由 | 固定单点 |

### 6.2 请求类型

| 请求 | 输入 | 输出 | 调用者 |
|------|------|------|--------|
| `persist_put_values` | batch PUT entries | durable value_refs | coord_sched（leader-follower：多 batch 合并） |
| `read_value` | `value_ref` | owning value bytes | 读路径（tree hit value 后） |
| `read_page_values` | `value_read_group { page_fid, refs[] }` | owning value bytes[] | 读路径（MultiGet / Scan 的 tree hit values） |
| `freed_slots` | page_base, class_idx, freed_mask | void | tree_sched（sub-LBA value 回收） |
| `recycle_whole` | class_idx, page_base | void | tree_sched（LBA-aligned value 回收，TRIM 由 tree_sched 先完成） |
| `install_recovered_state` | `live_extents`, `global_value_head`, `data_area_end_lba`, optional `dead_class_hints` | void | boot recovery（一次性初始化） |

注：

1. `alloc_page` 和 `return_page` 不再是跨 scheduler 请求——它们是 `persist_put_values` 内部的本地操作（value_alloc_sched 自己持有 open frames 和 per-class pools）。
2. `read_value` 是 Point GET 使用的单值包装；`read_page_values` 是 MultiGet / Scan 使用的页级批量原语。两者都由 `value_alloc_sched` 服务：先查 dirty open frames，再查 value_page `readonly_frame_cache`，miss 时通过本核 `nvme_sched` 读取并回填 cache。返回 copy 后的 owning bytes，调用方无需管理 DMA frame 生命周期。
3. `install_recovered_state` 是 boot-only 初始化接口；steady-state 下不走这条路径。recovery 只交出 occupied truth，allocator 内部自行重建 `hole_pages` / pools / `generic_free_spans`。

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

    struct free_span_descriptor {
        paddr base;
        uint32_t span_lbas;
        uint16_t class_idx_or_invalid;               // UINT16_MAX = 暂无可用 class hint
    };

    // ── Per-class pools ──
    struct per_class_state {
        flat_hash_map<paddr, hole_page_descriptor> hole_pages;  // 带空洞页
        local::queue<paddr, 256> whole_page_pool;               // 整页空闲
        local::queue<paddr, 64>  extent_free_pool;              // multi-LBA extent
    };
    small_vector<per_class_state, 16> classes;

    // ── Whole-free region，但暂时无法归入某个 class pool ──
    // 典型来源：boot recovery 后，某些 region 语义上确定 free，
    // 但没有足够 surviving refs 可立即唯一推断 class。
    intrusive_list<free_span_descriptor> generic_free_spans;

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

`install_recovered_state()` 的职责边界固定为：

1. recovery 提供 `live_extents`（occupied truth）和 `global_value_head`；
2. `value_alloc_sched` 先清空旧的 free metadata / open frame 残留，再从 `live_extents` 反推出：
   - sub-LBA partially-free 页的 `hole_pages`
   - class 可判定的 `whole_page_pool` / `extent_free_pool`
   - class 暂不可判定的 `generic_free_spans`
3. `dead_class_hints` 只用于 class 归桶或 TRIM 优化；没有它也不能泄漏 free 空间。

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

其中 `whole_page` source 只会来自已经完成过 `value_page` invalidate barrier 的 free pool；因此这里允许继续用裸 `frame_id` 命中本地 cache，而不会把旧页像误认成新对象。

### 6.5 `handle_read_value` / `handle_read_page_values`

Tree-path value 读取统一在 `value_alloc_sched` 上完成。Point GET 使用 `handle_read_value(value_ref)`；MultiGet / Scan 先在调用方按 value page 分组，再调用 `handle_read_page_values(value_read_group)`。

```cpp
struct value_read_group {
    frame_id page_fid;                               // 同一 value page
    small_vector<value_ref, 8> refs;                 // 全部属于 page_fid
};
```

```text
handle_read_value(value_ref vr):
    group = build_single_value_group(vr)
    values = handle_read_page_values(group)
    cb(values[0])

handle_read_page_values(value_read_group group):
    fid = group.page_fid

    // ── 1. 先查 dirty open frames（当前正在填充的页）──
    if hit_open_frame(fid) → frame:
        goto serve_from_frame

    // ── 2. 查 readonly_frame_cache ──
    if frame = readonly_frame_cache.get(fid):
        goto serve_from_frame

    // ── 3. Cache miss → NVMe read ──
    frame = alloc_dma_frame(fid)
    nvme_sched->read(fid.base, frame->dma_buf, fid.span_lbas * lba_size)
    // NVMe read 完成后：
    frame->st = clean_readonly
    readonly_frame_cache.put(fid, frame)
    goto serve_from_frame

serve_from_frame:
    result = []
    for vr in group.refs:
        header = reinterpret_cast<value_object_header*>(frame->dma_buf + vr.byte_offset)
        assert(header->magic == VALUE_MAGIC)
        assert(header->body_len == vr.len)
        verify_crc32c(frame->dma_buf + vr.byte_offset + sizeof(header), vr.len, header->body_crc)
        result.push(copy_bytes(frame->dma_buf + vr.byte_offset + sizeof(header), vr.len))
    cb(result)
```

**设计要点**：

1. **请求内按页分组**：MultiGet / Scan 先在调用方按 `frame_id` 分组，避免把同页多个 `value_ref` 变成多条独立消息。
2. **dirty frame 命中**：sub-LBA page 停留在 `open_frames` 的时间窗口内，tree-path read 可直接从 DMA buffer 读取，零 NVMe。这对频繁 flush + 小 value 的工作负载有意义。
3. **copy 返回**：返回 owning bytes 而非 `value_view` + `frame_pin`。DMA frame 生命周期完全封闭在 `value_alloc_sched` 内部，`pin_count` 保持 `uint32_t` 不需要 atomic。copy 发生在 CRC 校验后，数据在 L1/L2 中是热的，成本最低。上层（如网络发送）最终也需要 copy，在这里做等价于在那里做。
4. **无 coherence 开销**：value_alloc_sched 既是 writer 又是唯一的 cache owner。hole-fill writeback 后直接更新本地 cache，不需要跨 shard invalidate barrier。

### 6.6 `handle_return_page`

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

`handle_return_page()` 只更新 allocator metadata。writeback completion 后，`value_alloc_sched` 直接把 updated frame 转为 `clean_readonly` 放回本地 cache。无需跨 shard invalidate（`value_alloc_sched` 是 value_page cache 唯一 owner）。

### 6.7 `handle_freed_slots`（sub-LBA 回收核心）

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

### 6.8 `recycle_whole_page` 统一路径

```text
recycle_whole_page(class_idx, page_base):
    1. 从本地 readonly_frame_cache 删除该 page 的 frame（如有）
    2. 放回 classes[class_idx].whole_page_pool
    // TRIM 由调用方（tree_sched）在投递 recycle 之前完成（等待 TRIM 回调后再投递）
    // 无需跨 shard invalidate（value_alloc_sched 是 value_page cache 唯一 owner）
```

multi-LBA extent 的 `handle_recycle_whole` 同理：从本地 cache 删除覆盖该 extent 的 frames，再放回 `classes[class_idx].extent_free_pool`。

### 6.9 TRIM 顺序协议

`recycle_whole_page` 路径中，TRIM 必须在回收之前完成。因为 per-core 模型下 TRIM（tree_sched 核心的 qpair）和后续写入（value_alloc_sched 核心的 qpair）在不同 qpair 上，没有隐式顺序保证：

```text
tree_sched: submit_trim(page_base) → 等待 TRIM 完成回调
tree_sched: freed_slots / recycle_whole → value_alloc_sched
// TRIM 已完成 → 页可安全重分配
// value_alloc_sched 拿到页后写入，不会被迟到的 TRIM 覆盖
```

TRIM 在回收路径上（非写关键路径），等待完成不影响前台延迟。

### 6.10 `dirty_pages` / `deferred_freed` 生命周期

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

`fronts` 中的每个 `front_read_set` 持有 `std::shared_ptr<memtable_gen>`。当 prs 释放时，这些引用一起释放。

### 8.3 `value_handle` 生命周期

```text
创建（insert_memtable_entries 内）：
  1. active gen 的 kv_arena.allocate() 存 key bytes（probe-then-allocate）
  2. active gen 的 kv_arena.allocate() 存 value bytes → 得到 value_view
  3. memtable_entry.vh = { durable = value_ref, hot = value_view }
  4. btree_map 的 bucket push 新版本

存活期间：
  - gen 活 → kv_arena 活 → arena 中所有 chunk 活 → key/value bytes 稳定可读
  - memtable hit 时 lookup_memtable 直接返回 value_handle.hot（zero-copy view）
  - value bytes 独立于 page/frame cache（见 runtime_memory_and_cache.md §9.3）
  - tree-path value read 使用 `read_value(value_ref)` copy 返回 owning bytes，不复用 arena

释放触发：
  - memtable_gen 从所有 published_read_set 中摘除
  - 最后一个 shared_ptr<memtable_gen> 析构 → use_count 归 0 → gen 析构
  - gen 析构按反向声明顺序：先 table（只存 view，析构 trivial），后 kv_arena
  - kv_arena 析构 → vector<unique_ptr<char[]>> 析构 → 所有 chunk 一次 free
  - 所有 key/value bytes 同时消失；value_ref 进入回收判定（见下）

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
  ├── fan-out WAL ──────────────────────────────────────────┤
  │                                                         │
  │  ┌─────────────────┐  ┌─────────────────┐              │
  │  │ front_sched[0]  │  │ front_sched[1]  │  ...         │
  │  │ write_wal_entries() │ │ write_wal_entries() │        │
  │  │  (value 已durable)│ │  (value 已durable)│            │
  │  │  1. WAL FUA     │  │  1. WAL FUA     │              │
  │  └────────┬────────┘  └────────┬────────┘              │
  │           │                    │                        │
  │           └────── reduce ──────┘                        │
  │                                                         │
  ├── fan-out memtable ─────────────────────────────────────┤
  │                                                         │
  │  ┌─────────────────┐  ┌─────────────────┐              │
  │  │ front_sched[0]  │  │ front_sched[1]  │  ...         │
  │  │ insert_memtable_entries() │ │ insert_memtable_entries() │
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
tree_sched ── fold memtables into sorted key groups ─────────┤
  │  生成 memtable winners / losers                           │
  │                                                           │
  ▼                                                           │
tree_lookup_scheds ── keys_to_leaf_groups ── reduce ─────────┤
  │  基于 manifest.leaf_order 做 affected leaf mapping         │
  │                                                           │
  ▼                                                           │
tree_worker_scheds ── build_leaf_candidates ── reduce ───────┤
  │  读 old leaf / merge / compact                            │
  │                                                           │
  ▼                                                           │
tree_sched ── plan delta + submit writes ── nvme_sched(s) ───┤
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
// retired 由 old G0 析构时自动 enqueue reclaim
tree_sched ── maybe update superblock
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

### 10.3 tree-domain schedulers 单线程

1. `tree_sched` 上的 tree allocator 不需要锁。
2. `tree_sched` 上的 `tree_manifest` 构造不需要并发保护。
3. `tree_sched` 上的 `recovery_safe_lsn` 推进不会并发。
4. 每个 `tree_lookup_sched` 的 node cache / miss-coalescing 状态也都是 owner-local，不需要锁。
5. 每个 `tree_worker_sched` 的 candidate build 临时状态与 read-domain 访问同样是 owner-local，不需要锁。

### 10.4 跨 scheduler 的 read_handle 安全

reader pin `publish_catalog` 后：
1. catalog 内的 prs（shared_ptr）保证 memtable_gen 和 checkpoint_guard 不会被提前释放。
2. reader 使用的 manifest 是 immutable snapshot，不受新 flush 影响。
3. reader 的 `read_lsn` 固定，不受后续 publish 影响。

## 11. 异常与错误处理

### 11.1 post-LSN 失败矩阵

batch 已拿到 `batch_lsn` 后，失败语义按 batch 当前所处阶段区分：

1. **value phase 失败**
   - `coord_sched.release_batch(batch_lsn)`
   - 客户端收到错误
   - 若已有 partial durable value，则按 orphan 路径处理

2. **all-WAL barrier 之前的 WAL 失败**
   - 该 batch 还未进入 memtable phase
   - `coord_sched.release_batch(batch_lsn)`
   - 客户端收到错误
   - 若已有 partial durable WAL/value，recovery 会按未完成 batch / orphan 清理

3. **memtable phase 失败**
   - 此时 all-WAL barrier 已经成功，batch 不再允许 clean release
   - 必须触发运行时终止，交给 recovery 收敛

概要 §7.1 规则 9 仍成立：如果 value object 已 durable 但 WAL 最终没 durable，该 value object 可立即回收或留给 recovery 当垃圾清理。

注：pre-LSN 阶段（如 `coord_sched.handle_assign_lsn` 内的 canonicalization 失败、参数校验错误）可以安全返回错误给客户端，因为此时尚未分配 `batch_lsn`。

### 11.2 WAL backpressure

segment 分配失败 → 请求排队等待 → flush + reclaim 释放 sealed segments → 恢复分配。
不允许跳过 WAL 写入继续 memtable insert（概要 §11.4 规则 5）。

### 11.3 Data Area 空间不足

tree allocator 和 value allocator 在中间相遇 → 拒绝新 PUT → 返回空间不足错误。
不影响已 commit 数据的安全性。
