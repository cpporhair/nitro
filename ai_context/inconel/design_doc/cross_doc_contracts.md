# Inconel 跨文档一致性契约表

> **用途**：任何文档编辑后，按本表检查所有出现点是否一致。不靠通读全部文件。
>
> **规则**：修改任何 scheduler handle 签名、struct 字段、pipeline 跳转、owner 归属时，必须同步更新本表列出的所有出现点。如果出现点之间有矛盾，以概要 `design_overview.md` 为准。

## 1. Scheduler Handle 签名

每个 handle 的参数列表在多个文件中出现。改签名时必须全部同步。

| Handle | 规范签名 | 出现点 |
|--------|---------|--------|
| `assign_lsn` | `(raw_batch)` → `(batch_lsn, canonical_entries, entry_count, route_table)` | RSM §2.3, WP §2.1 |
| `publish_batch` | `(batch_lsn)` → `void` | RSM §2.3, WP §2.1 |
| `release_batch` | `(batch_lsn)` → `void` | RSM §2.3, WP §10.7, OV §7.4 |
| `acquire_read_handle` | `()` → `read_handle { cat, read_lsn }` | RSM §2.3, RAP §2.1, RAP §4.2 |
| `capture_flush_frontier` | `()` → `flush_frontier { durable_lsn, old_guard }` | RSM §2.3, FF §2.2/8.1；055 §5.2。pin 当前 guard + 读 durable_lsn；置位 `catalog_update_in_progress_`（已置位则返回 `catalog_update_in_progress` 错误）。impl `coord/scheduler.hh` |
| `frontier_switch` | `(old_guard, new_manifest, retired, release_plan)` → `void` | RSM §2.3, FF §4.2/8.1；055 §5.3。第 4 参为 `core::flush_release_plan`（by value，从 `tree_flush_result.flushed_gens_by_front` 经 `extract_release_plan` 提取，055 §5.1/B1）。handler 两段式：阶段 A 构造 G1/PRS2/CAT2 + reserve G0.retired（可抛、不改已发布态）；阶段 B append retired 到 G0 + `install_cat`（noexcept，B2）。D1=安装瞬间 `cat->durable_lsn`。impl `coord/scheduler.hh::handle_frontier_switch` |
| `end_flush_round` | `()` → `void` | 055 §5.4/§6（spec 新增的串行化兜底 seam）。清 `catalog_update_in_progress_`，必须在 `release_gens` 之后。impl `coord/scheduler.hh` |
| `install_cat` | `(new_publish_catalog)` → `void` | RSM §2.3, FF §4.2 |
| `write_wal_entries` | `(batch_lsn, entry_count, entries[])` → `void` | RSM §3.4-3.5, WP §2.3/§10.7;M06 起由 front `prepare_wal_fragment / install_wal_segment / commit_wal_plan / abort_wal_plan` + L3 `write_path::write_wal_fragment` 实现(044/045);本行保留为概念签名。INC-057/054 起 `prepare_wal_fragment` 结果为 `variant<wal_prepare_issue_plan, wal_prepare_committed, wal_prepare_needs_segment>`:`issue_plan`=leader 发 FUA 并 commit/abort;`committed`=follower(group 内已被 leader FUA 落盘,只 adopt cursor,不碰 NVMe、不 commit);`needs_segment`=单 participant 触发 segment install。物理在飞 plan 恒为 1,group commit 只合并 logical participant。 |
| `insert_memtable_entries` | `(batch_lsn, entries[])` → `void` | RSM §3.4-3.5, WP §2.3/§10.7 |
| `enter_memtable_phase` | `(batch_lsn)` → `void`（纯串行点：无 owner 状态变更；其 continuation 在 coord 上派发该 batch 的全部 memtable fragments，与 `close_gate` continuation 的 seal_active 派发在 coord 队列全序——OV §7.1 不变量 4 的机制落点） | WP §2.3 冻结约束 4, OV §7.1；051 §4.1/§5.1 |
| `batch_lookup` | `(keys[], read_lsn, front_read_set)` → `batch_lookup_results[]` | RSM §3.4/3.7, RAP §5.1-5.3 |
| **`lookup_memtable`** | **`(key, read_lsn, front_read_set)`** → `variant<value_ref, tombstone, miss>` | **RSM §3.4/3.7, RAP §4.2/4.3/5.3, OV §8.1/14.2** |
| **`scan_memtable`** | **`(begin, end, read_lsn, front_read_set)`** → `scan_result_set` | **RSM §3.4, RAP §6.2/6.3, OV §14.4** |
| `collect_eligible_gens` | `(durable_lsn)` → `eligible_gens[]` | RSM §3.4/3.8, FF §2.2/8.1 |
| `seal_active` | `()` → `front_read_set` | RSM §3.6, OV §9.2 |
| `release_gens` | `(gen_id_list)` → `void` | RSM §3.8, FF §4.3 |
| `tree_flush` | `(tree_flush_request)` → `tree_flush_result` | RSM §4.2, FF §3.1/§8.1, CM tree |
| `keys_to_leaf_groups` | `(flush_lookup_req)` → `flush_leaf_group_result[]`（在 `tree_read_domain.lookup` 上执行） | RSM §4.2/§4.7, FF §3.2/§3.4, CM tree |
| `build_leaf_candidates` | `(flush_worker_req)` → `flush_candidate_batch`（在 `tree_read_domain.worker` 上执行） | RSM §4.2/§4.8, FF §3.2/§3.4, CM tree |
| `current_shard_partitions` | `()` → `shared_ptr<const shard_partition_map>` | CM registry, RSM §1/§4.7 |
| `install_shard_partitions` | `(shared_ptr<const shard_partition_map>)` → `void` | CM registry, RSM §1/§4.7 |
| `shard_partition_map::route` | `(key)` → `shard_idx` via 一次二分 | RSM §4.7, OV §8.1/§14.2, RAP §4/§5/§5.4 |
| `tree_read_domain::advance` | `()` → `bool` 代驱 lookup + worker arms | RSM §4, CM tree |
| `persist_put_values` | `(batch PUT entries)` → `durable value_refs` | RSM §6.2, WP §2.1/5.4 |
| `read_value` | `(value_ref)` → `owning value bytes` | RSM §6.5, RAP §4.2/4.5/9.3 |
| `read_page_values` | `(value_read_group { page_fid, refs[] })` → `owning value bytes[]` | RSM §6.5, RAP §5.2/6.2/9.3 |
| `reclaim_values` | `(dead_value_refs[])` → `void` | RSM §6.7, FF §7.2；056 reclaim consumer 的下游调用方 |
| `drain_trim_pending` | `()` → `void` | RSM §6.8/§6.9 |
| `reclaim_sink::post_retired / post_gen_losers` | `(retired_objects&& / losers&&)` → `void` | 056 §5.1。guard/gen 析构在任意线程 post `reclaim_task` 到 tree_sched（**mpmc** ingress，非 per_core）；core 抽象接口 + atomic 进程级 sink，teardown 先 null。impl `tree/owner_scheduler.hh` |
| `tree_read_domain::invalidate_range` | `(range_ref, geom)` → `void`（**派到 read_domain owner core 执行**） | 056 §5.5。reclaim old_range 进 allocator 前的跨 shards barrier；tree_sched `loop>>concurrent>>flat_map(submit_invalidate_range)>>all()` fan-out + wait-all-acks；`node_cache.take()` 只在 read_domain 自己 core 跑（pin>0 panic）。impl `core/tree_read_domain.hh` |
| `alloc_segment` | `(stream_id, sealed_info?)` → `segment_runtime*` | RSM §5.3, WP §7.2 |
| `reclaim_check` | `(flush_durable_frontier)` → `void` | RSM §5.4, RW §12.4；**056 修正：传 `flush_durable_frontier=min(flush_max_lsn,superblock_safe_lsn)`，不是 `recovery_safe_lsn`**——后者含 `wal_frontier` 会与段回收循环死锁（056 §5.4 B3）。production caller 在 `tree/sender.hh`（flush_durable_frontier 推进后驱动） |
| `tree_lookup` | `(key, manifest)` → `variant<leaf_value, leaf_tombstone, absent>` | RSM §4.7, RAP §4.2 |

