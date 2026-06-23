# 064 Recovery Boot Design

## 状态

064A 的 boot skeleton 已提交：`09b6870 nitro: inconel: add recovery boot skeleton`。

当前代码已覆盖 064B、064C 和 064D 的 same-shape 子集：

- empty clean boot。
- empty tree + complete WAL replay，包含 delete-only/no-op tombstone frontier carrier。
- existing tree + WAL empty scanner/install。
- existing tree + WAL nonempty 时，如果 WAL winners 已被 tree 覆盖，reset WAL 并启动。
- existing tree + WAL delta 时，支持所有受影响 leaf 都能写同 range next slot、merge 后不 split 的 same-shape replay。

064D 仍未覆盖的 full CoW merge 场景会继续 fail-fast：leaf split、shadow slot exhausted 需要换新 range、internal propagation、root range change。

本文是完整 recovery boot 的目标规格，并记录当前分阶段实现边界。

## 背景

063 已经把前台写入、seal、flush、读路径接通到自动维护循环里，但进程重启后仍然缺完整 recovery。完整 recovery boot 的职责是：运行时启动前先从盘面恢复 superblock、clean tree、WAL 中尚未进入 tree 的完整 batch、value/free-space 状态，然后一次性构造 clean runtime。

这里不引入运行态内部 hidden root pipeline。Recovery 是 service start 前的 boot phase；boot 期间没有前台 IO，可以用同步顺序流程驱动 NVMe。进入运行态后，seal/flush/WAL reclaim/checkpoint 仍由现有 pump/runtime 循环负责。

064 原先讨论过的 pre-LSN/backpressure 是相邻问题，但它属于正常运行态写入调度，不应混入 recovery boot。本设计不处理它。

## 硬不变量

1. Recovery 不扫描 Value Area，不读 value body。
2. `live_value_refs` 是 Value Area 占用状态的唯一真相；`dead_value_refs` / hints 只优化 TRIM 和 class bucketing，不参与 correctness。
3. Recovery 不从 scratch 重建已有 tree，不把 shadow range 退化成 slot 0。
4. WAL 只 replay 完整 canonical batch：`actual_count == entry_count`。
5. tombstone 是正常 record，必须参与 winner 判定，不能在 recovery merge 前提前丢弃。
6. WAL reset 必须发生在 tree writes durable 且必要 superblock update durable 之后。
7. runtime install 前不发布任何可见 CAT；启动后只能看到 clean state。
8. 没有合法 superblock 时不自动 format；只有显式 `--force-format` 才能破坏性重建。
9. WAL reset 后，clean tree 必须能重新推出同一个 `recovered_durable_lsn`；superblock 当前不存 LSN，所以 tree records 是 durable frontier 的唯一持久载体。

## 当前实现约束

- `format::superblock` 已有 A/B superblock POD、CRC、`inspect_superblock`、`choose_newer_superblock`。
- `format::wal.hh` 已有 WAL header/trailer/entry POD、CRC、entry encode/decode helper。
- `format::tree_page.hh` / `tree/page_reader.hh` 已有 tree page inspect/reader。
- `runtime::build_runtime` 在 064A 后可接受 superblock 派生出的 profile/geometry。
- `coord_sched` 构造函数支持 `initial_cat` 和 `next_lsn`。
- `value_alloc_sched::install_recovered_value_space(...)` 已存在，且不会扫描 Value Area。
- `tree_sched` 还缺 recovered manifest/allocator/frontier installer。
- `wal_space_sched` 还缺 boot-time scanner/reset installer。

## 术语

- **selected superblock**：A/B 中由 `choose_newer_superblock` 选出的合法 superblock。
- **clean tree**：selected superblock 指向的 tree，加上完整 WAL replay 后形成的最终 tree。
- **complete WAL batch**：同一 `lsn` 下，所有 entry 的 `entry_count` 一致，且 `actual_count == entry_count`。
- **logical winner**：某 logical key 在 tree leaf records 与 complete WAL batches 中 `data_ver/lsn` 最大的 record。
- **recovered durable LSN**：`max(tree_max_data_ver, max_complete_wal_lsn)`。注意 WAL 为空但 tree 非空时不能是 0。
- **boot merge**：service start 前执行的 flush-like merge，不是运行态内部 pipeline。

## 数据结构

### recovered_boot_state

