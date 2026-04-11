# 020 — Tree-Local Flush Design Sync Review

> reviewer 视角结论。基于 7 份设计文档逐份通读、cross_doc_contracts 交叉核对、对照 `020_tree_local_flush_design_sync.md` 的验收 checklist；本 step 不涉及生产代码和测试，本文只记录文档一致性问题。

## 结论

当前 **可以收**。

首轮 review 列出 3 条跨文档矛盾 + 4 份文档内部过时描述，共 7 条阻塞项。二轮 reviewer 独立读取每个修改点 + 独立执行 grep 自检后确认全部修复到位；Phase 0 退出条件（"正式设计文档已经能单独作为后续实现依据，不再依赖 `pipeline.codex.md` 或临时指导去解释冲突"）已达成，可以进入 `flush_development_plan.md` Phase 1 "front input / memtable common carrier"。

已经落地的部分：

1. flush 模块的正式边界冻结在 `tree_flush_request -> tree_flush_result`，`flush_and_frontier_switch.md §3.1/§8.2` 和 `runtime_state_machine.md §4.2` 一致。
2. `tree_sched / tree_lookup_sched / tree_worker_sched` 的三域拆分和 `tree_read_domain[]` 拓扑在 `runtime_state_machine.md §4`、`flush_and_frontier_switch.md §3.4`、`runtime_memory_and_cache.md §7.1/§10.2` 三处完整对齐。
3. `tree_manifest.leaf_order` 的语义冻结在 `runtime_state_machine.md §4.5`；二轮修复后 `design_overview.md §4.4/§5.2` 和 `code_modules.md` core/ 域对象表也已同步到 `leaf_order`，`keys_to_leaf_groups()` 的约束已明确写成 "不允许退化成逐 key root descend、不允许扫全树 leaf"。
4. `cross_doc_contracts.md` 的 handle 签名表、struct 字段表、owner 归属表都同步到了新拓扑。
5. flush Phase 2 已经从 `tree_sched` 单体 fold+write 改写成 "外层 flush round + 内层 tree-local flush sub-pipeline" 的 sender 组合；二轮修复后 `design_overview.md §14.6` 已删除显式 `enqueue_reclaim()`，与 `flush_and_frontier_switch.md §8.1` "由旧 G0 析构触发" 一致。
6. scheduler 数量从 "7 类" 统一更新为 "8 类"；`runtime_state_machine.md §1` 表格、§9.3 flush 时序图、§10.3 并发安全论证、`design_overview.md §1.6` 架构图、§1.7 系统组件总览、§14 执行域约定、`code_modules.md` memory/ Frame Cache 归属、`INDEX.md` flush §3 导航 8 处复述点已全部同步。

剩余非阻塞 cosmetic 观察（NB-1 / NB-2）见 "二轮验证" 节，不作为收口阻塞项。

Finding 第 8 条（`recovery_and_wal_reclaim.md` 未同步 tree domain 拆分）是 scope 外的遗留风险，不在本 step 修复范围内；建议 Phase 1 代码开工前先开 `021_recovery_tree_domain_sync.md` 独立 step 处理。

## Findings

### 1. 已修复：`design_overview.md §14.6` 曾显式 `enqueue_reclaim()`，与 `flush_and_frontier_switch.md §8.1` 冲突

- 原问题位置：
  - `design_overview.md` 旧第 1893 行：`>> on(tree_sched, enqueue_reclaim())`
  - `flush_and_frontier_switch.md` 第 529 行注释：`// retired 由旧 G0 析构时投递 reclaim，此处不直接 enqueue`
  - `runtime_state_machine.md §4.6` `checkpoint_guard::~checkpoint_guard()` 内：`tree_sched->enqueue_reclaim(std::move(retired))`
- 原具体问题：
  - 权威语义是"reclaim 由旧 `checkpoint_guard` 析构时自动投递"，发生点是最后一个 reader 释放该 guard 的时刻，和 flush pipeline 本身没有同步关系。
  - `design_overview.md §14.6` 的伪码把 `enqueue_reclaim()` 当成 flush 管线中的一步，放在 `install_cat2` 之后、`update_superblock_async` 之前。这既不准确（此刻 old G0 多半还没析构），也和 `flush_and_frontier_switch.md §8.1` 直接冲突。