> 缩写：OV=design_overview, RSM=runtime_state_machine, WP=write_path_and_pipeline, RAP=read_api_and_pipeline, FF=flush_and_frontier_switch, RW=recovery_and_wal_reclaim, RMC=runtime_memory_and_cache, CM=code_modules, ODF=on_disk_formats

## 2. 关键 Struct 字段

struct 在概要定义、详细设计细化。字段变更时需同步。

| Struct | 关键字段 | 定义点 | 引用点 |
|--------|---------|--------|--------|
| `coord_state` | `next_lsn, gate, current_cat (catalog_store), ready_set, cat_epoch, catalog_update_in_progress`（055 §6：`catalog_update_in_progress` 统一串行 seal 与 flush 的 CAT 安装——capture_flush_frontier/close_gate 取、end_flush_round/open_gate 放；取代 spec 早期的 `seal_in_progress`） | RSM §2.1 | — |
| `front_state` | `owner_id, active, imms, wal` | RSM §3.1 | — |
| `memtable_gen` | `gen_id, st, front_owner_index, min_lsn, max_lsn, kv_arena, table, loser_durable_refs`（无内嵌 refcount；生命周期由 `std::shared_ptr<memtable_gen>` 管理；flush 可写 `loser_durable_refs`） | OV §5.1, RSM §3.2 | FF §3.3, RW §6 |
| `gen_arena` | `chunks: vector<unique_ptr<char[]>>, bump_next, bump_end`；`allocate(src, len) -> string_view`；只保存 key bytes，不保存 value body | RSM §3.2 | RMC §9.3 |
| `memtable_entry` | `data_ver, kind, vh`（trivially copyable POD） | RSM §3.3 | RAP §4.3 |
| `value_handle` | `durable: value_ref`（POD）；memtable 不保存 value body | OV §5.1, RSM §3.3 | RAP §4.4 |
| `publish_catalog` | `prs, durable_lsn (atomic), epoch` | OV §5.4 | RSM §2.1/2.3 |
| `published_read_set` | `tree_guard, fronts, epoch` | OV §5.3 | FF §4.2, RAP §2 |
| `front_read_set` | `active, imms` | OV §5.3 | RSM §3.4/3.7, RAP §4.2/4.3/5.3/6.3 |
| `read_handle` | `cat, read_lsn` | OV §5.5 | RAP §2.1-2.4 |
| `flush_frontier` | `durable_lsn, old_guard`（capture 产出，pin 本轮 base guard） | 055 §5.1, core/flush_round.hh | FF §2.2, RSM §2.3 |
| `flush_release_plan` | `gen_ids_by_front: vector<vector<uint64_t>>`；`gen_ids_for(i)`。frontier_switch subtract + release_gens 共用的 by-value carrier（B1，不持 tree_flush_result 引用） | 055 §5.1, core/flush_round.hh | FF §4.2/4.3 |
| `checkpoint_guard` | `manifest, retired { old_slots, old_ranges, old_tree_values }` | OV §5.2, RSM §4.6 | FF §4.1/5 |
| `tree_manifest` | `root_slot, slot_map, leaf_order` | OV §4.4/§9.4, RSM §4.5 | FF §3.4/§3.8, RW §7.2 |
| `tree_allocator` | `head, free_ranges, shared_heads` | RSM §4.4 | RW §9.1 |
| `data_area_heads` | `tree_head_lba (atomic), value_head_lba (atomic)`；reservation boundary 使用 release/acquire，不能作为独立 relaxed collision check | RSM §4.3 | RSM §4.4 |
| `value_space_manager` | `global_free_extents, sparse partial_pages/by_page_delta/buckets, cached_partial_index, trim_withheld/trim_inflight, acknowledged_alloc_floor, pressure budgets`；不保存 DMA frame，不提交 NVMe I/O | RSM §6.3, INC-051 | CM value |
| `value_alloc_state` | `space, round pages, resident_partial, dirty_round_pages, value_read_cache, io_policy` | RSM §6.3 | CM value |
| `batch_ctx` | `batch_lsn, entry_count, canonical_entries, fragments` | WP §2.2 | — |
| `page_frame` | legacy contiguous carrier: `id, st, buf, byte_len, pin_count, crc_valid`；仅供旧 cache policy compatibility path 使用 | RMC §5.4 | — |
| `segmented_page_frame` | real NVMe carrier: `id, st, pages[], pin_count, crc_valid`；multi-LBA = 多个 LBA DMA pages，不要求连续 DMA，不用 SGL | RMC §5.7, plan 038 | nvme/lba_page.hh |
| `value_page_frame` | `segmented_page_frame` + `class_idx, slots_per_lba, free_count, mode`；当前 value runtime 已使用 segmented frame carrier，frame-local free summary 仍是后续细化项 | RMC §5.5, plan 038 | RSM §6.3 |
| `frame_id` | `base, span_lbas, dom` | RMC §5.2 | — |
| `shard_partition` | `fence_upper_off (u32), fence_upper_len (u16), _pad0 (u16), shard_idx (u32)`；POD 12B | core/shard_partition.hh (step 030 §2.1) | RSM §4.7, OV §8.1 |
| `shard_partition_map` | `fence_pool: string, shards: vector<shard_partition>`；最后一个 shard 必须是 +∞ sentinel（`fence_upper_len == 0`） | core/shard_partition.hh (step 030 §2.1) | RSM §4.7, OV §1.7/§8.1 |
| `tree_read_domain<Cache>` | `read_domain_index (u32), partitions: shared_ptr<const shard_partition_map>, node_cache: Cache, lookup: unique_ptr<tree_lookup_sched<Cache>>, worker: unique_ptr<tree_worker_sched<Cache>>`；继承 `tree_read_domain_base` 并填充其 `lookup_sched` / `worker_sched` base pointers；legacy `clock_cache/slru_cache` 特化包装 segmented read-domain | core/tree_read_domain.hh (step 030 §2.3) | RSM §1/§4, OV §1.7, CM tree |
| `tree_read_domain_base` | `read_domain_index (u32), lookup_sched: tree_lookup_sched_base*, worker_sched: tree_worker_sched_base*, virtual advance()` | core/tree_read_domain.hh (step 030 §2.3) | core/registry.hh `tree_read_domains` |

