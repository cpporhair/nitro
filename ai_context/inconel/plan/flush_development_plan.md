# Tree-Local Flush 开发计划与 Checklist（临时）

> 本文基于 `flush_module_guide.md`，把 flush 模块开发进一步落成可执行的阶段计划和 checklist。
>
> 分工：
>
> 1. `flush_module_guide.md` 负责说明 flush 模块的目标、边界、sender 组合形状和 planning seam
> 2. **本文**负责说明"先做什么、后做什么、每一阶段做到什么算完成"
> 3. `027_worker_inmemory_tree_proposal.md` 是 worker 侧重构的详细设计文档；其余 step（Phase 8 / Phase 9）的详细设计由本文在架构冻结状态下直接生成
>
> 本文是临时开发文档，放在 `ai_context/inconel/plan/`。等 flush 模块开发完成、正式设计回写到 `design_doc/` 之后，本文连同 `027_worker_inmemory_tree_proposal.md` / `flush_module_guide.md` 统一删除。

## 1. 开发总原则

### 1.1 先改设计，再写代码

flush 模块的第一个阶段必须是**正式设计文档同步**。

理由：

1. 当前正式设计文档和临时开发指导之间仍存在边界差异
2. 如果不先同步正式设计，后续实现者很容易按旧设计切代码
3. tree-local flush 模块牵涉 `tree_sched / tree_lookup_sched / tree_worker_sched / leaf_order / bounded writes` 多个 seam，先统一设计比先写代码更重要

因此：

1. 第一个 step 计划必须是设计文档变更
2. 在该 step 完成前，不进入 flush 相关 production 代码实现

### 1.2 本计划只覆盖 tree-local flush

本文的计划范围严格限定在草案已经冻结的 **tree-local flush module**：

```text
capture base / pin input
-> fold workset
-> worker fanout (一次)
-> owner 侧合并（纯内存）
-> paddr 分配 + CRC
-> bounded writes
-> device flush
-> tree_flush_result
```

本计划只覆盖这条 pipeline 本体，不讨论 request 之前的来源，也不讨论 `tree_flush_result` 之后的消费流程（frontier_switch / release_gens / guard reclaim 由外层 step 接管）。

### 1.3 Phase 7 / 8 合并为一次 owner 侧闭环（2026-04-15 修正）

原计划把 Phase 7（Root-Stable Writer）和 Phase 8（Shape-Changing Paths）分两步做——先 root-stable，后 split / consolidation / root-change。**这个拆法违反项目规则约束 A 和 `feedback_layered_complete_not_prototype`**：

1. Phase 7 单独 ship 意味着 production 代码里有 `split / consolidation / root-change` 的 `unsupported_*` stub，属于"先收窄实现，后补齐"——约束 A 明令禁止。
2. `flush_changed_node` 的数据结构 shape 由 Phase 8（split 多 page + 新 separator）决定；Phase 7 单独落会先按单 page 形状，Phase 8 必然回头改 plan / new manifest rebuild / writer 全链路。
3. `tree_local_flush` 顶层 pipeline + bounded writes + nvme_flush + finish_flush_round 的闭环编排只编一次。
4. INDEX.md v1 语义冻结"Phase 7 单独不可作为 v1 交付；root-stable 与 shape-changing 都必须完成"。Phase 7 单独 ship 只在开发中间态有意义。

**修正**：Phase 7 / 8 合并为新 **Phase 9（Owner 侧闭环）**，一次做完 root-stable + shape-changing + paddr 分配 + writer + new manifest 构造。

与此配套，worker 侧的重构（新数据结构、删 cascade、删 Phase 5 残留）独立为新 **Phase 7（Worker 侧输出模型重构）**；中间加一步 **Phase 8（Dead code / 死注释 / 旧测试 sweep）** 保证 Phase 9 在干净代码库上开工。

### 1.4 每个阶段必须保留 sender seam

后续开发和 review 时，按这些 sender seam 切。注意 Phase 7 之后，seam 列表**会简化**——从"两次 fanout"合并为"一次 fanout"（见 §2.Gap 3 和 Phase 7）：

当前（Phase 6 之前）的 seam：

1. `capture_flush_base_and_pin_input`
2. `fold_memtables_into_sorted_key_groups`
3. `dispatch_key_partitions_to_lookup`
4. `merge_lookup_leaf_groups`
5. `dispatch_leaf_partitions_to_workers`
6. `plan_tree_delta_from_candidates`
7. `submit_tree_page_writes_bounded`
8. `finish_flush_round`

Phase 7 之后的 seam：

1. `capture_flush_base_and_pin_input`
2. `fold_memtables_into_sorted_key_groups`（内含 leaf-对齐 partition）
3. `dispatch_partitions_to_workers`（**唯一** fanout；worker handle 做 mapping + build + shape-change 一体）
4. `merge_worker_proposals`（owner 侧合并，含 paddr 分配 + CRC + writes）
5. `finish_flush_round`

**不要**在单个阶段里跨多个 seam 做"大一统 flush 实现"。

### 1.5 现有 memtable 与 flush 共用数据结构的边界

这里需要明确区分两类对象：

1. **已有 front-domain 对象**
   - `memtable_gen`
   - `memtable_entry`
   - 以及它们已有的 key/value metadata
   - 这些不是 flush 模块新发明的数据结构，后续实现直接复用，不在 flush phases 里"重做一套 memtable"