```cpp
struct recovered_boot_state {
  format::format_profile profile;
  core::tree_geometry tree_geometry;

  core::tree_manifest clean_manifest;
  uint64_t recovered_durable_lsn;
  uint64_t next_lsn;

  format::paddr tree_alloc_head;
  std::vector<format::range_ref> tree_free_ranges;

  std::vector<value::live_value_extent> live_value_extents;
  std::vector<value::dead_class_hint> dead_hints;

  tree::superblock_slot active_superblock_slot;
  uint64_t superblock_generation;

  bool wrote_tree_pages;
  bool wrote_superblock;
  bool reset_wal;
};
```

`value::live_value_extent` 使用现有定义：

```cpp
struct live_value_extent {
  paddr    base;
  uint16_t byte_offset;
  uint32_t len;
};
```

`value::dead_class_hint` 使用现有定义：

```cpp
struct dead_class_hint {
  paddr    page_base;
  uint16_t class_idx;
};
```

### recovered_record

```cpp
enum class recovered_record_kind : uint8_t {
  value,
  tombstone,
};

struct recovered_record {
  std::string key;
  uint64_t data_ver;
  uint64_t lsn;
  recovered_record_kind kind;
  std::optional<format::value_ref> value;
  enum class source : uint8_t { tree, wal } source;
};
```

Tree leaf record 的 `data_ver` 来自 `leaf_record_header.data_ver`；WAL record 的 `data_ver` 等于 `wal_entry_header.lsn`。

### recovered_tree_snapshot

```cpp
struct recovered_tree_snapshot {
  core::tree_manifest manifest;
  std::vector<recovered_record> records;
  std::vector<format::value_ref> referenced_values;
  uint64_t max_data_ver;
  absl::flat_hash_set<format::paddr> allocated_ranges;
};
```

### recovered_wal_scan

```cpp
struct recovered_wal_scan {
  std::vector<recovered_record> complete_records;
  std::vector<format::value_ref> referenced_values;
  std::vector<format::value_ref> incomplete_orphan_values;
  uint64_t max_complete_lsn;
  bool saw_non_empty_wal;
};
```

`incomplete_orphan_values` 只用于 hints/TRIM。它不能影响 recovered durable LSN。

## Pipeline

### 1. Read Superblock

入口文件沿用 064A 已建的 `apps/inconel/recovery/boot.hh`，后续可拆出 `superblock_reader.hh`。

步骤：

1. 读取 LBA 0 和 LBA 1。
2. 用 `format::inspect_superblock` 验证 magic/version/CRC。
3. 用 `format::choose_newer_superblock` 选择 selected superblock。
4. 从 selected superblock 派生 `format_profile`：
   - `lba_size`
   - data area base/end
   - tree page size / shadow slots
   - WAL base / segment size / segment count
   - value class table
   - value quantum / group size
5. 用 `profile_is_self_consistent` 和 `runtime::validate_build_inputs` 同级规则 fail-fast。

064A 先只支持 4096B boot LBA。完整实现如果要支持非 4K superblock LBA，需要先解决“读取 superblock 前未知 logical LBA size”的 bootstrapping 问题；在此之前，非 4K superblock 直接报 unsupported。

如果 A/B 都无效：

- 无 `--force-format`：报错退出。
- 有 `--force-format`：走 format path，不走 recovery。

### 2. Scan Tree

输入：selected superblock 的 `root_base_paddr`。

如果 root 为空：

- `manifest = tree_manifest::empty(&tree_geometry)`。
- `records = {}`。
- `referenced_values = {}`。
- `max_data_ver = 0`。
- `allocated_ranges = {}`。

如果 root 非空：

1. 从 `root_base_paddr` 开始 DFS/BFS 遍历 reachable ranges。
2. 对每个 reachable range，从 `shadow_slots_per_range - 1` 向 0 扫描：
   - 读完整 tree page。
   - `zero_page` / `bad_crc` / `bad_magic` 视为该 slot 无效，继续向低 slot 找。
   - 第一个 `tree_page_status::ok` slot 是该 range 当前版本。
3. 如果 selected superblock 可达的某个 range 找不到任何有效 slot：
   - 这是盘面 corruption，fail-fast。
   - WAL 只负责补齐 selected tree 之后的 durable changes；它不应掩盖 selected tree 自身不可读。
