# Step 027 — Worker 侧 flush 输出模型重构（内存混合图）

> 本 step 对应 `flush_development_plan.md` 的 **Phase 7**。**Phase 8**（Dead code sweep）和 **Phase 9**（Owner 侧闭环）的详细设计直接在 `flush_development_plan.md` §3.Phase 8 / §3.Phase 9 里；本文只作为 Phase 7 的独立详细设计文档。Phase 9 消费本 step 产出的数据结构。
>
> 核心变更：worker 从"按 old_paddr 产 `flush_changed_node`"（step 026A 的 manifest overlay 模型）改为"产一棵内存混合图（`mem_tree_node` + old_paddr 引用）"。Worker 在自己视角下独立完成**所有结构决策**（leaf rewrite / leaf merge / leaf split / internal rewrite / internal split / consolidation / root split 并增层），完全**不触及** `tree_allocator`——新 page 只存在于内存，paddr 分配统一归 owner 侧（Phase 9）做。
>
> 状态：草案 / 进行中
>
> 本文只在开发期间保留；最终设计回写到 `design_doc/` 后，本文连同 `flush_development_plan.md` 一并删除。

---

## 1. 本 step 解决的问题

Step 026A landed 了 worker 的"manifest overlay"输出模型（`flush_worker_result.changed_nodes` 按 paddr → `flush_changed_node` 映射）。这个模型对 root-stable + no-split 路径勉强成立，但对 split / root-change 路径不能正确表达：

1. **Worker cascade 到 root 产的是 passive old page copy**——worker 本地看不到其他 worker 的 separator 贡献，无法对 shared ancestor 作出正确结构决策；一旦要 split，worker 给的 internal copy 要么被 owner 推翻、要么模型只支持 root-stable。
2. **输出按 paddr 映射本身就是过早约束**——split 产生的新 sibling page、consolidation 产生的新 range、root split 产生的新层全都没有 paddr；原模型用 placeholder / boolean 标记硬套，不自然。
3. **和 owner 合并算法形态不对齐**——Phase 9 的合并算法需要"两棵 worker 局部树做 CoW merge"的语义；per-paddr dict 不是树结构，owner 得反向重建结构关系，多一层 impedance。

本 step 替换为**内存混合图**：worker 输出从它自己视角看的**一棵完整新树**（可能比 old tree 层数多），用内存指针连接新 page，未改动 subtree 通过 old_paddr 引用原位。Worker 只做纯内存构造 + CPU 决策，不分配 paddr、不写盘、不改 manifest。

---

## 2. 新输出数据结构

### 2.1 `mem_tree_node`

```cpp
struct mem_tree_node {
    format::node_type          type;              // leaf / internal

    // Worker 构造的新 page 字节。无 CRC、无 paddr。
    std::vector<char>          content;

    // 这个新节点"代替了哪些 old page"——支持 N-to-1 的 leaf merge。
    // 多数情况一个元素（普通 rewrite / consolidation / split 的某张输出）；
    // Leaf merge 时 2+（合并的 old leaves 都列进去）；
    // 纯新 page（split 产生的 sibling 或 root split 的新层）= 空列表。
    absl::InlinedVector<format::paddr, 2> replaces_old_paddrs;

    // 仅 internal 有效：children 引用列表（长度 N）
    std::vector<child_ref>     children;

    // 仅 internal 有效：separator keys 列表（长度 N-1）
    std::vector<std::string>   separators;
};

struct child_ref {
    std::variant<
        format::paddr,                            // old tree 里未改动的 subtree
        std::unique_ptr<mem_tree_node>            // worker 新造的 subtree
    > target;
};
```

**关键约束**：

- `content` 是 page 字节（已按 ODF §4 格式化），但 **CRC 先不填**（paddr 分配后再填）
- `replaces_old_paddrs` 为空表示"这是新造 page，没有 old 对应"；非空表示"此新节点替换了这些 old page 的作用"
- `children` / `separators` 之间 invariant：`len(separators) == max(0, len(children) - 1)`
- `child_ref` 用 variant：paddr 分支 = 点名引用 old tree 的 unchanged subtree（靠 `base_manifest.resolve(paddr)` 定位），unique_ptr 分支 = 链到 worker 新造的节点

### 2.2 `worker_tree_proposal`

Worker 整个输出的顶层封装：

