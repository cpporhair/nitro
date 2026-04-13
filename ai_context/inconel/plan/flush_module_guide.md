# Tree-Local Flush Pipeline 开发指导（临时）

> 本文是 tree-local flush pipeline 开发的**指导文档**。它的职责不是直接充当某一个 implementation step；它只回答三类问题：
>
> 1. flush 模块到底要产出什么、边界停在哪里
> 2. flush 模块内部应该如何分块、哪些算法/owner 边界必须先冻结
> 3. 后续如何基于本文继续拆小的开发计划，而**不再依赖** `pipeline.codex.md`
>
> 正式系统语义仍以 `design_overview.md`、`flush_and_frontier_switch.md`、`runtime_state_machine.md` 为准；本文只补“如何开始把 tree-local flush pipeline 做出来”的开发指导。
>
> 本文放在 `ai_context/inconel/plan/`，是**阶段性的临时文档**。等 flush 模块开发完成、相关正式设计已经回写到 `design_doc/` 且各 step 详细设计齐备后，本文应删除。

## 1. 文档定位

### 1.1 本文解决的问题

现有正式文档已经回答了：

1. flush 在系统中的高层语义是什么
2. tree / coord / value / nvme 等模块在系统里的关系是什么

但要真正开始 flush 模块开发，还缺一层中间文档：

1. tree-local flush 先收成什么形态
2. 哪些结构和算法必须在第一批开发里立住
3. 哪些内容不属于这个 pipeline，本阶段不要混进来
4. 将来怎么按这份文档继续拆 implementation plans

本文就是这层中间文档。

### 1.2 本文与其他文档的关系

| 文档 | 角色 |
|---|---|
| `design_overview.md` | 唯一顶层规范，决定 flush 的系统级正确性边界 |
| `flush_and_frontier_switch.md` | flush 的正式语义背景；本文不展开模块外流程 |
| `runtime_state_machine.md` | tree/coord/front/value/nvme 的 owner 状态与 handle 契约 |
| **本文** | flush 模块开发入口：模块边界、内部拆分、planning seam、分阶段实现约束 |

结论：

1. 实现 tree-local flush pipeline 前，先看 `flush_and_frontier_switch.md` 理解正式语义背景。
2. 拆开发计划时，以**本文**为直接输入。
3. 以后如果 `pipeline.codex.md` 和本文冲突，以本文和正式 design doc 为准。

### 1.3 开发入口约束

**flush 模块开发的第一步必须是：更新正式设计文档，消掉现有冲突。**

原因很直接：

1. 这份文档是阶段性的开发指导，不是正式设计来源。
2. 如果 `design_doc/` 里的正式文档和本文仍然冲突，后续实现者很容易直接按正式设计里的旧边界切代码。
3. 因此在任何 production 代码开始之前，必须先把本文里已经确定的 flush 模块边界，同步回正式设计文档。

这条约束意味着：

1. **第一份 flush step 计划不是代码实现计划，而是设计文档变更计划。**
2. 在这一步完成前，不应开始 `tree_sched / tree_lookup_sched / tree_worker_sched / builder / registry` 的 production 实现。
3. 设计同步完成后，后续各步再按本文的 planning seam 继续拆。

第一步设计同步至少应覆盖：

1. `flush_and_frontier_switch.md`
2. `runtime_state_machine.md`
3. `code_modules.md`
4. 必要时 `cross_doc_contracts.md` / `design_overview.md`

同步目标不是把所有实现细节一次写完，而是先让正式设计在以下关键点上不再冲突：

1. `tree_sched / tree_lookup_sched / tree_worker_sched` 的职责边界
2. flush 期间 old leaf read / candidate materialization 的归属
3. `tree_manifest.leaf_order` 的存在与语义
4. tree-local flush pipeline 只做到 `tree_flush_result` 的边界

## 2. Flush 模块的目标与边界

### 2.1 flush 模块的直接产物