2. **flush 模块新增的 shared carrier**
   - `tree_flush_request` / `tree_flush_result`
   - flush round state
   - `worker_tree_proposal`（Phase 7 引入，替代 Phase 5/6 的 `flush_mapping_req` / `flush_worker_req` / `flush_candidate_batch`）
   - `mem_tree_node` / `child_ref`（Phase 7 引入，替代 Phase 6 的 `flush_changed_node`）

换句话说：

1. **memtable 本体不是本计划的实现目标**；flush 读取和引用它。
2. **flush 自己的跨-scheduler carrier 是本计划的实现目标**；不能边写算法边临时拼。

---

## 2. Spec Gap 决议

以下三条是 Phase 9（Owner 侧闭环）开工前的**不可谈判前提**。按项目规则约束 C（"设计缺口禁止自补 spec"），都必须在 step 开工前定掉并回写 `design_doc/`。三条均已决议（2026-04-15）。

### 2.1 Gap 1 — Loser 归属：memtable-only 与 tree-side 分离

Flush round 产出两类 retire 对象，必须分别挂在不同的生命周期对象上，**禁止合并**。

#### 2.1.1 Memtable-only loser（fold 产生）

**定义**：fold 时被同 key 更新版本超越的 memtable entry 中的 value_ref，**从未进过 tree**。

**冲突来源**：

- FF §3.3 / §5.3 / §7.1 明确：挂 owning `memtable_gen.loser_durable_refs`，gate = gen ref→0。
- RSM §4.2 的 `tree_flush_result` 字段声明里含 `memtable_losers` vector，暗示"loser 作为 flush result 返回外层"。
- 代码里 `tree/flush_types.hh:316` 和 `tree/flush_round_state.hh:160` 各有一个 `memtable_losers` 字段，**没有 producer**——fold 已按 FF §3.3 路径直接 push 进 `gen.loser_durable_refs`（`tree/memtable_fold.hh:140-178`）。

**决议**：按 FF §3.3 定死，result / round_state 那两个悬挂字段删除。

1. `core::memtable_gen.loser_durable_refs` 是 memtable-only loser **唯一挂接点**。
2. 删掉 `tree_flush_result.memtable_losers` 字段。
3. 删掉 `flush_round_state.memtable_losers` 字段。
4. RSM §4.2 的 `tree_flush_result` 声明更新，去掉 `memtable_losers`。
5. 回收语义已在 FF §5.3 / §7.1 写清楚，不新增描述。

**正确性论据**：

- memtable-only loser V_A 的可观测路径唯一：`reader.CAT.PRS.fronts[f] → gen_X.table → entry.vh.durable = V_A`。
- 路径上 `gen_X` 是唯一必经 shared_ptr 节点。
- V_A 的 reclaim gate = "这条路径无 reader 可达" = `gen_X.ref_count == 0`。
- 一个 gen 可以跨多个 tree_guards（sealed 之后错过某轮 flush 仍留 imms，tg 已被那轮 frontier_switch 切换），所以任一单一 tree_guard 的 pin 集都是 gen pin 集的真子集。
- 挂到任何 tree_guard / 全局 retire list / flush round pin_bundle，都会在"gen 跨 tg"场景下提前回收（`read_lsn >= V_A.data_ver` 的长读 reader 仍在读 V_A 字节）→ use-after-free。

#### 2.1.2 Old tree value（worker merge 产生）

**定义**：worker 的 `merge_and_build_leaf` 在合并 old leaf 记录 + memtable winners 时，识别为"曾经是 tree-visible winner、本轮被 memtable winner 覆盖（或被 tombstone 取代）"的 value_ref。

**冲突来源**：

- FF §4.1 / §5.1 / §5.2 / §7.1 明确：挂到 `G_K.retired.old_tree_values`（旧 guard 的 retire 包），frontier_switch 在构造 CAT2 时把 `tree_flush_result.retired` 里的内容 append 到 G_K.retired；gate = G_K ref→0。
- 当前代码里 worker 会 push 到 `flush_leaf_candidate.retired_old_values`（`tree/candidate_build.hh:252-258`），最终汇进 `flush_worker_result.retired_old_values`。但**没有任何 handle 把它们抬进 `flush_round_state.retired.old_tree_values` 或 `tree_flush_result.retired.old_tree_values`**——这条路径在 Phase 9 里接通。
- `core/checkpoint_guard.hh:43-49` 当前还没有 `retired` 字段（注释明确说留给 frontier_switch step）。

**决议**：按 FF §4.1 / §5.1 / §5.2 定死。

1. Old tree value 的**唯一**挂接点是 `checkpoint_guard.retired.old_tree_values`（G_K，即本轮 flush 的旧 guard），**禁止**挂 `memtable_gen` 或任何全局结构。
2. Phase 7 worker 产出的 `worker_tree_proposal.retired_old_values` 由 Phase 9 tree_sched 合并 handle 汇总进 `round_state.retired.old_tree_values`，再由 `finish_flush_round` 填进 `tree_flush_result.retired`。
3. `tree_flush_result.retired` 作为值传给 frontier_switch step（coord_sched 的 `handle_frontier_switch`，**不在 Phase 9 范围**）；frontier_switch 负责把 retired 内容 append 到 G_K 的 `checkpoint_guard.retired`。
4. `core::checkpoint_guard` 扩 `retired_objects retired` 字段（Phase 9 做）；destructor 里的 reclaim_task 投递逻辑由 frontier_switch step 实现，Phase 9 不做。

