# Inconel 详细设计：Recovery 与 WAL Reclaim

> 依据：`ai_context/inconel/design_overview.md`（唯一概要规范）
>
> 本文细化 recovery 各步骤的具体算法、tombstone 物理删除规则、recovery_safe_lsn 的运行时推进以及 WAL segment 回收条件。
>
> **必读**：概要 §12.0 最高准则。任何与之矛盾的理解都是错的。

## 1. Recovery 总览

**Recovery 就是一次在 boot 时执行的 flush。** 它不扫描 Value Area，不重写 tree 到 slot 0，不需要 `dead_value_refs` 保证 allocator 正确性。它做的事和 steady-state flush 同构：确定 logical winners → 增量 CoW 合并进现有 tree → TRIM 旧 slots → 重建 allocator runtime state。

```text
boot recovery 输入：
  1. superblock A/B（格式常量、区域边界、root hint）
  2. Data Area tree leaf records（CRC-valid，通过 tree 遍历获取）
  3. WAL segments（surviving complete batches）
  —— 以上三者足够。不读任何 value body。

boot recovery 输出：
  1. tree 经过一次增量 flush（WAL winners CoW 合并进现有 tree，旧 slots TRIMmed）
  2. clean tree_manifest
  3. clean checkpoint_guard
  4. value allocator/free pool runtime state（从 live_value_refs 反推，live 是唯一真相）
  5. clean publish_catalog（CAT_clean）
  6. next_lsn = recovered_max_lsn + 1
```

核心不变量：

1. `live_value_refs`（从 logical winners 中提取的 value 类 winner 的 `value_ref`）是 Value Area 占用状态的**唯一真相**。不在 `live_value_refs` 中的 slot 就是 free，无论是已知 dead、orphan（value durable 但 WAL 未写）、还是从未写入。
2. `dead_value_refs`（`all_referenced - live` + incomplete batches）只是辅助信息：帮助识别 dead 页的 class、加速 TRIM 判定。即使完全丢弃 `dead_value_refs`，allocator 仍可以仅从 `live_value_refs` 反推出完整的占用/空闲状态。

## 2. Step 1：读取 Superblock

```text
1. 读 LBA 0（superblock A）和 LBA 1（superblock B）
2. 对每份：校验 magic == SUPERBLOCK_MAGIC，计算 CRC
3. 选择规则：
   - 两份都 CRC 通过 → 取 generation 更大的
   - 只有一份 CRC 通过 → 取该份
   - 两份都不通过 → 格式化损坏，终止并报错
4. 从选中的 superblock 提取格式常量：
   - lba_size, tree_page_size, shadow_slots_per_range
   - wal_base_paddr, wal_segment_size, wal_segment_count
   - data_area_base_paddr, data_area_end_paddr
   - value_size_classes
   - root_base_paddr（作为扫描 hint，不是 correctness source）
```

## 3. Step 2：从 Root 遍历 Tree 收集 Leaf Records

### 3.1 方法

从 `superblock.root_base_paddr` 出发，递归遍历 tree 结构，收集所有 CRC-valid leaf records。

**不做 Data Area 全量扫描。** Recovery 不需要读任何 value body，也不需要扫描 Value Area。Tree 遍历只接触 tree pages。

### 3.2 遍历流程

```text
if superblock.root_base_paddr == null_paddr:
    // 空树（格式化后首次启动，或上次 clean recovery 后无数据）
    leaf_record_pool = empty
    return

// 从 root 开始递归遍历
// superblock 中没有 slot_map，遍历时需要扫描 shadow range 确定有效 slot
traverse(superblock.root_base_paddr):

traverse(range_base):
    // 在 shadow range 中找到最新有效 slot
    best_slot = null
    for slot_idx = shadow_slots_per_range - 1 downto 0:
        slot_lba = range_base.lba + slot_idx * (tree_page_size / lba_size)
        读取前 sizeof(tree_slot_header) bytes

        if magic != TREE_PAGE_MAGIC:
            continue                             // 空 slot 或 torn write
        读取完整 tree_page_size bytes
        if CRC 不通过:
            continue                             // torn write
        best_slot = slot_idx
        break                                    // 最高有效 slot 即最新版本

    if best_slot == null:
        // 整个 range 无有效 slot → 该子树数据由 WAL 覆盖
        return

    page = 解析 best_slot 的 tree page

    if page.type == leaf:
        解析所有 leaf records → 加入 leaf_record_pool
    else:  // internal
        for each child_range_base in page.children:
            traverse(child_range_base)            // 递归
```

**为什么从高 slot 向低 slot 扫描**：同一 range 内，后写的 slot index 更大，代表更新的版本。从高到低找到的第一个 CRC-valid slot 就是该节点的最新有效版本。

### 3.3 正确性保证

遍历可能遇到的异常情况及其安全性：