flush 模块先做的是 **tree-local flush pipeline**。它接收一组已经 sealed、并且在本轮 flush 期间 pinned 的 memtable gens，把它们物化到当前 tree snapshot 上，产出 tree-local 结果：

```text
tree_flush_request {
  sealed memtable gens[]
  recovery_safe_lsn
}

tree_flush_result =
  success { base_guard, new_manifest, retired, flushed_gens, memtable_losers, flush_max_lsn }
  failure { error }
```

本文只关心从 `tree_flush_request` 到 `tree_flush_result` 的 contract，不展开 result 之后的消费流程。

### 2.2 flush 模块负责什么

tree-local flush pipeline 负责：

1. 捕获 `base_guard/base_manifest`
2. 从 pinned memtables 构造 sorted flush workset
3. 根据 `base_manifest` 把 sorted keys 映射成 affected leaf groups
4. 并行构造 leaf candidate pages
5. 在 tree owner 上规划 tree delta / manifest delta / write plan
6. 写 tree pages，并执行一次 device flush
7. 返回 success/failure

### 2.3 flush 模块暂时不负责什么

这些内容不属于本文要实现的 pipeline 本体。本文不讨论 request 之前的来源，也不讨论 result 之后的消费流程；实现时必须显式保留 seam，不能为了“先跑通”把边界揉在一起。

### 2.4 tree-local flush 的 PUMP sender 视图

tree-local flush 的 pipeline 只有**一次 fan-out / fan-in**。worker 做全部 page 级重活（mapping + 读旧页 + merge + 构造新页 + 处理 split），tree_sched 只做轻量协调（fold / 合并结果 / 分配 NVMe 地址 / 写盘）：

```text
tree_local_flush(base_guard, gens)
  = tree_sched->submit_flush_fold(req)                    // fold + partition
  >> flat_map([owner](flush_fold_result&& fr) {
       return dispatch_partitions_to_workers(fr)           // 一次 fanout 到 K 个 worker
         >> flat_map([owner](auto&& worker_results) {
              return owner->submit_flush_merge(worker_results);  // 合并 + 分配 NVMe 地址
            })
         >> flat_map([owner](auto&& write_plan) {
              return submit_tree_page_writes_bounded(write_plan)
                >> flat_map([owner](bool) {
                     return core::registry::local_nvme()->flush();
                   })
                >> flat_map([owner](bool) {
                     return owner->finish_flush_round();
                   });
            });
     }) >> flat()
```

如果按 owner seam 读这段 sender：

1. `tree_sched` 负责 round owner 状态（fold / merge / allocate / write coordination）
2. `dispatch_partitions_to_workers(...)` 是**唯一一次** `tree_sched -> tree_worker_sched` 的 sender fanout
3. `tree_lookup_sched` 不参与 flush pipeline——它只服务前台读路径
4. bounded writes completion 全收齐之后，才允许 `nvme()->flush()`

### 2.5 fanout 子流程的 sender 视图

只有一次 fanout。每个 worker 独立完成自己 partition 的全部 page 工作：

```text
dispatch_partitions_to_workers(fold_result)
  = loop(fold_result.partitions.size())
  >> concurrent()
  >> flat_map([](flush_fold_result& ctx, size_t i) {
       auto& part = ctx.partitions[i];
       auto* worker = registry::tree_worker_at(part.read_domain_index);
       return worker->submit_flush_work({
           .round_id = ctx.round_id,
           .read_domain_index = part.read_domain_index,
           .base_manifest = ctx.base_manifest,
           .groups = part.groups,
           .recovery_safe_lsn = ctx.recovery_safe_lsn,
       });
     })
  >> to_vector()
```

worker handle 随 phase 增长：

| Phase | worker handle 做什么 | 返回什么 |
|---|---|---|
| Phase 5 | keys_to_leaf_groups mapping | leaf groups |
| Phase 6 | mapping + 读旧页 + merge + 构造 candidate | candidate pages |
| Phase 8 | mapping + build + split 处理 | candidates with split info |