**正确性论据**：

- old tree value V_B 的可观测路径唯一：`reader.CAT.PRS.tree_guard → G_K.manifest.resolve(range_base) → old slot leaf record.value_ref = V_B`。
- 路径上 `G_K` 是唯一必经 shared_ptr 节点（manifest 是 immutable，但它本身依附于 guard 生命周期）。
- V_B 的 reclaim gate = `G_K.ref_count == 0`。

#### 2.1.3 两类 loser 为什么不能合并

两条可观测路径经过**互不替代的 shared_ptr 节点**：

- `V_A`（memtable-only）必经 `gen_X`（fronts 路径）
- `V_B`（old tree value）必经 `G_K`（tree_guard 路径）

对同一条 flush round：

- `gen_X` 的 pin 集 ⊉ `G_K` 的 pin 集
- `G_K` 的 pin 集 ⊉ `gen_X` 的 pin 集

两个 pin 集互不包含，也就不存在一个"更紧的单一 gate"能同时覆盖两者。合并挂接意味着取两个 gate 的**合取**（最晚那个），会把 tree-side 的 V_B 回收时钟拖到 memtable-side 的 gen 长读节奏上——没有正确性收益，只有延迟回收代价。

### 2.2 Gap 2 — `flushed_max_lsn` 的语义与 Empty Round 计算规则

**背景**：三种 "empty round" 情形在代码里存在：

| Case | sealed_gens | fold workset | tree delta | 含义 |
|---|---|---|---|---|
| 1 | 空 | 空 | 空 | 调用方传入空输入（无 eligible gen） |
| 2 | 非空但 gens 的 table 全空 | 空 | 空 | gen 作为空壳存在（max_lsn 仍为初始值 0） |
| 3 | 非空有条目 | 非空 | 可能空 | 有真实 batch 被覆盖，但 tree 侧可能产零 delta（tombstone compact / 零差异） |

三种情形对 `flushed_max_lsn` 的期望：case 1 / 2 应返 0，case 3 应返 `max(sealed_gens.max_lsn)` 推进 frontier。

**冲突来源**：

- RSM §4.2 的 `tree_flush_result.flushed_max_lsn` 字段只定义了类型（uint64_t），没明确是"本轮贡献"还是"本轮结束后累计"。
- FF §8.1 pipeline 用 `update_flush_max_lsn(tree_flush_result.flushed_max_lsn)` 但没规定 update 是 `max()` 还是 assign。
- 当前 `_flush_merge` 在两个分支都硬写 `flushed_max_lsn = 0`（`tree/owner_scheduler.hh:344, 388`），与 case 3 的期望不一致。

**决议**：按"本轮贡献 + 消费者取 max"定义。

1. **字段语义**：`tree_flush_result.flushed_max_lsn` = "本轮 flush 覆盖的 sealed_gens 中的最大 gen.max_lsn"。
2. **计算规则**：`max(pinned_gens[*].max_lsn)`，空集（case 1）按 max(empty)=0 约定返 0。
3. **消费者合并规则**：coord 的 `update_flush_max_lsn` handle 必须按 `tree_state.flush_max_lsn = max(tree_state.flush_max_lsn, result.flushed_max_lsn)` 合并，**禁止**直接 assign。
4. 三种 empty round 情形在此规则下自动正确：case 1 的输入空集 max=0；case 2 的每个 gen max_lsn=0 → max=0；case 3 按 pinned_gens 的真实 max 推进。

**对代码的影响**：

- `flush_round_state.flushed_max_lsn`（`tree/flush_round_state.hh:162`）：round 开始 pin 住 gens 之后一次算出 `max(pinned_gens[*].max_lsn)`，写入此字段。
- `tree/owner_scheduler.hh:388`（merge 主路径 result）：从 `round.flushed_max_lsn` 取值填入（不再硬写 0）。
- `tree/owner_scheduler.hh:344`（Case 1 merge 返空 result）：保留 `flushed_max_lsn = 0`。

**文档同步**：

- RSM §4.2 的 `tree_flush_result.flushed_max_lsn` 字段注释补充语义。
- FF §8.1 的 `update_flush_max_lsn(v)` 明确规定为 `flush_max_lsn = max(flush_max_lsn, v)`。

### 2.3 Gap 3 — Owner 侧合并算法

Worker 侧的决议见 `027_worker_inmemory_tree_proposal.md`（Phase 7 的详细设计）。本节只记 owner 侧决议。

#### 2.3.1 输入形态（来自 Phase 7 / 027）

Owner 的合并 handle 接收 `std::vector<worker_tree_proposal>`。每个 proposal 是一棵 worker 视角下的局部新树（`mem_tree_node` 混合图）+ `touched_old_pages`（供合并时做 old content 参考）+ `retired_old_values`（供 Gap 1 / §2.1.2 汇总）。

详见 027 §2。

#### 2.3.2 合并算法（三阶段）

**阶段 1：纯内存合并**（CPU only，不触 NVMe / tree_allocator）

自底向上遍历合并树：