## 3. Owner 归属

每类可变状态只有一个 owner scheduler。归属变更影响多处。

| 状态 | Owner | 声明点 | 违反检测 |
|------|-------|--------|---------|
| value logical placement metadata (`global_free_extents`, `partial_pages`, `cached_partial_index`, trim withheld/inflight, alloc floor) | `value_space_manager` inside `value_alloc_sched` | RSM §6.3, CM value, INC-051 | 如果 scheduler/cache layer 旁路维护 allocator truth → 错 |
| value resident frames/cache (`round pages`, `resident_partial`, `dirty_round_pages`, `readonly_frame_cache`) | `value_alloc_sched` | RSM §6.1/6.3/6.5 | 如果 `value_space_manager` 保存 frame 指针或提交 NVMe I/O → 错 |
| **value_page readonly_frame_cache（读写共享）** | **`value_alloc_sched`** | RSM §6.1/6.5 | 如果 front_sched、tree_lookup_sched 或 tree_worker_sched 持有 value_page cache → 错 |
| WAL tail frame | `front_sched` | RMC §7.1, RSM §3.9 | — |
| tree node read-only frame cache（普通读路径 + flush old-leaf read） | `tree_read_domain` shard | RMC §7.1/9.1, RSM §4/§4.7/§4.8 | 如果 front_sched 或 tree_sched 持有 tree_node cache → 错 |
| tree flush write buffers | `tree_sched` | RMC §7.1 | — |
| active/sealed memtable gens | `front_sched` | RSM §3.1 | — |
| flush frontier snapshot (`durable_lsn` + `tree_guard`) | `coord_sched` | RSM §2.3, FF §2.2 | 如果 tree_sched 自行拼装当前 frontier → 错 |
| tree allocator + retire queues | `tree_sched` | RSM §4.1 | — |
| WAL segment pool | `wal_space_sched` | RSM §5.1 | — |
| publish_catalog / publish_gate | `coord_sched` | RSM §2.1 | — |

