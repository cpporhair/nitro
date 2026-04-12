# 020 — Tree-Local Flush Design Sync

> 实现第二十步。先把 tree-local flush pipeline 需要的正式设计同步完成，让后续代码实现只依赖 `design_doc/` 就能落地 flush 模块，不再回头解释 `pipeline.codex.md` 与正式设计之间的旧冲突。

## 本 step 覆盖的目标

| 目标 | 说明 |
|---|---|
| G1 | 把 flush 模块的正式边界冻结成 `tree_flush_request -> tree_flush_result` |
| G2 | 把 `tree_sched / tree_lookup_sched / tree_worker_sched` 的职责边界写成正式设计 |
| G3 | 把 `tree_manifest.leaf_order`、`tree_read_domain`、tree-local sender 组合写成权威版本 |
| G4 | 消除正式设计里“tree_sched 单体 fold + old leaf read + write tree”的旧表述 |

## 文件结构

```text
design_doc/
├── design_overview.md               — 顶层 scheduler 拓扑、flush 规范步骤、典型 pipeline
├── flush_and_frontier_switch.md     — Phase 2 改写成 tree-local flush pipeline
├── runtime_state_machine.md         — tree domain state / handle / leaf_order / tree_worker_sched
├── runtime_memory_and_cache.md      — tree read-domain cache ownership 与 invalidate barrier
├── code_modules.md                  — 代码归属与 pipeline 入口
├── cross_doc_contracts.md           — handle / owner / pipeline 一致性检查
└── INDEX.md                         — 索引中的 scheduler 数量与章节定位
```

## 设计目标

1. 让未来实现者只看 `design_doc/` 就能知道 flush 模块当前应实现的正式边界。
2. 保留完整 flush 语义文档，但把其中的 tree-local 子流程单独收成清晰模块。
3. 不在这一步偷带 runtime skeleton、测试 harness 或任何 production 代码。

## 设计决策

| # | 决策点 | 结果 | 说明 |
|---|---|---|---|
| `D1` | 本 step scope | **只做文档同步，不写生产代码** | Phase 0 的产物是正式设计，不是 runtime skeleton |
| `D2` | flush 正式边界 | **`tree_flush_request -> tree_flush_result`** | `frontier_switch / release_gens / reclaim` 是模块外消费者 |
| `D3` | tree 域拓扑 | **`tree_sched + tree_lookup_sched + tree_worker_sched + tree_read_domain[]`** | lookup/worker 分工明确，避免 monolithic tree owner |
| `D4` | old leaf read 归属 | **放到 `tree_worker_sched`** | `tree_sched` 只保留 round owner / delta planning / write coordination |
| `D5` | batch leaf mapping | **依赖 `tree_manifest.leaf_order`** | 不允许逐 key root descend，也不允许扫全树 leaf |
| `D6` | cache 归属 | **tree node cache 归 `tree_read_domain` shard** | lookup 与 worker 共享同一 read-domain shard，tree_sched 不再持有旧 leaf 读 cache |
| `D7` | memtable 数据结构边界 | **复用现有 `memtable_gen / memtable_entry`** | flush 只新增自己的 request/result/workset carrier，不重做 memtable 本体 |

## 详细设计

### 1. `design_overview.md`

同步顶层规范，只做三件事：

1. scheduler 拓扑从“7 个 scheduler”更新为包含 `tree_worker_sched` 的 tree read-domain 结构。
2. `§9.4 Flush 的规范步骤` 明确 Phase 2 是 tree-local flush pipeline，并显式停在 `tree_flush_result`。
3. `§14.6 Flush / Frontier Switch` 的 sender 组合从单体 `tree_sched(fold/write/build_manifest)` 改成“外层 flush round + 内层 tree-local flush sub-pipeline”。

### 2. `runtime_state_machine.md`

这里是本 step 的核心落点，需要补齐四类正式对象：

1. `tree_flush_request` / `tree_flush_result`
2. `tree_read_domain`
3. `tree_manifest.leaf_order`
4. `tree_lookup_sched` / `tree_worker_sched` 的 handle surface

同时要在正式设计里写清楚：

