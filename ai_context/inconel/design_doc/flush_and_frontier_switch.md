# Inconel 详细设计：Flush 与 Frontier Switch

> 依据：`ai_context/inconel/design_overview.md`（唯一概要规范）
>
> 本文细化 sealed gen 选择、tree-local flush pipeline、tree_manifest 增量构造、retire list 挂接、root-change/root-stable 两类 flush 以及 old CAT/guard 回收。

## 1. Flush 总览

一次 flush round 把已发布的前台 sealed gens 物化进后台 tree，使 WAL replay 窗口缩短、memtable 可释放、旧值可回收。

Flush **不是提交点**（概要 §2.1）。它不决定数据是否可见，不阻塞前台写入，不暂停 reader。

### 1.1 Flush Round 三阶段

| 阶段 | Owner | 输入 | 输出 |
|------|-------|------|------|
| Phase 1: 选择与收集 | tree_sched + coord_sched + front_scheds | 当前 published frontier | eligible gens 的 snapshot |
| Phase 2: Tree-Local Flush | tree_sched + tree_read_domains (lookup + worker arms) + nvme_scheds | `tree_flush_request { base_guard, sealed_gens[], recovery_safe_lsn }` | `tree_flush_result` |
| Phase 3: Frontier switch | coord_sched | `tree_flush_result.success` | CAT2（新 reader 开始使用） |

## 2. Phase 1：Sealed Gen 选择

### 2.1 Eligibility 规则

概要 §9.3 冻结：

```text
gen.max_lsn <= 当前已发布 durable_lsn → flush eligible
```

不做 partial-gen flush。一个 gen 要么整代参与本轮 flush，要么整代继续留在 imms 中。

### 2.2 选择策略

```text
1. tree_sched 向 coord_sched 请求 capture_flush_frontier()
   frontier = { durable_lsn, old_guard }
2. 向每个 front_sched 投递 collect_eligible_gens(frontier.durable_lsn) 请求
3. 每个 front_sched 回复：
   eligible_gens = [ gen for gen in imms if gen.max_lsn <= frontier.durable_lsn ]
   // 返回的是 std::shared_ptr<memtable_gen>，保证 gen 在 flush 期间不被释放
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

## 3. Phase 2：Tree-Local Flush

### 3.1 Tree-Local Flush 的输入与输出

Phase 2 本身被正式收成一个独立模块：

```text
tree_flush_request {
  base_guard,                // frontier.old_guard
  sealed_gens[],             // 本轮选中的 sealed gens
  recovery_safe_lsn,
}

tree_flush_result =
  success {
    new_manifest,
    retired,
    flushed_gens_by_front,
    memtable_losers,
    flushed_max_lsn,
  }
  or failure { error }
```

边界约束：

1. Phase 2 只做到 `tree_flush_result`。
2. `frontier_switch / release_gens / old guard reclaim` 都属于 Phase 3 及其后续消费者。
3. `tree_flush_result.success` 必须足以让外层流程构造 `G1 / PRS2 / CAT2`，而无需再回看 Phase 2 的内部临时数组。

### 3.2 Tree-Local Flush 的 sender 组合

Phase 2 在 sender 形状上看成：

```text
tree_local_flush(base_guard, sealed_gens, recovery_safe_lsn)
  = on(tree_sched, capture_flush_base_and_pin_input())
  >> on(tree_sched, fold_memtables_into_sorted_key_groups())
  >> dispatch_key_partitions_to_lookup()
  >> on(tree_sched, merge_lookup_leaf_groups())
  >> dispatch_leaf_partitions_to_workers()
  >> on(tree_sched, plan_tree_delta_from_candidates())
  >> on(tree_sched, submit_tree_page_writes_bounded())
  >> on(nvme_sched, nvme_flush())
  >> on(tree_sched, finish_flush_round())