4. 对 internal page：
   - 解析 separator records 和 rightmost child。
   - child paddr 必须是 range base。
   - child range 必须落在 data area 且 range 对齐。
   - 递归扫描 child range。
5. 对 leaf page：
   - 解析所有 leaf records。
   - 同一 clean snapshot 内 logical key 必须唯一；跨 leaf 重复 key 是 tree corruption。
   - 收集 value record 的 `value_ref`。
   - 更新 `max_data_ver`。
6. 构建：
   - `slot_map[range_base] = selected_slot_index`
   - `root_slot = tree_geometry.slot_paddr(root_base, root_slot_index)`
   - `root_range_base = root_base`
   - `leaf_order`
   - `reverse_topology`
   - `allocated_ranges`

注意：recovery 只遍历 selected root 可达的 tree pages。未被 selected root 可达的新 flush pages 如果存在，要么之后的 superblock update 已失败，要么 WAL 未 reset；它们由 WAL replay 或后续 reclaim 处理，不通过 Data Area 全扫发现。

### 3. Scan WAL

遍历整个 WAL area：

```text
for segment_index in [0, wal_segment_count):
  segment_base = wal_base + segment_index * segment_lbas
  read segment header
  inspect_wal_segment_header(header, superblock.format_version)
```

Header 处理规则：

- `bad_magic`：该 segment 当作 empty/free，跳过。
- `bad_crc` / `bad_version`：该 segment 不可信，跳过。
- header CRC 正确但 `segment_index` / `device_id` 与物理位置不一致：fail-fast，避免 replay 错 segment。

Trailer 只是 hint：

1. 读取 segment 末尾 `TRAILER_RESERVED`。
2. `inspect_wal_sealed_trailer` 通过，且 `segment_gen` 匹配 header，且 `write_end` 在 entries 区间内，才使用 `write_end` 作为扫描上界。
3. trailer 无效时不用 trailer；扫描上界退回 entries usable end。

Entry 扫描规则：

- WAL entry 是紧密排列的，不按 quantum 对齐。
- `offset = sizeof(wal_segment_header)`。
- 每次调用 `format::decode_wal_entry(span_from_offset, header.segment_gen, ...)`。
- 成功时 `offset += decoded_total_len`。
- `truncated`：停止当前 segment，视为 torn tail。
- `bad_segment_gen`：停止当前 segment，视为旧 generation 残留。
- `bad_total_len` / `bad_op_type` / `bad_crc`：停止当前 segment 后续扫描；这些字节之后不可信。

扫描输出按 `lsn` 分组：

```cpp
absl::btree_map<uint64_t, std::vector<decoded_wal_entry>> entries_by_lsn;
```

完整 batch 判定：

1. 同一 `lsn` 的所有 entry 必须有相同 `entry_count`。
2. 同一 `lsn` 内 logical key 不允许重复；重复 key 说明 canonical WAL invariant 破坏，fail-fast。
3. `actual_count == entry_count` 才进入 `complete_records`。
4. `actual_count != entry_count` 的 batch 丢弃；其中已解析出的 PUT `value_ref` 放入 `incomplete_orphan_values`，只作为 dead hint。

`max_complete_lsn` 只统计 complete batches；incomplete batch 不推进 durable frontier。

### 4. Build Logical Winners

先从 scanned tree 构建 `tree_by_key`，再按 `lsn` 升序应用 complete WAL records。

规则：

- `winner(key)` 取最大 `data_ver`。
- `data_ver` 相等时：
  - tree 与 WAL 内容完全一致：幂等，任取。
  - 同一 key/同一 data_ver 但 value_ref/kind 不一致：fail-fast。
- tombstone 与 value 一样参与 winner 判定。

输出：

```text
logical_winners[key] = recovered_record
recovered_durable_lsn = max(tree_snapshot.max_data_ver,
                            wal_scan.max_complete_lsn)
next_lsn = recovered_durable_lsn + 1
```

`next_lsn` 溢出时 fail-fast。

Value refs：

```text
live_value_refs = winners 中 kind == value 的 value_ref
all_known_value_refs = tree_snapshot.referenced_values
                     ∪ wal_scan.referenced_values
                     ∪ wal_scan.incomplete_orphan_values
dead_known_refs = all_known_value_refs - live_value_refs
```