- 为什么这是问题：
  - 未来实现者如果按 §14.6 切 pipeline，会试图在 flush 管线里插入一个 `enqueue_reclaim` 节点，拿到的 retired 集合语义错位（要么提前 enqueue 还没真正 old 的 guard，要么重复 enqueue）。
  - Phase 0 目标就是"正式设计文档不再相互解释冲突"，这是最明显的一条。
- 修复后行为：
  - `design_overview.md:1916` 已把显式 `>> on(tree_sched, enqueue_reclaim())` 替换为注释行 `>> // retired 由旧 G0 析构时自动投递到 tree_sched，不在此处 enqueue（见 flush_and_frontier_switch.md §8.1）`。
  - `runtime_state_machine.md §4.6` destructor 行为保持不变，仍是权威路径。
  - 二轮 grep 独立验证：`rg 'enqueue_reclaim' ai_context/inconel/design_doc/` 只剩 `runtime_state_machine.md:733` 一处。
  - 注释行 `>> // ... >> maybe_root_change(` 的伪码语法略奇，但不影响语义，留作 NB-1 非阻塞观察。

### 2. 已修复：`tree_manifest` 是否含 `leaf_order` 两处源头定义曾不一致

- 原问题位置：
  - `design_overview.md §5.2` 旧第 766-771 行：`struct tree_manifest { paddr root_slot; /* immutable mapping ... */ };` — 没有 `leaf_order`。
  - `code_modules.md` L0 core/ 域对象表第 78 行：`| tree_manifest | root_slot, slot_map | ...` — 没有 `leaf_order`。
  - `runtime_state_machine.md §4.5` 第 687-697 行：`tree_manifest` 含 `small_vector<leaf_span, 0> leaf_order;`，并给出字段语义。
  - `cross_doc_contracts.md §2` 第 58 行：`tree_manifest | root_slot, slot_map, leaf_order | OV §4.4/§9.4, RSM §4.5 | FF §3.4/§3.8, RW §7.2`。
- 原具体问题：
  - 概要（`design_overview.md`）和代码模块文档两处源头定义都漏掉了 `leaf_order`；只有详细设计（`runtime_state_machine.md`）和 contracts 表补上了。
  - `cross_doc_contracts.md §2` 引用的 `OV §4.4` 章节（"运行时 tree frontier 只存在于内存 manifest"）内正文其实也没有 `leaf_order` 字样——这条引用指不到内容。
- 为什么这是问题：
  - 按照 INDEX.md 定位表，`design_overview.md §5` 是"核心运行时对象"的规范入口，实现者先读这里，看到的 `tree_manifest` 结构体就是权威定义。如果这里没有 `leaf_order`，实现者可能把它当成运行时可选字段，甚至不定义。
  - `code_modules.md` L0 core/ 域对象表是"代码归属"的入口，实现者查表发现 tree_manifest 只有 `root_slot, slot_map`，就不会在 `core/tree_manifest.hh` 里放 `leaf_order`。
  - `cross_doc_contracts.md §2` 已经把 `leaf_order` 写成"必须一起同步的关键字段"，但源头定义没跟上，contracts 表的可检查性受损。
- 修复后行为：
  - `design_overview.md:786-791` `tree_manifest` struct 已改为包含三个字段：
    ```cpp
    struct tree_manifest {
        paddr root_slot;
        slot_locator_map slot_map;              // immutable: range_base -> exact slot locator
        small_vector<leaf_span, 0> leaf_order;  // runtime-only immutable leaf spans, key-ordered
    };
    ```
  - `design_overview.md:737` §4.4 已加第 4 条：`tree_manifest 在实现上还同时携带一份 runtime-only immutable leaf_order，供 flush 侧做 batch leaf mapping；它的覆盖性、有序性和不重叠不变量见 runtime_state_machine.md §4.5`。
  - `code_modules.md:78` L0 core/ 域对象表 tree_manifest 行的关键字段列已补 `leaf_order`，引用方列已加 `tree_worker(只读消费)`。
  - 二轮 grep 独立验证：`rg -l 'leaf_order' ai_context/inconel/design_doc/` 覆盖 6 份文档（design_overview / runtime_state_machine / code_modules / cross_doc_contracts / flush_and_frontier_switch / INDEX）；`runtime_memory_and_cache.md` 不需要出现 `leaf_order`，该文档聚焦 frame/cache 层，`leaf_order` 属于 tree 结构 metadata，不在其责域内。

### 3. 已修复：Scheduler 数量 `7` vs `8` 曾自相矛盾