```
for each old_paddr appearing in any worker's proposal:
    contribs = 收集所有 worker 对该 old_paddr 的贡献（mem_tree_node）

    if len(contribs) == 1:
        # 只有一个 worker 改这张 page —— 直接接受其 mem_tree_node
        accept contribs[0]

    elif contribs 全是 paddr 引用:
        # 所有 worker 都只是引用未改动 —— 保留 paddr 引用
        保持不变

    else (多 worker 都造了 mem_tree_node):
        # shared ancestor / leaf（仅当 partition 未对齐时才会出现 shared leaf）
        old_content = contribs[0].touched_old_pages[old_paddr]
        merged_content = merge_and_rebuild(old_content, all contribs)
        # 合并可能触发 split → 产多张 mem_tree_node + 新 separator
        # 新 separator bubble up 到下一轮 parent level 处理

全局 root 重判：
    合并完成后检查 root children + separators 是否塞得进一张 page
    不够 → 新增一层（tree_sched 全局视角，非任一 worker 的局部判断）
    够 → 保持当前 root layer
```

**阶段 2：Paddr 分配 + CRC**（单线程，tree_allocator 串行访问）

后序遍历合并后的 mem_tree，给每个 mem_tree_node 分配 paddr：

```
for each mem_tree_node n (post-order):
    if n.replaces_old_paddrs has exactly one paddr:
        old_range_base = n.replaces_old_paddrs[0].range_base
        if manifest.slot_map[old_range_base] + 1 < shadow_slots_per_range:
            # 同 range 下一 slot
            n.paddr = old_range_base + (slot_index + 1) * page_lbas
            old_slot 进 retired.old_slots
        else:
            # shadow slot 满 → consolidation → 分配新 range
            new_range = tree_allocator.allocate()
            n.paddr = new_range.base (slot 0)
            old_range 整体进 retired.old_ranges
    elif n.replaces_old_paddrs.empty():
        # 纯新 page（split sibling / 新层 root）
        new_range = tree_allocator.allocate()
        n.paddr = new_range.base (slot 0)
    else:
        # N-to-1 leaf merge（027 的 pairwise merge 结果）
        # 选其中任一 old_paddr 的 range 走下一 slot（如果够）
        # 其他 old_paddr 的 range 整体进 retired.old_ranges
        ...

    # Parent 的 children 里对此 n 的引用从 "unique_ptr" 解析为 paddr
    if n.type == internal:
        patch children 里所有 unique_ptr child_ref 为 paddr（用这些 child 的 .paddr）
    compute n.content.crc = tree_page_compute_crc(n.content)
```

**阶段 3：写盘 + flush**

- Bounded NVMe writes 发送所有新 page 到各自 paddr（并发度受限，避免单核拥塞）
- 所有 writes 完成后一次 `nvme_flush`
- 构造 new `tree_manifest`：
  - `slot_map` 增量更新（从 old manifest 复制 + apply 本轮变化）
  - `leaf_order` 重建（sorted by fence_lower，含 split 出的新 leaf / 合并消除的 leaf）
  - `reverse_topology` 重建（新 internal 树形）
  - `root_slot` / `root_range_base` 按本轮是 root-stable 还是 root-change 决定
- 如果 root-change：异步投递 `update_superblock` 请求（RSM §4.9）
- 填 `tree_flush_result` 字段

#### 2.3.3 Fallback α：Tree_sched 罕见 NVMe read

**场景**：合并过程中，tree_sched 需要某张 page 的 old content 做 diff 参考，但所有 worker 的 `touched_old_pages` 都没覆盖——即没有 worker 在自己局部树里触达过该 page，但合并后的 split 级联却要求改这张 page。

**触发条件**：partition 不对齐 leaf（当前按 INC-040 将对齐，发生概率趋近 0）+ 合并触发意料之外的级联。

**处理**：tree_sched 直接发 NVMe read 拉 old page。这是**唯一**允许 tree_sched 走 NVMe 的入口，加 metric 计数（正常应常年为 0）。

**理由**：

1. 发生概率极低（INC-040 完成后近乎零）
2. 其他候选方案（跨 read_domain 委托读 / worker 预防性多带 old content）开销更高
3. 热路径仍保持"tree_sched 不 NVMe"语义

#### 2.3.4 不做的结构变化

- **Internal underflow merge**：10 亿 KV 规模下 internal 总空间 ~170MB，不 merge 浪费 < 3%（典型 workload），浪费上限受控，不值当加算法复杂度。
- **Cross-worker leaf merge**（边界稀疏）：概率极低，接受偶发浪费。Worker 内的 pairwise merge（见 027）已覆盖大部分稀疏场景。
- **合并后再跑一遍 shape-change 检查**：合并算法本身自底向上就已做 shape-change 判定，不需要单独后扫。

### 2.4 红线（候选新增进 `cross_doc_contracts.md`）

> **红线（loser 归属分离）**：
>
> - Memtable-only loser 必须挂 `memtable_gen.loser_durable_refs`，gate = gen.ref→0。**禁止**挂任何 `checkpoint_guard` / 全局结构 / flush round pin_bundle。
> - Old tree value 必须最终挂 `checkpoint_guard.retired.old_tree_values`（G_K，旧 guard），gate = guard.ref→0。**禁止**挂任何 `memtable_gen`。
> - 两类 retire list **不得合并**。可观测路径经过不同且互不替代的 shared_ptr 节点；合并会导致 use-after-free 或无收益的回收延迟。

---

## 3. 阶段计划

### Phase 0 — 正式设计文档同步