| 情况 | 处理 | 为什么安全 |
|------|------|-----------|
| superblock root 是最新的 | 遍历得到完整 leaf records | WAL 只有 flush 后的新 entries，merge 正确 |
| superblock root 是旧的（flush 改了 root 但 superblock 未更新） | 遍历旧 tree，少了最近一轮 flush 的 leaf records | 那轮 flush 对应的 WAL segments 未被回收（recovery_safe_lsn 未推进），WAL replay 补齐 |
| 某个 internal node torn write | 该子树不可达 | 对应 gens 的 WAL segments 未回收，WAL replay 补齐 |
| 某个 leaf slot torn write | 该 range 回退到上一个有效 slot | 回退版本对应的更老数据由 WAL replay 覆盖 |
| root 为 null | 空 leaf_record_pool | WAL 包含一切 |

**核心保证链**：superblock 未更新 → recovery_safe_lsn 未推进 → 对应 WAL segments 未回收 → WAL replay 补齐所有遍历不可达的数据。

### 3.4 Leaf Record Pool

```cpp
struct recovered_leaf_record {
    std::string key;                                 // logical key
    uint64_t data_ver;                               // batch_lsn 语义
    record_kind kind;                                // value / tombstone
    value_ref vr;                                    // kind == value 时有效
    paddr source_slot;                               // 来自哪个 slot（诊断用）
};

// 按 (key, data_ver) 索引
multi_map<string, recovered_leaf_record> leaf_record_pool;
```

### 3.5 Recovery 不读 Value Body

Recovery 全程零 value 读取。leaf record 中的 `value_ref` 只是地址指针，recovery 直接复用它写入新 tree。value body 的完整性由写路径的 FUA ordering 保证（value write 先于 WAL FUA），surviving WAL entry 的 value_ref 一定指向已 durable 的 value object。

## 4. Step 3：扫描 WAL Segments

### 4.1 扫描所有 Segments

```text
for index = 0; index < wal_segment_count; index++:
    seg_base_lba = wal_base_paddr.lba + index * (wal_segment_size / lba_size)

    // ── 读取 header ──
    读取前 sizeof(wal_segment_header) bytes

    if magic != WAL_SEGMENT_MAGIC || CRC 不通过:
        // 无效 segment（FREE 或格式化后未使用）→ skip
        continue

    segment_gen = header.segment_gen

    // ── 尝试读取 sealed trailer ──
    读取 segment 末尾 TRAILER_RESERVED 区域
    has_trailer = (trailer.magic == 0x5345414C
                   && trailer.segment_gen == segment_gen
                   && trailer CRC 通过)

    // ── 扫描 entries ──
    offset = HEADER_SIZE
    limit = has_trailer ? trailer.write_end : (wal_segment_size - TRAILER_RESERVED)

    while offset < limit:
        读取 wal_entry_header（从 seg_base_lba + offset/lba_size 处）

        if header.segment_gen != segment_gen:
            // 旧 generation 残留 → 停止本 segment 扫描
            break

        payload_len = header.total_len - sizeof(wal_entry_header) - 4
        if offset + header.total_len > limit:
            // entry 不完整 → 停止
            break

        读取完整 entry（header + payload + crc）
        校验 CRC

        if CRC 不通过:
            // 损坏 entry → 停止本 segment 扫描
            // 后续 bytes 不可信
            break

        解析 entry → 加入 wal_entry_pool
        offset += header.total_len
```

### 4.2 WAL Entry Pool

```cpp
struct recovered_wal_entry {
    uint64_t lsn;                                    // batch_lsn
    uint32_t entry_count;                            // 该 batch 的 canonical record 总数
    uint8_t op_type;                                 // PUT / DELETE
    std::string key;
    value_ref vr;                                    // PUT 时有效
};

// 按 lsn 分组
map<uint64_t/*lsn*/, vector<recovered_wal_entry>> wal_entry_pool;
```

## 5. Step 4：Assemble Complete Batches

### 5.1 完整性判定

```text
for each (lsn, entries) in wal_entry_pool:
    expected_count = entries[0].entry_count
    // 所有 entry 的 entry_count 字段应一致（同一 batch）
    assert all entries have same entry_count

    actual_count = entries.size()

    if actual_count == expected_count:
        // 完整 batch → 加入 complete_batches
        complete_batches[lsn] = entries
    else:
        // 不完整 batch → 丢弃
        // 这些 entries 对应的 value objects 成为 orphan（后续回收）
        incomplete_batch_lsns.insert(lsn)
```

概要 §11.6 规则 6：只有 `actual_count == entry_count` 的 batch 才算完整。

### 5.2 WAL 中同 Key 唯一性

概要 §11.2 额外约束 6：WAL 保存的是 canonical batch image。因此同一 `batch_lsn` 在 WAL 中对同一逻辑 key 最多出现一次。不需要在 complete batch 内部再做去重。