- 原问题位置：
  - `design_overview.md §1.7` 旧第 245 行："从系统分层看，Inconel 由 7 类 scheduler 组成："——下方实际列出 1-8 共 8 类。
  - `runtime_state_machine.md §1` 旧第 5 行："本文细化七类 scheduler 的内部状态"——下方 §1 表格只列 `coord / front / tree_lookup / tree / wal / value / nvme` 7 行，**漏 `tree_worker_sched`**。
  - `INDEX.md` 第 23 行："§1 系统定位与边界 | 8 种 scheduler"——已更新为 8，但源文档没跟上。
- 原具体问题：
  - `design_overview.md` 是自相矛盾：标题说 7，正文列 8。
  - `runtime_state_machine.md` 是双重遗漏：开头数字 + §1 表格都没同步。同文件 §4 确实补了 `tree_worker_sched` 的完整设计，但 §1 没跟上。
  - `INDEX.md` 单独跑前了，成了"索引比源头更新"的怪状态。
- 为什么这是问题：
  - 实现者如果先读 `runtime_state_machine.md §1` 总览表，不会看到 `tree_worker_sched`，会把它误以为是 `tree_sched` 的子组件或辅助线程。
  - Phase 0 的显式目标之一就是"scheduler 拓扑从 7 个更新为包含 `tree_worker_sched` 的 tree read-domain 结构"（见 `020_tree_local_flush_design_sync.md §1`），这条数字没同步就不算完成。
- 修复后行为：
  - `design_overview.md:263` §1.7 已改为 "从系统分层看，Inconel 由 8 类 scheduler 组成："。
  - `runtime_state_machine.md:5` 开头已改为 "本文细化八类 scheduler 的内部状态…"。
  - `runtime_state_machine.md:14` §1 表格已新增一行 `tree_worker_sched | K（运行时参数，与 tree_lookup_sched 成对） | read_domain 引用、candidate-build 临时状态 | read_domain_index 对应同 shard tree_lookup_sched`。
  - 二轮 grep 独立验证：`rg '7 类|七类|7个|七个' ai_context/inconel/design_doc/` 无匹配；`rg -c 'tree_worker_sched' ai_context/inconel/design_doc/` 显示全部 7 份文档均已覆盖（共 33 次出现）。

### 4. 已修复：`runtime_state_machine.md` §9.3 时序图和 §10.3 论证曾未跟上 §4

- 原问题位置：
  - `§9.3 Flush / Frontier Switch 完整时序`（旧 1453-1480 行）
  - `§10.3 tree-domain schedulers 单线程`（旧 1500-1505 行）
- 原具体问题：
  - §9.3 时序图还是：
    ```
    tree_sched ── fold visible state ──
    tree_sched ── write new tree slots ── nvme_sched(s) ──
    ```
    完全没有 `tree_lookup_sched.keys_to_leaf_groups` / `tree_worker_sched.build_leaf_candidates` 两段，与同文件 §4.7-§4.8 和 `flush_and_frontier_switch.md §8.2` 的 sub-pipeline 不符。
  - §10.3 列出 1. `tree_sched` 上的 tree allocator / 2. tree_manifest 构造 / 3. recovery_safe_lsn 推进 / 4. 每个 `tree_lookup_sched` 的 node cache——四条，**没有 `tree_worker_sched`**。tree_worker 同样是单线程 owner-local，这条论证缺一条会让读者怀疑 worker 是否共享可变状态。
- 为什么这是问题：
  - 即使 §4 正文完整，读者如果按"先看时序图再看细节"的习惯阅读，会被 §9.3 的旧画法误导。
  - §10.3 的并发安全论证是实现 review 时的 checklist，漏掉 worker 会让 worker 的无锁性质变成隐含约定。
- 修复后行为：
  - `runtime_state_machine.md:1458-1490` §9.3 时序图已重画为：
    ```
    tree_sched ── choose eligible gens
    ├── fan-out front_scheds: pin imms ── reduce
    tree_sched ── fold memtables into sorted key groups
    tree_lookup_scheds ── keys_to_leaf_groups ── reduce
    tree_worker_scheds ── build_leaf_candidates ── reduce
    tree_sched ── plan delta + submit writes ── nvme_sched(s)
    nvme_sched ── NVMe FLUSH
    coord_sched ── build G1, PRS2, install CAT2
    // retired 由 old G0 析构时自动 enqueue reclaim
    tree_sched ── maybe update superblock
    ```
    与 `flush_and_frontier_switch.md §8.2` sub-pipeline 逐段对应。
  - `runtime_state_machine.md:1516` §10.3 已加第 5 条：`每个 tree_worker_sched 的 candidate build 临时状态与 read-domain 访问同样是 owner-local，不需要锁。`