始终同一 handle、同一 fanout、同一 fan-in。后续 phase 扩展 handle 逻辑，不另起 fanout。

这段冻结的是：

1. worker fanout 的输入输出边界
2. collect/join 发生的位置必须回到 `tree_sched`
3. `tree_lookup_sched` 不参与 flush fanout

后续所有 step 文档，都应该先回答自己切的是 tree-local flush sender 的哪一段。如果一个 step 文档连自己切的是哪一段都没说清楚，后面大概率会把设计边界和实现边界混在一起。

## 3. 模块拆分与 owner 边界

### 3.1 两类 scheduler 的职责

flush 模块按两类职责拆：

```text
tree_sched        = round owner / 轻量协调（fold / merge / allocate / write）
tree_worker_sched = 全部 page 级重活（mapping / read / merge / build / split）
```

`tree_lookup_sched` **不参与 flush pipeline**——它只服务前台读路径（point lookup / range scan）。flush 的 page 工作统一放 `tree_worker_sched`，原因：

1. **代码隔离**。lookup 和 worker 分开是为了维护性，flush 逻辑统一在 worker，避免散布两个 scheduler。
2. **cache 复用**。worker 和 lookup 在同一个 `tree_read_domain` 共享 cache shard。正常运行时 lookup 处理前台读把 tree page 读进 cache，worker 做 flush 时直接 cache hit。这个收益来自 read_domain 架构，和 flush 在哪个 scheduler 无关。
3. **一次 fanout**。mapping 和 build 在同一个 worker handle 里，不需要两轮 fan-out/fan-in。

#### `tree_sched`

flush round owner，只负责 owner-only 状态与轻量协调：

1. 接收请求
2. 捕获 base guard / base manifest
3. pin memtable gens
4. 构造 sorted workset + partition
5. 分发 worker task（一次 fanout）
6. 合并 worker 结果
7. 分配 NVMe 地址（shadow slot / new range）
8. 提交 tree writes + device flush
9. 返回 `tree_flush_result`

所有会修改 tree manifest / tree allocator / retired 集合 / flush round 状态的逻辑只在 `tree_sched` 上执行。

#### `tree_worker_sched`

它服务 flush 的全部 page 级工作。一个 worker handle 随 phase 增长：

```text
Phase 5: flush_key_partition → keys_to_leaf_groups() → flush_leaf_group[]
Phase 6: + read/decode old leaf → merge → build candidate pages
Phase 8: + handle leaf split → build split page images
```

worker 的约束（所有 phase 共有）：

1. 不分配 slot / range（tree_allocator 在 tree_sched 上）
2. 不修改 manifest
3. 不执行 root change 决策（返回 split info 让 tree_sched 处理）

### 3.2 read domain 作为固定拓扑

`tree_lookup_sched` 和 `tree_worker_sched` 按 core 成对配置，并共享同一个 tree node cache shard：

```text
tree_read_domain[i] {
  lookup = tree_lookup_sched on core i
  worker = tree_worker_sched on core i
  cache  = tree node cache shard on core i
}
```

约束：

1. `read_domain_index` 是 flush task 的调度目标
2. `tree_sched` 不跨 core 直接访问 cache
3. `tree_sched` 只按 `read_domain_index` 投递 task 到对应 scheduler

这条拓扑本身就应该成为未来拆 step 的稳定前提，不要把 lookup/worker/cache 的 owner 关系留到实现中临时决定。

## 4. 开发前必须先冻结的 carrier

### 4.1 现有 memtable 与 flush shared carrier 的边界

这里必须先拍板一件事：flush 模块并不会重新定义一套 memtable。

要区分两类对象：

1. **front-domain 既有对象**
   - `memtable_entry`
   - `memtable_gen`
   - `value_handle`
   - sealed / pinned gen 生命周期
   - loser_durable_refs 挂接点
   - per-front gen grouping / release shape