```cpp
struct worker_tree_proposal {
    flush_round_id                     round_id;
    uint32_t                           read_domain_index;
    flush_stage_status                 st;

    // Worker 视角下的局部新树根。
    // Worker 可能判定 root split / 增层——此时 root 是新造的 internal node，
    // 比 base_manifest.root_slot 所在的层高一层。
    // 若 worker 未改动 root 所在 subtree，root 可以直接是 old_paddr 引用
    // （通过 child_ref 第一级即可表达）——但实现上为简单起见，root 始终是
    // std::unique_ptr<mem_tree_node> 非空；若 worker 未改 root 层，root.children
    // 里挂 paddr 引用表示未改动。
    std::unique_ptr<mem_tree_node>     root;

    // Worker cascade 路径上带出的 old page content，供 tree_sched 合并
    // shared ancestor 时做 diff 参考。Key 是 old_paddr（worker 实际读到过的）。
    absl::flat_hash_map<format::paddr, std::vector<char>> touched_old_pages;

    // 被本 worker 在 merge_and_build_leaf 里判定为"被 memtable winner 覆盖的
    // old leaf value_ref"——走 Gap 1 / 1B 路径挂到 G_K.retired.old_tree_values。
    // Owner 侧（Phase 9）从多 worker 结果里汇总后挂到 checkpoint_guard。
    absl::InlinedVector<core::retired_value_ref, 64> retired_old_values;
};
```

### 2.3 删除的结构

Step 026A / Phase 5 引入的这些过渡态结构本 step 全部删除：

| 结构 | 原用途 | 处理 |
|---|---|---|
| `flush_changed_node` | paddr → 单 page 映射 | 删除；被 `mem_tree_node` 替代 |
| `flush_worker_result.changed_nodes` | worker 主输出 | 改为 `worker_tree_proposal.root` |
| `flush_worker_result.base` | manifest 回指 | 删除（新树自包含，不需要） |
| `flush_mapping_req` / `flush_leaf_group_result` | Phase 5 两次 fanout 的中间态 | 删除 |
| `flush_merge_request.mapping_results` | tree_sched 聚合 mapping 结果 | 删除 |
| `merge_lookup_leaf_groups()` | Phase 5 fan-in dedupe | 删除 |
| `_leaf_mapping::*`（sender/op/req） | Phase 5 worker handle | 删除，逻辑折进 worker 的统一 handle |
| `cascade_climb_one_leaf()` / `cascade_step_*` | step 026A worker cascade 爬 reverse_topology | 删除；替换为 `mem_tree_node` 直接构造 |

---

## 3. Worker 算法修订

### 3.1 新 worker handle 输入输出

```cpp
struct flush_worker_req {
    flush_round_id                    round_id;
    uint32_t                          read_domain_index;
    const core::tree_manifest*        base_manifest;
    uint64_t                          recovery_safe_lsn;

    // Worker 分到的 sorted key_groups。partition 已由 tree_sched 按 leaf_order
    // 对齐好（见 §4.2），worker 内所有 key 都落在一组互不重叠的完整 old leaf
    // 里——不用自己做 keys_to_leaf_groups 映射。
    std::span<const flush_key_group>  key_groups;
};

// Worker 输出
using flush_worker_result = worker_tree_proposal;  // §2.2
```

### 3.2 Worker 内部处理步骤

```
1. 遍历 key_groups，用 base_manifest.leaf_order 把连续 key 归到各自 old leaf
   （这个"映射"在 worker 内部做，是纯内存遍历，不是独立 sender step）

2. 对每个 affected old leaf L：
   a. 读 L 的 old page（先查 read_domain cache；miss 则发 NVMe read，命中率
      由 key-range 路由保证）
   b. 把 L 的 old content 存进 touched_old_pages
   c. 跑 merge_and_build_leaf(old_content, keys_for_L, recovery_safe_lsn)
      → 产一或多张 mem_tree_node（leaf 级；>1 张即本张 leaf 被 split）
   d. 把 old leaf 的被覆盖 value_ref 收集进 retired_old_values

3. 运行 pairwise leaf merge pass（§3.3）

4. 沿 base_manifest.reverse_topology 爬每个 leaf 的 parent 链，构造
   mem_tree_node internal：
   a. 对每级 internal I，收集它的 children（有些是本 worker 改动的 leaf/subtree
      → 指针，有些是未改动的 sibling → old_paddr 引用）
   b. 构造 I 的 mem_tree_node content（separators + children list）
   c. 如果 I 在 worker 视角下 overflow → internal split → 产多张 mem_tree_node
   d. 如果 I 的 old shadow slot 满 → 表示 worker 视角下需要 consolidation，
      worker 照样产 mem_tree_node（无 old_paddr 归属，paddr 留给 plan 分配）
   e. old content 存进 touched_old_pages

5. 一直爬到 root；如果 root 也 overflow → worker 造新一层 internal，
   新 root 是该新层（局部视角下增层）

6. 返回 worker_tree_proposal
```

### 3.3 Pairwise leaf merge pass