### 5. 已修复：`design_overview.md` 架构图、§4.4、§14 执行域约定曾未跟上新拓扑

- 原问题位置：
  - `§1.6 系统整体架构` 旧 153-202 行 ASCII 图
  - `§4.4 运行时 tree frontier 只存在于内存 manifest` 旧 710-719 行
  - `§14 典型 PUMP Pipeline` 执行域约定 旧 1771-1777 行
- 原具体问题：
  - §1.6 架构图还是 `coord → value_alloc → front → tree scheduler → nvme`，tree 域被压成单体 `tree scheduler`。行 204 有一句文字补丁提到 `tree_lookup_sched`，但只面向读路径，**既没在图中出现，也没提 `tree_worker_sched`**。
  - §4.4 只列出 `range_base → slot_index / slot_seq / slot_paddr` 三种等价表达，没有一句提到 `leaf_order` 是 manifest 的额外 runtime-only 字段。
  - §14 执行域约定只列了 `coord_sched / front(owner) / tree_sched / wal_space_sched / nvme(dev)` 5 项。**漏 `tree_lookup_sched / tree_worker_sched / value_alloc_sched` 三项**。
- 为什么这是问题：
  - 架构图是概要文档的第一张图，实现者看图后建立的初步心智模型不含 tree 域拆分。
  - §14 执行域约定是 pipeline 伪码章节的字典，读者会把"字典里没列"当成"不存在"。
- 修复后行为：
  - `design_overview.md:149-219` §1.6 架构图已重画：`tree_sched` 下方扩展成三块 —— 左侧 `tree_lookup[]`（point lookup / leaf mapping）、右侧 `tree_worker[]`（old leaf read / candidate build），两者都接到同一块 `tree_read_domain[]`（tree_node cache）后再下沉到 logical B+ tree / value area / nvme。
  - `design_overview.md:737` §4.4 已加第 4 条说明 `tree_manifest` 同时携带 runtime-only immutable `leaf_order`，并回指 `runtime_state_machine.md §4.5` 的不变量。
  - `design_overview.md:1791-1800` §14 执行域约定已从 5 条扩到 8 条，新增 `tree_lookup_sched / tree_worker_sched / value_alloc_sched` 三项，并把 `tree_sched` 的职责描述改为 "tree-local flush round、allocator、manifest 构造和 tree-side reclaim 的 owner"。
  - 二轮非阻塞观察 NB-2：新架构图右侧 `tree_sched → tree_worker[]` 的边标签 "write / trim" 语义错位（worker 不执行 write/trim），但不影响其它文档一致性，留作 cosmetic 观察。

### 6. 已修复：`code_modules.md` memory/ Frame Cache 描述曾未反映 `tree_read_domain` 共享

- 原问题位置：
  - `memory/` 模块旧第 114 行：`| Frame Cache | readonly frame cache 抽象（tree_lookup_sched 和 value_alloc_sched 各自持有实例） |`
- 原具体问题：
  - 这行描述停在"各自持有实例"，没反映 `tree_lookup_sched` 和 `tree_worker_sched` 现在**共享**同一个 `tree_read_domain.readonly_frame_cache` shard 的事实。
  - `runtime_memory_and_cache.md §7.1` 明确归属：`tree_read_domain` 拥有 cache shard，`tree_lookup_sched / tree_worker_sched` 只是访问者。
- 为什么这是问题：
  - 实现者按 `code_modules.md` 分 memory/ 归属时，可能在 `tree_lookup_sched` 和 `tree_worker_sched` 上各自再开一个 `readonly_frame_cache` 实例，违反 §7.1 的 owner 归属。
- 修复后行为：
  - `code_modules.md:114` 已改为：`| Frame Cache | readonly frame cache 抽象（`tree_read_domain` 持有，lookup/worker 共享；`value_alloc_sched` 单独持有 value_page 实例） |`，与 `runtime_memory_and_cache.md §7.1` 的 owner 归属一致。