#### 目标

把 `flush_module_guide.md` 里已经冻结的 flush 模块边界，同步回 `design_doc/`，消掉正式设计和临时指导之间的冲突。

#### 主要产物

1. `flush_and_frontier_switch.md` 更新
2. `runtime_state_machine.md` 更新
3. `code_modules.md` 更新
4. 必要时 `cross_doc_contracts.md` / `design_overview.md` 更新

#### 退出条件

正式设计文档已经能单独作为后续实现依据，不再依赖 `pipeline.codex.md` 或临时指导去解释冲突。

### Phase 1 — Front Input / Memtable Common Carrier

#### 目标

先把 flush 要消费的前端输入对象和最小共用 carrier 立住，消掉"计划默认 front/memtable/gen snapshot 已存在"的隐含前提。

#### 主要产物

1. `memtable_entry`
2. `memtable_gen`
3. `value_handle`
4. sealed/pinned gen 的 readonly view
5. per-front gen grouping / snapshot carrier

#### 退出条件

后续 flush phases 不再假设"memtable / gen snapshot / per-front grouping 会在别处神奇出现"，而是已经有稳定可引用的前端输入对象。

### Phase 2 — Runtime Carrier 与 Topology

#### 目标

建立 flush 运行时最小地基，但不触碰真正的 tree-local merge 算法。

#### 主要产物

1. tree geometry 的单一来源
2. `tree_worker_sched` skeleton
3. registry / builder / read-domain wiring
4. flush shared headers 的最小骨架

#### 退出条件

后续步骤可以在不再改 geometry source、lookup/worker pairing、registry/builder wiring 的前提下继续往 sender seam 填内容。

### Phase 3 — Manifest Carrier 与 Round State

#### 目标

把 flush 必需的 runtime carrier 补齐。

#### 主要产物

1. `tree_manifest.leaf_order`
2. `tree_sched` skeleton（与 RSM §4.1 owner state 对齐）
3. round-owned immutable arrays / views
4. `tree_flush_request` / `tree_flush_result`
5. flush round state carrier

#### 退出条件

后续所有 sender seam 已经有稳定 carrier 可依赖，不需要边做算法边改 carrier。

### Phase 4 — Memtable Fold / Workset

#### 目标

实现 pinned sealed gens → sorted `flush_key_group[]`。

#### 主要产物

1. input pinning
2. sorted `flush_key_group[]`
3. `flush_key_partition[]`
4. winner / loser 分类（losers 直推 owning gen 的 `loser_durable_refs`）

#### 退出条件

`fold_memtables_into_sorted_key_groups()` 已可独立作为 sender seam 向下游提供稳定输入。

### Phase 5 — Worker Fanout / Leaf Mapping（**待在 Phase 7 重构**）

历史 landing：step 025。当前实现引入了 `_leaf_mapping` handle / `flush_mapping_req` / `merge_lookup_leaf_groups` 等独立 fanout 的中间态——Phase 7 把这些全部删除，合进 worker 的统一 handle。

### Phase 6 — Candidate Build（扩展 Worker Handle）

历史 landing：step 026 / 026A。当前实现引入了 `flush_changed_node` manifest overlay + `cascade_climb_one_leaf` worker cascade——Phase 7 把这些整块替换为 `mem_tree_node` 内存混合图模型。

### Phase 7 — Worker 侧 flush 输出模型重构

#### 目标

把 worker 从"按 old_paddr 产 `flush_changed_node`"（step 026A 的 manifest overlay 模型）改为"产一棵内存混合图"——worker 在自己视角下独立完成**所有结构决策**（rewrite / leaf merge / leaf split / internal split / consolidation / root split 增层），**不触及** `tree_allocator`，只产内存 page。

**详细设计见 `027_worker_inmemory_tree_proposal.md`**（完整作为本 Phase 的 design doc 使用）。

#### 主要产物

1. `mem_tree_node` / `child_ref` / `worker_tree_proposal` 类型
2. Worker 统一 handle `submit_flush_work`（取代 `_leaf_mapping` + `_process_candidates`）
3. Worker 内部 pairwise leaf merge pass（阈值 30% page_size）
4. Partition 改按 `base_manifest.leaf_order` leaf-对齐切
5. 删除 Phase 5 / 026A 过渡代码（`flush_changed_node` / `_leaf_mapping::*` / `cascade_climb_one_leaf` 等）

#### 退出条件

Worker 能对 root-stable + leaf split + leaf merge + root split 路径产出正确的 `worker_tree_proposal`。Phase 9 以此为输入。

### Phase 8 — Dead Code / 死注释 / 旧测试 Sweep

#### 目标

Phase 7 完成后，把架构切换期间失效的 production 代码和注释**物理删除**。不碰测试代码（测试编译失败留到 Phase 9 之后的新测试 step 统一清理）。

#### 主要产物

- 所有 §2.Gap 2 / 2.Gap 3 决议 + Phase 7 重构宣告死的 production symbol 从 production 代码里删除干净
- 死注释、过时 phase 展望记号全部清扫
- Production 代码编译通过
- 测试代码**允许编译失败**，且失败点全部落在本 step 删的死 symbol 上

#### Scope：要删的 production 代码

**Type / struct 层面**（如果 Phase 7 没删干净）：