```

这里固定三类 owner seam：

1. `tree_sched`
   - round owner
   - pin input / fold workset / fanout-fanin
   - tree delta / manifest delta / bounded writes
2. `tree_read_domain[shard_idx].lookup`
   - 用 `base_manifest->leaf_order` 做 key-group -> affected leaf mapping
   - shard 由 `current_shard_partitions()->route(key)` 决定，fold 侧 `memtable_fold::build_key_partitions` 与 read 路径共用同一张 shard_partition_map（step 030 §2.5）
3. `tree_read_domain[shard_idx].worker`
   - 读 old leaf / decode / merge / compact / candidate materialization
   - 与同 read_domain 的 lookup arm 共享 `node_cache`，candidate build 能吃到 lookup 刚热起来的 tree pages

### 3.3 Workset Fold 与 Memtable-Only Loser

Fold 的目标：对每个逻辑 key 计算本轮进入 tree-local flush 的 memtable winner，并把 losers 直接推入各自 owning gen 的 `loser_durable_refs`。如果 unfinished round 在同一 sealed gen 上重试，fold 开头对该 gen 执行 `loser_durable_refs.clear()` 后按本轮结果重建；成功 round 之后不再重复触碰这批 losers。

```text
// fold 开头：clear 每个 gen 的 loser_durable_refs（幂等保证）
for each gen in pinned_gens:
    gen->loser_durable_refs.clear()

for each key (std::string_view) in sorted(memtable keys from sealed_gens):
    // 利用 InlinedVector 内 data_ver 严格递增：
    //   back() = gen-local winner (O(1))
    //   [0..n-2] = intra-gen losers (零比较)
    // 跨 gen 只比较 K 个 gen-local winner (O(K))

    for each gen holding this key:
        // intra-gen losers: 直推 gen，零比较
        for entry in entries[0..n-2]:
            if entry.kind == value:
                gen->loser_durable_refs.push({ entry.vh.durable, entry.data_ver })
        extract gen-local winner = back()

    global_winner = max_by_data_ver(gen-local winners)

    // inter-gen losers: 直推各自 gen
    for gen-local winner != global_winner:
        if kind == value:
            gen->loser_durable_refs.push({ entry.vh.durable, entry.data_ver })

    emit flush_key_group {
        key,                       // string_view into winner's owning gen kv_arena
        winner_data_ver,
        winner_kind,
        winner_value,
        winner_pinned_gen_index,   // index into round_state.pinned_gens[]
    }
```

约束：

1. workset 只携带 durable `value_ref`，不复制 value body。
2. `memtable-only loser` 只挂接到 owning `memtable_gen`，本阶段不直接做 reclaim。
3. empty round 可以在 fold 后直接返回 success(empty delta)。
4. 同一个 sealed `memtable_gen` 不得同时属于两个 in-flight flush rounds；否则一个 round 的 clear+rebuild 会擦掉另一个 round 已挂接的 losers。

### 3.4 Leaf Mapping、Candidate Build 与写 Tree

`tree_sched` 不再自己读取 old leaf pages。Phase 2 里相关职责拆成三段：

```text
A. tree_read_domain[shard_idx].lookup: keys_to_leaf_groups()
   - 输入：sorted flush_key_groups + base_manifest->leaf_order
   - 输出：affected leaf groups
   - 约束：不允许逐 key root descend；不允许扫全树 leaf
   - fold 侧 `memtable_fold::build_key_partitions` 用同一张 `shard_partition_map` 决定 `read_domain_index`，保证 lookup / worker fan-out 到同一 read_domain（node_cache 命中率保证）

B. tree_read_domain[shard_idx].worker: build_leaf_candidates()
   - 输入：affected leaf groups + recovery_safe_lsn
   - 动作：
       1. 读取 old leaf page（通过 base_manifest.resolve）
       2. 合并 old leaf records + memtable winners
       3. 对 candidate image 做 page-local compact：
          tombstone && data_ver <= recovery_safe_lsn -> absent
       4. 返回 candidate proposals
   - 约束：
       1. worker 不分配 slot/range
       2. worker 不改 manifest
       3. split / merge / parent rewrite / root change 可以先返回 unsupported_shape_change

C. tree_sched: plan_tree_delta_from_candidates()
   - 基于 candidate proposals 规划：
       1. 哪些 leaf 零写入跳过
       2. 哪些 leaf/internal 需要写新 slot
       3. 哪些 old slot / old value / old range 进入 retired
       4. new_manifest.slot_map / new_manifest.leaf_order 如何更新
   - 然后执行 bounded writes + 一次 device flush
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

