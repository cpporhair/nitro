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
| `capture_flush_frontier` | `()` → `flush_frontier { durable_lsn, old_guard }` | RSM §2.3, FF §2.2/8.1 |
| `frontier_switch` | `(old_guard, new_manifest, retired, flushed_gens_by_front)` → `void` | RSM §2.3, FF §4.2/8.1 |
| `install_cat` | `(new_publish_catalog)` → `void` | RSM §2.3, FF §4.2 |
| `write_wal_entries` | `(batch_lsn, entry_count, entries[])` → `void` | RSM §3.4-3.5, WP §2.3/§10.7 |
| `insert_memtable_entries` | `(batch_lsn, entries[])` → `void` | RSM §3.4-3.5, WP §2.3/§10.7 |
| `batch_lookup` | `(keys[], read_lsn, front_read_set)` → `batch_lookup_results[]` | RSM §3.4/3.7, RAP §5.1-5.3 |
| **`lookup_memtable`** | **`(key, read_lsn, front_read_set)`** → `variant<value_view, tombstone, miss>` | **RSM §3.4/3.7, RAP §4.2/4.3/5.3, OV §8.1/14.2** |
| **`scan_memtable`** | **`(begin, end, read_lsn, front_read_set)`** → `scan_result_set` | **RSM §3.4, RAP §6.2/6.3, OV §14.4** |
| `collect_eligible_gens` | `(durable_lsn)` → `eligible_gens[]` | RSM §3.4/3.8, FF §2.2/8.1 |
| `seal_active` | `()` → `front_read_set` | RSM §3.6, OV §9.2 |
| `release_gens` | `(gen_id_list)` → `void` | RSM §3.8, FF §4.3 |
| `tree_flush` | `(tree_flush_request)` → `tree_flush_result` | RSM §4.2, FF §3.1/§8.1, CM tree |
| `keys_to_leaf_groups` | `(flush_lookup_req)` → `flush_leaf_group_result[]` | RSM §4.2/§4.7, FF §3.2/§3.4, CM tree |
| `build_leaf_candidates` | `(flush_worker_req)` → `flush_candidate_batch` | RSM §4.2/§4.8, FF §3.2/§3.4, CM tree |
| `persist_put_values` | `(batch PUT entries)` → `durable value_refs` | RSM §6.2, WP §2.1/5.4 |
| `read_value` | `(value_ref)` → `owning value bytes` | RSM §6.5, RAP §4.2/4.5/9.3 |
| `read_page_values` | `(value_read_group { page_fid, refs[] })` → `owning value bytes[]` | RSM §6.5, RAP §5.2/6.2/9.3 |
| `freed_slots` | `(page_base, class_idx, freed_mask)` → `void` | RSM §6.7, FF §7.2 |
| `recycle_whole` | `(class_idx, page_base)` → `void` | RSM §6.2 |
| `alloc_segment` | `(stream_id, sealed_info?)` → `segment_runtime*` | RSM §5.3, WP §7.2 |
| `reclaim_check` | `(recovery_safe_lsn)` → `void` | RSM §5.4, RW §12.4 |
| `tree_lookup` | `(key, manifest)` → `variant<leaf_value, leaf_tombstone, absent>` | RSM §4.7, RAP §4.2 |

> 缩写：OV=design_overview, RSM=runtime_state_machine, WP=write_path_and_pipeline, RAP=read_api_and_pipeline, FF=flush_and_frontier_switch, RW=recovery_and_wal_reclaim, RMC=runtime_memory_and_cache, CM=code_modules, ODF=on_disk_formats

## 2. 关键 Struct 字段

struct 在概要定义、详细设计细化。字段变更时需同步。