### 7. 已修复：`INDEX.md` flush §3 导航描述曾停在旧 "Fold 4 步"

- 原问题位置：
  - flush_and_frontier_switch.md 条目旧第 139 行：`| §3 Phase 2: Fold + Write Tree | fold 算法（4 步）、loser 处理、shadow slot 选择、consolidation | 核心 flush 逻辑 |`
  - "Fold 算法 4 步"小结旧 146-150 行：
    ```
    1. 收集所有 memtable key，排序
    2. 通过 manifest + internal 定位受影响 leaf
    3. 并发 NVMe 读所有受影响 leaf
    4. 内存 merge（per-key winner: tree vs WAL）
    ```
- 原具体问题：
  - 新 `§3` 的结构已经是 `§3.1 输入输出 / §3.2 sender 组合 / §3.3 workset fold / §3.4 leaf mapping + candidate build + 写 tree / §3.5-3.9 写 tree 细节`，原本的"Fold 4 步"算法已经拆到 `tree_sched.fold` + `tree_lookup_sched.keys_to_leaf_groups` + `tree_worker_sched.build_leaf_candidates` + `tree_sched.plan_tree_delta_from_candidates` 四个不同 owner。
  - INDEX.md 的小结把"定位 leaf / 读 leaf / 内存 merge"全部挂在 tree_sched 上描述，对应的是旧单体 fold，不是新 pipeline。
- 为什么这是问题：
  - INDEX.md 是"实现时按需查阅"的导航入口。导航描述过时会让实现者按"4 步 fold"去切代码，继续把 old leaf read 写到 `tree_sched` 上。
- 修复后行为：
  - `INDEX.md:139` §3 条目已改为：`| §3 Phase 2: Tree-Local Flush | tree-local flush sub-pipeline：fold / leaf mapping / candidate build / plan+write | 核心 flush 逻辑 |`
  - `INDEX.md:146-150` 小结已改写成 4 段 owner-based：
    ```
    1. tree_sched.fold: 对所有 sealed gens 做 per-key winner 裁决
    2. tree_lookup_sched.keys_to_leaf_groups: 用 manifest.leaf_order 把 sorted keys 映射到 affected leaves
    3. tree_worker_sched.build_leaf_candidates: 读 old leaf、merge、compact 出 candidate image
    4. tree_sched.plan_tree_delta_from_candidates: 写 tree slots + NVMe FLUSH
    ```

### 8. 超出 020 scope 但值得标记：`recovery_and_wal_reclaim.md` 未同步

- 位置：
  - `recovery_and_wal_reclaim.md` 全文
- 具体问题：
  - `020_tree_local_flush_design_sync.md` 的文件清单只列了 7 份，`recovery_and_wal_reclaim.md` 不在 scope 内。
  - 这份文档仍然把 recovery 的 tree merge 写在 `tree_sched` 上，不使用新 tree 域拆分；grep 显示它完全不提 `tree_lookup_sched / tree_worker_sched / tree_read_domain / leaf_order`。
  - 概要 §12 明确说"recovery = flush-like merge"，按这个语义 recovery 应当复用同一套 tree-local flush sub-pipeline，否则 recovery 的 leaf read 会成为 tree_sched 唯一还允许直接读 old leaf 的例外。
- 为什么这是问题：
  - 本 step 可以不处理，但它会成为下一个 Phase 漂移源。Phase 1/2 的实现者按 `recovery_and_wal_reclaim.md` 切 recovery 代码时，会复活"tree_sched 读 old leaf"这条旧语义。
- 期望修复（不作为本 step 的阻塞项，但建议在本 review 完成后立即补一个独立 step）：
  - 决定 recovery 是否复用 tree-local flush pipeline；或在 `recovery_and_wal_reclaim.md` 里显式写"recovery 是一次性 bootstrap 流程，不走 tree 域三类拆分"这一例外，并补上理由。

## 已做验证

本 step 没有代码和测试，verification 只是文档一致性静态检查。首轮 + 二轮合并记录：

### 首轮（finding 发现）

1. 逐份通读 7 份设计文档相关章节：
   - `runtime_state_machine.md` §1 / §4 / §9.3 / §10.3
   - `flush_and_frontier_switch.md` §1.1 / §3 / §8 全部
   - `design_overview.md` §1.6 / §1.7 / §4.4 / §5.2 / §9.4 / §14.6
   - `runtime_memory_and_cache.md` §7.1 / §10.2
   - `code_modules.md` L0 core/ / memory/ / L2 tree/
   - `cross_doc_contracts.md` §1 / §2 / §3 / §5 / §7
   - `INDEX.md` 全文