`dead_known_refs` 只转换为 `dead_class_hint` 或用于可选 TRIM；即使为空，value allocator 也必须能从 `live_value_refs` 正确重建 free-state。

### 5. Decide WAL Delta

Recovery 不把 full logical winners 全量重写进 tree。只把 WAL 中改变 clean tree 的部分交给 boot merge：

```text
wal_delta = {}
for each key where winner.source == wal:
  tree_record = tree_by_key.get(key)
  if tree_record missing:
    // 即使 winner 是 tombstone，也必须写入。
    // 原因：superblock 不持久化 durable_lsn；如果 reset WAL 前不把
    // 这个 data_ver 留在 tree 中，下一次 boot 会把 next_lsn 回退。
    wal_delta[key] = winner
  else if winner.data_ver > tree_record.data_ver:
    wal_delta[key] = winner
```

如果 `winner` 是 tombstone 且 tree 中有旧 value，必须进入 `wal_delta`，否则旧 value 会复活。

如果 `winner` 是 tombstone 且 tree 中没有旧 record，也仍然进入 `wal_delta`。这类 tombstone 读语义上可能是 no-op，但它承担 durable frontier carrier 的职责。未来 compaction 只有在另一个持久 frontier 已经覆盖它时，才能安全丢弃这种 tombstone。

### 6. Boot Merge

分三种情况：

#### 6.1 No Delta

`wal_delta.empty()`：

- 不写 tree page。
- `clean_manifest = scanned_manifest`。
- `wrote_tree_pages = false`。
- 仍然可以 reset WAL，因为此时 `recovered_durable_lsn` 已经由 selected tree 中的 `max_data_ver` 表示。

#### 6.2 Empty Tree + Delta

`!scanned_manifest.has_root()` 且 `wal_delta` 非空：

- 允许使用现有 empty-tree bootstrap flush 逻辑。
- 这是在空 tree 上第一次写 tree，不是 scratch rebuild。
- 输出新的 `clean_manifest`、new ranges、live value refs。

#### 6.3 Existing Tree + Delta

`scanned_manifest.has_root()` 且 `wal_delta` 非空：

- 必须以 scanned manifest 为 base 做增量 CoW merge。
- 不变 leaf/internal page 保留现有 slot。
- 有变化的 leaf 写同 range next slot；slot exhausted 时走 consolidation/new range。
- split/root change 使用现有 flush semantics。
- 不允许从 empty manifest 重建全量 logical winners。

实现方式可以选择：

1. 生成 boot-only sealed memtable gens，然后复用 tree flush merge 核心；
2. 或直接在 recovery module 中调用拆出来的 tree merge primitive。

无论哪种方式，都不能在 scheduler handler 内部创建 hidden root submit。Boot merge 在 runtime start 前完成。

当前 064D 已实现 same-shape 子集：

- WAL winner 已被 scanned tree 覆盖：不写 tree，reset WAL。
- WAL delta 命中 existing tree 时，按 `leaf_order.find_leaf_for_key` 定位 leaf。
- 每个受影响 leaf 必须仍写原 range 的下一 shadow slot。
- merge 后 leaf page 必须单页容纳；不做 split。
- internal/root range 不变时不写 superblock；高 slot 扫描在下一次 boot 识别新 leaf slot。

以下仍 fail-fast，等待 full 064D merge primitive：

- 任一受影响 leaf shadow slot exhausted。
- 任一 leaf merge 后需要 split。
- 需要分配新 tree range、更新 internal page、更新 root range 或做 tree height 变化。

Boot merge 输出：

```cpp
struct boot_merge_result {
  core::tree_manifest clean_manifest;
  format::paddr tree_alloc_head;
  std::vector<format::range_ref> tree_free_ranges;
  bool wrote_tree_pages;
  bool root_range_changed;
};
```

### 7. Persist Ordering

如果 boot merge 写了 tree pages：

1. 所有 tree writes 完成。
2. NVMe flush，保证 tree slots durable。

如果 root range base 改变：

1. 克隆 selected superblock layout。
2. 设置 `root_base_paddr = clean_manifest.root_range_base`。
3. `generation = selected.generation + 1`。
4. 重算 CRC。
5. 写 inactive superblock slot，FUA。
6. 记录新的 active superblock slot。

如果 root range base 未改变：

- 不需要写 superblock。
- 同一 root range 内 slot index 变化由 recovery 高 slot 优先扫描识别。