## 4. 数据源断言（读路径 handle）

概要 §8.1 冻结。详细设计展开 handle 签名时必须对照。

| Handle | 数据输入 | 来源 | ❌ 不是 |
|--------|---------|------|--------|
| `lookup_memtable` | active, imms | 请求携带的 `read_handle.cat->prs->fronts[owner]` (snapshot) | `front_sched` 当前 active/imms |
| `scan_memtable` | active, imms | 同上 | 同上 |
| `tree_lookup` | manifest | 请求携带的 `read_handle.cat->prs->tree_guard->manifest` (snapshot) | `tree_sched` 当前 manifest |
| `tree_lookup` 路由 | shard_idx | `current_shard_partitions()->route(key)`（全局 `shard_partition_map` snapshot） | 不是 `leaf_order.find_leaf_for_key(key) % K`，也不是 `front_owner % K` |
| `read_value` | value_ref | `lookup_memtable` 或 `tree_lookup` 返回的 durable `value_ref` | 不是 front_sched / tree_read_domain 本地读取 |
| `read_page_values` | value_read_group | 调用方对 tree-sourced `value_ref`s 做 request-local page grouping 后得到 | 不是逐条 `read_value()` fan-out |

## 5. Pipeline 跳转路径

改动任何 scheduler 交互时，检查 pipeline 跳转是否一致。

