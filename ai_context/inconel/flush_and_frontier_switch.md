# Inconel 详细设计：Flush 与 Frontier Switch

> 依据：`ai_context/inconel/design_overview.md`（唯一概要规范）
>
> 本文细化 sealed gen 选择、fold 算法、tree_manifest 增量构造、retire list 挂接、root-change/root-stable 两类 flush 以及 old CAT/guard 回收。

## 1. Flush 总览

一次 flush round 把已发布的前台 sealed gens 物化进后台 tree，使 WAL replay 窗口缩短、memtable 可释放、旧值可回收。

Flush **不是提交点**（概要 §2.1）。它不决定数据是否可见，不阻塞前台写入，不暂停 reader。

### 1.1 Flush Round 三阶段

| 阶段 | Owner | 输入 | 输出 |
|------|-------|------|------|
| Phase 1: 选择与收集 | tree_sched + front_scheds | 当前 durable_lsn | eligible gens 的 snapshot |
| Phase 2: Fold 与写 tree | tree_sched + nvme_scheds | gens snapshot + old manifest | new tree slots + new manifest |
| Phase 3: Frontier switch | coord_sched | new guard + new manifest | CAT2（新 reader 开始使用） |

## 2. Phase 1：Sealed Gen 选择

### 2.1 Eligibility 规则

概要 §9.3 冻结：

```text
gen.max_lsn <= 当前已发布 durable_lsn → flush eligible
```

不做 partial-gen flush。一个 gen 要么整代参与本轮 flush，要么整代继续留在 imms 中。

### 2.2 选择策略

```text
1. tree_sched 读取当前 durable_lsn（从 current_cat 获取）
2. 向每个 front_sched 投递 collect_eligible_gens(durable_lsn) 请求
3. 每个 front_sched 回复：
   eligible_gens = [ gen for gen in imms if gen.max_lsn <= durable_lsn ]
   // 返回的是 intrusive_ptr 引用，保证 gen 在 flush 期间不被释放
4. reduce() 汇总所有 front_scheds 的 eligible gens
```

### 2.3 Empty Round

如果所有 front_scheds 都没有 eligible gen，本轮 flush 空转返回。这是正常情况：

- seal 刚完成但 durable_lsn 还没推进到 gen.max_lsn
- 系统空闲，没有新写入

### 2.4 Flush 触发策略

Flush 不由 timer 定期触发，而是由以下事件驱动：

1. seal 完成后：新的 sealed gen 出现，检查是否可 flush
2. `durable_lsn` 跨过某个 gen 的 `max_lsn` 时：该 gen 变成 eligible
3. WAL 空间告急时：加速 flush 以回收 sealed segments
4. 内存压力时：加速 flush 以释放 memtable

具体的调度策略属于运行时调优，不是正确性约束。

## 3. Phase 2���Fold 与写 Tree

### 3.1 Fold 的输入

```text
输入：
  1. eligible_gens: 本轮选中的 sealed gens（来自所有 front_scheds）
  2. old_manifest: 旧 checkpoint_guard.manifest（当前 tree 结构 snapshot）
  3. old_tree_leaf_state: 通过 old_manifest 可以读取的旧 tree leaf records

输出：
  1. write_plan: 需要写入 tree 的 new/updated leaf records
  2. retired_objects: 被本轮覆盖的旧 slot / value_ref / range
  3. new_manifest: 新的 immutable tree_manifest
```

### 3.2 Fold 算法

Fold 的目标：对每个逻辑 key 计算本轮 flush 后应写入 tree 的 winner record（概要 §9.4 步骤 3）。

**PUMP 异步编排**：fold 不是逐 key 查 tree leaf（每个 key 一次 NVMe round-trip），而是 batch 化：

```text
// Step A: 收集所有 memtable keys，按 key 排序
all_keys = sort(union(所有选中 gens 中出现的 keys))

// Step B: 确定受影响的 leaf pages（通过 manifest + internal 下降）
affected_leaves = identify_affected_leaves(all_keys, old_manifest)

// Step C: 并发读取所有受影响的 leaf pages
leaf_pages = for_each(affected_leaves) >> concurrent() >> on(nvme) >> read_page >> reduce()

// Step D: 在内存中合并 memtable winners 和 old leaf records
```

