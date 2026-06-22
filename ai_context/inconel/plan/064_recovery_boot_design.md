# 064 Recovery Boot Design

## 背景

063 已经把前台写入、seal、flush、读路径接通到一个自动维护循环里，但进程重启后仍然只能依赖 `--force-format` 从空盘启动。下一步必须补齐 recovery boot：运行时启动时先从盘面恢复 superblock、tree manifest、WAL 中尚未进入 tree 的完整 batch，以及 value/free-space 状态，再把这些状态一次性安装进现有 runtime。

这里的目标不是再引入一条内部异步 pipeline，而是把 recovery 作为 boot 阶段的显式前置流程：boot pipeline 产出一个 `recovered_boot_state`，随后 normal runtime 从该状态构造。服务开始前不接受前台 IO，因此 recovery 可以使用同步顺序流程；进入运行态后，后台 flush、WAL reclaim、checkpoint 仍然由现有 pump/runtime 循环驱动。

064 原先讨论过的 pre-LSN/backpressure 是相邻问题，但它依赖正常运行态的写入调度，不应该和 recovery boot 混在一个改动里。本设计把它列为后续项。

## 当前实现约束

- `format::superblock` 已经定义 A/B superblock、CRC、`inspect_superblock`、`choose_newer_superblock`。
- `runtime::build_runtime` 目前硬编码 `format::kBootstrapFormatProfile` 和 `kBootstrapTreeGeometry`，只会构造空 manifest、空 CAT、空 active memtable。
- `coord_sched` 构造函数已经支持 `initial_cat` 和 `next_lsn`，可以直接安装恢复后的 durable frontier。
- `value_sched::install_recovered_value_space(...)` 和 `value_space_manager::install_recovered_state(...)` 已经存在，且设计上不扫描 Value Area。
- tree owner 目前没有公开的 recovered manifest/allocator installer。
- WAL scheduler 目前负责运行态 append/reclaim，但没有 boot-time scan/reset 入口。
- YCSB 入口当前禁止无 `--force-format` 启动，原因正是 recovery boot 未落地。

## 目标

1. 启动时读取 superblock A/B，选择 generation 最新且 CRC 合法的一份。
2. 从 superblock 派生 runtime `format_profile` 和 `tree_geometry`，不再要求运行态只能使用 bootstrap 常量。
3. 从 root tree 扫描 leaf records，构建 `tree_manifest`、leaf order、reverse topology，以及 live value refs。
4. 扫描所有 WAL segment，只接收 canonical 且完整的 batch；不完整尾部和 CRC/结构错误之后的记录作为未提交数据丢弃。
5. 以 `(key, data_ver)` 为冲突域，选择最大 `data_ver` 的记录；tombstone 必须参与恢复，避免旧 value 复活。
6. 将 WAL 中比 clean tree 更新的记录通过 flush-like merge 合入 tree，生成新的 clean manifest。
7. 用最终 clean manifest 的 live value refs 重建 value allocator/free-space；dead refs 只能作为 hint，不能作为正确性前提。
8. 整段清空 WAL segment，安装 clean CAT/front/tree/value/WAL runtime state。
9. 移除 YCSB 对 `--force-format` 的强制要求：有合法 superblock 时 recovery boot；显式 `--force-format` 仍然是破坏性重建。

## 非目标

- 不扫描 Value Area。Value Area 只由 tree leaf 的 live refs 决定占用。
- 不引入运行态内部 hidden root submit；recovery 在 service start 前完成。
- 不把 `dead_value_refs` 变成 correctness 依赖。
- 不通过 scratch rebuild 覆盖旧 tree，也不假设 shadow range 只有 slot 0 有效。
- 不在本步解决 064 pre-LSN/backpressure。

## Recovery Boot Pipeline

### 1. Read Superblock

新增 `apps/inconel/recovery/boot_recovery.hh`：

```cpp
struct recovered_boot_state {
  format::format_profile profile;
  core::tree_geometry tree_geometry;
  core::tree_manifest clean_manifest;
  uint64_t recovered_max_lsn;
  uint64_t tree_alloc_head_lba;
  std::vector<core::range_lba> tree_free_ranges;
  std::vector<value::live_value_extent> live_value_extents;
  std::vector<value::dead_class_hint> dead_hints;
  tree::superblock_slot active_superblock_slot;
  uint64_t superblock_generation;
};
```