2. grep 交叉验证：
   - `tree_worker_sched` 在 7 份文档的分布
   - `leaf_order` 在 7 份文档的分布
   - `7 类 / 七类` 残留位置
   - `enqueue_reclaim` 出现点
   - `handle_tree_flush / capture_flush_base_and_pin_input / fold_memtables_into_sorted_key_groups / dispatch_key_partitions_to_lookup / dispatch_leaf_partitions_to_workers / plan_tree_delta_from_candidates / finish_flush_round` 七个 sender seam 的出现点

### 二轮（修复后复核）

reviewer 独立读取每个修改点 + 独立跑 grep 自检，不采信实现者自述。详细 finding 逐项复核见下方 "二轮验证" 节。

对照 `020_tree_local_flush_design_sync.md §验证` 的 4 条 checklist：

- ✅ flush 边界停在 `tree_flush_result`
- ✅ 7 份文档全部出现 `tree_worker_sched / tree_read_domain / tree_manifest.leaf_order`；`design_overview.md §5.2` 的 `tree_manifest` struct 已补 `slot_map + leaf_order`；`code_modules.md` L0 core/ 域对象表的 tree_manifest 行已补 `leaf_order` 并把 `tree_worker` 列入只读消费者
- ✅ `runtime_state_machine.md §9.3` 时序图已重画为 `tree_sched.fold → tree_lookup_scheds.keys_to_leaf_groups → tree_worker_scheds.build_leaf_candidates → tree_sched.plan delta + submit writes`，与 §4.7-§4.8 和 `flush_and_frontier_switch.md §8.2` sub-pipeline 对齐
- ✅ 顶层概览（`design_overview.md`）/ 详细设计（`runtime_state_machine.md` / `flush_and_frontier_switch.md`）/ 缓存模型（`runtime_memory_and_cache.md`）/ 跨文档契约（`cross_doc_contracts.md` + `code_modules.md` + `INDEX.md`）四处表述一致

## 给实现修复的约束

修复 agent 只允许改 020 scope 内的 7 份设计文档：

- `design_overview.md`
- `runtime_state_machine.md`
- `flush_and_frontier_switch.md`
- `runtime_memory_and_cache.md`
- `code_modules.md`
- `cross_doc_contracts.md`
- `INDEX.md`

禁止范围：

- 不提交。
- 不写生产代码。
- 不读、搜索或修改任何测试文件（`test_*.cc` / `*_test.cc` / `tests/` / fixture / benchmark）。
- 不改 `apps/inconel/` 下任何源码文件。
- 不改 `recovery_and_wal_reclaim.md`（超出 020 scope，findings 第 8 条只是提醒，不在本轮修复内）。
- 不新建 runtime skeleton / registry / builder wiring。
- 不写 `leaf_order` 的具体构造代码。
- 不写 `keys_to_leaf_groups()` / `build_leaf_candidates()` 的 production 实现。
- 不碰 `CLAUDE.md` 和 `MEMORY.md`。
- 不处理无关工作树项。

必须完成（逐项对应 findings 1-7）：

1. `design_overview.md §14.6` 删除 `>> on(tree_sched, enqueue_reclaim())`，改为一行注释说明 reclaim 由旧 G0 析构触发。
2. `design_overview.md §5.2` `tree_manifest` struct 补 `leaf_order` 字段或注释；§4.4 补一句 `leaf_order` 是 manifest 的 runtime-only immutable 字段的说明；`code_modules.md` L0 core/ 域对象表的 tree_manifest 行关键字段列补 `leaf_order`。
3. `design_overview.md §1.7` 第 245 行 `7 类` → `8 类`；`runtime_state_machine.md §1` 开头 `七类` → `八类`；§1 表格加 `tree_worker_sched` 行。
4. `runtime_state_machine.md §9.3` 时序图补 `tree_lookup_sched.keys_to_leaf_groups` 和 `tree_worker_sched.build_leaf_candidates` 两段；§10.3 并发安全论证加 `tree_worker_sched` 单线程条。
5. `design_overview.md §1.6` 架构图补 tree read domain；§4.4 补 `leaf_order` 说明；§14 执行域约定补 `tree_lookup_sched / tree_worker_sched / value_alloc_sched` 三条。
6. `code_modules.md` memory/ 模块 Frame Cache 行改成 `tree_read_domain 持有 / lookup/worker 共享 / value_alloc_sched 单独持有`。
7. `INDEX.md` flush_and_frontier_switch.md 条目第 139 行 `§3 Phase 2` 描述改写；146-150 行 "Fold 算法 4 步" 小结改写成 4 段 owner-based。