```
扫 worker 产出的所有 leaf mem_tree_node，按 key range 排序：
for i in 0 .. n-2:
    L_i     = leaves[i]
    L_i+1   = leaves[i+1]
    if len(L_i.content) + len(L_i+1.content) - header_overlap <= page_size * 0.30:
        # 合起来不到 30% 一张 page —— 合并
        合并 L_i 和 L_i+1 的 records 到一张新 mem_tree_node L_merged
        L_merged.replaces_old_paddrs = L_i.replaces_old_paddrs ∪ L_i+1.replaces_old_paddrs
        用 L_merged 替换 L_i 和 L_i+1 在 parent.children 里的两个 entry
        parent.separators 也相应减少一个
```

**阈值 30%** 是 v1 初始值，post-v1 可调。**只做 pairwise**，不做 N-张合一。**不做 internal merge**（详见 `flush_development_plan.md` §2.Gap 3 决议里的空间浪费估算：10 亿 KV 下 internal 总空间 ~170MB，不 merge 浪费 < 3% in typical workload）。

### 3.4 Cross-worker 边界不处理

Worker 对自己 scope 内的 leaf 做 merge，但**不管**相邻 worker 的 leaf。如果本 worker 最边缘的 leaf 稀疏、隔壁 worker 最边缘的 leaf 也稀疏，这对"跨边界对"**不合并**——接受偶发空间浪费。

理由：cross-worker merge 需要跨 read_domain 的协调，算法复杂度高，发生概率低（partition 按 key 量等切，边界恰好切在两张连续稀疏 leaf 中间的概率很小）。

### 3.5 Worker 不做的决策

即使 worker 算出了 shape-changing 形态（leaf split / internal split / consolidation / root split），worker 也**不**：

- 分配 paddr（`tree_allocator` 不访问）
- 填 CRC（`content` 的 CRC 字段保持 0 或任意值，owner 侧统一重算）
- 写 NVMe
- 修改 manifest
- 判定"全局是否真的需要 root split"（worker 只判自己视角；owner 侧基于合并后的贡献集重判，见 `flush_development_plan.md` §2.Gap 3）

---

## 4. Pipeline 结构变化

### 4.1 现状（两次 fanout，Phase 5 + Phase 6）

```
tree_sched.submit_flush_fold
  → fold_fold_result (partitions)
  → loop >> concurrent >> worker.submit_leaf_mapping       ← fanout #1
  → to_vector
  → tree_sched.submit_flush_merge (aggregate leaf_mapping)
  → round_state.leaf_groups
  → loop >> concurrent >> worker.submit_process_candidates ← fanout #2
  → to_vector
  → tree_sched.submit_flush_merge_candidates (未实现，Phase 9 的职责)
```

### 4.2 目标（一次 fanout）

```
tree_sched.submit_flush_fold
  → fold_fold_result (partitions, leaf-对齐到 base_manifest.leaf_order)
  → loop >> concurrent >> worker.submit_flush_work         ← 唯一 fanout
  → to_vector
  → tree_sched.submit_flush_merge (Phase 9 实现)
```

单一 worker handle `submit_flush_work(flush_worker_req)` 取代 `submit_leaf_mapping` 和 `submit_process_candidates`。

### 4.3 Partition 对齐

`build_key_partitions` 要改——当前是按 workset.size() 等量切（可能切进 leaf 中间），改为按 `base_manifest.leaf_order` 的 leaf 边界 **range-based** 切分：

```
new build_key_partitions:
  let L = base_manifest.leaf_order.size()
  let P = min(worker_count, L)
  for each key in sorted workset:
    leaf_idx = base_manifest.leaf_order.find_leaf_for_key(key)
    partition_idx = (leaf_idx * P) / L           // worker N 拥有 leaves [N*L/P, (N+1)*L/P)
    归入 partition[partition_idx]
  // 不变量：
  //   1. 同一 leaf 的所有 key 落在同一 partition（leaf 不会被切分）
  //   2. 同一 partition 内的 leaves 在 tree 里 tree-adjacent（连续的一段 leaf）
  //   3. 不同 partition 吃不相交的 leaf 段
```

**为什么必须 range-based（不是 `leaf_idx % worker_count`）**：§3.3 pairwise leaf merge 要求 worker 内相邻 leaf 在 tree 里也相邻——否则合并两张 leaf 的 key range 会跨越别 worker 持有的 leaves，产生 key range overlap。`% worker_count`（round-robin）不满足此不变量。

---

## 5. 本 step 工作清单