只有完成上述 durable 步骤后，才能 reset WAL。

Crash 窗口：

| crash 点 | 下次 boot 行为 |
|----------|----------------|
| tree writes 前 | old tree + old WAL，重新 replay |
| tree writes 后、flush 前 | 可能读不到新 tree，old WAL 仍在，重新 replay |
| tree flush 后、superblock update 前 | old root + old WAL，重新 replay；如果 root range 未变，高 slot 可能已可见，仍幂等 |
| superblock update 后、WAL reset 前 | new root + old WAL，WAL replay 幂等 |
| WAL reset 中 | new root durable；残留 WAL 可能 partial，scanner 停止/忽略，不影响 tree 中 clean state |
| WAL reset 后 | clean boot |

### 8. WAL Reset

Recovery 完成后，WAL 不再保留需要 replay 的数据。

Reset 可以选择：

- 写零覆盖整个 WAL area；或
- TRIM 整个 WAL area，前提是本设备 read-after-trim 返回全零已验证。

064A 当前采用写零。完整实现继续使用写零最保守；将来如果切 TRIM，必须在 real NVMe guide 中记录设备前提。

Reset 后必须 flush，避免 superblock/tree 已 clean 但 WAL reset 未 durable 的重复 replay。重复 replay 是幂等的，但 clean boot latency 和日志诊断会被污染。

### 9. Rebuild Allocators

#### 9.1 Tree Allocator

`clean_manifest.slot_map` 是 tree 占用真相。

```text
range_lbas = tree_geometry.range_lbas()
allocated = clean_manifest.slot_map.keys()

if allocated empty:
  tree_alloc_head = data_area_base
else:
  tree_alloc_head = max(range_base.lba + range_lbas for range_base in allocated)

free_ranges = every aligned range in [data_area_base, tree_alloc_head)
              that is not in allocated
```

Boot merge 如果分配了新 range，必须把这些 new ranges 计入 `allocated` 后再计算 head/free list。

安装到 tree scheduler 时：

- `state.alloc.head = tree_alloc_head`
- `state.alloc.free_ranges = free_ranges`
- `state.alloc.range_lbas = tree_geometry.range_lbas()`
- `state.alloc.shadow_slots = tree_geometry.shadow_slots_per_range`
- `shared_heads.tree_head_lba = tree_alloc_head.lba`

#### 9.2 Value Allocator

Recovery 只传 occupied truth：

```text
live_value_extents = normalize(live_value_refs)
dead_hints = class hints derived from dead_known_refs
```

调用：

```cpp
value_sched.install_recovered_value_space(
  live_value_extents,
  tree_alloc_head.lba,
  dead_hints);
```

`value_space_manager::install_recovered_state(...)` 负责从 live extents 反推出：

- global free extents
- partial pages
- whole page pools
- cached/trim metadata
- `value_head_lba`

Recovery 不直接构造这些内部结构。

### 10. Runtime Install

`runtime::build_runtime` 最终需要从 `runtime_initial_state` 构造：

```cpp
struct runtime_initial_state {
  format::format_profile profile;
  core::tree_geometry tree_geometry;
  core::tree_manifest manifest;
  uint64_t durable_lsn;
  uint64_t next_lsn;
  format::paddr tree_alloc_head;
  std::vector<format::range_ref> tree_free_ranges;
  tree::superblock_slot active_superblock_slot;
  uint64_t superblock_generation;
  std::vector<value::live_value_extent> live_value_extents;
  std::vector<value::dead_class_hint> dead_hints;
};
```

Builder 安装顺序：

1. registry 持有 `tree_geometry` 生命周期。
2. 用 recovered manifest 构建 `checkpoint_guard`。
3. 构建 empty front read sets：fresh active memtable，无 imms。
4. 构建 `publish_catalog(prs, durable_lsn, epoch)`。
5. 构建 coord：`next_lsn = durable_lsn + 1`。
6. 构建 tree scheduler 后安装 allocator/frontiers/superblock slot：
   - `flush_max_lsn = durable_lsn`
   - `superblock_safe_lsn = durable_lsn`
   - `recovery_safe_lsn = durable_lsn`
7. 构建 value scheduler 后安装 recovered value space。
8. 构建 WAL scheduler 后确认 empty reusable pool。
9. 从 `manifest.leaf_order` 构建并发布 shard partition map。