2. **flush 模块新增的 shared carrier**
   - `tree_flush_request` / `tree_flush_result`
   - round state
   - `flush_key_group[]`
   - `flush_lookup_req` / `flush_leaf_group_result`
   - `flush_worker_req` / `flush_candidate_batch`

冻结约束：

1. flush 只复用前者，不重做 memtable 本体
2. flush 自己新增的是跨 scheduler 传递的 shared carrier
3. 如果前端路径尚未落代码，flush 开发计划里必须单独先做：
   - 最小 `memtable_entry / memtable_gen / value_handle`
   - sealed/pinned gen readonly view
   - `collect_eligible_gens()` / `release_gens()` 相关的最小输入输出 shape
4. 在这些前置对象存在前，tree-local flush 的 fold/workset step 不能开工

### 4.2 `tree_manifest` 的 per-manifest `leaf_order`

因为 tree 是 CoW，`tree_manifest` 是 immutable snapshot，所以 leaf 顺序索引也必须是 per-manifest immutable 数据，而不是全局 mutable leaf 链表。

建议 shape：

```text
tree_manifest {
  root_slot
  slot_map
  leaf_order[]   // sorted by key range
}

leaf_order entry {
  lower_bound
  upper_bound
  leaf_range_base
  leaf_slot_paddr
  path_hint
}
```

冻结约束：

1. `leaf_order` 跟 manifest/guard 生命周期走
2. 新 flush 构造新的 manifest + 新 `leaf_order`，不能原地修改旧 `leaf_order`
3. `leaf_order` 不保存 cache frame 指针
4. `leaf_order` 不单独持久化；recovery 重新构造 runtime manifest 时重建
5. `keys_to_leaf_groups()` 必须依赖 `base_manifest->leaf_order`，不能 fallback 到“每个 key 独立从 root descend”

### 4.3 tree geometry 需要单一来源

flush 写侧一定会消费：

1. `tree_page_size`
2. `shadow_slots_per_range`

这两个参数不能继续散落在 ad hoc 常量里。开始 flush 模块开发前，必须先给 tree 写侧一个 runtime-visible 的 single source。

这里不冻结最终 carrier 名字，但冻结两条规则：

1. builder / tree owner / manifest / writer 只能从这一处读 tree geometry
2. 以后切到正式 runtime source 时，不能再推翻一次 flush API

### 4.4 round-owned immutable arrays

flush 中间接口应该优先按“round-owned immutable arrays + span”组织，而不是每个阶段各自产生 owner-local mutable container。

原因：

1. flush task 会跨 scheduler fanout
2. 这些 task 的输入本质上是本轮固定快照
3. 让 `tree_sched` round state 成为唯一 owning side，生命周期最容易证明

因此未来 step 设计时，默认选型应是：

1. `tree_sched` round state 持有 owning arrays
2. lookup/worker task 只拿 span/view
3. task completion 后结果再汇总回 `tree_sched`

## 5. 必须按模块 seam 落地的核心算法

### 5.1 memtable fold -> sorted workset

flush 输入是一组 pinned sealed memtable gens。flush 期间这些 gens 不释放，因此 workset 可以只引用 key/value metadata。

```text
sealed memtable gens
  -> sorted flush_key_group[]
```

`flush_key_group` 的语义：

```text
key
winner
memtable_losers
```

冻结约束：

1. 不复制 key bytes
2. 不复制 value body
3. value winner 只保存 durable `value_ref`
4. tombstone 作为普通 winner 进入 tree merge
5. `memtable_losers` 只作为结果返回；本模块不做物理 reclaim

### 5.2 `keys_to_leaf_groups()` 是第一批必须做对的算法

输入：

```text
sorted flush_key_group[]
base_manifest->leaf_order
```

输出：

```text
flush_leaf_group[]  // 每个 affected leaf 一组 updates
```

算法本质：