**级联性**：consolidation 可能向上级联（概要 §10.7），但因为每个节点有 `X = shadow_slots_per_range` 个 slots，级联概率按几何级数衰减。摊销写放大 = `X / (X - 1)`。

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

new_manifest.leaf_order = rebuild_leaf_order_from_tree_delta(
    old_manifest.leaf_order,
    write_plan,
    allocations,
    consolidations,
    split_or_merge_changes,
)
```

new_manifest 是 immutable 的：构造完成后不再修改。`slot_map` 与 `leaf_order` 必须在同一个 snapshot 构造过程中一起更新，不能只更新其中一侧。

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

这一步消费的是 `tree_flush_result.success`，对应 `coord_sched.handle_frontier_switch(old_guard, new_manifest, retired, flushed_gens_by_front)`。
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
  其中 old range 真正进入 `tree_allocator.free_ranges` 前，还必须先完成跨 shards 的 `tree_node` invalidate barrier（见 `runtime_memory_and_cache.md` §10.2）。
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

挂接时机：
  - fold 期间直接 push
  - unfinished round 若在同一 sealed gen 上重试，允许 clear+rebuild
  - 不需要等到 finish_flush_round 再 commit

释放条件：
  1. owning gen 不再被任何 published CAT 触达（gen 释放）
  2. loser.data_ver <= recovery_safe_lsn（不可能在 recovery 中翻盘）
  两个条件都满足后 → 经 tree_sched TRIM 后投递到 value_alloc_sched 回收
```

### 5.4 Pre-WAL Orphan Value（→ front_sched 立即）

```text
触发条件：PUT 的 value object 已 durable 但 WAL record 最终没 durable

处理：batch 在 all-WAL barrier 前失败后走 `release_batch(batch_lsn)`；orphan value 可由运行时立即回收，或留给 recovery 清理

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

3. 成功 round 挂接完成后，后续 fold / 其他 flush round 不再触碰该 value_ref。
   唯一例外是 unfinished round 在同一 sealed gen 上的 retry：允许先 clear
   该 gen 的 loser_durable_refs，再按本轮 fold 结果完整重建。
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
  - `tree_sched.superblock_safe_lsn` 可在 `nvme_flush` 完成后直接推进到本轮 `flush_max_lsn`
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
  - 在该 update completion 之前，`superblock_safe_lsn` 保持旧值；完成后再推进到这轮 flush 覆盖的 `flush_max_lsn`
```

**为什么不等 superblock 更新完才 install CAT2？**

1. Superblock 更新只是为了给 recovery 一个更好的 hint，不是 correctness source。
2. Recovery 的 correctness 来自 leaf records + WAL，不来自 superblock 的 root_base_paddr。
3. 如果在 superblock 更新前崩溃，recovery 从旧 superblock root 遍历 tree 得到一部分 leaf records，缺失的部分由 WAL replay 补齐（因为 `recovery_safe_lsn` 未推进，对应 WAL segments 未被回收）。

**但**：只有 root-change flush 会因为 superblock 未更新而阻塞 WAL reclaim。root-stable flush 不更新 superblock 仍然是安全的，因为 recovery 从相同的 root range base 出发可以扫描到最新 root slot。对应的统一规则见 `runtime_state_machine.md` §4.9。