### 写路径
```
coord_sched(assign_lsn) → value_alloc_sched(persist_put_values) → fan-out front_scheds(write_wal_fragment) → reduce → coord_sched(enter_memtable_phase) → fan-out front_scheds(write_memtable_fragment) → reduce → coord_sched(publish_batch/release_batch)
```
出现点：OV §1.8/7.1/14.1, WP §1/2.1/§2.3 冻结约束 4（enter_memtable_phase 串行点为 M12/051 §4.1 机制落点）

### 读路径 (Point GET)
```
coord_sched(acquire_read_handle)
  → front_sched(lookup_memtable with PRS snapshot)
  → [miss] → shard_idx = current_shard_partitions()->route(key)
    → tree_read_domain_at(shard_idx)->lookup_sched.tree_lookup(with read_handle manifest)
  → [hit value] → value_alloc_sched(read_value)
```
出现点：OV §8.1/14.2, RAP §4.1/4.2/4.5

### Seal
```
coord_sched(close_gate) → fan-out front_scheds(seal_active) → reduce → coord_sched(build_prs1, install_cat1, open_gate)
```
出现点：OV §9.2/14.5, RSM §2.6, WP §9.3
注（055 §6.1b/B4）：`close_gate` 取 / `open_gate` 放 `catalog_update_in_progress_`，与 flush 的 frontier_switch 互斥；防 frontier_switch 插进 seal 的 close→install 窗口致 epoch 校验失败、gate 卡死。

### Flush
```
coord_sched(capture_flush_frontier)             # 置 catalog_update_in_progress_
  → fan-out front_scheds(collect_eligible_gens) → reduce
  → [无 eligible gen → coord_sched(end_flush_round) 收尾，no-op round (B3)]
  → tree_sched(tree_flush = tree_local_flush)    # 内联推进 flush_max_lsn/superblock_safe_lsn(owner_scheduler.hh)
  → coord_sched(frontier_switch)                 # 装 CAT2 + retired 挂 G0
  → fan-out front_scheds(release_gens) → reduce
  → coord_sched(end_flush_round)                 # 清 catalog_update_in_progress_
```
出现点：OV §9.4/14.6, FF §1.1/8.1；实现 055 §4（pipeline/flush_round.hh）。
注：spec §8.1 的独立 `update_flush_max_lsn` 跳在当前实现中由 `tree_local_flush` 内联完成（055 §8）；`end_flush_round` 是 055 新增的串行化收尾跳，异常路径经 `any_exception → end_flush_round → rethrow` 保证清位。

### Reclaim（056 step 2）
```
被动触发：reader 释放 read_handle / seal / flush 装 CAT2
  → 旧 CAT refs→0 → PRS → fronts/gen + tree_guard(G0) refs→0
  → ~checkpoint_guard()/~memtable_gen() post reclaim_task → tree_sched.reclaim_q (mpmc)
主动消费：rt::reclaim_once() / tree::reclaim_once()（持 tree_mutation_gate token）
  → tree_sched.prepare_reclaim_round 产出 bounded plan
  → old_slots/old_ranges: fan-out 各 read_domain invalidate_range → all() (wait-acks) → owner 清 non_leaf_cache
  → nvme TRIM (old_slots 逐 slot / old_ranges 整段) bounded
  → tree_allocator.recycle(old_ranges)
  → value: data_ver ≤ recovery_safe_lsn → reclaim_values(dead) bounded；否则 deferred_value_reclaim
  → wal::reclaim_check(flush_durable_frontier) → wal 更新 global_min_unreclaimed_lsn cell
  → 后续 reclaim prepare 基于 recovery_safe_lsn 扫 deferred
  → release token
```
出现点：FF §5/§7, RSM §4.2/§4.4/§4.9/§8；实现 056（owner_scheduler.hh / tree_read_domain.hh / wal）。
注：reclaim_round 与 flush 的 tree 阶段经 owner-local FIFO `tree_mutation_gate` 互斥（056 §5.8.3，与 coord `catalog_update_in_progress_` 正交）；tree owner handler 只做 plan/finish/abort，不启动 hidden root sender。