- `flush_changed_node`
- `flush_worker_result`（如果还有残余 `changed_nodes` 字段引用）
- `flush_mapping_req`
- `flush_leaf_group_result`
- `flush_merge_request.mapping_results`
- `tree_flush_result.memtable_losers`（Gap 1 决议删的字段）
- `flush_round_state.memtable_losers`（同上）
- `candidate_build_state`
- `candidate_need_read` / `candidate_done` / `candidate_decision`
- `flush_leaf_candidate` / `flush_candidate_batch`（如果 Phase 7 重构后已不再使用）

**Handle / sender / op**：

- `_leaf_mapping::*`（sender / op / req / schedule / submit）
- `_process_candidates::*`
- `_build_leaf_candidates::*`
- `tree_worker_sched_base` 上的 `schedule_leaf_mapping` / `schedule_process_candidates` / `submit_leaf_mapping` / `submit_process_candidates` / `submit_build` 方法
- `tree_worker_sched_base` 上的 `leaf_mapping_q` / `candidates_q` / `build_q` queue 字段

**Function / helper**：

- `keys_to_leaf_groups()`（`tree/leaf_mapping.hh` 整文件可能全删）
- `merge_lookup_leaf_groups()`
- `cascade_climb_one_leaf()` + `cascade_step_result` / `cascade_step_outcome`
- `process_candidate_groups()`
- `flush_read_budget()`
- `make_candidate_build_state()`

**Sender pipeline 组合**（`tree/sender.hh`）：

- `build_candidates_for_partition()`
- `build_leaf_candidates()`（旧包装）
- `on_candidate_need_read()`
- `check_candidates_not_done()`

以上不是完整清单，实施时以"Phase 7 未引用 + 新架构不需要 = 删"为准则。

#### Scope：要删 / 更新的注释

**整段删除**：

- `tree/candidate_build.hh` 顶部注释（描述旧 cascade 协议）
- `tree/leaf_mapping.hh` 顶部注释（整文件删）
- `tree/owner_scheduler.hh` 的 Phase 5 fanout 描述
- `tree/worker_scheduler.hh` 的 Phase 2/5/6 三个 handle 描述
- 任何 `// Phase 6 will do X` / `// stub for Phase 7` / `// 待 Phase 8 扩展` 之类的 phase 展望注释

**更新**：

- 任何引用 `step 022` / `step 023` / `step 025` / `step 026` / `step 026A` 的实现注释——如果仍适用于新架构，改成"Historical landing: step xxx"；不适用则整段删
- `tree/flush_types.hh` / `tree/flush_round_state.hh` 顶部注释里的 Phase 标号

**保留**：

- 描述"为什么当前形态这样"的设计理由注释（如 `tree_manifest::resolve` 的 panic 理由）
- 红线注释（memory / lifetime / ownership）

#### 测试代码的处理（不主动删）

本 step 不删测试文件。测试的命运分两种：

**A. 因为 sweep 删了死 production 代码而编译失败的测试**：

- **保持失败**，不修、不删。留到 Phase 9 之后的新测试重写 step 统一清理。
- 失败症状：`error: 'flush_changed_node' was not declared in this scope` 等，指向本 step 删的 symbol。

**B. 其他原因失败的测试**：

- 说明 sweep 意外破坏了某个**活代码**的路径，或暴露了**原本就潜伏的问题**。
- 必须正查——如果是"本 step 删了不该删的东西"则修正删除范围；如果是"暴露原本就有的 bug"则作为独立问题记录，留到 Phase 9 或专项 step。

**允许读测试代码的范围**：仅为区分 A / B 两类失败、定位失败来自哪一行。**不允许**基于测试 assertion 反推新架构 spec。实施者在读测试前必须显式声明"我要读测试文件"。

#### 明确不做

- 不主动删测试文件
- 不重写 / 不新增测试
- 不保证 test target 编译通过
- 不删 `plan/` 目录
- 不改 `design_doc/`（Gap 1 / 2 的文档同步等 Phase 9 landing 时一起回写）
- 不改 `known_issues.md`

#### 退出条件

1. Production 代码编译通过（`cmake --build build -j$(nproc)` 对 production target）
2. `grep -rn '<死 symbol>' apps/inconel/` 在 production `.hh` / `.cc` 里零命中
3. 测试编译失败点**都**指向本 step 删的死 symbol（验证 sweep 没误伤活代码）
4. 无"Phase N will do X" / "TODO: Phase Y" 的死注释残留

### Phase 9 — Owner 侧闭环（原 Phase 7 + 8 合并）

#### 目标

在 Phase 7 提供的 worker 输出之上做合并 + 持久化：收 worker 输出、纯内存合并、分配 paddr、写盘、构造 new manifest、返回 `tree_flush_result`。一个 step 内把 root-stable + shape-changing + paddr 分配 + writer + new manifest 构造全部做完。

#### 主要产物

##### 9.1 Tree_sched 合并 handle（`_flush_merge`）实装

- 重构现有 `_flush_merge` 接收 `std::vector<worker_tree_proposal>`（取代当前的 `flush_merge_request.mapping_results`）
- 实装 §2.Gap 3.2 阶段 1 的自底向上合并算法
- 处理 shared old_paddr 冲突（old content 来自 worker 的 `touched_old_pages`）
- Split 级联 + 新 separator bubble up
- 全局 root split 重判（增层）
- 汇总所有 worker 的 `retired_old_values` 进 `round_state.retired.old_tree_values`