以下伪码描述 Step D 的逐 key 合并逻辑：

```text
for each logical_key in all_keys:

    // ── 收集所有候选 ──
    all_entries = 在所有选中的 gens 中，收集该 key 的全部 entries
    memtable_winner = all_entries 中 data_ver 最大的 entry
    old_tree_record = 在已读入内存的 leaf pages 中查找该 key

    // ── 标记 loser（§3.3）──
    for entry in all_entries where entry != memtable_winner:
        if entry.kind == value:
            entry.owning_gen.loser_durable_refs.push({ entry.vh.durable, entry.data_ver })

    // ── 比较 ──
    if memtable_winner 不存在:
        // 该 key 没有新前台更新，tree 侧保持不变
        skip

    if old_tree_record ���存在:
        // 该 key 是新 key（tree 中无历史）
        flush_winner = memtable_winner

    else if memtable_winner.data_ver > old_tree_record.data_ver:
        // 前台版本更新
        flush_winner = memtable_winner
        // old_tree_record 的 value_ref（如果是 value）需要进入 retired

    else:
        // old_tree_record.data_ver >= memtable_winner.data_ver
        // 不应发生：概要保证 batch_lsn 单调递增，且 flush 只处理已发布数据
        // 如果 data_ver 相同，说明是同一条记录的重复 replay → 幂等，skip
        assert(memtable_winner.data_ver == old_tree_record.data_ver)
        skip

    // ── 确定写入内容 ──
    if flush_winner.kind == value:
        new_leaf_record = { data_ver, kind = value, key, value_ref = flush_winner.vh.durable }
        // steady-state flush 不重写 value body（概要 §9.4 步骤 4）

    else:  // tombstone
        if old_tree_record 存在且 old_tree_record.kind == value:
            // value → tombstone：必须写 tombstone record（概要 §10.3 规则 1）
            new_leaf_record = { data_ver, kind = tombstone, key }
            // old_tree_record.value_ref 进入 retired

        else if old_tree_record 存在且 old_tree_record.kind == tombstone:
            // tombstone → tombstone：只有满足 recovery_safe 条件才能物理删除
            if old_tree_record.data_ver <= recovery_safe_lsn:
                // 可以物理删除（absent）
                remove_record = true
                // 不写入新 record
            else:
                // 保守保留 tombstone
                new_leaf_record = { data_ver = memtable_winner.data_ver, kind = tombstone, key }

        else:
            // 无旧 tree record：纯新 tombstone
            // 该 key 可能只在 memtable 中出现然后被 DELETE
            // 此时 tree 中不需要写 tombstone（无历史 value 需要遮蔽）
            skip
```

### 3.3 Memtable-only Loser 处理

在 fold 过程中，同一 gen 内同一 key 可能有多个版本（不同 batch 写入的）。这些版本中只有 `data_ver` 最大的是 winner，其余是 loser。

```text
对于每个 gen 中的 loser memtable_entry（data_ver < winner.data_ver 的 PUT entries）：
  loser.vh.durable（即 value_ref）→ 挂到 owning memtable_gen.loser_durable_refs
```

挂接时机：fold 遍历 gen 记录时，立即识别并挂接。

**注意**：这些 loser 的 `hot_blob` 不需要特殊处理。hot_blob 的释放跟随 memtable_entry 的生命周期（通过 intrusive_ptr 引用计数），与 durable value_ref 的回收是独立路径。

### 3.4 写 Tree Slots

将 fold 产出的 write_plan 应用到 tree：

```text
1. 按���辑 key 排序 write_plan
2. 对每个受影响的 leaf：
   a. 读取旧 leaf page（通过 old_manifest resolve）
   b. 合并 write_plan 中属于该 leaf 的记录
      - 新增 / 更新 / 删除（tombstone → absent）
   c. 如果合并后超出页容量 → split（产生新 leaf + 更新 parent internal）
   d. 如果合并后过于稀疏且相邻 leaf 也稀疏 → merge（可选优化，v1 可不做）
3. 写入新 leaf slot：
   - 在该 leaf 的 shadow range 中选择下一个空 slot（slot_index + 1）
   - 如果所有 slots 都已用完 → consolidation（见 §3.6）
   - 序列化 leaf records 到新 slot page
   - 提交 NVMe write
4. 如果有 internal node 需要更新（split / consolidation 产生了 child base 变化）：
   - 同样写入 internal node 的 shadow range 下一个空 slot
   - 级联向上直到不再需要更新
```

