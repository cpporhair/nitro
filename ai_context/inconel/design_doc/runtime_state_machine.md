# Inconel 详细设计：运行时状态机

> 依据：`ai_context/inconel/design_overview.md`（唯一概要规范）
>
> 本文细化八类 scheduler 的内部状态、请求类型、handle 逻辑和交互协议。不重复概要中的系统语义，只在需要时引用章节编号。

## 1. Scheduler 总览

| Scheduler | 实例数 | Owner 状态 | 路由策略 |
|-----------|--------|-----------|---------|
| `coord_sched` | 1 | `next_lsn`、`publish_gate`、`current_publish_catalog` | 固定单点 |
| `front_sched` | N（运行时参数） | WAL stream、active/sealed memtable gens | `key_hash % N` |
| `tree_read_domain` | K（运行时参数，通常 `< front_sched_count`） | own lookup / worker scheduler；持 `tree_node` `readonly_frame_cache` shard、路由用 `shard_partition_map` snapshot | `current_shard_partitions()->route(key)` 一次二分得 `shard_idx`（step 030 §2.7） |
| ↳ `tree_lookup_sched` | 与 `tree_read_domain` 1:1 | miss-coalescing 表、lookup-local frame pool | 继承 `tree_read_domain_index`；cache 访问走 `read_domain_->node_cache` |
| ↳ `tree_worker_sched` | 与 `tree_read_domain` 1:1 | candidate-build 临时状态 | 继承 `tree_read_domain_index`；cache / paired_lookup 均通过 `read_domain_->...` 间接拿 |
| `tree_sched` | 1 | tree allocator、flush 状态、retire 队列 | 固定单点 |
| `wal_space_sched` | 1 | segment free pool、alloc head | 固定单点 |
| `value_alloc_sched` | 1 | `value_space_manager`（free-space / partial / cached-partial / trim / floor metadata）、round/resident segmented frames、value_page `readonly_frame_cache` | 固定单点 |
| `nvme_sched` | 每核心 × 每设备（v1 单盘 = 每核心 1 个） | SPDK qpair | 各 scheduler 使用本核心的实例 |

**路由层提醒（step 030 §2.7）**：`shard_partition_map` 是读路径（`tree::lookup`）和 flush fold 路径（`memtable_fold.build_key_partitions`）唯一共享的路由决策源，确保"同 key → 同 shard" 不变量在两条路径上同时成立。map 由 builder 在启动时装入一个单 shard 占位（空树 → `{ upper=+∞, idx=0 }`），每次 flush 完成由 tree_sched 重建并 `install_shard_partitions()` 原子替换（B1 目标设计；step 030 不触发 rebuild，仅建立占位 + API）。

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

    // ── Seal/Flush CAT 安装互斥（055 §6）──
    // 统一串行标志：seal 与 flush 都在 coord 上安装 CAT（CAT1 / CAT2），必须互斥。
    // capture_flush_frontier / close_gate 置位（已置位则 fail-fast）；
    // end_flush_round / open_gate 清位。覆盖 flush-vs-flush + flush-vs-seal + seal-vs-seal。
    bool catalog_update_in_progress = false;

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
    // catalog_update_in_progress 统一挡 seal 与 flush（055 §6）：flush 在飞时不发起 seal
    if !catalog_update_in_progress && seal_conditions_met():
        catalog_update_in_progress = true        // close_gate 内置位
        initiate_seal_round()
        // seal_round 的 open_gate 完成后清 catalog_update_in_progress = false

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
    uint32_t front_owner_index = UINT32_MAX;         // owning front index; UINT32_MAX = invalid sentinel
    uint64_t min_lsn = UINT64_MAX;                   // 该 gen 中最小 batch_lsn
    uint64_t max_lsn = 0;                            // 该 gen 中最大 batch_lsn

    // ── Per-gen bump arena ──
    // kv_arena 只持有 key bytes。
    //   - key 通过 table 的 std::string_view 指向 arena 切片
    //   - value bytes 不进入 memtable；entry 只保存 durable value_ref
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
    // flush fold 期间直接挂接 memtable-only losers。成功 round 继续保留；
    // unfinished retry on the same sealed gen may clear+rebuild this list.
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
2. **批量释放 key bytes**：gen 析构一次 sweep 释放所有 chunk，不是百万次 delete。
3. **POD-friendly**：`memtable_entry` 与 `value_handle` 都保持 trivially copyable POD，`absl::InlinedVector` 的 relocate 走 trivial `memcpy` 路径。
4. **不保存 value body**：value body 在 WAL 之前已经 durable 到 Value Area；memtable 只保存 `value_ref`，避免第二份 DRAM copy 与长期占用。

**为什么 shared_ptr 而不是 intrusive_ptr**：gen 量级小（每 front 几到十几个），shared_ptr 的 16B pointer + 融合 control block 总占用远小于写一套 intrusive_ptr glue 的维护成本；`memtable_gen` 既不需要 `shared_from_this` 也不需要 `weak_ptr`，切 shared_ptr 没有 corner case。

### 3.3 `memtable_entry`

```cpp
struct memtable_entry {
    uint64_t data_ver;                              // 语义等价于 batch_lsn

    enum class kind : uint8_t { value, tombstone } k;

    // kind == value 时有效；memtable_entry 整体是 trivially copyable POD
    value_handle vh;
};

// POD：durable 是盘上位置。memtable 不保存 value bytes。
struct value_handle {
    value_ref durable;
};

struct retired_value_ref {
    value_ref vr;
    uint64_t data_ver;                              // 用于和 recovery_safe_lsn 比较
};
```

**value 内存的 owner 模型（冻结）**：