> **实现现状 gap（055 §8.B5，2026-06-16）**：上面"install CAT2（立即，不等 superblock）"是**目标时序**，但当前 `tree/sender.hh::finalize_root_change` 把 superblock FUA 放在 `tree_flush_result` 返回**之前**（`begin_update_superblock → perform_superblock_io → finalize_flush_round`），而生产 `flush_round_once` 的 `frontier_switch`（装 CAT2）在 `tree_local_flush` 返回**之后**——即实际是 **superblock-FUA → CAT2**，与本节目标反序。**correctness-safe**（superblock 只是 recovery hint，crash 丢内存 CAT2 无妨；仅 root-change 罕见路径多一次 reader 可见延迟），但不是本节声称的时序。reorder（CAT2 先、superblock 异步在后）属 `tree_local_flush` 编排重构，随 step 2 / INC-024（update_superblock 正式化）一并做。

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
  │               │   └── 各 front_read_set 中 std::shared_ptr<memtable_gen> use_count--
  │               │       │
  │               │       └── 如果 gen ref → 0：
  │               │           ├── gen 析构（反向声明顺序）
  │               │           │   ├── table 先析构（POD entries，trivial）
  │               │           │   ├── kv_arena 后析构 → vector<unique_ptr<char[]>>
  │               │           │   │   → 所有 chunk 一次 sweep → key bytes 全部消失
  │               │           │   └── （durable value_ref 不在此释放）
  │               │           └── gen.loser_durable_refs 进入回收判定
  │               │
  │               └── tree_guard (G0) shared_ptr ref → 0 → 析构
  │                   │
  │                   └── G0.retired 投递到 tree_sched
  │                       │
  │                       ├── old_slots → TRIM
  │                       ├── old_ranges → TRIM → tree_node invalidate barrier → tree_allocator.recycle
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
- tree_sched 先按 dead `value_ref` 收集一批 `dead_value_refs`
- 然后统一投递 `reclaim_values(dead_value_refs[])` 到 value_alloc_sched
- sub-LBA / whole-page 的 page-level 聚合，以及 whole-free page 的 TRIM 完成态，都在 value_alloc_sched owner 内部处理

完整的 value_alloc_sched handle 逻辑见 `runtime_state_machine.md` §6.4-§6.8。

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
  >> on(coord_sched, capture_flush_frontier())
  >> for_each(front_sched_list)
     >> concurrent(N)
     >> flat_map([frontier](auto* fs) {
            return fs->collect_eligible_gens(frontier.durable_lsn);
        })
     >> reduce(merged_gens, merge)
  >> on(tree_sched, tree_local_flush({
         .base_guard = frontier.old_guard,
         .sealed_gens = merged_gens,
         .recovery_safe_lsn = tree_sched.recovery_safe_lsn,
     }))
  >> on(tree_sched, update_flush_max_lsn(tree_flush_result.flushed_max_lsn))
  // flush_max_lsn 必须在 tree_local_flush 成功并完成 nvme_flush 之后更新：
  // 语义是"已 durable 进 tree"
  >> on(coord_sched, frontier_switch(
         frontier.old_guard,
         tree_flush_result.new_manifest,
         tree_flush_result.retired,
         tree_flush_result.flushed_gens_by_front))
  >> for_each(front_sched_list)
     >> concurrent(N)
     >> flat_map([tree_flush_result](auto* fs) {
            return fs->release_gens(tree_flush_result.flushed_gens_by_front[fs->owner_id]);
        })
     >> reduce()
  // retired 由旧 G0 析构时投递 reclaim，此处不直接 enqueue
  >> then([](auto&&) {
         if root_changed:
             return on(tree_sched, update_superblock_async(new_root_base_paddr, covered_lsn));
         else:
             return just();
     })
  >> flat()
```

### 8.2 Tree-Local Flush Sub-Pipeline

```text
tree_local_flush(req)
  = on(tree_sched, capture_flush_base_and_pin_input(req.base_guard, req.sealed_gens))
  >> on(tree_sched, fold_memtables_into_sorted_key_groups())
  >> dispatch_key_partitions_to_lookup()
  >> on(tree_sched, merge_lookup_leaf_groups())
  >> dispatch_leaf_partitions_to_workers()
  >> on(tree_sched, plan_tree_delta_from_candidates())
  >> on(tree_sched, submit_tree_page_writes_bounded())
  >> on(nvme_sched, nvme_flush())
  >> on(tree_sched, finish_flush_round())
```

`dispatch_key_partitions_to_lookup()` 与 `dispatch_leaf_partitions_to_workers()` 的 fan-in 都必须回到 `tree_sched`，因为 manifest delta、slot/range 分配、retired 挂接和最终结果组装都只允许发生在 tree owner 上。

### 8.3 异常处理

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