### 3.5 Shadow Slot 选择

对于每个需要写入新版本的节点（leaf 或 internal）：

```text
current_slot_index = old_manifest.slot_map[range_base]
next_slot_index = current_slot_index + 1

if next_slot_index < shadow_slots_per_range:
    // 正常写入下一个 slot
    write to slot at next_slot_index
    new_manifest.slot_map[range_base] = next_slot_index
    old_slot_paddr = resolve(range_base, current_slot_index)
    retired.old_slots.push(old_slot_paddr)
else:
    // slots 用完 → consolidation
    consolidation(range_base)
```

### 3.6 Consolidation

当一个 shadow range 的所有 slots 都已使用时，需要 consolidation：

```text
1. 分配新 range（从 tree_allocator）
2. 将当前节点的最新版本写入新 range 的 slot 0
3. 更新父节点的 child_base 指针：range_base → new_range.base
   （这次更新需要写入父节点的下一个 shadow slot）
   （父节点如果也满了 → 递归 consolidation）
4. 旧 range 整体进入 retired.old_ranges

new_manifest.slot_map[new_range.base] = 0
new_manifest.slot_map.erase(old_range_base)
```

**级联性**：consolidation 可能向上级联（概要 §10.7），但因为每个节点有 `X = shadow_slots_per_range` 个 slots，级联概率按几何级数衰减。摊销写放大 = `X / (X - 1)`��

### 3.7 Root 变化

三种情况会导致 root 变化：

1. **Root split**：root leaf/internal 满了需要 split → 创建新 root internal → `root_base_paddr` 变化
2. **Root consolidation**：root 的 shadow range 满了 → 新 range → `root_base_paddr` 变化
3. **空树 → 非空**：首次 flush 创建第一个 leaf → `root_base_paddr` 从 null 变为新 range base

Root 不变（root-stable flush）是更常见的情况：root internal 通常扇出度很高，很少需要更新。

### 3.8 New Manifest 构造

```text
new_manifest = deep_copy(old_manifest)

for each (range_base, new_slot_index) in write_plan:
    new_manifest.slot_map[range_base] = new_slot_index

for each new_range in allocations:
    new_manifest.slot_map[new_range.base] = 0

for each old_range in consolidations:
    new_manifest.slot_map.erase(old_range.base)

if root changed:
    new_manifest.root_slot = resolve(new_root_range_base, new_root_slot_index)
```

new_manifest 是 immutable 的：构造完成后不再修改。

### 3.9 NVMe FLUSH

所有 tree slot 写入完成后：

```text
nvme_sched->flush()
```

FLUSH 保证所有 tree slot 写入已 durable（概要 §10.5）。此时新 frontier 可以安全发布。

## 4. Phase 3：Frontier Switch

### 4.1 构造 New Guard 与挂接旧 Guard

本轮 flush 产出的 retired 对象必须挂在**旧 guard G0** 上，而非新 guard G1（概要 §9.5）。原因：旧 reader 仍 pin 着 G0 读旧 tree 结构；只有 G0 析构后，旧 slots/values 才不再被任何 reader 引用。

```text
// 把本轮 retired 对象挂到旧 guard G0
G0.retired.old_slots.append(old_slots)
G0.retired.old_ranges.append(old_ranges)
G0.retired.old_tree_values.append(old_tree_values)

// 新 guard G1 只持有新 manifest，retired 为空
G1 = checkpoint_guard {
    manifest = new_manifest,   // immutable, shared_ptr
    retired = {},              // 空：本轮 retired 已挂在 G0 上
}
```

### 4.2 构造 PRS2 与安装 CAT2

在 `coord_sched` 上执行：