Boot read steps：

1. 读取 A/B 两个 superblock LBA。
2. 用 `inspect_superblock` 验证 magic/version/layout/CRC。
3. 用 `choose_newer_superblock` 选出 active slot。
4. 将 superblock 字段转换为 runtime `format_profile`：
   - namespace/data area boundaries
   - tree page/shadow/range geometry
   - WAL base/segment/count/quantum
   - value class layout
5. 对格式约束做 fail-fast 校验，尤其是 page size、shadow slots、WAL segment 对齐、data area 范围。

如果 A/B 都无效：

- YCSB 在没有 `--force-format` 时直接报错。
- 不自动 format，避免误清真实数据。

### 2. Scan Tree

输入：`superblock.root_base_paddr`。

如果 root 为空：

- manifest 为 `tree_manifest::empty(tree_geometry)`。
- leaf records 和 live refs 为空。
- tree allocator head 从 data area base 开始。

如果 root 非空：

1. 以 `root_base_paddr.lba` 为 range base，从该 range 的最高 shadow slot 向低 slot 读页。
2. 第一个 CRC/header 合法的 slot 是该 range 的当前版本。
3. 解析 page header，按 node 类型递归扫描 child range base。
4. 每个 range 只接受一次，构造 `slot_map[range_base] = slot_index`。
5. 对 leaf：
   - 读取所有 key/value/tombstone records。
   - 构建 leaf fence 到 `leaf_order_index`。
   - 收集 live value refs。
6. 构建 `reverse_topology`，用于后续 flush 计算父子关系。
7. allocator head 设置为扫描到的最大 range end 之后；未被 manifest 引用且落在 head 前的完整 range 可进入 free list。

注意：这一步必须按 range 的 shadow slots 扫描合法最新 slot，不能把 tree 退化成 slot 0。

### 3. Scan WAL

遍历 `[wal_base_lba, wal_base_lba + wal_segment_lba_count * wal_segment_count)`：

1. segment 内按 `record_quantum_bytes` 对齐扫描。
2. 读取 header、entries、trailer，验证 magic/version/header CRC/trailer CRC。
3. 只接受 canonical batch：
   - `actual_count == entry_count`
   - entry records 都合法
   - batch 不跨越 segment 边界
4. 遇到空白 quantum 表示该 segment 后续为空。
5. 遇到损坏或 incomplete record 时停止当前 segment 后续扫描。
6. 记录 `recovered_max_lsn = max(batch_lsn)`。

WAL scan 的输出不是直接进入 running WAL scheduler，而是 boot-only 的 logical delta：

```cpp
struct recovered_record {
  key_type key;
  uint64_t data_ver;
  uint64_t lsn;
  value_ref ref;
  bool tombstone;
};
```

### 4. Merge Logical State

先把 tree leaf record 加入 `logical_map`，再按 WAL batch LSN 顺序加入 WAL record。冲突规则：

- primary key 是 logical key。
- 如果新记录 `data_ver` 更大，替换旧记录。
- 如果 `data_ver` 相同，以更大的 LSN 作为 tie-breaker。
- tombstone 是正常记录，参与替换；最终 clean tree 中 tombstone 是否保留按现有 tree/compaction 规则处理，但 recovery 期间不能丢弃它。

然后只把 WAL 中真正改变 clean state 的 delta 交给 tree merge。完整设计要求使用现有 flush merge 语义，以当前 manifest 为 base 做 CoW 增量写入：

- WAL 为空：不写 tree，直接使用 scanned manifest。
- WAL 非空：生成 boot-only sealed memtable gen，调用 tree flush merge 的核心逻辑，产出新的 manifest 和 `flush_max_lsn = recovered_max_lsn`。
- 不允许从空 manifest 重建全量 tree 来替代已有 manifest；这个会绕开 shadow range 和 allocator 的正确性。

### 5. Rebuild Allocators

tree allocator：

- 基于 final manifest 的 range set 计算 allocated ranges。
- `head` 指向下一个未用 range。
- 可回收 range 放入 `free_ranges`。
- `shared_heads.tree_head_lba` 发布为最终 head。

value allocator：

- 输入 final manifest 的 live value extents。
- 调用 `value_sched::install_recovered_value_space(live_extents, tree_alloc_head_lba, dead_hints)`。
- `dead_hints` 只用于减少重启后的空间碎片，不影响正确性。