## 6. Step 5：Rebuild Logical Winners

### 6.1 Merge Leaf Records 与 WAL Entries

```text
// 将所有 recovery source 合并到一个按 (key, data_ver) 索引的集合
for each record in leaf_record_pool:
    candidates[record.key].push({
        data_ver = record.data_ver,
        kind     = record.kind,
        vr       = record.vr,
        source   = "leaf",
    })

for each (lsn, entries) in complete_batches:
    for each entry in entries:
        candidates[entry.key].push({
            data_ver = lsn,            // data_ver 语义等价于 batch_lsn
            kind     = (entry.op_type == PUT) ? value : tombstone,
            vr       = entry.vr,       // DELETE 时无效
            source   = "wal",
        })
```

### 6.2 Per-Key Winner 判定

```text
for each (key, candidate_list) in candidates:
    winner = candidate_list 中 data_ver 最大的那条
    // 如果有多条相同 data_ver（leaf + wal 重复）→ 幂等，任取一条

    logical_winners[key] = winner
```

概要 §12.3 第 7 点：rebuild 在逻辑 key 维度上按 `data_ver` 幂等。

### 6.3 Live Value Ref 集合

从 logical_winners 中提取所有 kind == value 的 `value_ref`：

```text
live_value_refs = { winner.vr | winner in logical_winners.values() if winner.kind == value }
```

这个集合用于：
1. 确保这些 value objects 不被回收
2. 重建 value allocator 的 free pool

## 7. Step 6：Flush WAL Winners 进现有 Tree

### 7.1 核心原则

这一步就是一次 steady-state flush：把 WAL 中尚未体现在 tree 里的 winners 增量 CoW 合并进现有 tree。不是"重建"，不是"从 scratch 构造"，不需要把任何 slot 重写回 slot 0。

- 不变的 leaf page → 保留现有 slot，零写入
- 有变化的 leaf page → CoW 写入同一 range 的 next slot
- 结构变化（split/新 key） → 正常分配新 range

如果 WAL 为空（所有 batch 都已被 flush 进 tree），recovery **零 tree page 写入**。

### 7.2 增量合并算法

```text
// Step A: 识别 WAL-only winners（tree 中没有、或 tree 中版本更旧的 key）
wal_changes = {}
for each (key, winner) in logical_winners:
    tree_record = leaf_record_pool 中该 key 的记录（如有）
    if tree_record 不存在:
        wal_changes[key] = winner                    // 新 key
    else if winner.data_ver > tree_record.data_ver:
        wal_changes[key] = winner                    // WAL 版本更新
    else:
        // tree 中已是最新 → 无需修改
        pass

if wal_changes 为空:
    // WAL 没有引入任何变化 → tree 已经是 clean 状态
    // 只需 TRIM 每个 range 中的旧 slots，构建 clean_manifest
    goto Step C

// Step B: 在现有 tree 上执行 flush-style merge（与 flush_and_frontier_switch.md §3 同构）
// 按 key 排序 wal_changes，确定受影响的 leaf pages
// 对每个受影响的 leaf page：
//   1. 合并 wal_changes 中属于该 leaf 的记录
//   2. 写入同一 range 的 next slot（CoW）
//   3. 如果 range slots 用完 → consolidation（分配新 range）
//   4. 如果合并后 leaf 溢出 → split
// 不变的 leaf page 和 internal page → 保留现有 slot，零写入

// Step C: 构建 clean_manifest
// 遍历结果 tree（包括未修改的节点），记录每个 range 的当前有效 slot_index
clean_manifest = tree_manifest {
    root_slot = resolve(root_range_base, current_root_slot_index),
    slot_map = { range_base → current_slot_index | for each range in tree },
}
```

### 7.3 NVMe FLUSH

```text
if 有任何 tree page 写入:
    nvme_flush()  // 保证所有新写入的 tree slots 已 durable
// 如果零写入（WAL 为空），不需要 FLUSH
```

### 7.4 空树情况

如果 `logical_winners` 为空（没有任何 live 数据）：
```text
root_range_base = null_paddr
clean_manifest = tree_manifest { root_slot = null, slot_map = {} }
```

如果 tree 遍历为空但 WAL 有 winners → 从空白开始构建（分配新 range，写 slot 0），本质是 empty tree 上的一次 flush。

## 8. Step 7-8：Superblock 更新与 TRIM

### 8.1 更新 Superblock

```text
new_root_range_base = clean_manifest 中 root 所在的 range_base
if new_root_range_base != superblock.root_base_paddr:
    superblock.root_base_paddr = new_root_range_base
    superblock.generation++
    计算 CRC
    FUA 写入 inactive superblock slot
```

### 8.2 TRIM 旧 Tree