1. 从首 key 出发定位初始 leaf
2. 同 leaf 范围内连续吸收 updates
3. key 跨出当前 range 时，用 `find_leaf_index_from()` 向前跳
4. 稀疏 key 允许二分/分层跳跃
5. 不允许退化成逐 key lookup
6. 不允许扫描全树 leaf

相邻 partition 可能命中同一 leaf，所以 fan-in 后必须由 `tree_sched` 统一 dedupe/merge。

manifest 不变量失败，例如：

1. `leaf_order` 缺失
2. fence range 不完整
3. entry 区间重叠

属于 consistency bug，应 fail-fast，而不是普通 flush failure。

### 5.3 `build_leaf_candidates()` 只做 page materialization

worker 读取 old leaf page 后，在本地做：

1. old records + memtable winners merge
2. page-local compact
3. candidate image 构造
4. candidate proposal 返回

在这一步里：

1. touched key 的 upsert/delete 要落实
2. untouched key 原样带入
3. `tombstone && data_ver <= recovery_safe_lsn` 可 opportunistic drop

但这里不要偷做：

1. slot 选择
2. parent rewrite
3. root 变化决策
4. manifest 更新

这四项必须继续留在 `tree_sched`。

### 5.4 tree delta / manifest delta / bounded writes

`tree_sched` 在收齐 candidates 后，再做：

1. tree delta planning
2. manifest delta planning
3. page write planning
4. bounded inflight writes
5. 单次 device flush

等待点冻结：

1. key partitions fanout 后，要 collect 全部 `flush_leaf_group_result`
2. leaf partitions fanout 后，要 collect 全部 `flush_build_result`
3. 所有 tree page writes completion 返回后，才能执行 device flush

## 6. 用本文拆 implementation plans 的原则

本文不是 step 文档，但它要求后续的 step 文档按这些 seam 来拆。

### 6.1 推荐的 planning 单元

后续开发计划应该围绕这些单元拆，而不是把整个 flush 当成一个 step。

但在这些单元之前，还有一个强制的 **step 0 / step 1**：

0. **设计文档同步**
   - 先把本文已经冻结的 flush 模块边界同步回 `design_doc/`
   - 在正式设计不再冲突之前，不进入 production 代码实现

设计同步完成后，再进入下面这些单元：

1. **front input / memtable common carrier**
   - `memtable_entry`
   - `memtable_gen`
   - `value_handle`
   - sealed / pinned gen 的只读输入形状
   - per-front grouping / release shape
   - flush 可直接引用的 key/value metadata
2. **carrier / topology**
   - tree geometry 单一来源
   - registry / builder / read domains
   - `tree_sched` / `tree_worker_sched` skeleton
3. **manifest / runtime carrier**
   - `tree_manifest.leaf_order`
   - flush round state
   - 请求/结果 carrier
4. **memtable fold**
   - pinned gens
   - sorted `flush_key_group[]`
   - partition policy
5. **worker fanout / leaf mapping**
   - `tree_sched` 拆阶段（fold + merge）
   - PUMP sender pipeline 编排（一次 fanout）
   - worker `_leaf_mapping` handle + `keys_to_leaf_groups()` 算法
   - fan-in merge / dedupe
6. **candidate build**（扩展同一 worker handle，同一 fanout）
   - old leaf read/decode
   - merge/compact
   - candidate proposal
7. **root-stable writer**
   - tree_sched merge handle 扩展：NVMe 地址分配 / tree delta planning
   - bounded writes
   - device flush
   - tree-local result
8. **tree shape growth**（扩展同一 worker handle）
   - leaf split（worker 构造 split page images，tree_sched 分配地址）
   - parent rewrite
   - root change

这些是“推荐 planning 单元”，不是固定 step 数。后面可以继续细分，但不要跨这些 seam 乱切。

### 6.2 每个 step 必须显式写清楚三件事

未来每一个 flush 相关 step，都必须在文档里明确：

