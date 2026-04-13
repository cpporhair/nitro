# Tree-Local Flush 开发计划与 Checklist（临时）

> 本文基于 `flush_module_guide.md`，把 flush 模块开发进一步落成可执行的阶段计划和 checklist。
>
> 分工：
>
> 1. `flush_module_guide.md` 负责说明 flush 模块的目标、边界、sender 组合形状和 planning seam
> 2. **本文**负责说明“先做什么、后做什么、每一阶段做到什么算完成”
>
> 本文是临时开发文档，放在 `ai_context/inconel/plan/`。等 flush 模块开发完成后，本文应删除；版本历史里保留的是正式设计文档的演进和各 step 的详细设计。

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
-> lookup fanout
-> worker fanout
-> tree delta / write plan
-> bounded writes
-> device flush
-> tree_flush_result
```

本计划只覆盖这条 pipeline 本体，不讨论 request 之前的来源，也不讨论 `tree_flush_result` 之后的消费流程。

### 1.3 先做 root-stable/no-split，再做 shape-changing paths

开始阶段只应覆盖：

1. empty-tree bootstrap
2. existing-tree root-stable / no-split

以下内容留给后续单独阶段：

1. leaf split
2. parent rewrite
3. root change
4. consolidation

### 1.4 每个阶段必须保留 sender seam

后续开发和 review 时，默认按这些 sender seam 切：

1. `capture_flush_base_and_pin_input`
2. `fold_memtables_into_sorted_key_groups`
3. `dispatch_key_partitions_to_lookup`
4. `merge_lookup_leaf_groups`
5. `dispatch_leaf_partitions_to_workers`
6. `plan_tree_delta_from_candidates`
7. `submit_tree_page_writes_bounded`
8. `finish_flush_round`

不要在单个阶段里跨多个 seam 做“大一统 flush 实现”。

### 1.5 现有 memtable 与 flush 共用数据结构的边界

这里需要明确区分两类对象：

1. **已有 front-domain 对象**
   - `memtable_gen`
   - `memtable_entry`
   - 以及它们已有的 key/value metadata
   - 这些不是 flush 模块新发明的数据结构，后续实现直接复用，不在 flush phases 里“重做一套 memtable”

2. **flush 模块新增的 shared carrier**
   - `tree_flush_request` / `tree_flush_result`
   - flush round state
   - `flush_lookup_req` / `flush_leaf_group_result`
   - `flush_worker_req` / `flush_candidate_batch`
   - `flush_key_group[]` / `flush_key_partition[]`
   - 这些才是 tree/local lookup/worker 之间共用的数据结构，需要在本计划里显式落地

换句话说：

1. **memtable 本体不是本计划的实现目标**；flush 读取和引用它。
2. **flush 自己的跨-scheduler carrier 是本计划的实现目标**；不能边写算法边临时拼。

## 2. 阶段计划

## Phase 0 — 正式设计文档同步

### 目标

把 `flush_module_guide.md` 里已经冻结的 flush 模块边界，同步回 `design_doc/`，消掉正式设计和临时指导之间的冲突。

### 主要产物

1. `flush_and_frontier_switch.md` 更新
2. `runtime_state_machine.md` 更新
3. `code_modules.md` 更新
4. 必要时 `cross_doc_contracts.md` / `design_overview.md` 更新

### 必须同步的点

1. `tree_sched / tree_lookup_sched / tree_worker_sched` 的职责边界
2. old leaf read / candidate materialization 的归属
3. `tree_manifest.leaf_order` 的存在与语义
4. tree-local flush pipeline 只做到 `tree_flush_result` 的边界
5. PUMP sender 组合视角下的 tree-local flush 本体

### 这一步不做

1. 不写 production flush 代码
2. 不新建 runtime objects / schedulers
3. 不补测试

### 退出条件

正式设计文档已经能单独作为后续实现依据，不再依赖 `pipeline.codex.md` 或临时指导去解释冲突。

## Phase 1 — Front Input / Memtable Common Carrier

### 目标

先把 flush 要消费的前端输入对象和最小共用 carrier 立住，消掉“计划默认 front/memtable/gen snapshot 已存在”的隐含前提。

### 主要产物

1. `memtable_entry`
2. `memtable_gen`
3. `value_handle`
4. sealed/pinned gen 的 readonly view
5. per-front gen grouping / snapshot carrier
6. flush 会直接引用的 key/value metadata 形状

### 必须落地的点

1. flush 明确复用 front-domain 的 `memtable_gen / memtable_entry`，不自造第二套 memtable
2. `memtable_gen` 至少要有：
   - sealed / pinned 生命周期
   - key/value/tombstone entry 访问路径
   - `loser_durable_refs` 挂接点
3. `memtable_entry` 依赖的 `value_handle { durable, hot }` 契约必须一起落地；否则 winner/loser carrier 没有稳定 payload
4. flush 侧读取这些对象时只拿 ref/view，不复制 value body
5. 明确最小 front flush-support seam：
   - `collect_eligible_gens()` 产出的 snapshot shape
   - `release_gens()` 消费的 gen-id / gen-ref shape
   - `flushed_gens_by_front` 的分组规则
6. 这一步只做 flush 所需的最小输入契约，不展开完整 front publish / WAL / seal pipeline

### 这一步不做

1. tree-local flush 算法
2. tree runtime topology
3. lookup/worker fanout
4. tree writes
5. 完整 front publish / WAL pipeline

### 退出条件

后续 flush phases 不再假设“memtable / gen snapshot / per-front grouping 会在别处神奇出现”，而是已经有稳定可引用的前端输入对象。

## Phase 2 — Runtime Carrier 与 Topology

### 目标

建立 flush 运行时最小地基，但不触碰真正的 tree-local merge 算法。

### 主要产物

1. tree geometry 的单一来源
2. `tree_worker_sched` skeleton
3. registry / builder / read-domain wiring
4. flush shared headers 的最小骨架
5. `tree_sched` 的 landing point 冻结为下一阶段，而不是在本阶段自造 owner state

### 必须落地的点

1. `tree_read_domain[i] { lookup, worker }` 的 pairing 拓扑先立住；cache ownership migration 延后到 worker 真正开始读 old leaf 前的专门 step
2. `tree_worker_sched` 的 sender surface / req-res shape
3. runtime registry 可以定位 lookup shards、worker shards
4. 先把跨 scheduler 会共用的类型壳子立住：
   - `flush_round_id`
   - `flush_lookup_req`
   - `flush_worker_req`
   - 以及对应 result shell
5. `tree_sched` 不在本阶段自造 `rounds map` 一类临时 owner state；其正式 skeleton 与 RSM §4.1 对齐，放到下一阶段和 round carrier 一起落

### 这一步不做

1. `leaf_order`
2. workset folding
3. `keys_to_leaf_groups()` sender surface 与 mapping 算法
4. candidate build 算法
5. tree writes
6. `tree_sched` runtime install
7. cache / frame pool / inflight 的 owner 迁移

### 退出条件

后续步骤可以在不再改 geometry source、lookup/worker pairing、registry/builder wiring 的前提下继续往 sender seam 填内容；`tree_sched` 和 final cache ownership 则留给后续专门阶段收口。

## Phase 3 — Manifest Carrier 与 Round State

### 目标

把 flush 必需的 runtime carrier 补齐。

### 主要产物

1. `tree_manifest.leaf_order`
2. `tree_sched` skeleton（与 RSM §4.1 owner state 对齐）
3. round-owned immutable arrays / views
4. `tree_flush_request` / `tree_flush_result`
5. flush round state carrier
6. memtable-facing readonly views

### 必须落地的点

1. `tree_sched` 不再是空 seat；它的 skeleton 与 RSM §4.1 的 owner state 对齐，而不是自造临时字段
2. `leaf_order` 跟 manifest 同生命周期
3. round state 成为所有中间数组的唯一 owning side
4. lookup/worker fanout 输入可以只传 span/view
5. 明确 flush 对现有 `memtable_gen / memtable_entry` 只读依赖的 view/ref 形状
6. 这一阶段结束后，shared carrier 的 owning side 与 borrowed view 边界固定

### 这一步不做

1. 真正的 fold 算法
2. lookup mapping 算法
3. candidate build
4. tree writes
5. final cache ownership migration

### 退出条件

后续所有 sender seam 已经有稳定 carrier 可依赖，不需要边做算法边改 carrier；`tree_sched` 也已经以正式 owner state 形状进入 runtime 设计。

## Phase 4 — Memtable Fold / Workset

### 目标

实现 pinned sealed gens -> sorted `flush_key_group[]`。

### 主要产物

1. input pinning
2. sorted `flush_key_group[]`
3. `flush_key_partition[]`
4. winner / loser 分类
5. `memtable_winner_ref` / `memtable_loser_ref` 一类的 workset carrier

### 必须落地的点

1. 不复制 key bytes
2. 不复制 value body
3. winner 只携带 durable `value_ref`
4. loser 只记录在结果 carrier 中，不做 reclaim
5. empty-round fast path
6. workset carrier 只能引用现有 memtable objects，不能隐式拥有它们

### 这一步不做

1. leaf mapping
2. page materialization
3. tree writes

### 退出条件

`fold_memtables_into_sorted_key_groups()` 已可独立作为 sender seam 向下游提供稳定输入。

## Phase 5 — Worker Fanout / Leaf Mapping

### 架构

Phase 5 建立 flush pipeline 的跨 scheduler 骨架：**一次 fan-out 到 K 个 `tree_worker_sched`，一次 fan-in 回 `tree_sched`**。worker handle 随后续 phase 增长（Phase 6 加 build，Phase 8 加 split），但 pipeline 结构（一次 fanout）不变。

`tree_lookup_sched` 不参与 flush pipeline——它只服务前台读路径。flush page 级工作统一在 worker，原因：代码隔离 + cache 复用（worker/lookup 共享 read_domain cache，正常运行时 lookup 已经把 tree page 读进 cache）+ 一次 fanout 省掉多轮往返。

### 目标

1. `tree_sched` 拆阶段：`submit_flush_fold`（fold + partition）+ `submit_flush_merge`（合并 worker 结果）
2. worker 新增 `_leaf_mapping` handle + `keys_to_leaf_groups()` 映射算法
3. `sender.hh` 用 PUMP sender 组合编排 fanout/fan-in pipeline

### 主要产物

1. PUMP pipeline 编排（`loop >> concurrent >> flat_map(worker->submit_leaf_mapping)`)
2. `keys_to_leaf_groups()` — sorted merge 算法
3. `merge_lookup_leaf_groups()` — fan-in 合并 / dedupe
4. `tree_sched` 的 `_flush_fold` / `_flush_merge` 两个 PUMP sender surface
5. worker 的 `_leaf_mapping` PUMP sender surface

### 必须落地的点

1. 必须依赖 `base_manifest->leaf_order`
2. 不允许退化成逐 key root descend
3. 不允许扫全树 leaf
4. 相邻 partition 命中同一 leaf 时，回到 `tree_sched` 统一 merge
5. `leaf_order` 不变量失败应 fail-fast
6. 跨 scheduler 调用通过 PUMP sender pipeline，不在 advance() 内部手动 enqueue

### 这一步不做

1. old leaf read / candidate build（Phase 6 扩展同一 worker handle）
2. tree writes（Phase 7）
3. split（Phase 8 扩展同一 worker handle）

### 退出条件

worker fanout → fan-in → merge 已形成稳定闭环，产出 `flush_leaf_group_result[]`。Phase 6 可在不改 pipeline 结构的前提下扩展 worker handle。

## Phase 6 — Candidate Build（扩展 Worker Handle）

### 架构

Phase 6 在 Phase 5 建立的 **同一次 fanout** 上扩展 worker handle：worker 从"只做 mapping"升级为"mapping + 读旧页 + merge + 构造 candidate"。pipeline 结构不变，fan-in 回 tree_sched 的 merge handle 也相应扩展以处理 candidate results。

### 目标

扩展 worker handle，实现 `build_leaf_candidates()`。

### 主要产物

1. old leaf read/decode（通过 read_domain cache / NVMe）
2. merge old records + memtable winners
3. page-local compact（tombstone && data_ver <= recovery_safe_lsn）
4. `flush_leaf_candidate` 返回

### 必须落地的点

1. worker 不做 slot/range 分配（tree_allocator 在 tree_sched 上）
2. worker 不改 manifest
3. `tombstone && data_ver <= recovery_safe_lsn` 的 compact 规则
4. split / consolidation / parent rewrite 形态必须显式 `unsupported_*`
5. 同一个 worker handle，同一次 fanout——不另起 fan-out/fan-in

### 这一步不做

1. tree delta planning / NVMe 地址分配（Phase 7 扩展 tree_sched merge handle）
2. bounded writes / device flush（Phase 7）
3. split 处理（Phase 8 扩展同一 worker handle）

### 退出条件

worker fanout 已能稳定产出 candidate proposals。pipeline 结构与 Phase 5 相同。

## Phase 7 — Root-Stable Writer

### 目标

把 tree-local flush 先闭环到：

1. empty-tree bootstrap
2. existing-tree root-stable / no-split

### 主要产物

1. `plan_tree_delta_from_candidates()`
2. `submit_tree_page_writes_bounded(...)`
3. device flush
4. `finish_flush_round()`

### 必须落地的点

1. page writes completion 全收齐后才能 device flush
2. `tree_flush_result` 只表达 tree-local 结果
3. empty-tree bootstrap 正常建树
4. existing-tree root-stable rewrite 正常切到新 slot
5. slot 用尽 / split / root-change 先显式 unsupported

### 这一步不做

1. shape-changing tree writes

### 退出条件

tree-local flush 在有限树形上已经形成可验证闭环。

## Phase 8 — Shape-Changing Paths

### 目标

把 tree-local flush 从 root-stable/no-split 扩到真正的树结构变化路径。

### 主要产物

1. leaf split
2. parent rewrite
3. root change
4. consolidation

### 必须落地的点

1. child -> parent 依赖推进
2. manifest delta 包含新 root / 新 ranges / slot_map 变更
3. root-stable 与 root-change 的后续边界清晰
4. consolidation 和 split 不互相偷语义

### 这一步不做

1. 模块外流程

### 退出条件

tree-local flush 不再只支持受限树形。

## 3. Checklist

## A. 开工前 Checklist

- [ ] 已阅读 `flush_module_guide.md`
- [ ] 已确认当前阶段仍然处于 tree-local flush module 范围内
- [ ] 已确认本阶段属于哪个 sender seam
- [ ] 已确认正式设计文档是否已覆盖本阶段边界
- [ ] 如果正式设计与临时指导仍冲突，先写设计同步 step，不进入代码实现

## B. 每阶段计划 Checklist

- [ ] 文档明确本阶段的目标
- [ ] 文档明确本阶段的输入输出 carrier
- [ ] 文档明确涉及的 scheduler owner
- [ ] 文档明确本阶段**不做**什么
- [ ] 文档明确哪些 tree shape 必须 `unsupported_*`
- [ ] 文档明确最少验证范围

## C. 实现中 Checklist

- [ ] 没有把模块外流程混进 tree-local flush 阶段
- [ ] 没有把 lookup / worker / owner 的职责打穿
- [ ] 没有让 `tree_sched` 直接跨 core 访问 cache
- [ ] 没有让 `tree_lookup_sched` 偷做 page materialization
- [ ] 没有让 `tree_worker_sched` 偷做 slot/range 分配
- [ ] 没有把未实现语义藏成 silent fallback

## D. 阶段完成 Checklist

- [ ] 对应 sender seam 已形成稳定输入输出
- [ ] 本阶段 carrier 不再需要在下一阶段大改
- [ ] unsupported path 明确且 fail-fast
- [ ] 至少有一组针对本阶段 seam 的验证
- [ ] 代码与当前正式设计文档一致

## E. 模块完成 Checklist

- [ ] 正式设计文档已完全覆盖 flush 模块的最终边界
- [ ] tree-local flush 闭环完成
- [ ] root-stable / root-change 都有明确语义
- [ ] 每个 sender seam 都已有明确输入输出与 fail-fast 边界
- [ ] 已完成最终验收：前端侧模拟输入足够量 memtable，多轮 flush 后 mock NVMe 写入结果正确，且这些 key 可读出
- [ ] 临时指导文档和临时开发计划可以删除

## F. 最终验收 Checklist

- [ ] 能从前端侧稳定构造多代 sealed memtable gens 作为 flush 输入
- [ ] 已执行多轮 flush，而不是只跑单轮 happy path
- [ ] 已检查每轮 `tree_flush_result` 的 manifest / retired / flushed_gens / losers 是否符合预期
- [ ] 已直接检查 mock NVMe 中 tree page 的实际写入结果
- [ ] 已通过读路径验证这些 key 仍可正确读出
- [ ] 已覆盖更新、覆盖写、tombstone、跨多轮 flush 累积这几类场景

## 4. 下一步建议

如果按本文直接开工，下一份 step 文档应该是：

1. **flush 设计文档同步**

而不是任何代码实现 step。

这一步完成后，下一份代码相关 step 文档建议是：

1. `front input / memtable common carrier`

原因：

1. flush 的输入对象必须先存在，后面的 workset fold 才不是空谈
2. 不先把 front/memtable 输入契约立住，后面的 `topology / leaf_order / lookup / worker / writer` 都会建立在隐含前提上