```text
// 新 tree 写入的 ranges 已经在 tree_allocator 中登记
// 旧 tree 占用的 ranges 需要被 TRIM

// 每个 range 只保留 clean_manifest 中记录的当前有效 slot，TRIM 其余旧 slots
for each (range_base, current_slot_index) in clean_manifest.slot_map:
    for slot_idx = 0; slot_idx < shadow_slots_per_range; slot_idx++:
        if slot_idx != current_slot_index:
            slot_lba = range_base.lba + slot_idx * (tree_page_size / lba_size)
            trim(paddr { device_id = 0, lba = slot_lba }, tree_page_size / lba_size)

// TRIM 不再属于当前 tree 的旧 range（如果增量合并中 consolidation 产生了新 range 替换旧 range）
for each old_range_base not in clean_manifest.slot_map:
    trim(old_range_base, tree_page_size * shadow_slots_per_range / lba_size)
```

### 8.3 回收 Dead Value Extents

```text
// 在 Step 2/4 扫描期间，也记录所有在 leaf/WAL 中出现过的 value_ref。
// 这是“已知旧引用”集合，不是占用真相；占用真相只有 live_value_refs。
all_referenced_value_refs = 所有 leaf record + WAL entry 中出现的 value_ref 的并集

// dead value = 在 surviving refs 中出现过、但不属于当前 logical winners 的 value_ref
dead_value_refs = all_referenced_value_refs - live_value_refs

// 加上 incomplete batch 中 PUT 的 value_ref（orphan）
for lsn in incomplete_batch_lsns:
    for entry in wal_entry_pool[lsn]:
        if entry.op_type == PUT:
            dead_value_refs.insert(entry.vr)

// dead_value_refs 只承担两类“快路径清理”职责：
//   1. 对 >= LBA 的 definitively-dead extent 直接 TRIM
//   2. 给 Step 9.2 的 whole-free page/extent 提供 class hint
// 它不是 allocator correctness 的前提。
for vr in dead_value_refs:
    class_idx = find_min_class_idx(vr.len + sizeof(value_object_header))
    class_size = value_size_classes[class_idx]

    if class_size >= lba_size:
        // LBA-aligned / multi-LBA：整个 extent 可安全 TRIM
        span_lbas = class_size / lba_size
        trim(vr.base, span_lbas)

    else:
        // sub-LBA：不能逐 slot TRIM（会破坏同页 live siblings）
        // 是否能整页 TRIM 由 Step 9.2 按 "free = whole value area - live" 判定
        pass
```

关键点：

1. `dead_value_refs` 不是 correctness source。某个 page/extent 即使完全没出现在 `dead_value_refs` 中，只要它不在 `live_value_refs` 中，recovery 结束后它仍必须回到 allocatable 状态。
2. Step 8.3 只是“已知 definitively-dead extents 的快路径清理”；完整的 value free-state 由 Step 9.2 从 live set 的语义补集重建。

## 9. Step 9：重建 Allocator Runtime State

### 9.1 Tree Allocator

`clean_manifest.slot_map` 是 tree 占用状态的唯一真相（与 value allocator 同一原则）。`[data_area_base, tree_alloc_head)` 区间内，不在 `slot_map` 中的 range 就是 free。

```text
// tree allocator head = 新 tree 占用的最高地址之后的第一个可分配位置
if clean_manifest.slot_map 非空:
    max_range_base_lba = max(range_base.lba for range_base in clean_manifest.slot_map.keys())
    tree_alloc_head = max_range_base_lba + (tree_page_size * shadow_slots_per_range / lba_size)
else:
    tree_alloc_head = data_area_base_paddr.lba

// 从 slot_map 反推 free_ranges：遍历 [data_area_base, tree_alloc_head) 中所有
// range 对齐地址，不在 slot_map 中的就是 free（纯内存计算，零 NVMe 读）
range_size_lbas = tree_page_size * shadow_slots_per_range / lba_size
free_ranges = []
for lba = data_area_base_paddr.lba; lba < tree_alloc_head; lba += range_size_lbas:
    if { device_id = 0, lba } not in clean_manifest.slot_map:
        free_ranges.push(range_ref { base = { 0, lba }, slot_count = shadow_slots_per_range })

tree_allocator = {
    head = { device_id = 0, lba = tree_alloc_head },
    free_ranges = free_ranges,
}
```

### 9.2 Value Allocator

**核心原则（§1 不变量重申）**：`live_value_refs` 是 Value Area 占用状态的唯一真相。`occupied = expand(live_value_refs)`；`free = 整个 Value Area 地址空间 - occupied`。`dead_value_refs` 只是辅助信息（帮助识别 class、加速 TRIM），不是 correctness source。

Recovery 与 allocator 的职责边界固定如下：