```text
1. 读取安装瞬间当前 catalog 的 durable_lsn = D1
   // D1 不是 flush 重新计算的，是当前 CAT 已连续发布到的位置（概要 §9.4 约束 10）

2. 构造 PRS2:
   PRS2 = {
       tree_guard = G1,
       fronts = 对每个 front_sched:
           front_read_set {
               active = 当前 active（不变）,
               imms = 从当前 imms 中移除本轮已 flush 覆盖的 gens
           },
       epoch = new_epoch,
   }

3. CAT2 = {
       prs = PRS2,
       durable_lsn = D1,    // 原样继承
       epoch = new_epoch,
   }

4. install_cat(CAT2)
   // 旧 CAT (CAT1 或更早) 通过 shared_ptr 引用计数延迟释放
```

### 4.3 通知 Front Scheds 移除 Gens

Frontier switch 后，已被 flush 覆盖的 gens 需要从各 front_sched 的 imms 列表中移除：

```text
for each front_sched:
    release_gens(flushed_gen_ids_for_this_front)
```

这个操作是异步的，不阻塞新 reader。front_sched 收到后从自己的 imms 列表中移除对应 gen。但 gen 的真正释放仍取决于 shared_ptr 引用计数（旧 CAT / 旧 PRS 可能仍持有引用）。

## 5. Retire List 挂接规则

### 5.1 Old Tree-Visible Value（→ old guard）

```text
触发条件：本轮 flush 使得某个 tree-visible leaf record 的 value_ref 被取代

挂接目标：旧 checkpoint_guard (G0) 的 retired.old_tree_values

入口：
  - fold 发现 old_tree_record 是 value 且被新 winner 覆盖
  - fold 发现 old_tree_record 是 value 且新 winner 是 tombstone

时机：fold 算法中确认覆盖关系时立即挂接

释放条件：
  最后一个 pin G0 的 reader 释放 → G0 析构 → retired 投递到 tree_sched → TRIM/recycle
```

### 5.2 Old Tree Slot / Range（→ old guard）

```text
触发条件：写入新 slot 使旧 slot 失效；consolidation 使旧 range 失效

挂接目标：旧 checkpoint_guard (G0) 的 retired.old_slots / old_ranges

释放条件：同 5.1
```

### 5.3 Memtable-Only Loser Durable Ref（→ owning gen）

```text
触发条件：fold 时同 key 在 memtable 中有多个版本，只有最新的进入 tree

挂接目标：loser entry 所属 memtable_gen 的 loser_durable_refs

释放条件：
  1. owning gen 不再被任何 published CAT 触达（gen 释放）
  2. loser.data_ver <= recovery_safe_lsn（不可能在 recovery 中翻盘）
  两个条件都满足后 → 经 tree_sched TRIM 后投递到 value_alloc_sched 回收
```

### 5.4 Pre-WAL Orphan Value（→ front_sched 立即）

```text
触发条件：PUT 的 value object 已 durable 但 WAL record 最终没 durable

处理：front_sched 在 write_entries 异常路径中立即回收，或留给 recovery 清理

不进入 retire list，因为它从未进入 live read / recovery 可见状态
```

### 5.5 双重挂接防护

概要 §10.5 回收语义规则 6 要求每个被覆盖的 `value_ref` 恰好进入一次 retire 流程。

保证措施：

```text
1. tree-visible value_ref 只在 fold 计算 write_plan 时被识别和挂接，
   fold 对每个 key 只执行一次比较 → 不会重复挂接

2. memtable-only loser 只在遍历 gen 记录时被识别，
   每条 memtable_entry 只属于一个 gen → 不会跨 gen 重复

3. 一旦挂接到 retire list，后续 fold / 其他 flush round 不再触碰该 value_ref
   （因为它已不在 tree-visible 状态或 memtable 中）
```

## 6. Root-Change 与 Root-Stable 两类 Flush

### 6.1 Root-Stable Flush（常见路径）

```text
条件：本轮 flush 没有 root split / root consolidation
      new_manifest.root_slot 可能变了（root 节点写入了新 slot），
      但 root_base_paddr（root range base）没变

后续：
  - 不需要更新 superblock
  - 不产生额外 metadata IO
  - 直接 install CAT2
```

### 6.2 Root-Change Flush

```text
条件：root split / root consolidation 导致 root_base_paddr 变化

后续：
  - install CAT2（立即，不等 superblock）
  - 异步 FUA 更新 superblock inactive slot：
    superblock.root_base_paddr = new_root_range_base
    superblock.generation++
    计算 CRC
    FUA 写入 inactive superblock slot
```