| Struct | 关键字段 | 定义点 | 引用点 |
|--------|---------|--------|--------|
| `coord_state` | `next_lsn, gate, current_cat, ready_set, seal_in_progress, cat_epoch` | RSM §2.1 | — |
| `front_state` | `owner_id, active, imms, wal` | RSM §3.1 | — |
| `memtable_gen` | `gen_id, st, min_lsn, max_lsn, kv_arena, table, loser_durable_refs`（无内嵌 refcount；生命周期由 `std::shared_ptr<memtable_gen>` 管理） | OV §5.1, RSM §3.2 | FF §3.3, RW §6 |
| `gen_arena` | `chunks: vector<unique_ptr<char[]>>, bump_next, bump_end`；`allocate(src, len) -> string_view` | RSM §3.2 | RMC §9.3 |
| `memtable_entry` | `data_ver, kind, vh`（trivially copyable POD） | RSM §3.3 | RAP §4.3 |
| `value_handle` | `durable: value_ref, hot: value_view`（POD）；`hot` 指向 owning gen 的 `kv_arena` 切片 | OV §5.1, RSM §3.3 | RAP §4.4 |
| `value_view` | `data: const char*, len: uint32_t`（POD；lookup_memtable 命中 value 时返回，生命周期与 read_handle 绑定） | RSM §3.3/3.7 | RAP §4.2/4.4 |
| `publish_catalog` | `prs, durable_lsn (atomic), epoch` | OV §5.4 | RSM §2.1/2.3 |
| `published_read_set` | `tree_guard, fronts, epoch` | OV §5.3 | FF §4.2, RAP §2 |
| `front_read_set` | `active, imms` | OV §5.3 | RSM §3.4/3.7, RAP §4.2/4.3/5.3/6.3 |
| `read_handle` | `cat, read_lsn` | OV §5.5 | RAP §2.1-2.4 |
| `checkpoint_guard` | `manifest, retired { old_slots, old_ranges, old_tree_values }` | OV §5.2, RSM §4.6 | FF §4.1/5 |
| `tree_manifest` | `root_slot, slot_map, leaf_order` | OV §4.4/§9.4, RSM §4.5 | FF §3.4/§3.8, RW §7.2 |
| `tree_allocator` | `head, free_ranges, shared_heads` | RSM §4.4 | RW §9.1 |
| `data_area_heads` | `tree_head_lba (atomic), value_head_lba (atomic)` | RSM §4.3 | RSM §4.4/6.3 |
| `value_alloc_state` | `dev_state, classes, open_frames, dirty_pages, deferred_freed, config` | RSM §6.3 | WP §5.4, RMC §7.1 |
| `per_device_value_state` | `bump_head_lba, shared_heads` | RSM §6.3 | RW §9.2 |
| `batch_ctx` | `batch_lsn, entry_count, canonical_entries, fragments` | WP §2.2 | — |
| `page_frame` | `id, st, dma_buf, byte_len, pin_count, crc_valid` | RMC §5.4 | — |
| `value_page_frame` | `class_idx, slots_per_page, free_bitmap, free_count, mode` | RMC §5.5 | RSM §6.3 (open_frames) |
| `frame_id` | `base, span_lbas, dom` | RMC §5.2 | — |
| `hole_page_descriptor` | `page_base, class_idx, free_mask` | RMC §6.4 | RSM §6.7 |

## 3. Owner 归属

每类可变状态只有一个 owner scheduler。归属变更影响多处。

| 状态 | Owner | 声明点 | 违反检测 |
|------|-------|--------|---------|
| value open frames (dirty_append / dirty_hole_fill) | `value_alloc_sched` | RMC §7.1, RSM §6.3 | 如果 front_sched 持有 → 错 |
| value placement metadata (pools, hole_page_list, generic_free_spans) | `value_alloc_sched` | RMC §6.4/7.1, RSM §6.3 | 如果 front_sched 持有 → 错 |
| **value_page readonly_frame_cache（读写共享）** | **`value_alloc_sched`** | RMC §7.1, RSM §6.1/6.5 | 如果 front_sched、tree_lookup_sched 或 tree_worker_sched 持有 value_page cache → 错 |
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
| `read_value` | value_ref | `tree_lookup` 返回的 `leaf_value_record.vr` | 不是 front_sched / tree_lookup_sched 本地读取 |
| `read_page_values` | value_read_group | 调用方对 tree-sourced `value_ref`s 做 request-local page grouping 后得到 | 不是逐条 `read_value()` fan-out |

## 5. Pipeline 跳转路径

改动任何 scheduler 交互时，检查 pipeline 跳转是否一致。

### 写路径
```
coord_sched(assign_lsn) → value_alloc_sched(persist_put_values) → fan-out front_scheds(write_wal_fragment) → reduce → fan-out front_scheds(write_memtable_fragment) → reduce → coord_sched(publish_batch/release_batch)
```
出现点：OV §1.8/7.1/14.1, WP §1/2.1

### 读路径 (Point GET)
```
coord_sched(acquire_read_handle) → front_sched(lookup_memtable with PRS snapshot) → [miss] → tree_lookup_sched(tree_lookup with read_handle manifest) → [hit value] → value_alloc_sched(read_value)
```
出现点：OV §8.1/14.2, RAP §4.1/4.2/4.5

### Seal
```
coord_sched(close_gate) → fan-out front_scheds(seal_active) → reduce → coord_sched(build_prs1, install_cat1, open_gate)
```
出现点：OV §9.2/14.5, RSM §2.6, WP §9.3

### Flush
```
tree_sched(check_trigger_conditions) → coord_sched(capture_flush_frontier) → fan-out front_scheds(collect_eligible_gens) → reduce → tree_sched(tree_flush) → tree_sched(update_flush_max_lsn) → coord_sched(frontier_switch) → fan-out front_scheds(release_gens)
```
出现点：OV §9.4/14.6, FF §1.1/8.1

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