##### 9.2 `tree_allocator` 实装

- `head` bump + 碰撞检测（通过 `data_area_heads` 原子 atomics，与 `value_alloc_sched` 共享）
- `free_ranges` 回收队列（invalidate barrier 目前留空——reclaim_q consumer 未实现，但 allocator 结构就位）
- `allocate()` / `push_back_bump()` / `recycle()` 三个入口

##### 9.3 Plan 阶段（paddr 分配 + CRC）

- 合并完的 mem_tree 后序遍历
- 每个 mem_tree_node 按其 `replaces_old_paddrs` 情形分配 paddr（详 §2.Gap 3.2 阶段 2）
- Parent 的 `child_ref` 从 unique_ptr 解析为具体 paddr
- CRC 计算填回 content

##### 9.4 Writer + device flush

- 合并后的新树遍历产出 `vector<write_desc>`
- `submit_tree_page_writes_bounded(write_descs)`：在 `sender.hh` 组 `mock_nvme::write_batch` pipeline（参考现有 `write_batch` 但改成 bounded 并发度）
- Writes all ack → 一次 `nvme_flush()`

##### 9.5 New manifest 构造

- `slot_map` 增量：从 old manifest 复制 + apply 本轮所有 mem_tree_node 的 paddr 变化
- `leaf_order` 重建：sorted 扫描合并后树，收集 leaf-级 mem_tree_node + old_paddr 引用的 leaves，按 fence_lower 排序，重建 `leaf_order_index`
- `reverse_topology` 重建：遍历合并后的 internal 层，建立 `leaf_parent_idx` + `internal_nodes` 表
- `root_slot` / `root_range_base` 更新（root-stable 时 range_base 不变，slot 可能换；root-change 时两者都变）
- **禁止 O(tree) 全扫描**——只遍历本轮 changed 的 subtree 路径 + affected leaves；未改动 subtree 通过 paddr 引用不展开

##### 9.6 Root-change 路径

- `update_superblock` async handle（tree_sched 的请求类型新增）
- Root-change 时：install CAT2 先（外层 coord 职责，不在本 step）；异步发 superblock FUA 写
- `superblock_safe_lsn` 推进规则（RSM §4.9）：root-stable 时完成 `nvme_flush` 即推 `flushed_max_lsn`；root-change 时等 `update_superblock` 完成再推

##### 9.7 `finish_flush_round`

构造 `tree_flush_result`：

- `st = ok`
- `new_manifest`（`shared_ptr<const tree_manifest>`）
- `retired`（从 round_state.retired 转移）
- `flushed_gens_by_front`（用现有 `build_flushed_gens_by_front`，按 `front_owner_index` 分组）
- `flushed_max_lsn`（从 round_state.flushed_max_lsn 取，Gap 2 规则）

清理 `round_state` 从 `tree_state.active_rounds` 移除。

##### 9.8 `checkpoint_guard` 扩字段

- 在 `core/checkpoint_guard.hh` 里加 `retired_objects retired` 字段（仅字段 + 默认空构造）
- **不**实现 destructor 的 reclaim_task 投递（那是 frontier_switch step）

##### 9.9 顶层 pipeline 组合

在 `tree/sender.hh` 里组 `tree_local_flush(...)` pipeline：

```
tree_sched.submit_flush_fold(req)
  → flush_fold_result (partitions leaf-对齐 by Phase 7)
  → loop >> concurrent >> worker.submit_flush_work
  → to_vector<worker_tree_proposal>
  → tree_sched.submit_flush_merge(worker_results)
  → tree_flush_result
```

#### 明确不做（属外部步骤）

- **Frontier_switch 本体**（coord_sched `handle_frontier_switch`，含将 `tree_flush_result.retired` 挂到 G_K 的动作）
- `checkpoint_guard` destructor 的 `reclaim_task` 投递到 `tree_sched.reclaim_q`
- `tree_sched.reclaim_q` 的 consumer（真正执行 TRIM + value recycle）
- `release_gens` 处理 front_sched.imms 移除
- 读路径路由改 key-range（INC-040，本 step 完成后启动）

本 step **只做到 `tree_flush_result` 返回**（flush_module_guide.md 的边界）。retired 字段在 result 里就位，后续消费由 frontier_switch step 接管。

#### 退出条件

1. 多轮 flush 可以产出正确的 `tree_flush_result`，覆盖纯 rewrite / consolidation / leaf merge / leaf split / internal split / root-stable / root-change
2. `new_manifest` / `retired` / `flushed_gens_by_front` / `flushed_max_lsn` 内容正确
3. Mock NVMe 上写出的 tree pages CRC valid、内容正确、slot/range 位置正确
4. 读路径回读已 flush 的 key 能拿到 post-flush 的 winner

#### 完成后的 Follow-Up

本 step 完成后，按 `known_issues.md` 必须启动的收敛项：

- **INC-040**（normal，阻塞解除）：把 `front → tree_lookup` 的路由从 hash-based（`front_owner % count`）换成 key-range based（`route_tree_lookup_for_key(manifest, key)`）。当前 spec（RSM §4.7）和代码 stub 都按 hash 写，与"同一 leaf page 只在一个 read_domain cache"的设计意图冲突。在 Phase 9 把 `tree_local_flush` 闭环、manifest / leaf_order 作为权威 key → leaf 映射源之后，即可把读路径路由切到 `manifest.leaf_order` 查询。配套改 RSM §4.7 / design_overview 读路径伪码 / registry.hh stub / read_api_and_pipeline.md。INC-003 的 sender API shape 问题跟着一起收敛（改成内部按 manifest+key 解析）。