**为什么不等 superblock 更新完才 install CAT2？**

1. Superblock 更新只是为了给 recovery 一个更好的 hint，不是 correctness source。
2. Recovery 的 correctness 来自 leaf records + WAL，不来自 superblock 的 root_base_paddr。
3. 如果在 superblock 更新前崩溃，recovery 从旧 superblock root 遍历 tree 得到一部分 leaf records，缺失的部分由 WAL replay 补齐（因为 `recovery_safe_lsn` 未推进，对应 WAL segments 未被回收）。

**但**：superblock 未更新意味着旧 WAL segments 不能提前回收（`recovery_safe_lsn` 不能推进到覆盖这轮 flush）。这是 `recovery_safe_lsn` 推进规则的约束（见 `runtime_state_machine.md` §4.8）。

## 7. Old CAT / Old Guard 回收链

### 7.1 回收触发链

```text
new CAT2 installed
  │
  ├── 旧 CAT1（或更早）：只剩 reader 的 shared_ptr 引用
  │   │
  │   └── 最后一个 reader 释放 read_handle
  │       │
  │       └── CAT1 shared_ptr ref → 0 → 析构
  │           │
  │           └── PRS1 shared_ptr ref → 0 → 析构
  │               │
  │               ├── fronts vector → 析构
  │               │   │
  │               │   └── 各 front_read_set 中 memtable_gen 的 intrusive_ptr ref--
  │               │       │
  │               │       └── 如果 gen ref → 0：
  │               │           ├── gen 析构 → memtable_entry 析构
  │               │           │   ├── value_handle 析构 → hot_blob ref--
  │               │           │   └── （durable value_ref 不在此释放）
  │               │           └── gen.loser_durable_refs 进入回收判定
  │               │
  │               └── tree_guard (G0) shared_ptr ref → 0 → 析构
  │                   │
  │                   └── G0.retired 投递到 tree_sched
  │                       │
  │                       ├── old_slots → TRIM
  │                       ├── old_ranges → TRIM → tree_allocator.recycle
  │                       └── old_tree_values → value reclaim 判定
```

### 7.2 Value Reclaim 判定

对于 old_tree_values 和 loser_durable_refs 中的 `retired_value_ref { vr, data_ver }`：

```text
if rvr.data_ver <= recovery_safe_lsn:
    // 安全回收
    recycle_value_slot(rvr.vr)
else:
    // 暂不回收：加入延迟回收队列，等 recovery_safe_lsn 推进
    deferred_value_reclaim.push(rvr)
```

recycle_value_slot 对不同 class 的处理（在 tree_sched 上）：
- LBA-aligned / multi-LBA：tree_sched 先 TRIM，再投递 recycle_whole(class_idx, page_base) 到 value_alloc_sched
- sub-LBA：tree_sched 按 page_base 聚合 freed slots，batch 投递 freed_slots { page_base, class_idx, freed_mask } 到 value_alloc_sched

完整的 value_alloc_sched handle 逻辑见 `runtime_state_machine.md` §6.4-§6.7。

### 7.3 延迟回收扫描

`tree_sched` 周期性检查 deferred_value_reclaim 队列：

```text
while deferred_value_reclaim.front().data_ver <= recovery_safe_lsn:
    vr = deferred_value_reclaim.pop_front()
    recycle_value_slot(vr)  // 走与 §7.2 相同的 tree_sched → value_alloc_sched 路径
```

## 8. PUMP Pipeline 编排

### 8.1 完整 Flush Pipeline