1. recovery 只负责求出 `occupied` 真相（归一化后的 `live_extents`）和 `global_value_head`；
2. `value_alloc_sched` 是 value free-space metadata 的唯一 owner；它必须把 `occupied` 反推出自己的 runtime free-state；
3. `hole_page_list` / `whole_page_pool` / `extent_free_pool` / `generic_free_spans` 都是 `value_alloc_sched` 的内部状态，不是 recovery 的跨组件输出。

Recovery 后的 free 空间分成两段：

1. `[tree_alloc_head, global_value_head)`：fresh/bump 区域。这里不需要显式枚举 free page，后续正常 bump 分配即可触达。
2. `[global_value_head, data_area_end_paddr)` 中除 `occupied` 外的全部地址：这是“历史上可能写过、但现在不 live”的上半区，必须在 boot 时由 `value_alloc_sched` 重建成 runtime 可分配状态。

第 2 段的 free-state 是对**整个上半区**做 `occupied` 的语义补集，不是对 `live ∪ dead` 这类“被 surviving refs 看见的页面集合”做补集。

```text
// ── Step 0: bump head 只看 live ──
// value allocator 从 data_area_end_paddr 向低地址分配（bump 单调递减）
// recovery 后，fresh bump frontier 可以直接推进到最低地址 live value 之前；
// 不 live 的旧页/旧 extent 会由 value_alloc_sched.install_recovered_state()
// 重建成 runtime free metadata。
if live_value_refs 非空:
    global_value_head = min(vr.base.lba for vr in live_value_refs)
else:
    global_value_head = data_area_end_paddr.lba

// ── Step 1: 从 live_value_refs 构建归一化 occupied truth（唯一真相）──

struct value_extent {
    uint64_t base_lba;
    uint16_t byte_offset;
    uint32_t class_idx;
    uint32_t span_lbas;          // sub-LBA/1-LBA = 1, multi-LBA > 1
};

live_extents: vector<value_extent>
for vr in live_value_refs:
    class_idx = find_min_class_idx(vr.len + sizeof(value_object_header))
    class_size = value_size_classes[class_idx]
    span_lbas = max(1, class_size / lba_size)
    live_extents.push({ vr.base.lba, vr.byte_offset, class_idx, span_lbas })

sort live_extents by (base_lba, span_lbas, byte_offset)

// ── Step 2: dead refs 只作为可选 class / TRIM hint ──
struct dead_class_hint {
    uint64_t base_lba;
    uint16_t byte_offset;
    uint32_t class_idx;
    uint32_t span_lbas;
};

dead_class_hints: vector<dead_class_hint>
for vr in dead_value_refs:
    class_idx = find_min_class_idx(vr.len + sizeof(value_object_header))
    class_size = value_size_classes[class_idx]
    dead_class_hints.push({
        vr.base.lba,
        vr.byte_offset,
        class_idx,
        max(1, class_size / lba_size),
    })

// ── Step 3: 把 occupied truth 交给唯一 owner ──
// recovery 不直接拼 hole_page_list / per-class pools；这些都是 value_alloc_sched 的内部状态。
value_alloc_sched.install_recovered_state(
    live_extents,               // 唯一 correctness source：occupied truth
    global_value_head,          // fresh bump frontier
    data_area_end_paddr.lba,    // Value Area 上界
    dead_class_hints,           // 可为空；仅用于 class 归桶/TRIM 优化
)
```

**回收总结**：

| 区域状态 | 真相来源 | 处理 |
|--------|-----------|------|
| sub-LBA 页内有 live slots | `live_value_refs` | `value_alloc_sched` 从 `live_slots` 反推 `free_mask`，写入 `hole_page_list` |
| ≥ LBA 的 live extent | `live_value_refs` | 视为占用，跳过 |
| whole-free region，class 可推断 | `occupied` 的补集 + 可选 dead hint | `value_alloc_sched` 负责 TRIM（必要时）+ per-class pool |
| whole-free region，无 surviving ref | `occupied` 的补集 | `value_alloc_sched` 必须保存为 owner-local `generic_free_spans` 或等价状态；不能泄漏 |

### 9.3 数据区域空间检查

```text
// 验证 tree 和 value 没有交叉
assert(tree_alloc_head <= global_value_head)
// 如果相等或交叉 → Data Area 已满，boot 后需要紧急 reclaim
```

## 10. Step 10-15：安装 Clean Runtime