1. value bytes 只由 Value Area / value page frame 承载；memtable 不保存 value body，也不返回 `value_view`。
2. 跨核心读通过 `read_handle → publish_catalog → prs → front_read_set → std::shared_ptr<memtable_gen>` 的 pin 链保活的是 memtable metadata 与 key bytes，不是 value body。
3. memtable hit value 时只返回 `value_ref`；调用方必须走 `value_alloc_sched.read_value(value_ref)` / `read_page_values(...)`。value 模块需要优化 recently-written / memtable-visible page residency，尽量让该读命中内存（见 `INC-055`）。
4. `memtable_entry` 与 `value_handle` 都是 trivially copyable POD；`absl::InlinedVector<memtable_entry, 1>` 的 relocate 是 trivial `memcpy`。

### 3.4 请求类型

| 请求 | 输入 | 输出 | 说明 |
|------|------|------|------|
| `write_wal_entries` | `(batch_lsn, entry_count, entries[])` | void | WAL FUA；value 已由 value_alloc_sched durable |
| `insert_memtable_entries` | `(batch_lsn, entries[])` | void | all-WAL barrier 成功后的 memtable insert |
| `seal_active` | — | `front_read_set` | seal 时由 coord_sched 投递 |
| `batch_lookup` | `(keys[], read_lsn, front_read_set)` | `batch_lookup_results[]` | MultiGet 的 front-local 批量 memtable 查找；只是 `lookup_memtable` 的顺序包装 |
| `lookup_memtable` | `(key, read_lsn, front_read_set)` | `variant<value_ref, tombstone, miss>` | point read（搜索 PRS snapshot 的 active/imms，概要 §8.1 step 3）；命中 value 时返回 durable `value_ref`，随后读路径调用 value scheduler 取 body |
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
        it->second.push_back(memtable_entry {
            data_ver = batch_lsn,
            k        = memtable_entry::kind::value,
            vh       = value_handle { .durable = entry.allocated_vr },
        })
    else:  // DELETE
        it->second.push_back(memtable_entry {
            data_ver = batch_lsn,
            k        = memtable_entry::kind::tombstone,
            vh       = {},
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
       cb(winner.vh.durable)  // 返回 durable value_ref；调用方再走 value_alloc_sched
   else:
       cb(tombstone)          // 上层返回 not found
```

**value_ref 生命周期契约**：`cb` 回传的是 durable `value_ref`，不携带 value bytes。调用链 `read_handle → cat → prs → front_read_set → std::shared_ptr<memtable_gen> → memtable_entry → value_handle.durable` 只保活 key bytes 与 entry metadata；value body 已经由写路径先落到 Value Area。调用方拿到 `value_ref` 后必须交给 `value_alloc_sched.read_value()` / `read_page_values()`，由 value cache 尽量命中 recently-written / memtable-visible page，miss 时从 NVMe 读 durable object。

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
3. 在段尾 `TRAILER_RESERVED` 固定区写 sealed trailer:
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

#### Entry group commit（唯一在飞 physical plan + logical waiters，INC-057 / 054）

实现上 WAL stream 任一时刻只有**一个在飞 physical `wal_append_plan`**(保证同一
tail LBA 只有一个 full-page image 在飞,避免整页覆写在设备上乱序覆盖已写 entry)。
front owner 在 idle 时把 `wal_pending_prepares_` FIFO 里已排队的多个 prepare
按 FIFO 合并进这一个 plan:

```text
plan.participants[0]            = leader（发 FUA、commit/abort 的 caller）
plan.participants[1..]         = followers（logical waiters）
front_sched.wal_pending_group_  = { plan_id, waiters[] }   // 仅当有 follower 时存在
```

- leader prepare 立即返回 `wal_prepare_issue_plan`,走 issue FUA → commit/abort。
- follower prepare callback 被 park 在 `wal_pending_group_`,**不返回到 L3**;
  leader `commit` 时 front owner fan-out `wal_prepare_committed{cursor_after,
  fragment_done}` 唤醒(无 I/O),`abort`/FUA failure 时 fan-out 同一 WAL device
  failure,所有 participant 都停在 memtable 前走 release。
- 唤醒后 front owner `drain_wal_pending_prepares()` 用 FIFO 残余 prepare 组下一个
  group。header/trailer/needs_segment 仍单 participant(不 group)。
- tail page snapshot(`begin_pending`)每个 group 只做一次,而不是每 batch 一次。

## 4. Tree Domain（`tree_sched` + `tree_read_domain × K`）

tree 域拆成两类 runtime 对象：

1. `tree_sched`
   - 单实例
   - 负责 flush round owner 状态、tree allocator、manifest delta、bounded writes、reclaim
2. `tree_read_domain<Cache>`（step 030 §2.3 / §6.5 decision G1）
   - `K` 实例，每 core 一个
   - 持 routing snapshot (`shared_ptr<const shard_partition_map>`)、tree-node cache shard、owned lookup/worker schedulers
   - 向 PUMP runtime tuple 暴露 `advance()` 代驱两个 scheduler；tuple 不再单独注册 lookup / worker

```cpp
template <cache_concept Cache>
struct tree_read_domain : tree_read_domain_base {
    uint32_t                                    read_domain_index;
    std::shared_ptr<const shard_partition_map>  partitions;     // routing snapshot
    Cache                                       node_cache;     // tree_node domain clean frames
    std::unique_ptr<tree_lookup_sched<Cache>>   lookup;
    std::unique_ptr<tree_worker_sched<Cache>>   worker;

    bool advance();                                              // drives lookup + worker
};

struct tree_read_domain_base {
    uint32_t                       read_domain_index;
    tree_lookup_sched_base*        lookup_sched;                 // set by derived ctor
    tree_worker_sched_base*        worker_sched;                 // set by derived ctor
    virtual bool advance() = 0;
};
```

两个 scheduler 都模板化 on `Cache`，持 `tree_read_domain<Cache>*` back-reference；对 `node_cache` 的所有 pin/put/contains 调用走 `read_domain_->node_cache.*` 直接内联，零虚调用（030 §6.1 decision A）。基类 `tree_read_domain_base` 上的 `lookup_sched` / `worker_sched` 非模板指针允许 registry (`tree_read_domain_at(idx)->lookup_sched`) 和 `tree::lookup` fan-out 在不知道 `Cache` 的情况下访问两个 arm。

`read_domain_index` 字段只存在 `tree_read_domain_base` 上（step 030 §6.6 decision I1）；scheduler base 通过 `virtual uint32_t read_domain_index() const` 反向读取，仅诊断/panic 路径使用。`tree_sched` 不持有 tree-node read cache。

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
    // 由旧 checkpoint_guard / memtable_gen destructor 投递；rt::reclaim_once()
    // 显式消费并驱动 read_domain invalidate / NVMe TRIM / value / WAL sender 链
    mpmc::queue<reclaim_task*> reclaim_q;
    deque<reclaim_task*> pending_reclaim;
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
| `tree_sched` | `prepare_reclaim_round` / `finish_reclaim_invalidates` / `finish_reclaim_trims` / `finish_reclaim_round` / `abort_reclaim_round` | token / bounded plan carriers | plan / finish carriers / void | first-class `tree::reclaim_once()` 的 owner seam；不在 handler 内隐藏 submit |
| `tree_sched` | `update_superblock` | `(root_base_paddr, covered_lsn)` | void | root-change flush 后异步更新；完成后推进 `superblock_safe_lsn` |
| `tree_lookup_sched` | `tree_lookup` | `(key, manifest)` | `variant<leaf_value, leaf_tombstone, absent>` | 普通读路径的 tree traversal |
| `tree_lookup_sched` | `keys_to_leaf_groups` | `flush_lookup_req` | `flush_leaf_group_result[]` | 基于 `manifest->leaf_order` 做 key-group 到 affected leaf 的映射 |
| `tree_worker_sched` | `build_leaf_candidates` | `flush_worker_req` | `flush_candidate_batch` | 读 old leaf / merge / compact，返回 candidate proposals |

注：

1. `tree_lookup` / `tree_scan` 不是 `tree_sched` 的请求。读路径绝不访问 tree_sched 的当前可变状态。
2. `keys_to_leaf_groups` 与 `build_leaf_candidates` 是 tree-local flush 的内部 sender seam，不对外暴露成前台 API。

### 4.3 Data Area 碰撞检测

概要 §10.4：tree 向高地址增长，value 向低地址增长，两端在中间相遇表示 Data Area 已满。碰撞检测必须是跨 owner 的 reservation / fence 协议，不能只靠两侧 relaxed 采样对方 head 后各自本地 bump。旧的 relaxed 论证无效：stale 方向不保守，tree 可能读到过高的旧 `value_head_lba`，value 可能读到过低的旧 `tree_head_lba`，二者都会把新区间分配进对方已经保留的区域。

```cpp
// 全局共享（per-device，v1 单设备只有一份）
struct data_area_heads {
    std::atomic<uint64_t> tree_head_lba;             // tree reservation boundary: store-release / load-acquire
    std::atomic<uint64_t> value_head_lba;            // value reservation boundary: store-release / load-acquire
};
```

`store-release` / `load-acquire` 是最小内存序要求，但它们只提供发布顺序，
不保证读到最新值，也不能把“两侧各自 check-and-bump”变成原子 claim。
真正的 correctness boundary 必须满足以下之一：

1. 通过单 owner / lock / CAS-over-whole-gap 等机制原子地 reserve 共享 free gap。
2. 通过 `INC-051` value-space floor 协议：tree 提升 floor 时先发布
   reservation request，value owner 在自己的 scheduler 上处理并确认该
   floor reservation；tree 只有在收到确认后才能使用新吞掉的 LBA 区间。

因此，下面 allocator 伪码中的 acquire/release load/store 只是 reservation
协议内部的发布/观察动作；不能被简化回 relaxed，也不能脱离 reservation
协议单独作为碰撞检测。

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
        if (next_end > shared_heads->value_head_lba.load(std::memory_order_acquire))
            return {};  // Data Area 已满
        range_ref r = { .base = head, .slot_count = shadow_slots_per_range };
        head.lba = next_end;
        shared_heads->tree_head_lba.store(head.lba, std::memory_order_release);
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

为避免把 `tree_node` read cache 在每个 `front_sched` 上重复一份，同时又不把共享锁带进 front hot path，读侧引入少量 `tree_read_domain`；每个 read_domain 拥有一个 `tree_lookup_sched` 作为 tree traversal 的执行者，并访问所属 read_domain 的 `node_cache` shard。

```cpp
struct tree_lookup_state {
    uint32_t lookup_id;
    tree_read_domain<Cache>* read_domain;           // 模板化 back-ref；
                                                    // node_cache 位于 read_domain

    // 可选：对同一 slot 的并发 miss 做 single-flight 合并
    flat_hash_map<frame_id, pending_read*> inflight_reads;
};
```

路由规则（step 030 §2.6 / §2.7 / §6.4 F2）：

1. 每个 key 的 home shard 由**全局安装的 `shard_partition_map`** 决定：
   ```
   shard_idx = core::registry::current_shard_partitions()->route(key)
   home       = core::registry::tree_read_domain_at(shard_idx)
   ```
   `shard_partition_map` 的 `shards` 按 fence_upper 升序排列，`route(key)` 是一次二分查找（成本 `log2(shard_count)`）。最后一个 partition 必须是 +∞ sentinel (`fence_upper_len == 0`)，由 builder 负责；不满足时 `route()` 直接 panic。
2. 读路径与 flush fold 路径（`memtable_fold::build_key_partitions`）**共享同一个 `shard_partition_map`**。由此推出 "同一个 leaf 的所有 key 永远路由到同一个 read_domain"，所以一张 leaf / internal / root page 在整个读 + flush 链路里最多只在一个 read_domain 的 `node_cache` 中驻留一份。Hash-based `front_owner % K` 路由会让同 leaf 的不同 key 落到不同 shard，在每个 shard 复制一份同一张 page，是明确禁止的反例。
3. 空树（`!manifest->has_root()`）由调用方短路成 `lookup_absent`，永远不进入 routing；bootstrap 装的 placeholder map（`shards=[{upper=+∞, idx=0}]`）使 "map 总是安装" 不变量在空树过渡期仍然成立，但读路径在短路后不会真正调 `route()`。
4. `front_sched` 在 memtable miss 时，用 `current_shard_partitions()->route(key)` 把一批 miss 按 home shard 预先分组，再把每组独立投递到对应 read_domain 的 `lookup`；**跨 shard 分组在 front 侧完成**，`tree::lookup(keys, manifest)` sender 内部自动做这件事，调用方不传 sched 指针（INC-003 / INC-040 一并收敛）。
5. `tree_read_domain` 实例数 `K` 通常小于 `front_sched_count`，并优先按 NUMA 分组放置；NUMA 仍然影响 read_domain 实例物理安放的核心选择，但**不改变路由算法**——同 shard 的 key 仍然只会落到一个 read_domain。
6. range scan 的 tree-side 遍历整体路由到一个选定的 read_domain（目前选调用方 NUMA 上的本地 shard），而不是把 leaf frames 借回多个 `front_sched`。

`shard_partition_map` 由 `tree_sched` 在每次成功 flush round 提交新 manifest 后 rebuild：`tree/sender.hh::tree_local_flush` 在 `continue_after_finalize_merge()` 之后 `then(rebuild_and_publish_shard_partitions)` 基于 `tree_flush_result.new_manifest->leaf_order` + `core::registry::tree_read_domain_count()` 调 `build_initial_shard_partition_map` 构造新 map，走 `rt::publish_shard_partitions` 两步安装（`install_shard_partitions` 替换全局指针 + 遍历所有 `tree_read_domain` 刷新各自 `partitions` snapshot）。seam 本身执行时仍在 tree_sched 这个 owner 核心上（`submit_finalize_flush_round` 的回调在 tree_sched 的 advance 上继续推进 pipeline），发布过程单线程、与 flush 提交点串行。

bootstrap 时 builder 预装一张单 shard `+∞` 占位 map（`build_initial_shard_partition_map` 对空 `leaf_order` 的合约），保证第一次 flush 之前 read 路径 / flush fold 路径都能对 empty tree 做 `has_root()==false` 短路 + 合法的 routing。空 round / fold-unsupported / flush_ok=false 三种 short-circuit 下 `new_manifest==nullptr`，此时跳过 rebuild，当前已装的 map 继续有效。

future heat-driven rebuild（非 flush 提交点触发、按访问模式重新切 shard）仍留给 coord_sched 或后续专门 rebuild seam；本条只冻结 "flush 提交 = 必须重建对应 `shard_partition_map`" 这一规则，避免 placeholder map 永远覆盖已有真实 leaf 布局的情况。

tree_lookup **不在 tree_sched 上执行**。它在路由得到的 read_domain 的 `lookup` scheduler 上执行，通过该 scheduler 本核的 `nvme_sched` 异步读 page。

原因：tree_lookup 的依赖（immutable manifest、NVMe read、`tree_read_domain.node_cache`）全部不需要 tree_sched 的可变状态；但如果仍把 tree traversal 留在 `front_sched`，又会引入跨 scheduler 借 frame / 归还 frame 的生命周期问题。把 tree traversal 与 tree-node cache 一起放进 `tree_read_domain`，可以保持 owner-local、无锁、无借页。

```text
// 在路由得到的 read_domain.lookup 上执行
tree_lookup(key, manifest):
1. if manifest->root_slot == null:
       return absent
2. slot_paddr = manifest->root_slot
3. root_page = read_domain_->node_cache.get_or_read(slot_paddr)
   // cache miss → 本核 nvme_sched->read → 回填 cache
4. page = root_page
5. 从 root 开始 B+ tree 下降：
   for each internal level:
       child_range_base = find_child(page, key)
       child_slot_paddr = manifest->resolve(child_range_base)
       page = read_domain_->node_cache.get_or_read(child_slot_paddr)
6. leaf_page = page
7. record = leaf_page.find(key)
8. if record 不存在:
       return absent
   else if record.kind == value:
       return leaf_value_record { data_ver, value_ref }
   else:
       return leaf_tombstone { data_ver }
```

**node frame cache**：每个 `tree_read_domain` 拥有一个 `readonly_frame_cache` shard（tree_node domain），由该 domain 的 `tree_lookup_sched` / `tree_worker_sched` 共享。tree page 以 `segmented_page_frame { dom = tree_node, st = clean_readonly }` 形式驻留在 LBA DMA pages 中。遍历期间通过 `pin_count` 防止驱逐；pin 释放后按 LRU / clock 可淘汰。cache 驱逐不影响正确性，因为 manifest 持有结构信息，page 可以随时从 NVMe 重新读取。

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
    tree_read_domain<Cache>* read_domain_;          // 与 lookup shard 共享 node_cache；
                                                    // `&read_domain_->node_cache`
                                                    // 直接传给 `process_flush_round<Cache>`
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
flush_durable_frontier = min(flush_max_lsn, superblock_safe_lsn)   // WAL 段回收 eligibility 用此
wal_frontier = wal_global_min_unreclaimed_lsn - 1   // 无未回收 WAL → flush_durable_frontier
recovery_safe_lsn = min(flush_durable_frontier, wal_frontier)     // value/tombstone GC 用此
```

> **实现裁决（056 §5.4 B3，2026-06-16）——两个 frontier，勿混**：
> - **WAL 段回收**（`wal::reclaim_check`）的 eligibility frontier = `flush_durable_frontier = min(flush_max_lsn, superblock_safe_lsn)`，**不是** `recovery_safe_lsn`。若把含 `wal_frontier` 的 `recovery_safe_lsn` 喂回段回收会循环死锁（最旧未回收段 `min_lsn=ms`、`max_lsn≥ms>ms-1≥recovery_safe_lsn` → 永不满足 `max_lsn≤frontier` → 段永不回收 → frontier 冻结）。
> - **`wal_frontier` 必须基于跨所有 stream、所有未回收 WAL 输入（sealed-未回收 + active 非空）的全局最低 LSN**，不能只看 sealed segments 的 min(min_lsn)——WAL 多 stream 无全局 LSN 序，慢 stream 的低 LSN 可能还在 active 段没 seal，只看 sealed 会让 frontier 偏高 → premature reclaim corruption。含 active 后 frontier 由最慢 stream 钳住，单调非降。
> - wal 发布 `wal_global_min_unreclaimed_lsn` 到共享 atomic（`core::wal_reclaim_frontier`），tree `recompute_recovery_safe_lsn` acquire 读。
> - 原 `wal_frontier = sealed segs 最小 min_lsn - 1` 的表述按此修正。

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
| `reclaim_check` | `flush_durable_frontier` | void（内部筛选并回收） |

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
handle_reclaim_check(flush_durable_frontier):
    // rt::reclaim_once() 在完成 tree/value reclaim 后，以 flush durable frontier 投递此请求
    // wal_space_sched 本地筛选可回收的 sealed segments
    reclaimable = []
    remaining = []
    for info in sealed_segments:
        if info.max_lsn <= flush_durable_frontier:
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

集中管理 value page 的写入执行、DMA frame 填充、NVMe FUA 写入（leader-follower 模式），以及 **value read 服务**（tree-path value 读取的唯一执行域）。通过本核 `nvme_sched` 提交 value FUA 写入、value page 读取和 TRIM。

free-space / partial-page / cached-partial candidate / trim withheld 的逻辑状态由 `value_space_manager` 持有；`value_alloc_sched` 只持有执行层 resident 状态（round pages、resident partial frames、readonly cache）并推进 I/O completion。`value_space_manager` 是 owner-local 同步 metadata component，不直接提交 NVMe I/O，也不保存 frame 指针。

最佳实践是把它部署在独占核心上；但语义上只要求它保持单实例 owner。读写共享同一份 `readonly_frame_cache`（value_page domain），避免跨 shard 缓存重复和 invalidate 开销。

| 属性 | 值 |
|------|---|
| 实例数 | 1（全局唯一） |
| Owner 状态 | `value_space_manager`、round pages、resident_partial frames、dirty_round_pages、本地 `readonly_frame_cache`（value_page domain，读写共享） |
| NVMe I/O | 通过本核 nvme_sched 提交 frame write/read、value FUA 写入和 TRIM |
| 路由 | 固定单点 |

### 6.2 请求类型

| 请求 | 输入 | 输出 | 调用者 |
|------|------|------|--------|
| `persist_put_values` | batch PUT entries | durable value_refs | coord_sched（leader-follower：多 batch 合并） |
| `read_value` | `value_ref` | owning value bytes | 读路径（tree hit value 后） |
| `read_page_values` | `value_read_group { page_fid, refs[] }` | owning value bytes[] | 读路径（MultiGet / Scan 的 tree hit values） |
| `reclaim_values` | `dead_value_refs[]` | void | tree_sched（dead value 回收，按 batch 投递） |
| `drain_trim_once` / `drain_trim_pending` | `max_ranges`, `max_lbas` | `value_trim_round_result` / void compatibility wrapper | maintenance / reclaim cadence |
| `install_recovered_state` | `live_extents`, `tree_alloc_head_lba`, `data_area_end_lba`, optional `dead_class_hints` | void | boot recovery（一次性初始化） |

注：

1. `alloc_page` 和 `return_page` 不再是跨 scheduler 请求——它们是 `persist_put_values` 内部的本地操作；logical placement / release / trim metadata 统一通过 `value_space_manager`，scheduler 不再持有 per-class free pools。
2. `read_value` 是 Point GET 使用的单值包装；`read_page_values` 是 MultiGet / Scan 使用的页级批量原语。两者都由 `value_alloc_sched` 服务：先查 dirty round frames，再查 value_page `readonly_frame_cache`，miss 时通过本核 `nvme_sched` 读取 segmented frame 并按策略回填 cache。返回 copy 后的 owning bytes，调用方无需管理 DMA frame 生命周期。
3. `install_recovered_state` 是 boot-only 初始化接口；steady-state 下不走这条路径。recovery 只交出 occupied truth，`value_alloc_sched` 清空执行层 frames/cache 后调用 `value_space_manager.install_recovered_state(...)` 重建 free-space / partial metadata；cached residency 从空开始。

### 6.3 Owner 状态

```cpp
struct value_alloc_state {
    // ── Logical placement metadata ──
    // Owns global_free_extents, sparse partial_pages, cached_partial_index,
    // trim withheld/inflight state, allocation pressure mode, and the
    // acknowledged tree/value alloc floor.
    value_space_manager space;

    // ── Round frames（当前 persist round 正在填充的 DMA pages）──
    // persist_put_values 中 leader 统一分配 slot、memcpy 到 round page，
    // 本轮结束后提交 FUA。logical claims 已经在 space round 中保留，
    // frame 本身由 scheduler 的 LBA DMA page pool / cache layer 管理。
    vector<round_page> pages;

    // ── Dirty / writeback tracking ──
    // 跟踪 value_alloc_sched 当前持有的 active round pages 的地址集合。
    // reclaim_values 命中 dirty page 时，space 先更新 logical metadata；
    // scheduler 只更新 frame-local summary，避免 resident frame stale。
    flat_hash_set<paddr> dirty_round_pages;

    // ── Resident partial pages ──
    flat_hash_map<paddr, resident_partial_entry> resident_partial;

    // ── Read cache（value_page domain）──
    readonly_frame_cache value_read_cache;

    // ── Execution policy ──
    value_io_policy io_policy;                       // cache admission / FUA batch / read prefill caps
};
```

状态边界固定为：

1. `value_space_manager` 是 logical free-space truth：`global_free_extents`、
   sparse `partial_pages` / `by_page_delta` / bucket index、`cached_partial_index`、
   trim withheld/inflight state、partial metadata budget 和 alloc-floor reconcile。
2. `value_alloc_sched` 是 execution owner：round/resident segmented DMA frames、value page
   readonly cache、cache epoch pin/take、NVMe frame read/write/trim submission 和 completion。
3. scheduler 只能通过 `space.begin_round()` / `allocate_batch()` /
   `commit()` / `abort()` / `release_values()` / `prepare_trim()` /
   `complete_trim()` 改 logical metadata；不能旁路维护 per-class pools、
   `hole_pages`、`generic_free_spans` 或 `trim_pending_pages`。
4. `cached_partial_index` 只存 page address + summary + cache epoch。actual
   resident frame 仍在 scheduler/cache layer；claim 后 pin/take 失败表示
   stale index，scheduler 必须 `abort(round)` 并 `erase_cached_partial(...)`
   后重新规划。
5. `install_recovered_state()` 先清空 `round pages` / `resident_partial` /
   `dirty_round_pages` / `value_read_cache`，再用 recovery 提供的 live extents 调用
   `space.install_recovered_state(...)`；不读取 Value Area payload，也不恢复
   cached residency。

### 6.4 `persist_put_values` 分配 / 写入流程

```text
handle_persist_put_values(entries):
    round = space.begin_round()
    reqs = build_allocation_requests(entries)
    claims = space.allocate_batch(round, reqs, current_allocation_policy())

    for claim in claims:
        switch claim.src:
        case cached_partial:
            frame = value_read_cache.take_or_pin(claim.page_base, claim.cache_epoch)
            if frame == stale_or_missing:
                space.abort(round)
                space.erase_cached_partial(claim.page_base, claim.cache_epoch)
                retry planning
            attach_round_page(frame)

        case nonresident_partial:
            frame = read_or_take_cached_page(claim.page_base) // returns segmented frame
            attach_round_page(frame)

        case new_whole_page:
            value_read_cache.erase_range(claimed_lba_range(claim))
            frame = alloc_zeroed_segmented_frame(claim.page_base)
            attach_round_page(frame)

        copy value bytes into frame at claim.byte_offset

    submit frame_write_desc[] with FUA through local nvme_sched

    on writeback success:
        space.commit(round)
        publish returned value_refs
        finalize_written_frames()

    on writeback failure / abort:
        space.abort(round)
        drop dirty frames from this round
```

source 边界：

1. `cached_partial` 只表示 `value_space_manager.cached_partial_index` 中有可用
   logical slot；actual frame 必须由 scheduler/cache layer 以
   `page_base + cache_epoch` pin/take。失败时这是 stale index，不是 logical
   metadata corruption。
2. `nonresident_partial` 表示 manager 已经扣减 partial bitmap，但 scheduler
   仍需要读整页（或命中 readonly cache）后才能 patch page image。
3. `new_whole_page` 来自 manager 的 whole-free extent；scheduler 先删除同
   range 的旧 readonly cache entry，再分配新 DMA frame 并清零，不需要读旧页。
4. `commit(round)` 只在 value FUA 成功后执行；失败或 stale cached claim 必须
   `abort(round)` 释放本 round claims。
5. `finalize_written_frames()` 只处理 resident frame state：full page 可直接转
   clean readonly / evict；仍 partial 的 page 调用
   `space.mark_cached_partial(...)` 更新 cached candidate index；不再回写任何
   per-class pool。

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

    // ── 1. 先查 dirty round frames（当前正在填充的页）──
    if hit_round_frame(fid) → frame:
        goto serve_from_frame

    // ── 2. 查 value_read_cache / readonly_frame_cache ──
    if frame = value_read_cache.get(fid):
        goto serve_from_frame

    // ── 3. Cache miss → NVMe read ──
    frame = alloc_segmented_frame(fid)
    nvme_sched->read_frame(frame_read_desc{fid.base, frame})
    // NVMe read 完成后：
    frame->st = clean_readonly
    value_read_cache.put(fid, frame)
    goto serve_from_frame

serve_from_frame:
    result = []
    for vr in group.refs:
        bytes = frame_byte_view(frame, vr.byte_offset)
        header = reinterpret_cast<value_object_header*>(bytes.data)
        assert(header->magic == VALUE_MAGIC)
        assert(header->body_len == vr.len)
        verify_crc32c(bytes.data + sizeof(header), vr.len, header->body_crc)
        result.push(copy_bytes(bytes.data + sizeof(header), vr.len))
    cb(result)
```

**设计要点**：

1. **请求内按页分组**：MultiGet / Scan 先在调用方按 `frame_id` 分组，避免把同页多个 `value_ref` 变成多条独立消息。
2. **dirty frame 命中**：sub-LBA page 停留在 `dirty_round_pages` 的时间窗口内，tree-path read 可直接从 segmented frame 读取，零 NVMe。这对频繁 flush + 小 value 的工作负载有意义。
3. **copy 返回**：返回 owning bytes 而非 `value_view` + `frame_pin`。DMA frame 生命周期完全封闭在 `value_alloc_sched` 内部，`pin_count` 保持 `uint32_t` 不需要 atomic。copy 发生在 CRC 校验后，数据在 L1/L2 中是热的，成本最低。上层（如网络发送）最终也需要 copy，在这里做等价于在那里做。
4. **无 coherence 开销**：value_alloc_sched 既是 writer 又是唯一的 cache owner。partial rewrite writeback 后直接更新本地 cache，不需要跨 shard invalidate barrier。

### 6.6 Writeback completion / cached partial admission

```text
finalize_written_frames(round_frames):
    for frame in round_frames:
        dirty_round_pages.erase(frame.page_base)
        frame.st = clean_readonly

        if frame.summary.is_partial_allocatable():
            value_read_cache.put(frame)
            space.mark_cached_partial(cached_partial_update {
                page_base = frame.page_base,
                kind = active_tail or cached_free_candidate,
                heat_seq = next_heat_seq(),
                cache_epoch = frame.cache_epoch,
            })
        else:
            space.erase_cached_partial(frame.page_base, frame.cache_epoch)
            value_read_cache.put_or_evict(frame)
```

writeback completion 之后，logical allocator metadata 已由 `space.commit(round)`
发布。这里不再执行 `return_page` / `hole_pages.insert` / `whole_page_pool.push`
这类 allocator 操作，只维护 resident frame state 和 cached-partial index。
无需跨 shard invalidate（`value_alloc_sched` 是 value_page cache 唯一 owner）。

### 6.7 `handle_reclaim_values`（batch reclaim 核心）

```text
handle_reclaim_values(dead_value_refs[]):
    by_page = group_by_page(dead_value_refs)

    // 先更新 logical metadata truth。
    space.release_values(dead_value_refs)

    // 再修正 scheduler-owned resident frame/cache summary。
    for (page_base, refs) in by_page:
        if frame = dirty_round_pages.find(page_base):
            apply_reclaim_to_dirty_frame_summary(frame, refs)
            continue

        if frame = value_read_cache.find(page_base):
            apply_reclaim_to_clean_frame_summary(frame, refs)
            if frame.summary.is_partial_allocatable():
                space.mark_cached_partial(update_from(frame))
            else:
                space.erase_cached_partial(frame.page_base, frame.cache_epoch)
                if frame.summary.is_all_free():
                    value_read_cache.erase(frame.page_base)
```

`space.release_values(...)` 是 free-space truth 的唯一更新入口。scheduler 的
resident-frame 修正只防止 open/clean cached frame summary stale；它不再维护
`hole_pages`、whole-page pool 或 `trim_pending_pages`。

聚合后的 page-level 处理规则：

1. dirty page
   - `space.release_values` 已经释放 logical slots
   - scheduler 直接更新 dirty frame summary；不需要 allocator-level
     `deferred_freed`
2. readonly cache 命中
   - 更新 frame summary
   - still partial → `mark_cached_partial`
   - full / all-free → `erase_cached_partial`；all-free 同时从 readonly cache
     删除，实际可分配状态由 manager metadata 决定
3. nonresident page
   - 只更新 manager metadata；没有 resident frame summary 要修
4. all-free page
   - manager 将其转入 whole-free / trim withheld 候选；scheduler 只有在
     `prepare_trim` 返回 plan 后才提交 NVMe TRIM

**幂等性**：slot reclaim 一律用 `|=` 聚合，不用计数递增。

### 6.8 Manager TRIM drain / `complete_trim`

```text
drain_trim_once(max_ranges, max_lbas):
    plan = space.prepare_trim(max_ranges, max_lbas)
    if plan.empty():
        return { noop = true }

    value_read_cache.erase_ranges(plan.ranges)
    submit NVMe TRIM for plan.ranges through local nvme_sched

    on trim completion ok:
        space.complete_trim(plan.id, true)
        return {
            noop = false,
            trimmed_ranges = plan.ranges.size,
            trimmed_lbas = sum(plan.ranges.len_lbas),
        }

    on trim completion error:
        space.complete_trim(plan.id, false)
```

也就是说 logical TRIM gating 由 `value_space_manager` 保证：

```text
all-free
    -> manager global_free_extents (logical free, trim eligible)
    -> prepare_trim withholds selected ranges
    -> NVMe trim complete
    -> complete_trim returns ranges to global_free_extents
```

### 6.9 TRIM 顺序协议

整页回收的关键不变量仍然是：**TRIM 必须先于重分配完成。**

当前落地方式是把这个顺序收回到 value owner 自己维护：

```text
tree_sched: reclaim_values(dead_value_refs[]) -> value_alloc_sched
value_alloc_sched: space.release_values(...) -> all-free ranges become trim candidates
value_alloc_sched maintenance: space.prepare_trim(...) -> submit TRIM -> space.complete_trim(...)
// complete_trim 之前，withheld ranges 不会重新进入 allocatable free truth
```

`persist_put_values` finalize 不负责 TRIM drain。reclaim 若把页凑成 all-free，
只改变 manager 的 logical state；真正何时发 TRIM 由
`value_alloc_sched.drain_trim_once()` 按 reclaim cadence 决定。061 runtime
maintenance 只在本轮 reclaim 非 no-op 后 drain 一次 trim；空闲 no-op round 不
drain trim，避免把 bootstrap / ordinary allocatable free extents 无条件 withhold
到 trim-inflight；
`drain_trim_pending()` 仅作为旧 void wrapper 保留。

### 6.10 `dirty_round_pages` / cached partial 生命周期

| 事件 | 动作 |
|------|------|
| `persist_put_values` 收到 `byte_claim` 并准备 DMA frame | `dirty_round_pages.insert(page_base)` |
| cached claim 的 `page_base + cache_epoch` pin/take 失败 | `space.abort(round)` + `space.erase_cached_partial(...)` + retry |
| value FUA 成功 | `space.commit(round)`，`dirty_round_pages.erase(page_base)`，frame 转 clean readonly |
| value FUA 失败 / abort | `space.abort(round)`，丢弃本 round dirty frame |
| writeback 后仍 partial | `value_read_cache.put(frame)` + `space.mark_cached_partial(update)` |
| writeback 后 full / all-free | `space.erase_cached_partial(page_base, cache_epoch)` |
| `handle_reclaim_values` 命中 dirty page | `space.release_values(...)` 已释放 logical slots；scheduler 同步更新 dirty frame summary |
| `handle_reclaim_values` 命中 readonly cache | 更新 frame summary，并按 partial/full/all-free 调用 `mark_cached_partial` / `erase_cached_partial` |

## 7. `nvme_sched`（NVMe Scheduler）

每核心 × 每设备各一个实例，每个实例拥有独立的 SPDK qpair。各 scheduler 使用本核心的 `nvme_sched`，不做跨核心 NVMe I/O 投递。v1 单盘场景下每核心 1 个 `nvme_sched`。

Inconel 复用 PUMP 框架已有的 NVMe scheduler（`src/env/scheduler/nvme/`）。
PUMP 侧已经提供 read/write、FUA `io_flags`、`flush()`、`trim_ns_lba()` 以及
批量 `get_pages/put_pages`。Inconel 侧只负责把 logical LBA frame 适配为
PUMP `page_concept`：

| 操作 | 状态 |
|------|------|
| read page | PUMP `get_pages` |
| write page | PUMP `put_pages` |
| write page FUA | `put_pages(..., SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS)` |
| NVMe FLUSH | PUMP scheduler `flush()` |
| TRIM | PUMP scheduler `trim_ns_lba()` |

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
  2. memtable_entry.vh = { durable = value_ref }
  3. btree_map 的 bucket push 新版本

存活期间：
  - gen 活 → kv_arena 活 → key bytes 与 entry metadata 稳定可读
  - memtable hit 时 lookup_memtable 直接返回 value_handle.durable（value_ref）
  - value bytes 不进入 memtable；recently-written / memtable-visible residency 由 value_alloc_sched 管理（见 INC-055）
  - memtable-path 与 tree-path value read 都使用 `read_value(value_ref)` / `read_page_values(group)` copy 返回 owning bytes，不复用 arena

释放触发：
  - memtable_gen 从所有 published_read_set 中摘除
  - 最后一个 shared_ptr<memtable_gen> 析构 → use_count 归 0 → gen 析构
  - gen 析构按反向声明顺序：先 table（只存 key view 与 POD entries），后 kv_arena
  - kv_arena 析构 → vector<unique_ptr<char[]>> 析构 → 所有 chunk 一次 free
  - 所有 key bytes 消失；value_ref 进入回收判定（见下）

durable value_ref 回收：
  - 如果该 value_ref 已经被 tree flush 覆盖（更新版本进入 tree）：
    → 挂在旧 checkpoint_guard.retired.old_tree_values 上
    → 最后 reader 释放旧 guard 后回收
  - 如果该 value_ref 从未进入 tree（memtable-only loser）：
    → flush fold 期间直接挂在 owning memtable_gen.loser_durable_refs 上
    → 未完成 round 若在同一 sealed gen 上重试，允许 clear+rebuild
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
tree_read_domains ── keys_to_leaf_groups ── reduce ──────────┤
  │  (routed via current_shard_partitions()->route(key);        │
  │   executed on each read_domain.lookup)                      │
  │                                                           │
  ▼                                                           │
tree_read_domains ── build_leaf_candidates ── reduce ────────┤
  │  (executed on each read_domain.worker; cache shared with   │
  │   paired lookup on the same read_domain)                   │
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
4. **CAT 安装互斥（055 §6，显式不变量）**：seal（装 CAT1）与 flush（frontier_switch 装 CAT2）是 coord 上仅有的两类 CAT 安装操作，由 `catalog_update_in_progress` 串行——任一在飞时另一方 fail-fast。**flush round 端到端串行**：`capture_flush_frontier` 置位、`end_flush_round`（在 `release_gens` 之后）清位，保证同一 sealed gen 不被两轮 fold、且 frontier_switch 不会插进 seal 的 close→install 窗口。pipelined flush 是 future opt（需 per-gen in-flight 追踪），v1 不做。

### 10.3 tree-domain schedulers 单线程

1. `tree_sched` 上的 tree allocator 不需要锁。
2. `tree_sched` 上的 `tree_manifest` 构造不需要并发保护。
3. `tree_sched` 上的 `recovery_safe_lsn` 推进不会并发。
4. 每个 `tree_read_domain` 的 `node_cache` 和所属 `tree_lookup_sched` / `tree_worker_sched` 都绑定在同一 core、单线程驱动（read_domain.advance() 代驱两个 arm），owner-local 无锁访问。
5. `shard_partition_map` 是 `shared_ptr<const ...>`：一旦装入即 immutable，多 read_domain 并发读同一对象；rebuild 通过 `install_shard_partitions()` 原子替换指针实现（B1 目标设计，step 030 尚未触发）。

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