修复完成后，至少跑一次 grep 自检：

- `rg '7 类|七类|7个|七个' ai_context/inconel/design_doc/` 应无结果
- `rg 'enqueue_reclaim' ai_context/inconel/design_doc/` 只应在 `runtime_state_machine.md §4.6` destructor 内出现
- `rg 'leaf_order' ai_context/inconel/design_doc/` 至少覆盖 `design_overview.md §4.4/§5.2/§9.4`、`runtime_state_machine.md §4.5`、`cross_doc_contracts.md §2`、`code_modules.md` core/ 和 tree/、`flush_and_frontier_switch.md §3.4/§3.8`、`INDEX.md`

## 收口判断

**本 step 作为 Phase 0 "设计文档同步" 收口。**

- A 跨文档矛盾 = 0（原 3 条全部修复）
- B 单文档过时 = 0（原 4 份文档内部过时描述全部更新）
- `020_tree_local_flush_design_sync.md §验证` 的 4 条 checklist 全部 ✅
- 二轮 reviewer 独立 grep 自检（`7 类|七类` 无匹配、`enqueue_reclaim` 仅 destructor、`leaf_order` 覆盖 6 份文档、`tree_worker_sched` 33 次 × 7 份文档）全部通过

下一步可以进入 `flush_development_plan.md` Phase 1 "front input / memtable common carrier" 的代码实现。在此之前仍然受 `flush_development_plan.md §1.1` 和 `flush_module_guide.md §1.3` 约束：不得在 Phase 1 step 计划成文前开始任何 production 代码。

findings 第 8 条（`recovery_and_wal_reclaim.md` 同步）不在本 step 修复范围内，仍然是 Phase 1 开工前的建议性独立 step；建议在 Phase 1 计划之前开一个 `021_recovery_tree_domain_sync.md` 处理，避免成为下一个语义漂移源。

两条非阻塞 cosmetic 观察（NB-1 / NB-2，见 "二轮验证 → 非阻塞观察"）是可选修复，不作为 Phase 0 → Phase 1 的阻塞项。

## 二轮验证

> 最终 verdict 见顶层 "结论" 与 "收口判断"。本节只记录二轮独立验证的原始证据。

基于 reviewer 独立读取修改点 + 独立 grep 自检，不采信实现者自述：

### Grep 自检（reviewer 独立执行）

- `rg '7 类|七类|7个|七个|7 个' ai_context/inconel/design_doc/` → 无匹配 ✅
- `rg 'enqueue_reclaim' ai_context/inconel/design_doc/` → 仅 `runtime_state_machine.md:733`（`checkpoint_guard::~checkpoint_guard()` 内）✅
- `rg -l 'leaf_order' ai_context/inconel/design_doc/` → 覆盖 6 份文件：`code_modules.md / INDEX.md / runtime_state_machine.md / design_overview.md / cross_doc_contracts.md / flush_and_frontier_switch.md`。`runtime_memory_and_cache.md` 不需要出现 `leaf_order`，该文档聚焦 frame/cache 层，`leaf_order` 属于 tree 结构 metadata，不在其责域内。✅
- `rg -c 'tree_worker_sched' ai_context/inconel/design_doc/` → 7 份文档共 33 次出现，全 7 份都覆盖。✅

### Finding 逐项复核