```text
// Step 10: 重置 WAL��必须整段 TRIM，不允许只清 header）
// 原因：如果只清 header，旧 entries/trailer 残留字节仍在盘上。
// 新 runtime 从 alloc_head=0 重新分配时，旧残留可能在下一次 recovery 中
// 被错误解析（跨 recovery 的 segment_gen 续接在当前设计中未定义）。
for each wal segment:
    TRIM entire segment
wal_space_state = {
    alloc_head = 0,
    free_pool = empty,
    // 所有 segments 从初始池重新分���
}

// Step 11: recovered_max_lsn
recovered_max_lsn = max(
    max(lsn for lsn in complete_batches.keys()),
    max(winner.data_ver for winner in logical_winners.values()),
    0  // 如果两者都为空
)

// Step 12: 安装新 active memtables + 初始化 value allocator 输入
for each front_sched (0 .. front_sched_count-1):
    front.active = std::make_shared<memtable_gen>(gen_id = next_gen_id++, st = active)
    front.imms = []
    front.wal = 新的空 wal_stream_state

// Step 12a: 将 recovered occupied truth 交给 value_alloc_sched
// recovery 只提供 live/occupied truth 与 bump head；
// value_alloc_sched 作为单一 owner，自行重建 hole_page_list / per-class pools /
// generic_free_spans 等 runtime free state（不按 hash 分发）
value_alloc_sched.install_recovered_state(
    live_extents,            // normalized occupied truth
    global_value_head,       // bump head
    data_area_end_paddr.lba, // Value Area 上界
    dead_class_hints,        // 可为空；只做 class 归桶/TRIM hint
)

// Step 13: PRS_clean
PRS_clean = published_read_set {
    tree_guard = checkpoint_guard { manifest = clean_manifest, retired = {} },
    fronts = [ { active = front[i].active, imms = [] } | for each front ],
    epoch = 1,
}

// Step 14: CAT_clean
CAT_clean = publish_catalog {
    prs = PRS_clean,
    durable_lsn = recovered_max_lsn,
    epoch = 1,
}
current_publish_catalog = CAT_clean

// Step 15: 开始服务
next_lsn = recovered_max_lsn + 1
superblock_safe_lsn = recovered_max_lsn  // clean runtime 的当前 superblock root 已覆盖整棵 clean tree
recovery_safe_lsn = recovered_max_lsn  // 所有旧版本都已收敛到 clean tree
```

## 11. Tombstone 物理删除规则

### 11.1 Boot Recovery 中

概要 §12.3 第 9 点：boot recovery 一律保守保留 tombstone。

原因：boot recovery 没有旧 runtime 的 `recovery_safe_lsn`，无法判断 tombstone 的 `data_ver` 是否足够老。但因为 recovery 最终会收敛成唯一的 clean tree，所有 tombstone 都对应真实的"该 key 曾有 value 后被 DELETE"的状态。

### 11.2 Steady-State 中

概要 §10.3 规则 2：后续 flush 中，tombstone 物理删除需满足：

```text
tombstone.data_ver <= recovery_safe_lsn
```

含义：即使在此刻崩溃并 recovery，该 tombstone 覆盖的所有更老版本都不可能从 WAL 或旧 leaf records 中出现。因此安全删除。

v1 的 steady-state tombstone GC 是**page-local opportunistic compaction**：

1. flush 仍只用 memtable-touched keys 来定位 `affected_leaves`
2. 只要某个 leaf 因本轮 flush 被重写，就会顺带检查该页上的 tombstone records
3. 满足 `data_ver <= recovery_safe_lsn` 的 tombstone 可以在新 leaf image 中直接省略（变成 absent）
4. v1 不为旧 tombstone 维护单独的全局 revisit / sweep 队列；未被触达的 leaf 继续保守保留 tombstone

### 11.3 实现

在 leaf rewrite 阶段（`flush_and_frontier_switch.md` §3.4）：

```text
candidate_records = merge(old_leaf.records, point_mutations_for_this_leaf)

for record in candidate_records:
    if record.kind == tombstone
       && record.data_ver <= recovery_safe_lsn:
        // 可以物理删除（变成 absent）
        // 不把该 tombstone 拷入新的 leaf image
        continue

    new_leaf_image.push(record)
```

关键点：

1. 这里处理的是**整页 compact**，不是“逐 key 主动 revisit 旧 tombstone”。
2. 因此，即使某个 tombstone 对应的 key 本轮没有新 memtable 更新，只要它所在 leaf 因其他 key 更新而被重写，也可以顺手物理删除。

## 12. Recovery-Safe LSN 与 WAL Reclaim

### 12.1 `recovery_safe_lsn` 定义

`recovery_safe_lsn` 是运行时量（不持久化），表达：

```text
所有 data_ver <= recovery_safe_lsn 的历史旧版本，
如果不是当前 durable winner，
就不可能再从 boot recovery 的输入（leaf scan + WAL scan）中出现。
```

### 12.2 推进规则

`recovery_safe_lsn` 的推进取决于三个条件：

```text
recovery_safe_lsn = min(
    flush_frontier,         // tree 已 flush 到这里
    superblock_safe_frontier, // 当前 on-disk superblock root 已能覆盖到这里
    wal_reclaim_frontier,   // 对应的 WAL segments 已回收或仍可安全解释
)
```

分解：