```text
flush_round()
  = on(tree_sched, check_trigger_conditions())
  >> on(tree_sched, read_current_durable_lsn())
  >> for_each(front_sched_list)
     >> concurrent(N)
     >> flat_map([durable_lsn](auto* fs) {
            return fs->collect_eligible_gens(durable_lsn);
        })
     >> reduce(merged_gens, merge)
  >> on(tree_sched, fold_visible_state(merged_gens, old_manifest))
  >> on(tree_sched, write_tree_slots(write_plan))
     // 内部：for_each(affected_leaves) >> concurrent() >> on(nvme) >> write_slot >> reduce()
  >> on(tree_sched, build_new_manifest(write_results))
  >> flat_map([](auto&& manifest) {
         return nvme_flush();
     })
  >> on(tree_sched, update_flush_max_lsn(max(选中 gens 的 max_lsn)))
  // flush_max_lsn 必须在 nvme_flush 之后更新：
  // 语义是"已 durable 进 tree"，NVMe FLUSH 完成才是 durable 保证点
  >> on(coord_sched, attach_retired_to_old_guard_and_build_g1_prs2_install_cat2(new_manifest, retired))
  >> for_each(front_sched_list)
     >> concurrent(N)
     >> flat_map([flushed_gens](auto* fs) {
            return fs->release_gens(flushed_gen_ids);
        })
     >> reduce()
  // retired 由旧 G0 析构时投递 reclaim，此处不直接 enqueue
  >> then([](auto&&) {
         if root_changed:
             return on(tree_sched, update_superblock_async());
         else:
             return just();
     })
  >> flat()
```

### 8.2 异常处理

Flush 中的异常处理遵循概要 CLAUDE.md §6.5 分层策略：

```text
操作级：
  - NVMe write slot 失败 → 该 slot 写入无效，旧 slot 仍可用（CoW 安全）
  - 但本轮 flush 必须中止，不能发布部分完成的 manifest

事务级：
  - 整轮 flush 失败 → 不 install CAT2，不修改 retire list
  - 所有已写入但未发布的新 slots 成为垃圾（TRIM 后无 CRC 引用）
  - 系统回到 flush 前状态，下次可以重试

全局级：
  - 如果 flush 连续失败（如 NVMe 硬件错误），系统仍可继续前台服务
  - WAL 空间最终会用完 → backpressure
```

## 9. Consolidation 详细流程

### 9.1 Leaf Consolidation

```text
输入：
  range_base: 当前 leaf 的 shadow range base
  latest_records: 当前最新 leaf slot 中的所有 records
  new_updates: 本轮 flush 需要写入的更新

流程：
  1. merged = merge(latest_records, new_updates)
     // 同 key → new_updates 中的优先（data_ver 更大）
  2. new_range = tree_allocator.allocate()
  3. 将 merged 写入 new_range slot 0
  4. 更新父 internal node：child_base[old_range_base] → new_range.base
     // 这需要写入父节点的下一个 slot
  5. retired.old_ranges.push(range_ref { old_range_base, shadow_slots_per_range })

new_manifest 更新：
  slot_map.erase(old_range_base)
  slot_map[new_range.base] = 0
```

### 9.2 Internal Consolidation

与 leaf 类似，但处理的是 separator keys + child pointers 而非 leaf records。

### 9.3 Root Split

```text
当 root 节点（leaf 或 internal）需要 split 时：

1. 创建两个新 range（left, right）
2. 旧 root records 分成两半，分别写入 left slot 0 和 right slot 0
3. 创建新 root range
4. 新 root = internal { separator, left.base, right.base }
5. 写入新 root range slot 0
6. root_base_paddr = new_root_range.base
7. 旧 root range → retired.old_ranges
```

## 10. Flush 与并发写的交互

### 10.1 Flush 不阻塞前台写

Flush 期间前台写正常进行：
1. 新写入进入当前 active memtable（不在 flush 选中的 gens 中）
2. `durable_lsn` 正常推进
3. reader 正常获取 read_handle

### 10.2 Flush 与 Seal 的交互

如果 flush 和 seal 并发：
1. seal 产生新的 sealed gen（原来的 active → sealed）
2. 新 gen 不会被当前正在进行的 flush round 选中（它的 max_lsn 还没被确认 <= durable_lsn）
3. 下一轮 flush 才可能选中它

### 10.3 连续 Flush Rounds

```text
flush round K:
  选中 gens: [gen_a, gen_b]
  产出: G_K, manifest_K, CAT_K+1

flush round K+1:
  ���中 gens: [gen_c]（在 round K 后新变成 eligible 的）
  old_manifest = manifest_K
  产出: G_K+1, manifest_K+1, CAT_K+2
  retired 挂在 G_K 上的对象仍由 G_K 保护
```

每轮 flush 都基于上一轮的 manifest 增量构造，不需要全量重建。