运行态启动后：

- point_get 看到 clean manifest + empty memtables。
- 新写入从 `next_lsn` 开始。
- WAL 中没有旧 replay work。

## 分步落地

### 064A: Boot Profile + Empty Clean Runtime

状态：已提交。

- 新增 recovery boot module。
- 读取 superblock A/B。
- runtime builder 支持 dynamic profile/geometry。
- YCSB 无 `--force-format` 时可从 empty clean disk boot。
- root 非空或 WAL 非空 fail-fast。

### 064B: WAL Scanner + Empty Tree Replay

状态：已提交。

目标：

- 增加 `recovery/wal_scanner.hh`。
- 扫描 complete batches。
- root 为空且 WAL 非空时，把 WAL winners flush 成第一棵 tree。
- reset WAL。
- 安装 recovered runtime。

验收：

- 写少量数据，不触发 flush。
- 进程退出/重启。
- 无 `--force-format` 启动后 point_get 命中。
- delete-only WAL 也要生成 tombstone frontier carrier，重启两次后 `next_lsn` 不回退。
- incomplete batch 不 replay。

### 064C: Existing Tree Scanner

状态：已提交。

目标：

- 增加 `recovery/tree_scanner.hh`。
- 支持 root 非空、WAL 空时 boot。
- 直接安装 scanned manifest，不写 tree。
- 从 leaf live refs 重建 value allocator。

验收：

- 写入并等待 flush。
- 重启无 `--force-format`。
- point_get 命中。
- WAL empty 时没有 tree writes。

### 064D: WAL Delta On Existing Tree

状态：部分提交；same-shape leaf replay 已落地，full CoW merge 未完成。

目标：

- scanned tree + complete WAL delta 增量 CoW merge。
- 支持 root range change / same root range new slot 两种 durable 顺序。
- reset WAL。
- YCSB 默认无 `--force-format` 支持完整数据恢复。

验收：

- flush 后继续写，制造 tree + WAL 混合状态。
- 重启无 `--force-format`。
- 旧 tree value、WAL update、WAL tombstone 都恢复正确。

## 测试计划

每个阶段至少跑：

- `cmake --build build_real --target inconel_ycsb inconel_real_nvme_compile_check -j 8`
- `git diff --check`
- review gates：
  - `rg -n 'pump::sender::submit|make_root_context|the_null_receiver' apps/inconel -g'*.hh' -g'*.cc' -g'!apps/inconel/test/**'`
  - `rg -n '\bvirtual\b|\boverride\b' apps/inconel --glob '!**/test*' --glob '!**/*test*'`
- 实盘前按 `ai_context/inconel/real_nvme_test_guide.md`，不要直接用 `build/`。

Real NVMe smoke：

1. Empty clean boot：
   - `--force-format --workload c`
   - 无 `--force-format --workload c`
2. WAL-only：
   - `--force-format --workload load --records small --no-flush-after-load`
   - 无 `--force-format --workload c` + verify samples
3. Tree-only：
   - load + explicit flush 或等 auto flush
   - 重启 read-only verify
4. Tree+WAL：
   - load + flush
   - 再写 update/delete，不 flush
   - 重启 verify old/new/deleted keys
5. Delete-only frontier：
   - 空盘只写 DELETE
   - recovery 后再重启一次
   - 确认后续 PUT 不复用旧 LSN

Corruption/negative：

- invalid superblock -> no auto format。
- complete WAL count mismatch -> batch ignored。
- CRC-valid WAL invariant violation -> fail-fast。
- selected tree reachable range 无有效 slot -> fail-fast。
- non-4096 boot LBA -> unsupported，直到 boot read 方案补齐。

## Review 关注点

- recovery 是否读了 Value Area。
- WAL scanner 是否错误按 quantum 对齐。
- `recovered_durable_lsn` 是否漏掉 tree-only 最大 `data_ver`。
- incomplete batch 是否推进了 durable frontier。
- tombstone 是否能遮住旧 tree value。
- existing tree + WAL 是否误用了 scratch rebuild。
- WAL reset 是否晚于 tree/superblock durable。
- same root range/new slot 是否无需 superblock update。
- dynamic tree geometry 生命周期是否覆盖所有 manifest。
- hidden root submit 是否重新出现在 scheduler handler 内。
- `--force-format` 是否仍然是显式破坏性操作。