**flush_frontier** = 最近一次成功 flush 的 `flush_max_lsn`。即 tree 已经物化了这些 batch 对应的 winner records。

**superblock_safe_frontier**：当前 on-disk superblock root 已经能够覆盖到的最大 flushed frontier。它不是“superblock 里存了一个 flush_lsn”，而是 runtime 对恢复可达性的判断量：

1. **root-stable flush**
   - `root_base_paddr` 不变，只是该 root range 中的 slot 前进了。
   - recovery 从 superblock 中的 `root_base_paddr` 出发，扫描这个 shadow range 时仍能找到最新 root slot。
   - 因此本轮 `nvme_flush` 完成后，`superblock_safe_frontier` 可以直接推进到该轮 `flush_max_lsn`，不需要更新 superblock。

2. **root-change flush**
   - `root_base_paddr` 改变。
   - 在异步 superblock 更新完成前，recovery 仍只能从旧 root base 出发；这轮及其之后的变更必须依赖 WAL 补齐。
   - 因此在 update completion 前，`superblock_safe_frontier` 不能推进；完成后再推进到该次更新覆盖的 `flush_max_lsn`。

**wal_reclaim_frontier** = 所有已回收 WAL segments 的 max_lsn 的最大值。即这些 WAL 信息已经在 tree 中体现，不再需要。

实际实现中，v1 可以简化为：

```text
if sealed_segments_not_yet_reclaimed 为空:
    wal_frontier = flush_max_lsn           // 无未回收 segment，WAL 不构成约束
else:
    wal_frontier = min(seg.min_lsn for seg in sealed_segments_not_yet_reclaimed) - 1

recovery_safe_lsn = min(
    flush_max_lsn,
    superblock_safe_lsn,
    wal_frontier,
)
```

### 12.3 WAL Segment 回收条件

概要 §9.5 规则 4：WAL segment 回收看的是 **recovery safety**。

一个 sealed WAL segment 可以被回收，当且仅当：

```text
条件 1: seg.max_lsn <= flush_max_lsn
    // 该 segment 中所有 batch 都已被 flush 进 tree

条件 2: seg.max_lsn <= superblock_safe_lsn
    // root-stable flush 可在无 metadata IO 的情况下推进这条边界
    // root-change flush 则必须等对应 superblock update completion
```

简化表达：

```text
segment 可回收 ⟺ seg.max_lsn <= recovery_safe_lsn
```

### 12.4 WAL Reclaim Pipeline

Sealed segment 元数据由 `front_sched` 换段时搭在 `alloc_segment` 请求上 push 给 `wal_space_sched`（零额外消息，见 `runtime_state_machine.md` §5.3）。回收判定由 `wal_space_sched` 本地完成：

```text
wal_reclaim_round()
  = on(tree_sched, compute_recovery_safe_lsn())
  >> on(wal_space_sched, reclaim_check(recovery_safe_lsn))
    // wal_space_sched 本地筛选 sealed_segments 中 max_lsn <= recovery_safe_lsn 的
    // 放回 free_pool（见 runtime_state_machine.md §5.4）
```

### 12.5 Reclaim 与 Flush 的协调

```text
典型生命周期：

1. segment 在 front_sched WAL stream 中被使用 [ACTIVE]
2. segment 写满 → seal，元数据搭在 alloc_segment 请求上 push 给 wal_space_sched [SEALED]
3. segment 中的 batch 全部 publish [SEALED, published]
4. 包含 segment 中 batch 的 gens 全部 flush [SEALED, flushed]
5. 对应 flush 的 superblock 更新完成 [SEALED, sb_updated]
6. tree_sched 推进 recovery_safe_lsn → 投递 reclaim_check 给 wal_space_sched
7. wal_space_sched 本地筛选 max_lsn <= recovery_safe_lsn → 回收 [FREE]
8. 下次某个 front_sched 需要新 segment → 重新分配 [ACTIVE, gen++]
```

## 13. Value Free Pool 闭环

### 13.1 Steady-State 的 Value 回收来源

| 来源 | 挂接位置 | 释放条件 |
|------|---------|---------|
| old tree-visible value | old checkpoint_guard.retired | guard 释放 + `data_ver <= recovery_safe_lsn` |
| memtable-only loser | owning memtable_gen.loser_durable_refs（flush fold 期间直接挂接；unfinished retry 可 clear+rebuild） | gen 释放 + `data_ver <= recovery_safe_lsn` |
| pre-WAL orphan | 无（直接回收或 recovery 清理） | WAL 写入失败时立即回收 |

### 13.2 Boot Recovery 的 Value 回收

Recovery 不是“枚举所有曾经分配过的 slot”，而是直接重建下面这个语义：

1. `occupied = clean tree / logical winners` 中的 `live_value_refs`
2. `free = 整个 Value Area - occupied`

其中：