### Recovery
```
read_superblock → traverse_tree(leaf_records) + scan_wal(entries) → merge(logical_winners) → flush_style_incremental_merge → nvme_flush → update_superblock → trim + rebuild_allocator → install_clean_runtime
```
出现点：OV §12.0/12.2/14.7, RW §1-15

## 6. 三条最高准则

任何理解如果需要以下红线项才能成立，说明理解错了。

### 6.1 读路径（概要 §8.0）

1. ❌ 在读路径中访问 `front_sched.active` / `front_sched.imms`
2. ❌ 在读路径中访问 `tree_sched` 当前 manifest
3. ❌ 把 `tree_lookup` 串行到 `tree_sched` 上执行
4. ❌ 需要额外机制保证"读操作期间不发生 seal/flush/frontier switch"

检测点：RSM §3.7, RAP §4.2/5.2/6.2

### 6.2 Tree 运行时（概要 §10.0）

1. ❌ 运行时 cache miss 时扫描 shadow range 的多个 slot 选最新
2. ❌ 需要"每个 range 只剩一个 slot"才能正常读服务
3. ❌ 把 flush/recovery 的目标理解为"重写回 slot 0"

检测点：RSM §4.5/4.7, FF §3.4-3.6, RW §7

### 6.3 Recovery（概要 §12.0）

1. ❌ 扫描或读取 Value Area
2. ❌ 把 tree 重写回 slot 0 或从 scratch 重建
3. ❌ 需要 `dead_value_refs` 才能保证 allocator 不泄漏
4. ❌ 引入盘面白名单之外的持久化元数据

检测点：RW §1 (总览), RW §7 (增量 flush), RW §9.2 (value allocator)

### 6.4 `reclaim_values` caller precondition（INC-052 trust boundary，056 §5.7）

`value::reclaim_values` 对输入 blind trust（value 侧不做 liveness defense）。caller（056 reclaim consumer 及任何未来 dead-set 推导方）**必须**保证每条投入的 `value_ref` 三条全满足，缺一即可能 silent data corruption：
1. **provenance**：由 flush/fold 判定已被 winner 覆盖（或 tombstone 取代）、不在 new manifest / 任何活跃 PRS —— 这是它进 `retired.old_tree_values` / `gen.loser_durable_refs` 的前提，不是 reclaim 阶段重新推导。
2. **guard/gen 释放**：guard_retired 来自 G0 refs→0；gen-loser 来自 gen refs→0。
3. **data_ver gate**：`data_ver ≤ recovery_safe_lsn`（含 wal_frontier）。

仅 (2)+(3) 不足以单独证明任意输入 dead，必须叠加 (1)。value 侧只加可观测 counter（`reclaim_stats.partial_into_untracked` 稳态应 ≈0），**不加** liveness 校验。

## 7. 跨文档引用索引

改动章节编号时，必须同步更新所有引用方。

| 被引用章节 | 引用方 |
|-----------|--------|
| RSM §4.7 (`tree_lookup_sched` 与 tree lookup) | RAP §4.2 "见 runtime_state_machine.md §4.7" |
| RSM §4.8 (`tree_worker_sched` 与 candidate build) | FF §3.4 |
| RSM §4.9 (recovery_safe_lsn) | FF §6.2 "见 runtime_state_machine.md §4.9" |
| RSM §6 (value_alloc_sched) | WP §5.3, RMC §6.2, RAP §4.5/9.3 |
| RSM §6.5 (`handle_read_value` / `handle_read_page_values`) | RAP §4.2/4.5/5.2/6.2 |
| RSM §5.3-5.4 (wal_space_sched) | RW §12.4 |
| WP §5 (value 分配流程) | RMC §6.2 |
| WP §2.3 / §10.7 (`write_wal_fragment`, `write_memtable_fragment`) | RSM §3.5 |
| RMC §5.4 (frame_pin) | RAP §9.2-9.3 |
| FF §3 (fold 算法) | RW §7.2 "与 flush_and_frontier_switch.md §3 同构" |
| OV §8.1 (Point GET 规则 + 数据源断言) | RSM §3.7, RAP §4 |
| OV §9.2 (seal 规范步骤) | RSM §2.6, WP §9.3 |
| OV §9.4 (flush 规范步骤) | FF §1-8 |
| OV §12.0 (recovery 最高准则) | RW §1 |
| OV §12.2 (recovery 步骤) | RW §2-15 |