1. **本 step 新增了哪个 owner / carrier / algorithm seam**
2. **本 step 明确不做什么**
3. **遇到哪些 tree 形态时必须 `unsupported_*` fail-fast**

否则很容易把“未实现的完整语义”伪装成“通用 flush”。

### 6.3 不应该混在同一个 step 的内容

下列内容不适合混做：

1. `leaf_order` carrier 与 split/consolidation 全实现
2. candidate build 与 root-stable writer
3. root-stable writer 与 shape-changing paths

原因不是这些内容毫无关系，而是它们属于不同 owner seam，混做会把验证面和回滚面同时放大。

## 7. 开发顺序上的约束

### 7.1 先做正式设计同步，再做 tree-local flush

因此：

1. 先做正式设计同步
2. 再把 tree-local flush 做成稳定模块

不要边写 production 代码边决定模块边界。

### 7.2 先有 root-stable/no-split，再做 shape-changing paths

`split / parent rewrite / root change / consolidation` 是第二批问题。开始开发时应先锁定：

1. 空树 bootstrap
2. existing-tree root-stable/no-split

其他 tree 形态必须显式 unsupported。这样既能让 flush 模块尽快形成闭环，又不会把 shape-changing 逻辑和基础 owner 拓扑绑死在一起。

## 8. 开发中的禁止项

开始 flush 模块开发后，以下做法明确禁止：

1. 把 `keys_to_leaf_groups()` 退化成逐 key root descend
2. 让 `tree_sched` 直接跨 core 访问 tree cache
3. 让 `tree_worker_sched` 偷做 slot/range 分配
4. 让 `tree_lookup_sched` 偷做 page materialization
5. 用 silent skip 掩盖 split/consolidation/root-change 未实现
6. 让模块外流程反向渗进 tree-local flush 模块，导致 owner 边界消失

## 9. 验证指导

未来每个 step 的验证应围绕 seam 做，而不是只看“最后能不能过一个大集成用例”。

至少要有这些验证层次：

1. carrier invariant
   - `leaf_order` 覆盖全空间、无重叠
   - round-owned arrays 生命周期正确
2. 算法正确性
   - memtable fold 的 winner/loser 分类
   - `keys_to_leaf_groups()` 在稀疏 key / 跨 partition 场景下的映射
   - candidate build 的 merge / compact 结果
3. owner 边界
   - lookup 只做映射
   - worker 只做 materialization
   - tree owner 才能改 manifest / allocator / retired
4. 时序正确性
   - page writes completion 未齐之前，不能 device flush
   - device flush 未完成之前，不能 `finish_flush_round`

### 9.1 最终验收验证

最终验收不应只停留在 seam 级单测，还必须有一组 end-to-end 验证，形状固定为：

1. 在前端侧模拟构造足够量的 sealed memtable gens
2. 对同一棵树执行多轮 flush
3. 校验 `tree_flush_result` 与预期 winner 一致
4. 直接检查 mock NVMe 上写出的 tree pages 是否正确
5. 再通过读路径把这些 key 读出来，确认 flush 后可见结果正确

也就是说，最终验收至少要同时回答三件事：

1. flush result 对不对
2. mock NVMe 上实际写进去的东西对不对
3. 写进去之后，这些 key 是不是真的能读出来

这里的“前端侧模拟输入”只是最终验收 harness 的输入方式，不改变本文的模块边界：实现范围仍然只做到 `tree_flush_request -> tree_flush_result`。

## 10. 这份文档之后怎么用

从现在开始，flush 模块开发按下面顺序推进：

1. 先基于本文写“设计文档同步”这一步
2. 设计同步完成后，再用本文选一个 planning 单元
3. 再为那个 planning 单元单独写 step 计划
4. 实现前，只回查正式语义文档，不再依赖 `pipeline.codex.md`

也就是说：

1. `pipeline.codex.md` 完成了“草案讨论”的角色
2. 本文接手“开发入口”的角色
3. 后续所有 flush step 都应该是对本文某个 planning 单元的局部落地