1. `[tree_alloc_head, global_value_head)` 这段 fresh 空间由 bump head 覆盖；
2. `[global_value_head, data_area_end_paddr)` 中除 `occupied` 外的全部地址，由 `value_alloc_sched` 依据 `occupied` 的语义补集，重建为 `hole_page_list` / `whole_page_pool` / `extent_free_pool` / `generic_free_spans` 等 owner-local metadata；
3. `dead_value_refs` 只是帮助某些 whole-free region 更快确定 class 并 TRIM；不是“free 能否找全”的前提。

因此，即使某个 crash-open page / orphan-only extent 从未出现在 leaf/WAL refs 中，只要它不在 `live_value_refs` 里，recovery 结束后它也必须重新回到 allocatable 状态，而不是悬空在 `global_value_head` 之上。

### 13.3 不泄漏论证

```text
每个 value object 从创建到回收的完整路径：

创建 → 前台 PUT value durable → value_ref 进入以下之一路径：

路径 A（正常提交 + flush）：
  value_ref → WAL + memtable → flush → tree leaf record
  → 下一次 flush 被更新版本覆盖
  → old value_ref 挂到 old guard retired
  → guard 释放后回收

路径 B（正常提交 + 同 gen 内被覆盖）：
  value_ref → WAL + memtable
  → fold 时输给同 key 更新版本（直接挂到 owning gen loser_durable_refs）
  → unfinished round 若在同一 sealed gen 上重试，先 clear 再重建
  → value_ref 挂到 owning gen loser_durable_refs
  → gen 释放且 recovery_safe 后回收

路径 C（value durable 后 WAL 失败）：
  value_ref → 前台 WAL 异常路径立即回收
  或 crash 后 recovery 识别为 orphan 并 TRIM

路径 D（crash 时尚在 memtable 但未 flush）：
  value_ref → WAL 中有完整 batch → recovery replay → 路径 A 或 B
  value_ref → WAL 中 batch 不完整 → dead value → recovery TRIM

每条路径都有明确的回收终点。不存在"value_ref 悬空但无人负责"的状态。
```

## 14. Recovery 的 PUMP Pipeline

```text
recover()
  = on(coord_sched, read_superblock_ab())
  >> then([](auto&& sb) {
         return when_all(
             on(tree_sched, traverse_tree_collect_leaf_records(sb.root_base_paddr)),
             scan_all_wal_segments(sb)
         );
     })
  >> flat()
  >> on(coord_sched, assemble_complete_batches(wal_entries))
  >> on(tree_sched, merge_and_rebuild_logical_winners(leaf_records, complete_batches))
  >> on(tree_sched, merge_winners_into_existing_tree_and_build_clean_manifest(winners))
     // 内部：只写受 WAL winners 影响的 tree pages；不读 Value Area，不重写 live values
  >> maybe_if_tree_pages_written(nvme_flush())
  >> on(tree_sched, update_superblock_if_needed(new_root))
  >> on(tree_sched, trim_old_tree_and_reclaim_known_dead_values(old_slots, dead_vrs))
  >> on(tree_sched, rebuild_allocator_state_from_live_set(live_vrs, dead_vrs, clean_manifest))
  >> on(wal_space_sched, reset_wal_pool())
  >> on(coord_sched, install_clean_runtime(clean_manifest, recovered_max_lsn))
```

## 15. 边界情况

### 15.1 空盘启动

格式化后首次启动：
- superblock 有效，root_base_paddr = null
- Data Area 全 TRIM（无 leaf records）
- WAL 无 segments
- → 空 logical winners
- → 空 tree
- → recovered_max_lsn = 0, next_lsn = 1

### 15.2 写入后立即崩溃

写入了一些 batch 但未 flush 就崩溃：
- superblock root 仍为 null（或旧值）
- Data Area 无（或有旧的）leaf records
- WAL 有 surviving entries
- → complete batches 从 WAL 恢复
- → 在空/旧 tree 上执行一次 recovery flush，得到包含这些 winners 的 clean tree

### 15.3 Flush 后崩溃（superblock 已更新）

- superblock root 有效
- Data Area 有新 tree leaf records
- WAL 可能有 flush 之后的新 entries
- → leaf scan 覆盖已 flush 数据
- → WAL scan 覆盖 flush 后的新数据
- → merge 后只对受影响的 tree 节点做 CoW，收敛到新的 clean tree

### 15.4 Flush 后崩溃（superblock 未更新）

- superblock root 指向旧 tree
- Data Area 有新旧两套 tree leaf records
- WAL 有 flush 涵盖的 entries 和 flush 后的 entries
- → leaf scan 收集新旧所有 records
- → WAL scan 收集所有 surviving entries
- → merge 时按 data_ver 幂等 → 结果正确
- → 在旧 tree 基础上增量 flush WAL winners，收敛到唯一 clean tree