另外，Phase 9 完成后需要独立的"新测试重写 step"——把 Phase 8 遗留的失败测试全量清理并按新架构重写。

---

## 4. Checklist

### A. 开工前 Checklist

- [ ] 已阅读 `flush_module_guide.md`
- [ ] 已确认当前阶段仍然处于 tree-local flush module 范围内
- [ ] 已确认本阶段属于哪个 sender seam
- [ ] 已确认正式设计文档是否已覆盖本阶段边界
- [ ] 如果正式设计与临时指导仍冲突，先写设计同步 step，不进入代码实现

### B. 每阶段计划 Checklist

- [ ] 文档明确本阶段的目标
- [ ] 文档明确本阶段的输入输出 carrier
- [ ] 文档明确涉及的 scheduler owner
- [ ] 文档明确本阶段**不做**什么
- [ ] 文档明确哪些 tree shape 必须 `unsupported_*`（仅限 Phase 7 之前；Phase 7 之后 shape-change 全部必须支持）
- [ ] 文档明确最少验证范围

### C. 实现中 Checklist

- [ ] 没有把模块外流程混进 tree-local flush 阶段
- [ ] 没有把 lookup / worker / owner 的职责打穿
- [ ] 没有让 `tree_sched` 直接跨 core 访问 cache
- [ ] 没有让 `tree_worker_sched` 偷做 slot/range 分配（Phase 7 之后：worker 不碰 paddr 分配）
- [ ] 没有把未实现语义藏成 silent fallback

### D. 阶段完成 Checklist

- [ ] 对应 sender seam 已形成稳定输入输出
- [ ] 本阶段 carrier 不再需要在下一阶段大改
- [ ] unsupported path 明确且 fail-fast（Phase 7 之后：不再有 unsupported shape-change）
- [ ] 至少有一组针对本阶段 seam 的验证
- [ ] 代码与当前正式设计文档一致

### E. 模块完成 Checklist

- [ ] 正式设计文档已完全覆盖 flush 模块的最终边界
- [ ] tree-local flush 闭环完成（Phase 9 完成）
- [ ] root-stable / root-change 都有明确语义
- [ ] 每个 sender seam 都已有明确输入输出与 fail-fast 边界
- [ ] 已完成最终验收（见 F）
- [ ] 临时指导文档和临时开发计划可以删除

### F. 最终验收 Checklist

- [ ] 能从前端侧稳定构造多代 sealed memtable gens 作为 flush 输入
- [ ] 已执行多轮 flush，而不是只跑单轮 happy path
- [ ] 已检查每轮 `tree_flush_result` 的 manifest / retired / flushed_gens / losers 是否符合预期
- [ ] 已直接检查 mock NVMe 中 tree page 的实际写入结果
- [ ] 已通过读路径验证这些 key 仍可正确读出
- [ ] 已覆盖更新、覆盖写、tombstone、跨多轮 flush 累积、root-stable 与 root-change 混合、split 后的读一致性

（验收 harness 是 production 代码形态，不是 test 文件；step 实现期间禁止读测试文件。）

---

## 5. 下一步建议

### 5.1 本计划当前状态

- Phase 0-6：已完成（landed as step 020-026A）
- Phase 7：详细设计已冻结在 `027_worker_inmemory_tree_proposal.md`，可直接开工
- Phase 8：详细设计在本文 §3.Phase 8，可直接开工
- Phase 9：架构决议 + 算法骨架冻结在本文 §2.Gap 3 + §3.Phase 9；进入实现前可以由下一会话基于本文和 `flush_module_guide.md` + `design_doc/` 生成更细的 C++ 级设计（如合并算法的具体 pseudo-code、tree_allocator signature、new manifest rebuild 的增量遍历算法等）

### 5.2 推进顺序

```
Phase 7 (027 设计 → 实装)
  ↓
Phase 8 (按本文 §3.Phase 8 执行 sweep)
  ↓
Phase 9 (基于本文 §2.Gap + §3.Phase 9 进一步生成详细设计 → 实装)
  ↓
新测试重写 step (Phase 9 外)
  ↓
INC-040 read-routing 切 key-range (Phase 9 外)
```

Phase 7 和 Phase 8 可以同一 dev cycle 推进（Phase 7 landing → Phase 8 sweep 是一个顺序性 pair）；Phase 9 是独立的大 step。

### 5.3 文档回写

Phase 9 完成时，以下 `design_doc/` 章节需要同步更新：

- RSM §4.2：`tree_flush_result` 字段声明（去掉 `memtable_losers`，`flushed_max_lsn` 语义注释）
- FF §3.3：losers 挂接路径描述
- FF §4.1 / §5.1 / §5.2：old tree values 挂 G_K.retired 的描述
- FF §8.1：`update_flush_max_lsn(v)` 规定为 `max()` 合并
- FF §3.4B / §3.5：worker 侧算法形态（Phase 7 重构结果）
- `cross_doc_contracts.md` §2：新增 §2.4 的 loser 归属红线
- `core/checkpoint_guard.hh`：加 `retired_objects retired` 字段