1. flush 对现有 `memtable_gen / memtable_entry` 的 `table` / `kv_arena` 是**只读复用**；允许在 fold 期间写 `memtable_gen::loser_durable_refs`
2. flush 新增的只是跨 scheduler 传递的 shared carrier
3. 真正的 workset carriers（如 `flush_key_group[]` 一类）留到后续 phase 落代码时实现

冻结后的 tree domain 分工：

```text
tree_sched
  = round owner
  + tree allocator
  + tree delta / manifest delta
  + bounded writes / device flush coordination

tree_lookup_sched
  = ordinary tree lookup
  + batch key -> affected leaf mapping

tree_worker_sched
  = old leaf read / decode
  + merge old leaf + memtable winners
  + compact tombstones
  + candidate page materialization
```

`tree_manifest` 必须新增 runtime-only immutable `leaf_order`，并冻结以下不变量：

1. 按 key range 有序。
2. 覆盖 manifest 当前可达的全部 leaf。
3. 区间不重叠。
4. 不保存 frame 指针。
5. 跟 manifest / guard 同生命周期。

### 3. `flush_and_frontier_switch.md`

本文仍然保留“完整 flush + frontier switch”语义，但需要把 Phase 2 写成 tree-local 子模块：

```text
capture frontier
-> collect eligible gens
-> tree_local_flush(tree_flush_request)
-> tree_flush_result
-> frontier_switch(tree_flush_result.success)
```

其中 tree-local flush 本体要用 sender 组合写清楚：

```text
capture base / pin input
-> fold memtables into sorted key groups
-> dispatch_key_partitions_to_lookup
-> merge_lookup_leaf_groups
-> dispatch_leaf_partitions_to_workers
-> plan_tree_delta_from_candidates
-> submit_tree_page_writes_bounded
-> nvme_flush
-> finish_flush_round
```

并明确：

1. old leaf read 与 candidate materialization 不再属于 `tree_sched`。
2. `new_manifest` 构造必须同步更新 `slot_map` 与 `leaf_order`。
3. Phase 3 frontier switch 的输入来自 `tree_flush_result.success`，而不是直接消费 Phase 2 的中间数组。

### 4. `runtime_memory_and_cache.md`

这里要把 tree node cache 的 owner 写清楚：

1. 普通读与 flush old-leaf read 都使用 `tree_read_domain` shard 的 `readonly_frame_cache`。
2. `tree_sched` 只持有 flush write buffers，不再持有 tree-node read cache。
3. old range 进入 `free_ranges` 前，需要对所有 `tree_read_domain` shards 做 tree-node invalidate barrier。

### 5. `code_modules.md` 与 `cross_doc_contracts.md`

这两份文档只做“把正式边界写得可检查”：

1. `code_modules.md` 补 tree worker 模块归属与 `tree_flush` 的高层 pipeline 入口。
2. `cross_doc_contracts.md` 同步 handle、owner、pipeline 和章节引用，作为未来 review 的 checklist。

## 明确不做的内容

本 step 不做：

- 重新实现 memtable 本体或复制一套 front-domain 数据结构
- runtime skeleton
- registry / builder wiring
- `leaf_order` 的具体构造代码
- `keys_to_leaf_groups()` / `build_leaf_candidates()` 的 production 实现
- flush 测试 harness

## 实施顺序

1. 先更新 `runtime_state_machine.md`，冻结 tree domain 的对象和职责。
2. 再更新 `flush_and_frontier_switch.md`，把完整 flush 中的 Phase 2 改写成 tree-local flush 子流程。
3. 再同步 `design_overview.md`、`code_modules.md`、`runtime_memory_and_cache.md`。
4. 最后更新 `cross_doc_contracts.md` 与 `INDEX.md`，清理章节引用和检查项。

## 验证

本 step 的验收不是跑代码，而是做文档一致性检查：

1. 正式设计里不再存在“flush 期间 old leaf read 在 tree_sched 上执行”的旧结论。
2. 正式设计里已经出现 `tree_worker_sched`、`tree_read_domain`、`tree_manifest.leaf_order`。
3. tree-local flush 的正式边界已经停在 `tree_flush_result`。
4. 顶层概览、详细设计、缓存模型、跨文档契约四处表述一致。