| Finding | 修改点 | 验证结果 |
|---|---|---|
| 1. `§14.6 enqueue_reclaim` 删除 | `design_overview.md:1916` | ✅ 已改为注释："retired 由旧 G0 析构时自动投递到 tree_sched，不在此处 enqueue（见 flush_and_frontier_switch.md §8.1）" |
| 2. `tree_manifest.leaf_order` | `design_overview.md:728-739` + `design_overview.md:786-791` + `code_modules.md:78` | ✅ §4.4 补第 4 条；§5.2 struct 含 `slot_map + leaf_order`；code_modules tree_manifest 行含 `leaf_order` 且 "引用方" 含 `tree_worker(只读消费)` |
| 3. Scheduler `8 类` | `design_overview.md:263` + `runtime_state_machine.md:5` + `runtime_state_machine.md:14` | ✅ OV §1.7 "8 类"；RSM 开头 "八类"；RSM §1 表格新增 `tree_worker_sched` 行 |
| 4. RSM §9.3 时序图 + §10.3 论证 | `runtime_state_machine.md:1458-1490` + `runtime_state_machine.md:1516` | ✅ §9.3 已插入 `tree_lookup_scheds ── keys_to_leaf_groups ── reduce` 和 `tree_worker_scheds ── build_leaf_candidates ── reduce` 两段；结尾以 "retired 由 old G0 析构时自动 enqueue reclaim" 注释收尾；§10.3 加第 5 条覆盖 `tree_worker_sched` |
| 5. OV §1.6 架构图 + §4.4 + §14 约定 | `design_overview.md:149-219` + `design_overview.md:737` + `design_overview.md:1791-1800` | ✅ 架构图已加 `tree_lookup[] / tree_worker[] / tree_read_domain[]` 三块；§4.4 第 4 条补 `leaf_order`；§14 执行域从 5 条扩到 8 条，含 `tree_lookup_sched / tree_worker_sched / value_alloc_sched` |
| 6. code_modules `memory/ Frame Cache` | `code_modules.md:114` | ✅ 改为 "`tree_read_domain` 持有，lookup/worker 共享；`value_alloc_sched` 单独持有 value_page 实例" |
| 7. INDEX.md §3 导航 | `INDEX.md:139` + `INDEX.md:146-150` | ✅ §3 条目 "tree-local flush sub-pipeline：fold / leaf mapping / candidate build / plan+write"；4 段 owner seam 小结已重写为 `tree_sched.fold → tree_lookup_sched.keys_to_leaf_groups → tree_worker_sched.build_leaf_candidates → tree_sched.plan_tree_delta_from_candidates` |

### 非阻塞观察（Non-Blocking）

二轮读文过程中发现两处小瑕疵，不作为本 step 的收口阻塞项；是否修复由实现者决定：

#### NB-1: `design_overview.md §14.6` 注释行 `>>` 语法略奇

位置：`design_overview.md:1915-1917`

```
 >> on(coord_sched, build_g1_prs2_and_install_cat2())
 >> // retired 由旧 G0 析构时自动投递到 tree_sched，不在此处 enqueue（见 flush_and_frontier_switch.md §8.1）
 >> maybe_root_change(
```

`>> // comment >> maybe_root_change(...)` 按 PUMP 伪码规范 parse 比较奇怪——`>>` 后直接接注释再接下一个 `>>`，视觉上像对空 operand 续写。§14 本身是 "非规范伪码"，所以严格 parse 不适用；但语法一致性可以更干净：

```
 >> on(coord_sched, build_g1_prs2_and_install_cat2())
 // retired 由旧 G0 析构时自动投递到 tree_sched，不在此处 enqueue（见 flush_and_frontier_switch.md §8.1）
 >> maybe_root_change(
```

只差一个多余的 `>>`，不影响语义理解，实现者可选修。

#### NB-2: `design_overview.md §1.6` 架构图右侧边标签语义错位

位置：`design_overview.md:186-196`

```
                     +---+---------+---+
                         |         |
               flush map |         | write / trim
              read miss  |         |
         +---------------+         +------------+
         |                                        |
 +-------v--------+                     +---------v--------+
 | tree_lookup[]  |                     | tree_worker[]    |
 | point lookup   |                     | old leaf read    |
 | leaf mapping   |                     | candidate build  |
```

左边箭头 `tree_sched → tree_lookup[]` 标签 "flush map / read miss"，对应 tree_lookup_sched 的两个入口——正确 ✅。

右边箭头 `tree_sched → tree_worker[]` 标签 "write / trim"，但 `tree_worker_sched` 不执行 `write` 也不执行 `trim`——这两项都是 `tree_sched` 自己的职责（`submit_tree_page_writes_bounded` + `reclaim` → `tree_sched`）。worker 只负责 `old leaf read / decode / merge / compact / candidate build`。

建议右边边标签改成更准确的描述，例如：

- "leaf fetch / candidate" 或
- "old leaf read dispatch" 或
- 直接留空，让 worker 方块的正文自描述

实现者可选修；不修也不影响其它文档的一致性。