- [ ] 新增 `mem_tree_node` / `child_ref` / `worker_tree_proposal` 类型（`tree/flush_types.hh` 或新文件）
- [ ] 删除 `flush_changed_node` / `flush_mapping_req` / `flush_leaf_group_result` 等过渡类型
- [ ] 删除 `_leaf_mapping` handle / sender / op
- [ ] 删除 `merge_lookup_leaf_groups()`
- [ ] 新增 worker 统一 handle `submit_flush_work`（替换 `submit_leaf_mapping` + `submit_process_candidates`）
- [ ] Worker `advance()` 里实现 §3.2 的处理步骤
- [ ] 改写 `tree/candidate_build.hh`：用 `mem_tree_node` 构造替代 `cascade_climb_one_leaf` 等
- [ ] 实现 pairwise leaf merge pass（阈值 30% page_size）
- [ ] 改 `build_key_partitions`：按 `base_manifest.leaf_order` leaf-对齐切
- [ ] 修改 `tree_sched` 的 `_flush_fold` handle：返回的 partitions 按新切法
- [ ] Tree_sched 的 `_flush_merge` handle 暂时保持 `unsupported_unimplemented` 返回（Phase 9 才真正实现 merge）——本 step 验收时 Phase 9 还没到，`_flush_merge` 可测"收到正确形态的 worker 输出"但不实际 merge

## 6. 本 step 明确不做

- Tree_sched 的合并算法（Phase 9 职责）
- Paddr 分配 / tree_allocator 实装（Phase 9 职责）
- CRC 填写（Phase 9 职责，合并完毕之后）
- NVMe 写 / flush / bounded writes（Phase 9 职责）
- New manifest 构造（Phase 9 职责）
- `checkpoint_guard` 扩 `retired` 字段（Phase 9 职责）
- update_superblock / frontier_switch / reclaim_q consumer（更外层职责）
- INC-040 的 read-side routing 切换（本 step 只管 flush partition 对齐，read 路径路由按 key-range 是 Phase 9 之后的 follow-up）

## 7. 验收

- [ ] Worker 对 root-stable 路径产出正确的 `worker_tree_proposal`（root 为 `mem_tree_node`，含 old_paddr 引用到未改 subtree）
- [ ] Worker 对 leaf split 路径产出多张 leaf `mem_tree_node` + 正确的 parent 调整
- [ ] Worker 对 leaf merge 路径（相邻对 ≤ 30% combined）产出合并后的单张 leaf
- [ ] Worker 对 root split 路径产出比 base_manifest 高一层的 tree proposal
- [ ] `touched_old_pages` 覆盖 worker 爬过的每张 old internal page（供 Phase 9 合并 shared ancestor 用）
- [ ] `retired_old_values` 里所有 value_ref 都对应"被 memtable winner 覆盖的 tree-visible old value"（Gap 1 / 1B 要求）
- [ ] 所有过渡态类型 / handle 删除干净，代码里无 dead reference

## 8. 与 Phase 9 的接口

Phase 9 直接以 `worker_tree_proposal` 作为输入。本 step 和 Phase 9 之间的数据契约：

- `worker_tree_proposal.root` 是自包含的局部新树（每个节点要么是完整 content 的 new page，要么是对 base_manifest 某 old_paddr 的引用）
- `touched_old_pages` 覆盖所有 worker 构造过的 new internal（含 cascade 到 root 的每层）
- `retired_old_values` 是本 worker 在 leaf merge 时识别出的"覆盖的 old tree value"——Phase 9 汇总进 `tree_flush_result.retired.old_tree_values`

Phase 9 **依赖本 step 已落地**（worker 产新形态）才能动手。

## 9. 进度记录

| 日期 | 内容 |
|---|---|
| 2026-04-15 | 本文创建 (原名 026B)。从 flush_development_plan 的 Phase 9 讨论里把 worker-side 决议拆出独立成 step。Gap 3 worker-side 决议完整记录。 |
| 2026-04-15 | Phase 7 实装 landed。Production code 全绿 (`cmake --build build` 对所有非 flush 测试目标和 main `inconel` binary 0 warning / 0 error)。`test_candidate_build` / `test_flush_carriers` / `test_leaf_mapping` 因引用已删 symbol 编译失败,留待 Phase 8 sweep。partition 算法按用户决议改为 range-based (worker N 拿 leaves `[N*L/P, (N+1)*L/P)`),§4.3 正文已同步改写。 |
| 2026-04-15 | Review fixes (P0 + 三条 P1)：(a) internal cascade 加 `affected_child_internals` child-ready 门控，防止 ancestor cache-hit + child 仍在 NVMe 路上时 `build_one_internal` 误解 child 已 built 的 panic；(b) `_flush_fold` 在 round_state 分配时 `flushed_max_lsn = max(pinned_gens.max_lsn)`（Gap 2），`_flush_merge` empty-workset 返 `ok`、`unsupported_unimplemented` 只留给 non-empty Phase 9 入口、result 始终带 `round.flushed_max_lsn`；(c) 按 Gap 1A 删掉 `tree_flush_result.memtable_losers` 与 `flush_round_state.memtable_losers` 两个悬挂字段；(d) §4.3 正文替换为 range-based 并补说明为什么 `% worker_count` 不合法。 |