WAL：

- Recovery merge 完成后，整段 reset/TRIM 所有 WAL segment。
- WAL pool 回到 empty reusable state。
- CAT durable LSN 为 `recovered_max_lsn`。
- coord next LSN 为 `recovered_max_lsn + 1`。

### 6. Install Runtime

`runtime::build_runtime` 增加一个可选 `initial_state`：

```cpp
struct runtime_initial_state {
  format::format_profile profile;
  core::tree_geometry tree_geometry;
  core::tree_manifest manifest;
  uint64_t durable_lsn;
  uint64_t next_lsn;
  uint64_t tree_alloc_head_lba;
  std::vector<core::range_lba> tree_free_ranges;
  tree::superblock_slot active_superblock_slot;
  std::vector<value::live_value_extent> live_value_extents;
  std::vector<value::dead_class_hint> dead_hints;
};
```

Builder 安装顺序：

1. registry 持有 `tree_geometry` 的生命周期，所有 manifest raw pointer 都指向它。
2. 构建 CAT：manifest + durable LSN + empty immutable fronts。
3. 构建 coord：`next_lsn = durable_lsn + 1`。
4. 构建 tree scheduler 后安装 recovered tree allocator/frontier/superblock slot。
5. 构建 value scheduler 后安装 recovered value space。
6. 构建 WAL scheduler 后 reset/install empty WAL pool。
7. 发布 shard partition map。

运行态启动后，所有前台 IO 看到的是 clean state；WAL 中不再有需要 replay 的旧记录。

## 分步落地

### 064A: Boot Profile + Empty Clean Runtime

- 新增 recovery boot module，完成 superblock A/B 读取、选择、profile/geometry 派生。
- runtime builder 接收 dynamic profile/geometry。
- 支持 root 为空且 WAL 为空的 clean boot。
- YCSB 在无合法 superblock 时要求用户显式 `--force-format`。

这一步不会声称完整恢复已有数据；遇到非空 root 或非空 WAL 必须 fail-fast。

### 064B: WAL Replay Into Empty Tree

- 增加 WAL scanner。
- 对 root 为空、WAL 非空的场景，用 boot-only sealed memtable gen 复用 tree flush bootstrap path。
- reset WAL，安装 recovered CAT/value/tree state。

这覆盖“写入后未 flush 就崩溃”的第一类真实 recovery。

### 064C: Existing Tree Scanner

- 增加 shadow range scanner 和 manifest builder。
- 支持 WAL 为空时直接从 existing tree boot，不产生 tree write。
- 支持 value allocator 从 live refs 重建。

这覆盖“flush 后 clean shutdown/crash”的恢复。

### 064D: WAL Delta On Existing Tree

- 把 WAL delta 作为 boot-only sealed gens 合入 scanned manifest。
- 验证 CoW allocator、superblock update、WAL reset 的顺序。
- YCSB 默认无 `--force-format` 启动。

## 测试计划

每个阶段至少跑：

- 普通构建。
- review gates：
  - `rg -n 'pump::sender::submit|make_root_context|the_null_receiver' apps/inconel -g'*.hh' -g'*.cc' -g'!apps/inconel/test/**'`
  - `rg -n '\bvirtual\b|\boverride\b' apps/inconel --glob '!**/test*' --glob '!**/*test*'`
- YCSB 实盘：
  1. `--force-format load` 写入数据。
  2. 停进程。
  3. 不带 `--force-format` 启动 `read`/`run`。
  4. 校验已写 key 的 point_get 命中和值一致。
- crash-like：
  1. 控制 workload 只写入少量数据，尽量停在 WAL-only。
  2. 重启 recovery。
  3. 校验 WAL replay 结果。
- flush-like：
  1. 写入超过 auto flush 阈值。
  2. 等待 flush 完成。
  3. 重启 recovery。
  4. 校验 tree scan + value allocator。

## Review 关注点

- recovery 期间是否读取了 Value Area。
- WAL incomplete batch 是否被错误 replay。
- tree scan 是否错误假设 slot 0。
- WAL empty 的 existing tree boot 是否发生了 tree write。
- hidden root submit 是否重新出现在 scheduler handler 内。
- dynamic tree geometry 生命周期是否覆盖所有 manifest 使用期。
- `--force-format` 是否仍然是显式破坏性操作，不会自动触发。

