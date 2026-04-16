# Step 029 — Owner 侧闭环（Phase 9）

> 本 step 对应 `flush_development_plan.md` §3.Phase 9。范围严格收紧到 owner 侧的合并 + 持久化闭环——把 Phase 7 输出的 `vector<worker_tree_proposal>` 物化进 tree，产出 `tree_flush_result`。
>
> Phase 7 已 landed（commit 1c5f853）；Phase 8 已取消（commit 4cc3156）。`_flush_merge` 当前对 non-empty workset 返 `unsupported_unimplemented`（`tree/owner_scheduler.hh:420`），本 step 替换成真正合并。
>
> 状态：草案 / 待 codex review。
>
> 终稿确认后，相关结论按本文 §15 回写 `design_doc/`。
>
> **本文不含端到端测试 step 的内容，也不含 INC-040 read-side 路由切换的内容**——两者各自独立，本 step 完成后另行启动；任何依赖"端到端测试已经写好"或"读路径已经按 key-range 路由"的下游推断都不要做。

---

## 1. 本 step 的目标与边界

### 1.1 目标

在 Phase 7 worker 输出之上把 tree-local flush 完整闭环走通。一个 step 内同时落 root-stable + shape-changing 两类路径，理由见 `flush_development_plan.md` §1.3：拆分会强迫 production 代码出现 `unsupported_*` shape-change stub，违反项目规则约束 A 与 `feedback_layered_complete_not_prototype`。

退出后系统具备：

1. `_flush_merge` 真正合并 worker proposals，而不是返回 `unsupported_unimplemented`。
2. `tree_allocator` 可分配/回收/碰撞检测整套就位（`free_ranges` 的 invalidate barrier 留空，因为 `reclaim_q` consumer 仍是外层 step 的活）。
3. 合并后的 mem_tree 经过 paddr 分配 + child_ref 解析 + CRC 填写后，bounded NVMe 写入 + 一次 `nvme_flush`。
4. New manifest（`slot_map`/`leaf_order`/`reverse_topology`/`root_slot`/`root_range_base`）按本轮变化增量构造，**不**做 O(tree) 全扫描。
5. Root-change 时异步发 `update_superblock`，按 RSM §4.9 推进 `superblock_safe_lsn`；root-stable 时直接推进。
6. `finish_flush_round` 产出 `tree_flush_result`，retired 字段就位（fields only；真正的回收投递归 frontier_switch / reclaim consumer，**不在本 step**）。
7. `checkpoint_guard` 加 `retired_objects retired` 字段（fields only；destructor 投递 reclaim_task 留给 frontier_switch step）。
8. 顶层 `tree_local_flush(...)` 在 `tree/sender.hh` 落地，与 §11 的 sender 编排对齐。

### 1.2 不在本 step 范围

下列内容**明确不做**——每条都标了"不做的理由 + 后续在哪里做"。Review 时按这条清单对账。

| 不做项 | 理由 | 后续在哪做 |
|---|---|---|
| `coord_sched.handle_frontier_switch` 本体（构造 G1 / PRS2 / 安装 CAT2 / 把 result.retired append 到 G_K.retired） | flush_module_guide.md §2.1 显式划线"tree-local flush 只到 `tree_flush_result`"；frontier_switch 是 result 的下游消费者，归 `coord_sched` | 后续 frontier_switch step（与 coord_sched 同期） |
| `~checkpoint_guard` 投递 `reclaim_task` 到 `tree_sched.reclaim_q` | destructor 触发依赖 frontier_switch 把 retired 挂到 G_K；G_K destructor 发请求与 reclaim consumer 是一对，单独做没人消费 | frontier_switch step 同期补 destructor + reclaim consumer |
| `tree_sched.reclaim_q` 的 consumer（真正执行 TRIM + value recycle 投递） | 与上一项同因；本 step 只让 `reclaim_q` 物理就位（已在 `tree/owner_scheduler.hh:78` 声明），不写消费端 | reclaim step（与 frontier_switch 同期或之后） |
| `release_gens` 处理 `front_sched.imms` 移除 | `release_gens` 是 `frontier_switch` pipeline 的 fan-out 步骤；本 step 只把 `flushed_gens_by_front` 填进 result，让外层有数据用 | frontier_switch step 的 fan-out |
| INC-040 read-side 路由切 key-range（front→tree_lookup_for_key 路由从 hash 改成 `manifest.leaf_order` 索引） | 与 flush 写侧无 share 状态，纯读路径 spec/code stub 改造；本 step 改完后才有"权威 key→leaf 映射源"，但切换本身不是 flush 闭环的一部分 | INC-040 follow-up step（本 step 完成后启动） |
| 端到端测试程序 / Phase 7 已删 symbol 的失败测试清理 | 本 step 完成后才有完整 production 闭环可对着写测试；先写测试会把现在的"中间形状"凝固成测试期望 | 端到端测试 step（本 step 完成后启动） |

### 1.3 必须遵循的硬约束

1. **v1 容量下限 10 亿 KV / 性能取向 RocksDB×5**（INDEX.md "项目硬约束"）。本 step 任何 carrier、热路径、合并算法都按 10 亿 KV 校准；反例：合并算法不能在 worker_count×leaf_count 上展开 O(全树) 扫描，new manifest rebuild 不能 O(tree) 全扫。
2. **核心引擎能力必须完整**：split / consolidation / root-change / 多类 retire / fail-fast 都要在本 step 一次做完，不允许"先 root-stable 再补 shape-change"（INDEX.md "v1 语义"+`feedback_layered_complete_not_prototype`）。
3. **构造 A — 收窄实现必须显式声明 + fail-fast**：本 step 的合法收窄只有"reclaim_q consumer 不实装"和"~checkpoint_guard destructor 不实装"两项；二者都在 §1.2 表里写明，`reclaim_q` 暂无 producer 调用、`checkpoint_guard` 暂无 destructor 副作用，不构成 silent fallback。除此以外的任何 shape（leaf split / leaf merge / consolidation / root-change / N-to-1 leaf merge / shared-ancestor 合并 / 全局 root split 重判）都必须正确处理。
4. **构造 B — 通用命名对应通用语义**：本 step 引入的命名按本文 §3 列表落，凡未覆盖完整语义的入口必须带限制词（`*_root_stable_only` / `*_single_leaf_only` 等）；本 step 不允许出现这些限制词，因为本 step 范围内的每条入口都必须做完整。
5. **构造 C — 设计缺口必须停下来报告**：Review 期间如果发现本文未覆盖的合并/分配 case，**禁止**自补"最简 spec"；停下来在 review 文档里登记缺口，等本文澄清后再实装。

### 1.4 输入与输出契约

```
输入：
  flush_merge_request {
      flush_round_id                    round_id;
      std::vector<worker_tree_proposal> worker_proposals;   // Phase 7 输出
  }

  parked round_state（tree_state.active_rounds[round_id]）：
      pinned_base_guard               // shared_ptr<const checkpoint_guard>
      pinned_gens                     // 本轮 fold 用过的 sealed gens
      recovery_safe_lsn               // 本轮 snapshot 的 GC 屏障
      workset                         // fold 输出（非空 → 走 Phase 9 真合并）
      partitions                      // leaf-对齐 partitions
      flushed_max_lsn                 // = max(pinned_gens[*].max_lsn) (Gap 2)
      st                              // 默认 ok；fold 已写过 partition_st 的话维持

输出：
  tree_flush_result {
      flush_stage_status                          st;
      std::shared_ptr<const core::tree_manifest>  new_manifest;
      core::retired_objects                       retired;     // 全口径
      flat_hash_map<uint32_t, InlinedVector<shared_ptr<memtable_gen>, 8>>
                                                  flushed_gens_by_front;
      uint64_t                                    flushed_max_lsn;
  }
```

`pinned_base_guard` 在整轮内保活 `base_manifest`；`worker_proposals` 携带的 `touched_old_pages` 字节也是合并阶段读 old 内容的唯一来源（详见 §4.4）。

---

## 2. 合并算法：自底向上 + shape-change（§2.Gap 3.2 阶段 1）

阶段 1 全部在 `tree_sched` 上、单线程、纯内存，**不**触 `tree_allocator`、不发 NVMe（除 §4.4 fallback α）。

### 2.1 输入形态回顾

每个 worker 给一棵局部 hybrid tree：

- `worker_tree_proposal.root` 是 `unique_ptr<mem_tree_node>`，从 worker 视角到的"新 root"——可能比 `base_manifest.root_slot` 所在层高一层（worker 本地 root split）。
- `mem_tree_node.replaces_old_paddrs`：
  - 1 个 paddr：rewrite / consolidation / split 的"承载"页（split 的剩余 sibling = 0 个）。
  - 0 个 paddr：纯新页（split sibling、worker 本地新增层）。
  - 2+ paddr：worker 内 pairwise leaf merge 已合并的旧 leaves（详见 027 §3.3 阈值 30%）。
- `child_ref.target` 是 `variant<paddr, unique_ptr<mem_tree_node>>`：
  - paddr 分支：worker 视角下未改动的 base_manifest 子树。
  - unique_ptr 分支：worker 新建子节点。
- `touched_old_pages: flat_hash_map<paddr, vector<char>>`：worker 实际读过的 old internal page 字节，供 owner 合并 shared ancestor 时做 diff 参考。
- `retired_old_values`：worker `merge_and_build_leaf` 识别到的"被 memtable winner 覆盖的 tree-visible old value"。

worker 不分配 paddr、不填 CRC、不改 manifest——这些是 owner 阶段 2 / 3 的活。

### 2.2 合并对象的命名

为避免和 `mem_tree_node`（worker 私有）混淆，Phase 9 owner 侧用一个新结构装合并产物：

```cpp
namespace apps::inconel::tree {

    // Phase 9 owner-side: assembled merge tree node.
    // Owns its content bytes, knows old paddr footprint, and
    // (after §3) carries the assigned new paddr + resolved children.
    struct merged_node {
        format::node_type         type;

        // Page bytes. CRC field stays 0 until §3.4 fills it.
        // For nodes that came directly from one worker's mem_tree_node
        // we move-construct from that node's `content`.
        std::vector<char>         content;

        // Old paddr(s) this merged_node replaces.
        // Same convention as mem_tree_node.replaces_old_paddrs.
        absl::InlinedVector<format::paddr, 2> replaces_old_paddrs;

        // For internal nodes: structural children (parallel to
        // `separators`, length N). After §3.3 every entry is a
        // resolved paddr (no unique_ptr); during §2 we still
        // carry unique_ptr<merged_node> for child slots that
        // have not yet had paddrs assigned.
        std::vector<merged_child> children;
        std::vector<std::string>  separators;  // length N-1

        // Filled in §3 (allocator pass).
        format::paddr             new_paddr{};       // {0,0} until assigned
        uint32_t                  new_slot_index = 0;
        format::paddr             new_range_base{};  // for slot_map update

        // Optional reverse pointer used by §3.3 when patching
        // child_base entries in `content` — populated lazily.
    };

    struct merged_child {
        std::variant<format::paddr, std::unique_ptr<merged_node>> target;
    };

}
```

这个结构和 `mem_tree_node` 形态相似但语义不同：`mem_tree_node` 是 worker 私有 view，`merged_node` 是合并完成的全局 view。一旦合并完毕，`merged_node` 是后续 §3 / §5 的唯一输入。

> 命名按构造 B：通用名 `merged_node` 对应"合并后的统一节点表示"，本 step 不引入 `merged_node_root_stable_only` 等带前缀变体。

### 2.3 阶段 1 总流程

```text
Step 0  收齐输入 + 异常路径短路
        ─ 任一 worker_proposal.st != ok → 立即返回 result.st = 该 status
        ─ 所有 proposal.root == nullptr → 返回 result.st = ok, new_manifest = base
          （nothing to do；也写空 retired，consumer 负责选择不切换 frontier）

Step 1  构造 contributor 索引
        index: paddr(old) → {worker_idx, mem_tree_node*} 列表
        来源 = 遍历每个 worker_proposal.root，递归收集每个 mem_tree_node
        的 replaces_old_paddrs[i] 和路径上经过的 child_ref(paddr) 引用。

Step 2  自底向上合并
        递归从 root 出发；对每个子树：
          - 合并子树 →（递归）
          - 合并当前层（详见 §2.4）

Step 3  全局 root split 重判（§2.5）

Step 4  汇总 retired_old_values（§2.7）
        round_state.retired.old_tree_values = ⋃ workers' retired_old_values
```

### 2.4 单 old_paddr 的合并裁决（核心）

考虑同一 old_paddr `P` 被多少 worker 触达：

| Case | contribs(P) 形态 | 处理 |
|------|---|---|
| **A. 无 worker 触达** | 空 | 不出现在合并树里，原 paddr 引用保持（这条天然由 paddr child_ref 表达）。 |
| **B. 单 worker，单 mem_tree_node** | `[(w_i, n_i)]`，且 `n_i` 在 `w_i.proposal` 里**唯一**承载 `P` | 直接把 `n_i` 上提为 `merged_node`；`n_i.children` 里的 `paddr` 子保持，`unique_ptr` 子递归合并。 |
| **C. 单 worker，多 mem_tree_node 都列了 P** | 单 worker 内 split，`P` 出现在第一个新 page 的 `replaces_old_paddrs` | 同 B：合并产物 = 该 worker 的多页序列（leaf split / internal split），全部上提，separators 一并搬运。 |
| **D. 多 worker，全部走 paddr 引用 P** | contribs(P) 为空但有多个 `child_ref{paddr=P}` | 不需要合并；P 在合并树里以 paddr 引用形态出现（多子节点共用同一 paddr 引用是合法的，因为他们最终都解析到同一 base_manifest 子树）。 |
| **E. 多 worker 都新建 mem_tree_node 替换 P** | `[(w_i, n_i), (w_j, n_j), …]` (k ≥ 2) | shared ancestor 真合并，详见 §2.4.E 算法。 |
| **F. N-to-1 leaf merge（worker 内）** | `n_k.replaces_old_paddrs.size() ≥ 2` | 阶段 1 不再处理多 old_paddr → 合并那些 P 的 contribs；那些 P 在合并产物里都被 `n_k` 表示，同一 `merged_node` 出现在每条 contributor 索引项里。允许出现（worker pairwise merge 已经合法压缩两张 leaf）；多 worker 不会同时给出同样的"多 old_paddr 单页"，因为 worker 的 partition 是 leaf-对齐的（详见 027 §4.3）——所以 case F 只会以 case B 或 C 出现，不和 case E 撞车。 |

#### 2.4.E shared ancestor 真合并

发生条件：**`internal` 节点 P 的 children 里跨 worker 各自做了不同改动**——典型场景是两条 worker 的子树各自 cascade 到 P 这一层。

输入：

```text
contribs = [(w_1, n_1), (w_2, n_2), ..., (w_k, n_k)]
old_content = worker_proposal.touched_old_pages[P]  // 任选 contribs 里第一个 worker 提供
```

算法（伪代码，`merged_node` 是 §2.2 的结构）：

```text
merge_internal_node(P, contribs, old_content) -> merged_node:
    decode old internal page from old_content
        → old_children[]   (N+1 paddrs)
        → old_separators[] (N strings)

    // ── Step 1: 把每个 contributor 规范化成 "old slot interval → substitution"
    //
    // worker mem_tree_node 允许一个新 child 覆盖多个 old child slot
    // （pairwise leaf merge 把 L1+L2 合成一页 → 新 leaf 的
    // replaces_old_paddrs = [L1, L2]；internal 同理，worker 的
    // `lookup_child_group` 在 candidate_build.hh:907-914 已经靠
    // `covered_paddrs` 做跳槽处理）。owner 侧必须对齐同样的语义，
    // 否则被吸收的 old slot 会被 Step 2 的 empty-view 分支重新带回。
    //
    // substitution_iv 结构：
    //   {
    //     start_old_slot:  uint32,        // 覆盖的第一个 old_child 槽位 i0
    //     end_old_slot:    uint32,        // i0 + len - 1（inclusive），即最后一个被吸收的槽位
    //     nodes:           [merged_node*],// 1 个 = 纯 rewrite / consolidation
    //                                      // 2+ = split sibling 序列（第 1 个承载老 paddr，其余 0 个）
    //     sibling_seps:    [string],      // len = nodes.size() - 1
    //     worker_index:    uint32         // 用于诊断 + 冲突决议
    //   }
    //
    // 规范化步骤（对每个 (w, n) 单独做）：
    //   按 n.children 的顺序扫一遍；用 worker 视角的 replaces_old_paddrs +
    //   paddr-child 引用，回映到 old_children 下标集合。
    //
    // 关键：对 unique_ptr 子节点，吸收集合 = child_node.replaces_old_paddrs
    //      （可以跨多个 old_children[i..i+k]）；
    //      对 paddr 子节点，吸收集合 = {那个 old_children[i]}（单槽位）。

    substitutions_per_contrib: vector<vector<substitution_iv>>
    for each (w, n) in contribs:
        subs_w = []
        for j in 0 .. n.children.size()-1:
            c = n.children[j]
            if c.target is paddr:
                i = find_old_child_index(old_children, get<paddr>(c.target))
                subs_w.append(substitution_iv{
                    start_old_slot = i, end_old_slot = i,
                    nodes = [nullptr],     // 不是 substitution，占位；Step 2 用 paddr 直通
                    is_passthrough_paddr = true,
                    ...
                })
            else:  // unique_ptr<mem_tree_node>
                child_node = get<unique_ptr>(c.target)
                covered = child_node.replaces_old_paddrs  // 可能 0 / 1 / 2+
                if covered.empty():
                    // 纯新页（split sibling / worker 新增层）——不对齐任何
                    // old slot，作为 "insertion" 挂在前一个 substitution 后面。
                    subs_w.back().nodes.append(child_node)
                    subs_w.back().sibling_seps.append(n.separators[j-1])
                    continue
                start_i = min_i(find_old_child_index(old_children, p) for p in covered)
                end_i   = max_i(...)
                // 不变量：covered 的所有 paddrs 在 old_children 里连续
                //         （worker 对自己 scope 内的相邻 leaf 做 pairwise
                //          merge，不会跨槽位 merge）
                check_contiguous(covered, old_children, start_i, end_i)
                subs_w.append(substitution_iv{
                    start_old_slot = start_i, end_old_slot = end_i,
                    nodes = [child_node], is_passthrough_paddr = false,
                    sibling_seps = [], worker_index = w,
                })
        substitutions_per_contrib.append(subs_w)

    // ── Step 2: 按 old_children 槽位汇总
    //
    // owner_slot_view[i] = 所有覆盖了 old_children[i] 的 substitution 列表。
    //                     同一 substitution_iv 覆盖 [s..e] 时，会出现在
    //                     owner_slot_view[s], owner_slot_view[s+1], ...,
    //                     owner_slot_view[e] 所有位置。
    //                     带 is_passthrough_paddr=true 的条目不进此视图
    //                     （它不是 substitution，只是"worker 没改该 slot"
    //                     的显式声明；Step 3 要和"无任何 contrib 触达 i"
    //                     区分开）。

    owner_slot_view: vector< vector<substitution_iv*> >(N+1)
    passthrough_marker: vector<bool>(N+1, false)   // 见 §2.4.E 要点 1
    for subs_w in substitutions_per_contrib:
        for iv in subs_w:
            if iv.is_passthrough_paddr:
                passthrough_marker[iv.start_old_slot] = true
                continue
            for i in iv.start_old_slot .. iv.end_old_slot:
                owner_slot_view[i].append(&iv)

    // ── Step 3: 线性扫 old_children，每次一个槽位决策
    //
    // 三种情形：
    //   (a) owner_slot_view[i] 空 → 保留 old_children[i] 作为 paddr child_ref
    //       （无 worker 改过该 slot）
    //   (b) owner_slot_view[i] 中所有 substitution 都是"同一 iv 的多次出现"
    //       （iv 覆盖 [s..e]，i ∈ [s..e]） → 把 iv.nodes 作为该槽位的
    //       substitution；跳过 [s+1..e] 的后续槽位（这些都被同一个 iv 吸收）。
    //   (c) owner_slot_view[i] 有多条不同 iv → 跨 worker 冲突，详见要点 2。

    new_children: vector<merged_child>
    new_separators: vector<string>
    i = 0
    while i <= N:
        view = owner_slot_view[i]
        unique_ivs = dedupe(view)   // 同一 iv 指针可能重复

        if unique_ivs.empty():
            // (a) passthrough 或无改动。二者对下游结果相同：paddr 直通。
            new_children.append(merged_child{paddr=old_children[i]})
            if i < N: new_separators.append(old_separators[i])
            i += 1
            continue

        if unique_ivs.size() == 1:
            iv = unique_ivs[0]
            // (b) 该 iv 可能覆盖 [s..e]；把它所有 nodes 和 sibling_seps 铺开
            for node_k in iv.nodes:
                new_children.append(merged_child{
                    target = unique_ptr<merged_node>{ lift_node(node_k) }
                })
            for sep in iv.sibling_seps:
                new_separators.append(sep)
            // 跨槽位被这个 iv 吸收时，槽位间的 old_separators 作废
            // （因为 iv 已经合并了左右 leaf 的 record，原 separator
            //  不再有含义）；连带的 "new separator 与前一个 child
            //  之间的 separator" 由调用方从前一个 old_separators[i-1]
            //  取，由 pick_separator 统一处理。
            i = iv.end_old_slot + 1
            if i <= N and i > 0:
                // 进入下一 child 时需要一个 separator；取 old_separators[i-1]
                // 或如果 iv 后紧跟另一个 substitution 再由 Step 3 下一轮决定
                if i <= N: new_separators.append(pick_boundary_sep(...))
            continue

        // (c) 多个不同 iv 命中同一槽位——constraint C 缺口，详见要点 2
        panic_inconsistency("merge_internal_node",
            "multiple worker substitutions targeting the same "
            "base_manifest slot (i=%u, ivs=%zu)", i, unique_ivs.size())

    // ── Step 4: 用 build_internal_pages 重排成新 internal page(s)
    //
    // 参数 new_children 和 new_separators 的长度不变量：
    //     len(new_separators) == len(new_children) - 1
    nodes = owner_build_internal_pages(P, new_children, new_separators,
                                        page_size)
    // 多页时：第一页 replaces_old_paddrs = [P]，后续页 = 空（split 兄弟）
    // sibling_separators 累积成 P 的父层用的"额外分隔符"，由调用者继续
    // bubble up。
    return nodes  // length ≥ 1
```

要点：

1. **passthrough 与 "无 worker 触达" 必须区分**：
   worker 在 `lookup_child_group` 里对未改的 old slot 显式给出 `child_ref{paddr=P_i}`（`candidate_build.hh:918-928` 的 "unchanged → paddr ref" 分支）；owner 侧如果把这类 passthrough 直接丢进 `owner_slot_view`，和 "另一 worker 的 substitution 改了同一 slot" 就会被计成冲突。所以 passthrough 用单独的 `passthrough_marker[i] = true` 记录——但下游只要看到 `owner_slot_view[i]` 为空就走 "保留 old paddr" 分支，passthrough 本身不影响决策结果。保留 passthrough_marker 是为了 **诊断时区分 "worker 明确说没改" 与 "根本没 worker 看到这一层"**，两者都走 paddr 直通。

2. **跨 worker 同槽位冲突（Step 3 case (c)）是 constraint C 缺口**：
   Phase 9 的 partition 是 leaf-对齐的（Phase 7 `build_key_partitions` 保证），所以不会有两个 worker 都改同一 leaf 层的 old_children[i]。但在 internal 层理论上允许——shared ancestor `P` 下的两个 child internals 可能各自被不同 worker cascade 到同一层。这种情况由递归 `merge_internal_node` 再下一层自然处理（两个 contribs 会出现在那层的 `contribs` 里），而不是在本层把两个 substitution 都贴到同一 slot。如果在本层真看到 case (c)，说明 worker 规范化出了问题——直接 `panic_inconsistency`，而不是 silent 选一个。

3. **`owner_build_internal_pages`**：和 worker 的 `build_internal_pages` 同算法，但写到 `merged_node.content`；split 时返回多页，并把"被切分位置的 separator"作为本节点对父层 bubble up 的 sibling separator。

4. **separator bubble up**：split 在合并里产生新 separator → 作为 contribution 沿父链向上一层送，触发父层 `merge_internal_node` 的 `pick_separator` 用新的；如果父也 split → 继续 bubble up；最终落到 root 处由 §2.5 处理。

### 2.5 全局 root split 重判

worker 视角的 root split 只是局部判断：worker A 觉得 root 满了 → 自己加了一层。但合并完成后，全局 root 是不是也满了，必须**在所有 worker 贡献都汇入合并树之后**重新判定一次。

```text
finalize_root(merged_node* combined_root):
    if combined_root is internal AND fits in single page (按 children + separators 估算):
        // 全局 root 可保持单层（哪怕某 worker 局部加过层，那层在
        // merge_internal_node 里自然合回去了）
        return combined_root

    if combined_root is internal AND need split:
        nodes = owner_build_internal_pages(combined_root.replaces_old_paddrs[0],
                                            combined_root.children,
                                            combined_root.separators, page_size)
        // 产生 ≥ 2 张 → 增层
        new_root = synthesize_layer_above(nodes, sibling_seps)
        return new_root

    if combined_root is leaf AND fits (单叶情况):
        return combined_root

    if combined_root is leaf AND need split:
        leaves = ... (按 page_size 切)
        new_root = synthesize_internal_layer(leaves, separators)
        return new_root
```

`synthesize_layer_above`：用一个新的 `merged_node{type=internal, replaces_old_paddrs={}}` 作为新 root，把分裂出来的兄弟作为 children。新 root 的 paddr 在阶段 2 由 allocator 分一个新 range，slot 0。

> 这一步不能委派给 worker：worker 只看自己 partition 的 cascade，没法判断"全局 root 是否真的需要再加一层"。

### 2.6 root-stable vs root-change 判定

合并完成后通过两条规则判定：

1. **root-stable**：
   - new root 节点的 `replaces_old_paddrs == [old root range_base]`，且
   - new root 节点的 paddr（阶段 2 决定）落在同一 old range（即 `slot_index < shadow_slots_per_range - 1`），且
   - new root 节点不是 §2.5 新合成的 layer。
2. **root-change**：上述任一不成立。

判定结果存到 `round_state` 一个临时字段（不进 `tree_flush_result` 字段，但用于 §6 `update_superblock` 和 §7 `superblock_safe_lsn` 推进）。

### 2.7 retired 汇总

```text
round_state.retired.old_tree_values =
    ⋃ over workers (proposal.retired_old_values)

// Note: round_state.retired.old_slots 和 round_state.retired.old_ranges
// 在阶段 2 paddr 分配时填（见 §3.2），不在阶段 1 填。
```

`old_tree_values` 在阶段 1 末尾就能填完，因为 worker 已经识别完毕；阶段 2 不会再产生新条目。

### 2.8 阶段 1 出口

- `combined_root: unique_ptr<merged_node>`（合并完成的根，所有内部节点 still 持 unique_ptr 子）
- `round_state.retired.old_tree_values`（已填）
- `is_root_change: bool`（按 §2.6 判定）

至此 nothing on disk has changed；进入阶段 2。

---

## 3. tree_allocator 实装

阶段 2 的前置：`tree_allocator` 要从当前的 placeholder（`tree/owner_scheduler.hh:66-69` 只有 `head` + `free_ranges` 两个字段）补全成 RSM §4.4 的形态。

### 3.1 数据结构与不变量

```cpp
namespace apps::inconel::tree {

    // RSM §4.4 — single owner = tree_sched.
    struct tree_allocator {
        // Range head = next allocatable range_base. Bumps upward
        // by `range_lbas_` on each fresh allocate(). Initialized
        // by recovery / format to `data_area_base_paddr`.
        format::paddr head{};

        // Recycled ranges (from §3.6 below). Reused before bumping
        // `head`. invalidate barrier is NOT yet implemented (Phase 9
        // declared narrowing — see §3.5); recycle() pushes here
        // unconditionally. Producer = §5.5 retired plumbing in
        // future frontier_switch step; in Phase 9 the queue is
        // empty in steady state.
        local::queue<format::range_ref, 4096> free_ranges;

        // Shared collision-detection atomics, owned by the runtime
        // (one instance per device). Write side here is value side
        // (RSM §4.3); we read value_head_lba relaxed and write
        // tree_head_lba relaxed.
        core::data_area_heads* shared_heads = nullptr;

        // Frozen at construction from format_profile.
        uint32_t range_lbas    = 0;     // page_lbas * shadow_slots_per_range
        uint32_t shadow_slots  = 0;     // shadow_slots_per_range

        // Allocation entry points — see §3.2/§3.3/§3.4/§3.5.
        format::range_ref allocate();
        void              push_back_bump(format::range_ref r);
        void              recycle(format::range_ref r);
    };

}
```

要点：

- 单 owner（`tree_sched`）→ 不要锁（§1.3 构造 + RSM §10.3）。
- `free_ranges` 用 `local::queue`（pump/core）—— same-thread producer/consumer，无原子开销。
- `shared_heads` 是 RSM §4.3 / §6.3 共享的两个 atomic（tree_head / value_head），从 runtime registry 里拿（`core::registry` 里要新增；详见 §3.6）。
- `range_lbas` / `shadow_slots` 从 `tree_geometry`（已有）里拿。

### 3.2 `allocate()`

```text
allocate() -> range_ref:
    1. if r = free_ranges.try_dequeue(): return r       // 优先复用
    2. // 碰撞检测：head 的下一段是否撞上 value 端
       value_low = shared_heads->value_head_lba.load(relaxed)
       next_end_lba = head.lba + range_lbas
       if next_end_lba > value_low:
           panic_inconsistency("tree_allocator::allocate",
               "data area exhausted")    // v1 没 reclaim 撞底 = 不可恢复
    3. r = range_ref{ .base = head, .slot_count = shadow_slots }
    4. head.lba = next_end_lba
    5. shared_heads->tree_head_lba.store(head.lba, relaxed)
    6. return r
```

注意：

- 撞底走 `panic_inconsistency`（已用于 INC-017 的 value 侧），不走 `unsupported_*`——空间撞底是 v1 不可恢复状态，不是合法功能缺口。
- `shared_heads->tree_head_lba.store(...)` 是 relaxed：value 侧只用它做 sanity，不依赖此读做正确性决策（同 RSM §4.3 既存约定）。

### 3.3 `push_back_bump(r)`

INC-017 在 value 侧引入的 rollback helper：bump 失败时把刚分出去的 range 倒着推回 `head`，避免泄漏。Phase 9 tree 侧需要同形态，因为合并 → 分配 → 写盘任何一步出错时也要能撤销这一轮的分配。

```text
push_back_bump(r):
    // r 必须是本轮刚 bump 出来的最末段（从 head 的反向看）。
    if r.base.lba + range_lbas != head.lba:
        panic_inconsistency("tree_allocator::push_back_bump",
            "range is not the last bumped range; rollback order violated")
    head.lba = r.base.lba
    shared_heads->tree_head_lba.store(head.lba, relaxed)
    // 不入 free_ranges：这块刚从 head 出去，没写过任何字节。
```

调用方约定：rollback 必须按反向顺序（最后分的最先 push back）。如果 rollback 顺序错乱 → fail-fast。

### 3.4 `recycle(r)`

```text
recycle(r):
    // RSM §4.4 / cross_doc 红线：进入 free_ranges 之前必须先完成
    // tree_node invalidate barrier。Phase 9 不实装 barrier(§1.2)，
    // 也不实装 reclaim_q consumer——这里直接进队，因为 v1 当前没有
    // 任何 producer 投递 retire 任务到 reclaim_q（frontier_switch /
    // ~checkpoint_guard 都还没接通）。
    //
    // 因此本 step 内 free_ranges 永远是空的；调用 recycle() 在 Phase 9
    // 范围内不会被任何代码触发（占位）。
    free_ranges.try_enqueue(r);
```

写在代码里只是把 RSM §4.4 的接口骨架就位，让后续 frontier_switch / reclaim consumer 落地时签名稳定；本 step 不允许 recycle 被实际走通——构造 A 显式声明：

> ```cpp
> // PHASE 9 NARROWING: recycle() is wired up to free_ranges but no
> // producer in-tree calls it. The tree_node invalidate barrier
> // mandated by RSM §4.4 / cross_doc §6.2 is intentionally NOT
> // installed here — frontier_switch step will both add the producer
> // (~checkpoint_guard / reclaim_q dispatch) and the barrier in one
> // change, since they are co-required.
> ```

### 3.5 与 INC-040 / 读路径的关系

`tree_allocator` 不依赖 INC-040 路由切换，互相独立。本 step 落 allocator 不会触发或要求 INC-040；如果 review 期发现这两块逻辑互相牵连，按构造 C 上报。

### 3.6 runtime 注入

- runtime builder 构造 `data_area_heads`（per-device）共享实例，注入给 `value_alloc_sched`（已有）和 `tree_allocator`（新增）。
- 新增 `core::registry::data_area_heads_singleton()` 让 builder 用同一指针装两个 scheduler。
- 装配顺序：构造 `data_area_heads` → 构造 `value_alloc_sched`（给指针）→ 构造 `tree_sched`（其内部初始化 `tree_allocator.shared_heads`）。

---

## 4. Plan 阶段：paddr 分配 + child_ref 解析 + CRC（§2.Gap 3.2 阶段 2）

### 4.1 总流程

```text
plan_phase(combined_root):
    nodes_in_post_order = post_order_walk(combined_root)
    for n in nodes_in_post_order:                         // §4.2
        assign_paddr(n, allocator)
    for n in nodes_in_post_order:                         // §4.3
        if n.type == internal:
            patch_child_base_in_content(n)
    for n in nodes_in_post_order:                         // §4.4
        n.content[crc_offset..crc_offset+4] =
            tree_page_compute_crc(n.content.data(), page_size)
```

后序遍历保证：分配子节点的 paddr 后，再处理父节点；patch 父节点 content 时所有子 paddr 已知；CRC 在 patch 完成后才算（CRC 覆盖整个 page，包括 child_base 字段）。

### 4.2 `assign_paddr(n)` 决策

按 `n.replaces_old_paddrs` 数量分支：

```text
case |replaces_old_paddrs| == 1:
    P = replaces_old_paddrs[0]
    // P 是 base_manifest 里某个 leaf / internal 的 range_base。
    cur_slot = base_manifest.slot_index(P)
    if cur_slot + 1 < shadow_slots_per_range:
        // 同 range 下一 slot
        n.new_range_base = P
        n.new_slot_index = cur_slot + 1
        n.new_paddr      = geom.slot_paddr(P, cur_slot + 1)
        round.retired.old_slots.push(geom.slot_paddr(P, cur_slot))   // §5.4
    else:
        // shadow slot 用尽 → consolidation：分配新 range，写 slot 0
        new_range = allocator.allocate()                           // §5.4 records range
        n.new_range_base = new_range.base
        n.new_slot_index = 0
        n.new_paddr      = new_range.base
        round.retired.old_ranges.push(geom.range_ref_from_base(P))

case |replaces_old_paddrs| == 0:
    // 纯新页（split sibling / 新合成的层）
    new_range = allocator.allocate()
    n.new_range_base = new_range.base
    n.new_slot_index = 0
    n.new_paddr      = new_range.base
    // 不向 retired 推任何东西

case |replaces_old_paddrs| >= 2:
    // 多 old → 1 new 的 leaf merge（worker pairwise merge 已做完）。
    // 选第一个（保持稳定顺序）作为"承载"——按 case 1 走 cur_slot+1
    // 或 consolidation；其他 old paddr 整个 range 进 retired.old_ranges。
    P_carrier = replaces_old_paddrs[0]
    // ...同 case 1 的 cur_slot + 1 / consolidation 选择 ...

    for j in 1 .. n.replaces_old_paddrs.size()-1:
        P_other = replaces_old_paddrs[j]
        round.retired.old_ranges.push(geom.range_ref_from_base(P_other))
        // P_other 整个 shadow range 都不再被引用（两张 leaf 合并后旧
        // leaf 的所有版本都失效）。
```

异常路径：

- `allocator.allocate()` 撞底 → `panic_inconsistency`（§3.2）。
- `slot_index()` 失败（base_manifest 里找不到 P）→ `manifest::resolve` 已 panic（`core/tree_manifest.hh:74`）。

### 4.3 `patch_child_base_in_content`

合并阶段写 page bytes 时把所有 unique_ptr 子的 child_base 字段填成占位符 `paddr{0,0}`（参考 worker 的 `child_to_paddr`）。Plan 阶段需要回填真正的 paddr：

```text
patch_child_base_in_content(n):
    parse n.content as internal_page
    for j in 0 .. n.children.size()-1:
        child = n.children[j]
        child_paddr = (child.target is paddr)
                      ? get<paddr>(child.target)
                      : get<unique_ptr<merged_node>>(child.target)->new_paddr

        if j < n.children.size() - 1:
            // 普通 separator-record；child_base 紧跟 separator key 字节
            internal_page_locate_child_base(n.content, j, child_paddr)
        else:
            // rightmost child — 紧贴 free_space_offset 之前的 sizeof(paddr)
            internal_page_set_rightmost(n.content, child_paddr)
```

- `internal_page_locate_child_base` 用现有 `format::internal_record_child_base()` helper（`format/tree_page.hh:128-138`）算偏移。
- rightmost 的位置由 `tree_slot_header.free_space_offset - sizeof(paddr)` 决定（page_reader.hh:148 已有 reader 端对应实现）。

不变量：本步不会 reorder children / separators；只覆盖 `paddr` 字段。

### 4.4 CRC

```text
n.content_crc_field = tree_page_compute_crc(n.content.data(), page_size)
```

`tree_page_compute_crc()` 已存在（`format/tree_page.hh:157`）。CRC 必须在 child_base 全部 patch 完之后才算，否则 CRC 校验会因为占位 `paddr{0,0}` 落空。

### 4.5 阶段 2 出口

每个 `merged_node` 都有：

- `new_paddr` 已分配
- `new_range_base` / `new_slot_index` 填好
- `content` 的 child_base 已回填，CRC 已写

`round.retired.old_slots` / `round.retired.old_ranges` 也都已填完。

---

## 5. Writer + device flush

### 5.1 写计划

阶段 2 后 `combined_root` 子树里的每个 `merged_node` 都对应一次 NVMe write。写描述组织为：

```cpp
struct write_desc {                  // 已存在 — format/types.hh:56
    uint64_t    lba;
    const void* data;
    uint32_t    num_lbas;
    uint32_t    flags;               // Phase 9 不带 FUA
};

std::vector<format::write_desc> writes = build_write_descs(combined_root);
```

`build_write_descs` 后序遍历 `combined_root`，每个节点产一条：`{.lba = n.new_paddr.lba, .data = n.content.data(), .num_lbas = page_lbas, .flags = 0}`。

> **不带 FUA**：单页 FUA 在 tree 写侧无意义——本步 §5.3 用一次 `nvme_flush()` 覆盖整批 writes 的持久化保证（`feedback_flush_vs_fua` 决议）。

### 5.2 Bounded writes

```text
submit_tree_page_writes_bounded(writes) =
    as_stream(writes)
    >> concurrent(N)                         // §5.2.1: N 选取
    >> on(local_nvme_sched)                  // 真正并发开始
    >> flat_map([](write_desc d) {
        return mock_nvme::write_one(local_nvme(),
                                    d.lba, d.data, d.num_lbas, d.flags);
    })
    >> all()
```

这个组合对应 `mock_nvme/sender.hh::write_batch` 的 bounded 变体。`write_batch` 当前用 `concurrent()`（无界）；Phase 9 改用有界，避免单核 NVMe qpair 拥塞。

#### 5.2.1 N 的选择

固定上限 `kMaxBoundedTreeWrites = 32`：

- 同一 NVMe 设备的 qpair 容量 ~64，留一半给 value 侧 / 同核其他 op。
- 对 10 亿 KV / 16K page，本数量级足够把单核单设备的写吞吐打满（NVMe 4K/16K 顺序写 ~1.5GB/s，32 in-flight × 16K ≈ 500KB pending，远小于 qpair 深度）。
- 没必要按 `worker_count` 推：worker 数和 NVMe 写并发数没有必然关系。

### 5.3 一次 device flush

```text
>> flat_map([](bool ok) {
    return mock_nvme::scheduler::flush(local_nvme());
})
```

所有 writes ack 后才发 FLUSH（reduce/all 的语义保证）。Flush 的 cb(true) 之后才允许进入 `finish_flush_round`。

### 5.4 写失败处理

- 单个 write 失败 → `mock_nvme::write_one` 返 `false` → `>> all()` 聚合失败 → 走 PUMP 异常路径。
- Phase 9 把整轮 flush 当事务：任何 write 失败 → 整轮 abort，`finish_flush_round` 拿到的 `result.st = unsupported_unimplemented`（暂用此值；FF §8.3 事务级失败语义；本 step 不引入新 status，因为 frontier_switch 的"flush_failed" enum value 由 frontier_switch step 引入）。
- 已 bump 出来的新 ranges 通过 `tree_allocator.push_back_bump()` 反向归还（§3.3）；`combined_root` 自然析构。
- **不**做"先 flush 局部再回滚"：未 flush 的写就算到了 NVMe 也不会被任何后续操作引用（new_manifest 根本没装），CoW 安全（FF §8.3 事务级）。

> Constraint A 显式：本 step 不对 write 失败做 retry / partial commit；任何一笔 NVMe write 失败 = 整轮 unsupported_unimplemented，外层 coord 决定是否重试。重试机制本身归 coord_sched / 上层调度，不在本 step。

---

## 6. New manifest 构造

> **核心红线**：禁止 O(tree) 全扫描；只走本轮 changed subtree + affected leaves。10 亿 KV 下 3.86M leaves（16K page）/ 15.6M leaves（4K page）是基线参照（INDEX.md §"容量快速校准"）。

新 manifest 是 immutable snapshot；本步构造完后通过 `make_shared<const tree_manifest>(...)` 装好，挂到 `tree_flush_result.new_manifest`。

### 6.1 数据来源

阶段 2 出来后已有：

1. `combined_root`（含每个新节点的 `new_paddr` / `new_range_base` / `new_slot_index`）
2. `round.retired.old_slots` / `old_ranges`
3. `base_manifest`（pinned）

构造分四步：

### 6.2 `slot_map` 增量更新

```text
new_slot_map = base_manifest.slot_map         // shallow copy（flat_hash_map）

walk combined_root post-order:
    for each n in walk:
        if n.replaces_old_paddrs.size() == 1:
            P = n.replaces_old_paddrs[0]
            if n.new_range_base == P:
                new_slot_map[P] = n.new_slot_index   // shadow slot bumped
            else:
                // consolidation: P 退役，新 range 进
                new_slot_map.erase(P)
                new_slot_map[n.new_range_base] = 0
        elif n.replaces_old_paddrs.size() >= 2:
            // pairwise leaf merge：carrier 走 case 1 同样路径
            P_carrier = n.replaces_old_paddrs[0]
            if n.new_range_base == P_carrier:
                new_slot_map[P_carrier] = n.new_slot_index
            else:
                new_slot_map.erase(P_carrier)
                new_slot_map[n.new_range_base] = 0
            for j in 1 ..:
                new_slot_map.erase(n.replaces_old_paddrs[j])
        else:  // 0 → 纯新 page
            new_slot_map[n.new_range_base] = 0
```

复杂度 = O(本轮 changed nodes)。未触及的 leaf / internal 直接通过 `flat_hash_map` 的浅拷贝继承。

> `flat_hash_map` 浅拷贝是 O(N)（N = 整 map size），10 亿 KV × 16K page 大约 ~3.86M leaves + 11K internals，hashmap 大约 ~16MB；浅拷贝代价单次 ~10ms（5GB/s memcpy 量级）。如果以后要进一步压成增量，可换 immutable hashmap (HAMT)；v1 不做（保留为 follow-up）。

### 6.3 `leaf_order` 增量重建

```cpp
// 现状：leaf_order_index 是 immutable 的 (fence_pool: string + spans:
// vector<leaf_span>)；rebuild 必须新建一份。
struct leaf_order_index {
    std::string                  fence_pool;
    std::vector<leaf_span>       spans;
};
```

策略：**neighbor-based fence + changed-window 重建**。

#### 6.3.1 Fence contract（拍板）

`leaf_order_index` 的 fence 语义收紧为 **neighbor-based exclusive upper**：

```text
对任意两个在 spans 里相邻的 leaf span[i], span[i+1]：
    spans[i].fence_upper == spans[i+1].fence_lower

spans[0].fence_lower = "" (表示 -∞ — 与 core/leaf_order.hh:154 fence convention D2 一致)
spans[last].fence_upper = "" (表示 +∞ — 同上)

中间 span 的 fence_lower / fence_upper 都**不得**为空串——空串只在"首/尾哨位"才有 ±∞ 语义，出现在中间位置属 corruption（rebuild 时 panic_inconsistency）。
```

这条 contract 有两个直接推论：

1. **`last_key + 1` 的 "lexicographic successor" 构造方案作废**——该方案对 0xFF 等极端字节串不总可定义。
2. **空 leaf 不得进 `leaf_order`**——空 leaf 没有可用的 `first_record.key` 作为 `fence_lower`，也不能借 `("", "")` 当中间条目（那会让 `find_leaf_for_key` 认为该 span 覆盖全空间）。worker 侧允许产空 leaf（`candidate_build.hh:469-478`），但 Phase 9 owner 在进入 `leaf_order` rebuild **之前**必须消除空 leaf，详见 §6.3.2。

#### 6.3.2 空 leaf 的 owner 侧消除

合并阶段 §2 完成后、Plan §3 之前，加一步 **prune-empty-leaves** pass：

```text
prune_empty_leaves(combined_root):
    walk combined_root post-order:
        if n.type == leaf AND leaf_page_reader(n.content).record_count() == 0:
            // 物理删除：把父 internal 里对 n 的 child_ref 整个摘掉
            //   - 前面的 separator 也要同步删（N-1 separators → N-2）
            //   - n.replaces_old_paddrs 里的每个 old leaf range_base
            //     直接进 round.retired.old_ranges（整个 shadow range 退役）
            //
            // 如果父 internal 摘掉后只剩一个 child，就递归压扁父层（父变成
            // 只有 rightmost child 的 "单 child internal"，该子直接上提为父层
            // 的替代节点，父层也被摘除）。
            //
            // 不变量：本 pass 保证 combined_root 出来后，任何 leaf 节点都
            //         有 record_count >= 1，也即 fence_lower 一定可取到。
            prune_child(n.parent, n, propagate)
```

要点：

1. **空 leaf 的触发场景**：(a) old leaf 所有 record 都是 tombstone 且 `data_ver <= recovery_safe_lsn`（worker merge 里全 drop）；(b) 极端情况下 pairwise merge 触发但两张原页都空（实际不会——worker 只在 merged 非空的情形 push mem_tree_node）。主要是 (a)。
2. **连带 old range 全部退役**：空 leaf 语义 = "本 key range 已无任何 live record"，其所有 old paddrs 都进 `retired.old_ranges`。
3. **不依赖 `leaf_page_reader` 以外的元数据**：record_count 从 page bytes 读，不需要额外标记。
4. **父压扁级联**：如果一个 internal 的所有 children 都是空 leaf，整个 internal 也消失。级联时父 internal 自己的 `replaces_old_paddrs` 也要进 `retired.old_ranges`——父本身的 old range 不再被任何新节点承载。
5. **整树塌陷规则（拍板决议，收敛原 §13 缺口）**：级联处理完之后，如果 `combined_root == nullptr`（整棵 flush 子树塌到 0 节点，即本轮 flush 覆盖的 key 空间全部被 tombstone 清空），**owner 侧合成一张空 root leaf** 作为新 root，而不是产出 `root_slot = {0,0}` 的空树态。具体规则：

   ```text
   if combined_root == nullptr after cascade:
       root_leaf = make_unique<merged_node>()
       root_leaf->type                 = leaf
       root_leaf->content              = build empty leaf page bytes (
                                           tree_slot_header with
                                           record_count=0, free_space_offset=
                                           sizeof(tree_slot_header),
                                           page_crc to be filled in §4.4)
       root_leaf->replaces_old_paddrs  = {}   // 纯新页——§4.2 case 0 路径
       root_leaf->children             = {}
       root_leaf->separators           = {}
       combined_root                   = std::move(root_leaf)
   ```

   配套行为全部走**已有路径**，不引入新分支：

   - **§3 Plan**：root_leaf 走 `|replaces_old_paddrs| == 0` 分支 → `allocator.allocate()` 分一个新 range，`new_paddr = new_range.base`（slot 0）。allocator bump 一次，消耗一个 shadow range（~16-64KB，10 亿 KV 尺度可忽略）。
   - **§5 Writer**：root_leaf 走正常 `write_one` 路径，一条 NVMe write + 一次 nvme_flush。
   - **§6.3 leaf_order**：`collect_leaf_diff` 把 root_leaf 当成普通新 leaf，`compute_neighbor_fence` 把它作为合并树 in-order 的唯一 leaf，`fence_lower = ""` / `fence_upper = ""`（首尾哨位规则，§6.3.1 单条 span 两端空 fence 合法）。`diff.removed` 含 base 里所有被塌陷覆盖的旧 leaves；Step B 的 window 退化为整条 `base_lo.spans`，整段替换成单条 `leaf_span`。
   - **§6.3.4 reverse_topology**：`internal_nodes = []`（没有 internal），`leaf_parent_idx = [kInvalidInternalIdx]`（`core/tree_reverse_topology.hh:58` 注释已声明此值代表 "leaf is root"）。
   - **§6.4 root_slot / root_range_base**：按普通流程 `root_slot = root_leaf->new_paddr` / `root_range_base = root_leaf->new_range_base`；**不**走 `{0,0}` 路径。
   - **§2.6 is_root_change**：此次必然是 root-change（`root_range_base` 变了），走 §7 异步 `update_superblock` 路径。

6. **本规则只覆盖"flush 产物"；boot / format 初态不在本 step 范围**：Phase 9 承诺的是 `tree_flush_result.new_manifest` 永远满足 `has_root() == true`。系统首次 boot 后、尚未执行过任何 flush 时的初始 manifest 如何构造（是 `root_slot = {0,0}` 还是预先写一张空 root leaf）归 format / boot-recovery step 决定——那条路径若仍产出 `root_slot = {0,0}`，首轮 flush 的 `build_key_partitions` 会返 `unsupported_shape_change`。该场景不是本 step 的设计缺口，由 format / boot step 配套解决。本 step 不引入针对"初始 flush 从 {0,0} 空树 bootstrap 建第一张 leaf"的路径。

---

```text
build_new_leaf_order(base_lo, combined_root) -> leaf_order_index:
    // Step A: 收集 diff —— combined_root 只含 non-empty leaves（§6.3.2 已清）
    diff = collect_leaf_diff(combined_root)
    if diff.empty():
        return base_lo                       // 共享同一份（shared_ptr）

    // Step B: 找 changed window [w_start, w_end) in base_lo.spans
    //
    // leaf_order 是 sorted by key range 的；diff.removed / diff.added 的 key
    // 边界决定了 "受本轮影响的连续区间"：
    //
    //   w_key_lower = min(
    //       min over diff.removed (span.fence_lower),
    //       min over diff.added   (new fence_lower)
    //   )
    //   w_key_upper = max(
    //       max over diff.removed (span.fence_upper),
    //       max over diff.added   (new fence_upper)
    //   )
    //
    // 然后在 base_lo.spans 上 binary search 找 [w_start, w_end) =
    //   { i : spans[i] 与 [w_key_lower, w_key_upper) 有交集 }。
    //
    // window 之外的 base span 原样继承（只做 fence_pool 指针 repoint）。
    // window 之内的 base span 整段丢弃，用 diff.added 的新 span 填回。

    // Step C: 构造新 spans
    //   [0 .. w_start)       ← 从 base 复制（fence 字节重定位到 new_fence_pool）
    //   [w_start .. w_end)   ← 完全用 diff.added（已按 fence_lower 排序）替换
    //   [w_end .. base_len)  ← 从 base 复制（同上）
    //
    // 边界不变量验证（失败 → panic_inconsistency）：
    //   1. 拼接后 spans[i].fence_upper == spans[i+1].fence_lower （内部）
    //   2. spans[0].fence_lower == "" AND spans[last].fence_upper == "" 仅当
    //      base_lo 就是全树头尾；window 位于中间时，
    //      spans[w_start-1].fence_upper == spans[w_start].fence_lower
    //      和 spans[w_end-1].fence_upper == spans[w_end].fence_lower
    //      都必须满足。
    //   这条不变量由 Step B 的 window 定义 + diff.added 的 fence 构造共同保证
    //   （见 §6.3.3 算法）。

    // Step D: 构造 new_fence_pool
    //   内存上一次性算总长度再 reserve，避免 reallocation：
    //       sum(span.fence_lower_len + span.fence_upper_len for span in new_spans)
    //   （相邻 span 共享 fence 字节可在 Step D 里做 dedupe，和当前
    //    leaf_order_index 的 "相邻 span 共享 fence 存储" 语义一致；
    //    v1 按"不 dedupe，简单 append"先做，后续优化走。）

    return leaf_order_index{ new_fence_pool, new_spans }
```

#### 6.3.3 Diff 收集与新 leaf 的 fence 构造

```text
collect_leaf_diff(combined_root) -> LeafDiff{removed, added}:
    diff = LeafDiff{}

    // 先 walk 出所有 new leaves 按 "合并树里的 key-order" 排序
    // （等价于合并树 in-order walk 的结果）
    new_leaves_in_order = []
    walk_inorder(combined_root):
        on internal n:
            递归 walk children[0], separators[0], children[1], ..., children[last]
            （paddr child 跳过：不在 combined_root 范围内）
        on leaf n:
            new_leaves_in_order.append(n)

    // 旧 leaves 的失效集合
    for n in new_leaves_in_order:
        for old_p in n.replaces_old_paddrs:
            diff.removed.insert(old_p)

    // 新 leaf 的 fence 构造：neighbor-based
    //
    // 对每张新 leaf n_k：
    //   - fence_lower:
    //       情况 1: n_k 是本轮合并树 in-order 的第一张 leaf，并且
    //               它在新全树里也位于最左（即 replaces 的最小 old leaf
    //               在 base_lo.spans[0]），则 fence_lower = "" (-∞)。
    //       情况 2: n_k 的合并树左邻是另一张新 leaf n_{k-1}，则
    //               fence_lower = leaf_page_reader(n_k.content).get(0).key
    //               （本 leaf 的 first record key）。
    //               同时校验：n_{k-1}.fence_upper == n_k.fence_lower。
    //       情况 3: n_k 左侧是未改 base leaf（在 combined_root 里以 paddr
    //               child_ref 出现），则 fence_lower = 本 leaf 的 first
    //               record key，并由 Step B 的 window 校验保证接得上
    //               base_lo 对应位置的 fence。
    //
    //   - fence_upper 用对称规则：
    //       情况 1: n_k 是合并树最右叶且 replaces 的最大 old leaf 在
    //               base_lo.spans[last]，则 fence_upper = "" (+∞)。
    //       情况 2: 合并树右邻是新 leaf n_{k+1}，则
    //               fence_upper = leaf_page_reader(n_{k+1}.content).get(0).key
    //               (= n_{k+1}.fence_lower)
    //       情况 3: 右侧是未改 base leaf → fence_upper = 那张 base leaf 的
    //               fence_lower（Step B window 右边界已知）。
    //
    // 结果：每张新 leaf 的 fence 都由 "实际存在的 record key" 或 "±∞ 哨位"
    //       或 "邻居的 fence" 决定，完全不依赖 last_key + 1。

    for n in new_leaves_in_order:
        (lo, up) = compute_neighbor_fence(n, combined_root, base_lo)
        diff.added.push({n.new_range_base, lo, up})

    return diff
```

**复杂度**：`walk_inorder` = O(combined_root size)；每张新 leaf 的 fence 构造 = O(1)（邻居即可得，无需二分 base_lo，window 边界在 Step B 算），合计仍是 O(combined_root size)。

> `leaf_order` 的 fence 字段在 `leaf_span` 里是 `uint16_t` 长度上限 64KB，`uint32_t` 偏移上限 4GB（`core/leaf_order.hh:48-56`）。上限远高于实际 key 长度。

#### 6.3.4 `tree_reverse_topology` 增量重建

`reverse_topology` 必须和 `leaf_order` / `combined_root` 保持一致，并且**不允许 O(tree) 全扫**。

`tree_reverse_topology` 当前形态（`core/tree_reverse_topology.hh:54-65`）：

```cpp
struct tree_reverse_topology {
    std::vector<internal_idx>        leaf_parent_idx;       // 平行 leaf_order.spans
    std::vector<internal_node_entry> internal_nodes;        // packed{range_base, parent_idx}
};
```

`internal_idx` **是 `internal_nodes` 里的 dense vector 下标**——这一点决定下面算法的结构：idx 必须整体重排，不存在"保留 base 原 idx 同时跳过被删条目"的兼容路径（那样会让下标整体平移，既存的 `parent_idx` 引用就全部失效）。

策略分三层：

1. **Split 映射必须按 "new parent of each leaf" 决定，不是按 "old parent range_base → new parent idx"**（收敛 review P0-1 finding）。一个 old internal 被 split 成多张时，旧 parent 下的未改 leaf 会分流到多张新 parent，单值映射不成立；正确办法是**从 new tree 的结构反向决定每张 leaf 的 parent**。
2. **Idx 稳定性**：承认 new_internal_nodes 是 **全量重建**——本 step 不保留任何 base 原 idx。v1 不实装 "tombstone / overwrite 以保持下标" 方案（这条复杂度不值得；base internals 总量 ~10.8K @ 16K page / ~247K @ 4K page，全量重建的 memcpy 成本远低于一次 flush 的 NVMe 写总成本）。
3. **`leaf_parent_idx` 总长度** = `new_lo.spans.size()`。这不是"O(tree) 全扫"的意思——它是**按 new_lo 线性填一次**。因为 leaf 总数本身就是我们要输出的向量长度，O(L) 是下界。合理的目标是让**每张 leaf 填 parent_idx 的单次代价 = O(1) 摊销**，而不是 O(log tree) 或 O(changed set) per-leaf。

---

**算法**：

```text
build_new_reverse_topology(base_topo, base_lo, new_lo, combined_root)
        -> tree_reverse_topology:

    // ── Step A: 全量分配 new_internal_idx
    //
    // 所有 new internal 都要一个新 idx。分两类来源：
    //   A.1 combined_root 里的每个 type==internal merged_node
    //       → 新 idx = appended index in new_internals
    //   A.2 base_topo.internal_nodes 里的 internals 中 range_base
    //       既没出现在 combined_root 也没被 retire 掉的
    //       → 同样 append 到 new_internals（分配新 idx）
    //
    // 两类都不复用 base 原 idx。
    //
    // 构造过程中同时建两个 hashmap（都是 O(|new internals|) 条目）：
    //   new_idx_by_range_base: paddr -> internal_idx
    //     给定一个 new range_base，拿到它在 new_internals 里的 idx。
    //   parent_range_base_for_new: paddr(self range) -> paddr(parent range)
    //     给定任意 new internal / 新 leaf 的 range_base，拿到它父的
    //     new range_base。root 用 sentinel paddr{0,0}。

    // ── Step B: 建 parent_range_base_for_new
    //
    // B.1 combined_root 走 pre-order，带着 parent stack：
    //       每次 visit 一个 internal 节点 N：
    //         parent_range_base_for_new[N.new_range_base] = parent.new_range_base
    //         (root case: parent.new_range_base = {0,0})
    //       然后 for each child of N:
    //         if child.target is unique_ptr<merged_node>:
    //           parent_range_base_for_new[child.new_range_base] = N.new_range_base
    //           递归（push N 到 stack）
    //         if child.target is paddr:
    //           // paddr 子是 base_manifest 里未改 subtree；它的 "新 parent"
    //           // 就是当前 N。paddr 指向的可能是 leaf 或 internal：
    //           //   - 如果是 leaf：登记 leaf_parent_by_range_base[paddr] = N.new_range_base
    //           //   - 如果是 internal：登记 unchanged_subtree_new_parent[paddr] = N
    //           //     （见 B.2）
    //           if is_leaf_range(child_paddr, base_lo):
    //               leaf_parent_by_range_base[child_paddr] = N.new_range_base
    //           else:
    //               unchanged_subtree_new_parent[child_paddr] = N.new_range_base
    //
    // B.2 对于 B.1 登记在 unchanged_subtree_new_parent 里的每个 old
    //     internal subtree 根 U：
    //       - U 以下所有 internals 在 new_internals 里都要有 entry（从
    //         base_topo.internal_nodes 复制进来，但分配新 idx）；
    //       - U 本身的父在 new tree 里是 unchanged_subtree_new_parent[U]；
    //       - U 子树内部的 parent 链在 base_topo 里已经是正确的 —— 但
    //         idx 需要映射到新分配的 new idx（通过 new_idx_by_range_base
    //         查询，range_base 不变，仅 idx 重编号）。
    //
    //   这里的 "U 子树" 枚举：从 base_topo 里做一次从 U 向下的 DFS，
    //   枚举所有 parent_idx 链回到 U（或 U 的后代）的 internals。
    //
    //   复杂度：= O(|U 子树的 internals 节点数|)，总和对本轮所有 U 求和
    //   = O(|base internals 里未被 combined_root 改写的那部分|)。
    //   最坏情况 = O(|base_internals|)，但这是 "全量重编号 idx" 的必经
    //   代价，不是 "O(tree) 全扫"。见 §6.3.5 复杂度说明。
    //
    //   对每条枚举到的 old internal I：
    //     new_internals.append(internal_node_entry{
    //       range_base = I.range_base,
    //       parent_idx = (I == U) ? new_idx_by_range_base[
    //                                   unchanged_subtree_new_parent[U]]
    //                              : new_idx_by_range_base[
    //                                   base_topo.internal_nodes[I.parent_idx].range_base]
    //     })
    //     new_idx_by_range_base[I.range_base] = size(new_internals) - 1

    // ── Step C: 填 combined_root 里 internals 的 parent_idx
    //
    // A.1 阶段 append new_internals 时 parent_idx 还是 placeholder；
    // 现在所有 new internal 的 idx 都分配好了，回填：
    //
    //   for each n in combined_root internals:
    //       parent_rb = parent_range_base_for_new[n.new_range_base]
    //       if parent_rb == {0,0}:
    //           n_entry.parent_idx = kInvalidInternalIdx
    //       else:
    //           n_entry.parent_idx = new_idx_by_range_base[parent_rb]

    // ── Step D: leaf_parent_idx —— 按 new_lo.spans 顺序填
    //
    //   总长 = new_lo.spans.size()，是输出不变量。
    //
    //   leaf_parent_idx.resize(new_lo.spans.size())
    //   for i in 0 .. new_lo.spans.size()-1:
    //       rb = new_lo.spans[i].leaf_range_base
    //       if leaf_parent_by_range_base.contains(rb):
    //           // 包括两种来源：
    //           //   (a) 新 leaf（combined_root 里的 leaf 节点）
    //           //   (b) 被 combined_root 里某 internal 以 paddr child_ref
    //           //       直接收编的未改 leaf（Step B.1 已登记）
    //           leaf_parent_idx[i] = new_idx_by_range_base[
    //               leaf_parent_by_range_base[rb]]
    //       else:
    //           // 该 leaf 的父在 base_topo 里未改，且父 internal 位于
    //           // "B.2 unchanged_subtree" 内，已经以新 idx 重登进
    //           // new_internals。查 base_topo 的原 parent range_base，
    //           // 再映射到 new idx。
    //           base_i = base_lo.find_index_by_range_base(rb)  // O(log L)
    //           old_parent_idx = base_topo.leaf_parent_idx[base_i]
    //           old_parent_rb  = base_topo.internal_nodes[old_parent_idx].range_base
    //           // 此 rb 必须已在 new_idx_by_range_base 里
    //           // （要么 combined_root 里，要么 B.2 枚举过）
    //           leaf_parent_idx[i] = new_idx_by_range_base[old_parent_rb]

    return tree_reverse_topology{ leaf_parent_idx, new_internals }
```

**关键正确性点（对齐 review P0-1）**：

1. **Split 场景**：old internal `I_old` 被 combined_root 重建成 `I_a`, `I_b`, …（split siblings，都在 `combined_root.internals` 里）。`I_old` 下的未改 leaves 出现在 `I_a` / `I_b` 等的 `children` 列表里（以 paddr child_ref 形态，因为 worker 在 `build_one_internal` / `lookup_child_group` 里把未改 child 以 paddr 形式带入；`candidate_build.hh:918-928`）。Step B.1 走 combined_root 递归时，为**每个**未改 leaf 登记一次 `leaf_parent_by_range_base[leaf_rb] = I_a/I_b/...`——因此每张未改 leaf 都精确拿到自己的 new parent，不经过单值映射。
2. **未改 subtree 场景**：combined_root 里某 internal `N` 的 child 是 paddr 指向一整棵未改 internal 子树 `U`——Step B.1 登记 `unchanged_subtree_new_parent[U] = N.new_range_base`；Step B.2 从 base_topo 枚举 U 子树里的所有 internals，给每个重新登记新 idx，parent 链沿用 base_topo 原关系（只换 idx）。U 以下的未改 leaves 在 Step D 走 "base_lo 查 base_i → base_topo.leaf_parent_idx → base internal range_base → new idx" 路径。
3. **Idx 稳定性**：全部重编号——Step D 从不直接使用 `base_topo` 里的任何 idx 值，一律转成 range_base 再映射回 new idx。

#### 6.3.5 复杂度声明（收敛 review Obs-1）

本 step 明确承认以下复杂度，并给出为何仍满足 10 亿 KV 约束：

| Step | 复杂度 | 说明 |
|---|---|---|
| §6.2 `slot_map` 浅拷贝 | O(\|base slot_map\|) ~ O(tree internals + tree leaves) | flat_hash_map 整份浅复制；10 亿 KV 16K page ~3.87M 条目 ~16MB，一次 memcpy-量级（几毫秒） |
| §6.3 `leaf_order` | O(combined_root size) + window slicing O(\|new_lo.spans\|) | window 外直接继承 spans + fence 字节指针重定位；window 内长度 = 本轮 changed leaves |
| §6.3.4 `reverse_topology` Step B.2 | O(\|unchanged internal subtrees touched by combined_root\|) | 上界 = \|base internals\|，但实际通常是 combined_root 触达的 subtree 根对应的局部 |
| §6.3.4 `reverse_topology` Step D | O(\|new_lo.spans\|) **线性扫 + 每条 O(1) 摊销** | leaf 总数本身就是输出向量长度（下界 O(L)），每条 parent 查找 = 一次 hashmap lookup + 可能一次 base_lo 二分查找 |

**是否违反 §1.1 "禁止 O(tree) 全扫"？**

不违反。本文"禁止 O(tree) 全扫"的目标语境在 §1.1 / §6 开头：**禁止那些随 tree 总节点数线性变化、但不随本轮 changed set 变化的扫描**（例子：对 new manifest 每条 leaf 都去 combined_root 里二分搜 parent = O(L · log |combined_root|)；那会被 10 亿 KV 放大到跑几百毫秒 / 轮）。

以下三条是可接受的 O(L) 工作量：

1. **leaf_parent_idx 输出是 size = L 的向量**，输出本身 O(L)；
2. **slot_map 浅拷贝** = 容器整份复制，memcpy 带宽足；
3. **reverse_topology Step B.2** 的最坏上界 = \|base internals\| ~ 10K-250K（远小于 leaves），每条 O(1)。

`feedback_perf_over_simplicity` 的取舍点：如果未来 benchmark 发现 manifest rebuild 占比超过 flush 总时长的 5%，才考虑做 persistent / sliced rebuild；v1 先把"够快、算法可证正确"的版本落下，不为性能提前引入 HAMT / rope 结构增加 carrier 复杂度。

10 亿 KV 下的量级参照（16K page / 32B key）：

- `slot_map` 浅拷贝：~16MB × 5GB/s memcpy 带宽 = ~3ms
- `leaf_parent_idx` 填充：3.87M × 一次 hashmap probe ≈ 50-80ms（一次 flush round 的 NVMe write 本身就 >1s，CPU 侧这点不是瓶颈）
- `internal_nodes` 重建：~10.8K × 一次 hashmap probe = ~几百微秒

如果进一步 audit 发现这几步在热路径上不够快，Phase 9 实装时可以做的独立优化路径（不影响设计正确性）：

- `slot_map` 改 immutable HAMT / persistent map
- `leaf_parent_idx` 改 "sliced rebuild"：只复写 window [w_start, w_end)

但这两条都属于 post-v1 ——Phase 9 的设计不依赖它们。

### 6.4 `root_slot` / `root_range_base` 更新

```text
new_root_node = combined_root           // §2.5 finalize_root 已确定
new_manifest.root_range_base = new_root_node->new_range_base
new_manifest.root_slot       = new_root_node->new_paddr
```

`has_root()` 依然按 `root_slot.lba != 0` 判定（`core/tree_manifest.hh:60`）。

### 6.5 装配

```cpp
auto new_manifest = std::make_shared<const tree_manifest>(tree_manifest{
    .root_slot         = new_root_paddr,
    .slot_map          = std::move(new_slot_map),
    .geom              = base_manifest->geom,             // 共享
    .leaf_order        = std::move(new_lo),
    .root_range_base   = new_root_range_base,
    .reverse_topology  = std::move(new_topo),
});
```

immutable contract：装好之后不再改任何字段；`shared_ptr<const>` 让多 reader pin 不撞改。

---

## 7. Root-change 路径

### 7.1 触发条件

`is_root_change`（§2.6）为 true。

### 7.2 处理流程

```
finish_flush_round 阶段（§9）：
    if is_root_change:
        // 1. 异步发 update_superblock，但不阻塞 result 回传
        tree_sched->schedule_update_superblock({
            .new_root_base_paddr = new_manifest->root_range_base,
            .covered_lsn         = round.flushed_max_lsn,
        });
        // 推 superblock_safe_lsn 推迟到 update_superblock cb 完成
    else:
        // root-stable：nvme_flush 已完成 → 直接推
        tree_state.superblock_safe_lsn =
            max(tree_state.superblock_safe_lsn, round.flushed_max_lsn);

    // recovery_safe_lsn 另算（RSM §4.9）：
    tree_state.recovery_safe_lsn = recompute_recovery_safe_lsn(tree_state);
    // 本步内 recovery_safe_lsn 可能推也可能不推（取决于 wal_frontier），
    // 不影响本轮 result 输出。
```

`schedule_update_superblock` 投到 tree_sched 自身的请求队列（异步，不阻塞 finish_flush_round 的 cb）。

### 7.3 `update_superblock` async handle

新增一个 tree_sched 上的请求类型：

```cpp
namespace _update_superblock {
    struct req {
        format::paddr                                   new_root_base_paddr;
        uint64_t                                        covered_lsn;
        std::move_only_function<void(bool)>             cb;
    };
    // op / sender / op_pusher 模板照 _flush_fold / _flush_merge 的形态做
    // （无需 leader-follower / variant；返回 bool ok）
}
```

handle 逻辑：

```text
handle_update_superblock(req):
    // 1. 读 current on-disk superblock (active slot)
    //    Phase 9 内 read 路径走本核 mock_nvme::read_one(lba=active_slot_lba, ...)
    //    然后用 format::inspect_superblock 验证；fail → panic（盘损坏）
    // 2. 在内存里构造新 superblock（继承所有字段，
    //    替换 root_base_paddr / generation = old.gen + 1，重算 CRC）
    // 3. 选择 inactive slot：
    //    inactive = (active_slot == A) ? B : A
    // 4. 发 mock_nvme::write_one(lba=inactive_slot_lba, ..., flags=FUA)
    //    （superblock 的 single-LBA write 需要 FUA：必须 durable
    //     才推 superblock_safe_lsn）
    // 5. cb(true) → callback 内：
    //    tree_state.superblock_safe_lsn =
    //        max(tree_state.superblock_safe_lsn, req.covered_lsn);
    //    tree_state.recovery_safe_lsn   = recompute(...)
```

#### 7.3.1 active vs inactive slot 怎么知道

- v1 boot recovery 装 `tree_state.active_superblock_slot ∈ {A, B}`（在 RSM §4.1 之外新增的 owner state；本 step 增加这个字段 + 初始化时由 builder 注入；recovery 完整闭环不在本 step）。
- 本 step 在 `tree_state` 里加：

  ```cpp
  enum class superblock_slot : uint8_t { A, B };
  superblock_slot active_superblock_slot = superblock_slot::A;
  ```

  - builder 装 runtime 时按 boot 决定（boot recovery 还没落，先按 A 默认）。
  - `update_superblock` 完成后 `active_superblock_slot = inactive`。

> **构造 A 显式声明**：`active_superblock_slot` 默认 A 是因为 boot recovery 还没完整接通；本 step 不依赖该字段的 boot 正确性，仅在 `update_superblock` 路径上读写。boot recovery 落地时会接管初始化。Phase 9 不保留 silent fallback——`update_superblock` 调用方必须保证此字段已被 builder 正确赋值，否则 `panic_inconsistency`。

### 7.4 superblock_safe_lsn 推进规则

按 RSM §4.9：

| 场景 | superblock_safe_lsn 推进时机 |
|---|---|
| root-stable | `nvme_flush` 完成（即 §5.3 完成）→ 在 `finish_flush_round` 同步推 |
| root-change | `update_superblock` async handle 的 cb 触发时推 |

**禁止**：root-change 情况下在 `finish_flush_round` 里同步推 superblock_safe_lsn——这会让 recovery 假定一个尚未在 disk superblock 上反映的状态。

`recovery_safe_lsn` 是三者 min（RSM §4.9）：

```text
recovery_safe_lsn = min(flush_max_lsn, superblock_safe_lsn, wal_frontier)
```

`wal_frontier` 由 `wal_space_sched` 维护（不是本 step），在 `recompute_recovery_safe_lsn` 里通过 `core::registry::wal_space()`（v1 还没装；本 step 写一个 stub function 返回 `UINT64_MAX` —— 等 wal 模块落地时取真值）。

> **构造 A 显式声明**：`recompute_recovery_safe_lsn` stub 返回 `min(flush_max_lsn, superblock_safe_lsn)`，**禁止**默认推到 `flush_max_lsn`——这会让 wal 在没落地前就被错误地认为可回收。

---

## 8. `finish_flush_round`

### 8.1 入口

`tree_sched::advance()` 在 `merge_q` 处理流程的尾部，把所有合并/写盘/manifest 构造做完后，进入 `finish_flush_round(round_state, is_root_change, new_manifest)`。

### 8.2 流程

```text
finish_flush_round(round_state, is_root_change, new_manifest, write_ok):
    // write_ok = §5 整轮 nvme writes + flush 是否成功

    if !write_ok:
        // 阶段失败（§5.4）
        result = tree_flush_result {
            .st              = unsupported_unimplemented,  // 见 §5.4 注释
            .new_manifest    = nullptr,
            .retired         = {},
            .flushed_gens_by_front = {},
            .flushed_max_lsn = round.flushed_max_lsn,
        }
        rollback_allocations(round)            // §3.3 push_back_bump 反向归还
        unpark_round(round.round_id)
        return result

    // success path
    if is_root_change:
        schedule_update_superblock_async(new_manifest->root_range_base,
                                          round.flushed_max_lsn)
    else:
        tree_state.superblock_safe_lsn =
            max(tree_state.superblock_safe_lsn, round.flushed_max_lsn)

    tree_state.flush_max_lsn =
        max(tree_state.flush_max_lsn, round.flushed_max_lsn)
    tree_state.recovery_safe_lsn = recompute_recovery_safe_lsn(tree_state)

    auto gens_by_front = build_flushed_gens_by_front(round.pinned_gens)
                          // 已存在：tree/owner_scheduler.hh:86
    auto retired       = std::move(round.retired)
    auto max_lsn       = round.flushed_max_lsn
    unpark_round(round.round_id)               // erase from active_rounds

    return tree_flush_result {
        .st                    = ok,
        .new_manifest          = new_manifest,
        .retired               = std::move(retired),
        .flushed_gens_by_front = std::move(gens_by_front),
        .flushed_max_lsn       = max_lsn,
    };
```

### 8.3 不变量

- result 出去之后，`round_state` 已从 `tree_state.active_rounds` 移除并析构。
- `result.retired` 完整（slots / ranges / values 都填）；frontier_switch step 后续把它 append 到 G_K.retired。
- `flushed_gens_by_front` 满足"按 `front_owner_index` 分组" + 不丢任何 gen（`build_flushed_gens_by_front` 已 panic 校验 `front_owner_index != UINT32_MAX`）。
- `flushed_max_lsn` 始终 = `round.flushed_max_lsn`（Gap 2 单一权威值），即使 success / failure 都返回。

---

## 9. `checkpoint_guard.retired` 字段扩展

### 9.1 改动

```cpp
// core/checkpoint_guard.hh
#include "./retired_objects.hh"

namespace apps::inconel::core {
    struct checkpoint_guard {
        std::shared_ptr<const tree_manifest> manifest;
        retired_objects                      retired;       // ← 新增
        // destructor 不变（Phase 9 不投递 reclaim_task — §1.2 说明）
    };
}
```

### 9.2 不实装的部分（构造 A 显式声明）

```cpp
// PHASE 9 NARROWING: ~checkpoint_guard does NOT post a reclaim_task.
// Section §1.2: that destructor side-effect lands in the
// frontier_switch step, together with the matching reclaim_q
// consumer in tree_sched. Adding the destructor here without the
// consumer would let `retired` silently leak when guards drop.
```

### 9.3 字段填充入口

本 step 内：

- 谁填 `retired`：**没有人**。`tree_flush_result.retired` 直接给到 frontier_switch step；后者负责把 `result.retired` `append` 到 `G_K.retired`。
- 默认值：`retired = {}`（empty `retired_objects`）。

> 因此本 step 内 `checkpoint_guard.retired` 在所有现有构造点都默认空：`tree_manifest::empty()` 路径、`tree_flush_request.base_guard` 等。本 step 的合并/写盘/manifest 流程**不**改动 base_guard.retired。

### 9.4 测试影响

不读测试（实现阶段规则）；构造任何新 checkpoint_guard 时按 `{manifest, {}}` 走，编译失败由其他 step 负责扫。

---

## 10. `tree_sched` advance 主循环改造

当前 `tree_sched::advance()`（`tree/owner_scheduler.hh:215-446`）有两个 drain：`fold_q` 和 `merge_q`。本 step 加第三个 `update_superblock_q`，并扩展 `merge_q` 的 handler。

### 10.1 drain 顺序

```text
advance():
    progress |= drain_fold_q(kMaxFoldOpsPerAdvance)
    progress |= drain_merge_q(kMaxMergeOpsPerAdvance)
    progress |= drain_update_superblock_q(kMaxUpdateSuperblockPerAdvance)
    return progress
```

`kMaxUpdateSuperblockPerAdvance = 4`：superblock 更新每轮最多 4 次，远超实际需要（每轮 flush 最多 1 次 update_superblock）。

### 10.2 `drain_merge_q` 改造

当前 handler（`tree/owner_scheduler.hh:359-436`）的 status determination 段（lines 414-421）现在分支：

```text
flush_stage_status result_st;
if round.st != ok:                       // fold-side error 透传
    result_st = round.st
elif round.workset.empty():              // case 2: 空 round
    result_st = ok                       // 直接返回，无需合并
else:
    // case 3: 真正合并 — Phase 9 入口
    result_st = run_full_merge_pipeline(round, r->args.worker_proposals)
    // run_full_merge_pipeline 内部完成：
    //   §2 合并 → §3 plan → §4 patch+CRC → §5 writes+flush →
    //   §6 manifest → §7 root-change schedule → §8 finish_flush_round
    //
    // 同步部分（§2-§4 + §6）就在当前 advance 调用栈上跑；
    // 异步部分（§5 NVMe writes + §7 update_superblock 的 schedule）
    // 通过 PUMP sender 走出 advance，等 callback 触发 finish_flush_round
    // 时才回填 result 并 cb()。
```

但 `_flush_merge::op` 的 callback 模式是同步立即 cb(result)；异步 NVMe writes 需要 PUMP sender 编排。这意味着本 step 的实装思路有两条选择：

#### 10.2.1 选项 X：merge handle 同步发 sender，结果透过 cb 异步回

```text
advance() → _flush_merge req →
    跑 §2-§4 同步部分 → 启动 §5 sender pipeline →
    pipeline 完成后回到 tree_sched.advance() 触发 _flush_merge cb
```

需要让 merge handle 内部不直接 cb，而是把 cb 移交到 §5 pipeline 末尾。具体做法：

- `_flush_merge::op` 不再调 `req->cb` 在 advance 内同步触发。
- 把 cb + round_id + (合并产物) 存到一个 `tree_state.pending_writes` 表里。
- §5 用 PUMP sender 从 `submit_tree_page_writes_bounded(...) >> mock_nvme::flush(...)` 编排，在最后一步用 `then` 调一个 `tree_sched->finish_pending_round(round_id, ok)` handle 同步触发存好的 cb。

#### 10.2.2 选项 Y：把 §5 + §6 + §7 + §8 全部从 merge handle 移到外层 sender pipeline（推荐）

正如 `flush_module_guide.md` §2.4 的目标 sender 形态：

```text
tree_local_flush(req) =
    submit_flush_fold(req)
    >> dispatch_partitions_to_workers
    >> to_vector<worker_tree_proposal>
    >> tree_sched->submit_flush_merge(...)        // §2-§4 + §6 内联做
    >> visit on result.st:                        // ok / unsupported_*
        if ok and is_root_change:
            >> mock_nvme::write_batch(write_descs, nvme)
            >> mock_nvme::flush(nvme)
            >> tree_sched->submit_update_superblock(...)
            >> tree_sched->submit_finalize_flush_round(...)
        if ok and root-stable:
            >> mock_nvme::write_batch(...)
            >> mock_nvme::flush(...)
            >> tree_sched->submit_finalize_flush_round(...)
```

**但**这违反 flush_module_guide.md §2.5 / §2.4 的"merge 内部完成 paddr 分配 + writer + flush + finish"；guide 把这整段都放在 `submit_flush_merge` 内。

**最终选择**：用选项 X（在 merge handle 内部异步 dispatch）。理由：

1. flush_module_guide.md §2.4 显式把 writer + flush + finish 都画在 `tree_sched->submit_flush_merge` 一层，分割成多个外层 sender 等于改 guide 的合同。
2. 选项 Y 把 `is_root_change` 分支暴露给 sender；按构造 B "通用命名对应通用语义"，`submit_flush_merge` 应该是一个能完整覆盖 root-stable + root-change 的入口，不应该让外层调用方做分支。
3. 选项 X 的 `tree_state.pending_writes` 表设计与现有 `active_rounds` 同形态，没有架构突兀感。

### 10.3 `pending_writes` 表

```cpp
struct pending_write_state {
    flush_round_id                     round_id;
    std::unique_ptr<merged_node>       combined_root;     // §2 出口
    std::shared_ptr<const tree_manifest> new_manifest;    // §6 出口
    bool                               is_root_change;
    std::vector<format::write_desc>    writes;
    std::move_only_function<void(tree_flush_result&&)> cb;  // 来自 _flush_merge req
};

flat_hash_map<uint64_t, std::unique_ptr<pending_write_state>>
    tree_state.pending_writes;
```

merge handle 同步部分完成后填 `pending_write_state` 并启动 §5 sender；sender 末尾的 callback 调 `tree_sched->finalize_pending_round(round_id, ok)`，后者从 `pending_writes` 取出条目并执行 §8 流程后触发 cb。

### 10.4 `submit_finalize_flush_round` handle

新增第三个 handle / op / sender：

```cpp
namespace _finalize_flush_round {
    struct req {
        uint64_t round_id;
        bool     write_ok;
        std::move_only_function<void(tree_flush_result&&)> cb;
    };
    // op / sender / op_pusher 同模板
}
```

`tree_sched::handle_finalize(req)`：

```text
handle_finalize(req):
    pw = pending_writes.extract(req.round_id)
    if !pw: panic_inconsistency(...)

    result = finish_flush_round(active_rounds[req.round_id],
                                 pw->is_root_change,
                                 pw->new_manifest,
                                 req.write_ok)
    pw->cb(std::move(result))                  // 触发原 _flush_merge cb
    req->cb(std::move(/*duplicate placeholder*/))
```

> 实际只需要一份 cb——`_flush_merge::req::cb` 即终点；`_finalize_flush_round::req::cb` 只是 PUMP 链推进的占位回调（返回 `bool` 或 `monostate`）。具体 callback 链路在实现时按 PUMP "自建 scheduler 模板" 严格走。

---

## 11. 顶层 `tree_local_flush(...)` pipeline

在 `tree/sender.hh` 增加：

**前置改动（收敛 review Obs-2）**：`flush_fold_result` 增加 `recovery_safe_lsn` 字段——值来自 `round_state.recovery_safe_lsn`（`flush_round_state.hh:73`，fold 启动时已经 pin 好了），避免顶层 sender 在 fold 外二次获取 round_state。

```cpp
// tree/flush_types.hh — 扩字段（Phase 9 新增）
struct flush_fold_result {
    flush_round_id                     round_id;
    flush_stage_status                 st;
    std::vector<flush_key_partition>   partitions;
    const core::tree_manifest*         base_manifest;
    uint64_t                           recovery_safe_lsn;   // ← 新增
};
```

对应 `tree_sched::advance()` 的 fold path：在所有产 `flush_fold_result` 的位置填 `rs.recovery_safe_lsn`（包括 empty-gens fast-path、empty-workset fast-path、partition-failed path 和正常 path——全部 4 个出口都要填；空 gens 时 round_state 不存在，可填 0）。

```cpp
inline auto
tree_local_flush(tree_sched* owner, tree_flush_request req)
{
    return owner->submit_flush_fold(std::move(req))
        >> flat_map([owner](flush_fold_result&& fr) {
            // 异常路径：fold 已经决定 unsupported_shape_change → 直接走
            // submit_flush_merge 让它产 result.st = same 透传
            //
            // 空 partitions（empty workset case 2）：partitions 空 →
            // loop(0) 产生空流 → to_vector 拿到空 vector → merge handle
            // 也 OK 接受。
            auto round_id          = fr.round_id;
            auto base_manifest     = fr.base_manifest;    // 借用 round_state pin
            auto recovery_safe_lsn = fr.recovery_safe_lsn;  // ← 来自 fold

            return loop(fr.partitions.size())
                >> concurrent()
                >> flat_map([fr, base_manifest, recovery_safe_lsn](size_t i) {
                    auto& part = fr.partitions[i];
                    auto* worker =
                        core::registry::tree_worker_at(part.read_domain_index);
                    return submit_flush_work(worker, flush_worker_req{
                        .round_id          = fr.round_id,
                        .read_domain_index = part.read_domain_index,
                        .base_manifest     = base_manifest,
                        .recovery_safe_lsn = recovery_safe_lsn,
                        .key_groups        = part.groups,
                    });
                })
                >> to_vector<worker_tree_proposal>()
                >> flat_map([owner, round_id](
                        std::vector<worker_tree_proposal>&& proposals) {
                    return owner->submit_flush_merge(flush_merge_request{
                        .round_id         = round_id,
                        .worker_proposals = std::move(proposals),
                    });
                });
        });
}
```

要点：

- 调用者（外层 flush 调度，本 step 之外）只看到一个 sender，类型 `tree_flush_result`。
- `recovery_safe_lsn` 必须从某处取——`round_state.recovery_safe_lsn`，但 fold 的 cb 当前不带这个值（只带 `partitions` + `base_manifest`）。本 step 改 `flush_fold_result` 加一个 `recovery_safe_lsn` 字段（已 pinned 在 round_state）。或者：让 worker 直接从 `tree_sched` 拿——但跨 scheduler 取值反而绕。
- `merge_q` 的 cb 是触发顶层 sender 的 cb，最终拿到的就是 `tree_flush_result`。

### 11.1 fold 异常路径

如果 `fold_fold_result.st != ok`（partitions empty 因为 unsupported_shape_change 等）：

- `partitions` 是 empty vec → `loop(0)` 产生空流 → `to_vector` 拿到空 vector → `submit_flush_merge` handle 内的 status determination 走 "round.st != ok → 透传" 分支，返回 `result.st = unsupported_shape_change`。

不需要在 sender 里加额外分支——错误状态从 fold round_state 自然带到 merge 出口。

### 11.2 worker 之间 NVMe read

worker 通过 `submit_flush_work`（已实装）在 `concurrent()` 下产生 NVMe reads；read 通过 `core::registry::local_nvme()` 走本核 `mock_nvme::scheduler`。本 step 不改这条路径。

---

## 12. 各子节退出条件 / 验证手段 / checklist

每子节落地后，按下表自检。**不写测试**（实现阶段规则）；验证手段是"production code 自我验证"——例如 panic 路径覆盖、字段存在性、sender 类型匹配 by compiler。

### 12.1 §2 合并算法

| 出口 | 验证 |
|---|---|
| `combined_root: unique_ptr<merged_node>` 非空 | merge handle 内 assertion：`combined_root != nullptr` 当 status==ok |
| `is_root_change` 已计算 | 字段就位，分支按 §2.6 走 |
| `round.retired.old_tree_values` 已填（worker 给的全部汇入） | sum of inputs == sum in round.retired |
| §2.4.E `lookup_old_child` / §2.5 `finalize_root` 不出 silent fallback | 任何无法决议的 case 走 `panic_inconsistency` |

Checklist：

- [ ] §2.4 表里 6 个 case 全部覆盖
- [ ] §2.4.E shared ancestor 算法在跨 worker / split bubble up 场景下产正确 `merged_node`
- [ ] §2.5 全局 root split 重判覆盖 leaf-rooted / internal-rooted 两种根
- [ ] §2.7 retired_old_values 汇总不丢

### 12.2 §3 tree_allocator

| 出口 | 验证 |
|---|---|
| `allocate / push_back_bump / recycle` 三入口签名就位 | 编译通过；recycle 入口存在但本 step 内无调用方 |
| `head` 单调递增；`shared_heads` 共享指针对齐 value 侧 | runtime builder 装配 `data_area_heads` 唯一 |
| 撞底 `panic_inconsistency` | 路径手动审 |
| `recycle` 旁路加注释（§3.4 narrowing） | 注释存在 |

Checklist：

- [ ] `tree_allocator` 字段全列：`head`、`free_ranges`、`shared_heads`、`range_lbas`、`shadow_slots`
- [ ] `data_area_heads` 注入路径走通（builder + registry）
- [ ] `push_back_bump` 反向归还顺序断言

### 12.3 §4 Plan

| 出口 | 验证 |
|---|---|
| 每个 merged_node 有非默认 `new_paddr` / `new_range_base` / `new_slot_index` | walk after assign_paddr：所有节点 `new_paddr.lba != 0` |
| internal 节点 content 里所有 child_base 已填真实 paddr | 在 patch 后用 `internal_page_reader.find_child` 自检（非测试，是 plan 内部 `assert`：从 children list 走一遍） |
| CRC 在 patch 之后才算 | 顺序在 §4.1 严格三段 |
| `round.retired.old_slots` / `old_ranges` 已填 | 与 plan 内 `case 1 / 2 / 3` 对应 |

Checklist：

- [ ] 后序遍历正确（先子再父）
- [ ] case 0 / 1 / 2+ 全覆盖
- [ ] consolidation case 把 old range 整个推进 retired.old_ranges

### 12.4 §5 Writer + flush

| 出口 | 验证 |
|---|---|
| `submit_tree_page_writes_bounded` sender 编译通过 | sender 类型推导 OK |
| 并发 `concurrent(32)` 在 `on(local_nvme)` 之前 | sender 编排顺序 |
| 一次 `flush` 在 `>> all()` 之后 | 阅读 sender 链 |
| 写失败 → 整轮 abort | `>> all()` 失败 → PUMP 异常路径 → §5.4 处理 |

Checklist：

- [ ] write_desc 不带 FUA
- [ ] flush 是单次而非每页一次
- [ ] 失败路径走 `tree_allocator.push_back_bump` 反向归还

### 12.5 §6 New manifest

| 出口 | 验证 |
|---|---|
| `new_manifest` 非空，immutable | `make_shared<const tree_manifest>` |
| `slot_map` 增量 = base + diff | 实装审：浅拷贝 + edit 不全扫 |
| `leaf_order` 增量重建 | 同上 + diff size 边界 |
| `reverse_topology` 重建 | Step C 处理 "leaf 未改但 parent 改写" |
| `root_slot` / `root_range_base` 一致（在 root-stable / root-change 两种情形） | §2.6 判定 + §6.4 装配 |

Checklist：

- [ ] 浅拷贝 + 增量更新，无 O(全 leaf) 扫描在 changed-set 之外
- [ ] `parse_fence_bounds` 处理空 leaf
- [ ] `leaf_order_index.fence_pool` 在新 manifest 上是新池字节，old fence 字节不残留
- [ ] reverse_topology Step C 用反向索引正确解析 parent 改写 case

### 12.6 §7 Root-change

| 出口 | 验证 |
|---|---|
| `is_root_change` true 时 schedule 异步 update_superblock | sender 编排存在 |
| `superblock_safe_lsn` 推进时机：root-stable 在 finish_flush_round；root-change 在 update_superblock cb | 两条路径分别走 |
| `recovery_safe_lsn` recompute 不超过 `superblock_safe_lsn` | 实装审 `recompute_recovery_safe_lsn` 用 `min(...)` |

Checklist：

- [ ] update_superblock handle 落地（req / op / sender / op_pusher）
- [ ] active_superblock_slot 字段存在并按 update_superblock cb 翻转
- [ ] FUA 写 inactive slot
- [ ] 异步：finish_flush_round 不等 update_superblock 完成

### 12.7 §8 finish_flush_round

| 出口 | 验证 |
|---|---|
| `tree_flush_result` 字段全填 | 5 字段都非默认 |
| `unpark_round` 把 round_state 从 `active_rounds` 清掉 | walk after 不留残值 |
| 失败路径不更新 `flush_max_lsn` / `superblock_safe_lsn` | 实装审 |

Checklist：

- [ ] success 与 failure 两条返回路径都填了 `flushed_max_lsn`（Gap 2 单一权威值）
- [ ] failure 时 rollback allocations
- [ ] success 时 retired 完整带出

### 12.8 §9 checkpoint_guard.retired

| 出口 | 验证 |
|---|---|
| `core/checkpoint_guard.hh` 加 `retired_objects retired` 字段 | 字段就位 |
| destructor 不变 | 注释 §9.2 narrowing |

Checklist：

- [ ] 所有现有 checkpoint_guard 构造点编译通过（aggregate init `{manifest, {}}`）

### 12.9 §11 顶层 pipeline

| 出口 | 验证 |
|---|---|
| `tree_local_flush(owner, req)` 函数存在 | sender 类型推导通过 |
| 调用方拿到的最终类型 = `tree_flush_result` | compute_sender_type 推导 OK |
| fold 失败 → result.st 透传 | sender 自然路径 |

Checklist：

- [ ] sender 编排与 flush_module_guide §2.4 完全对齐
- [ ] empty partitions / fold-failed 路径不死锁

---

## 13. 设计缺口与待澄清（构造 C）

如果 review 发现以下条目里任何一项设计不足以唯一决定实装，停下来上报，**不**自补 spec：

1. §2.4.E Step 3 case (c)"跨 worker 同槽位冲突"的 `panic_inconsistency` 策略：本文论证在 leaf-对齐 partition 下不可达，但 INC-040 读路径切 key-range 后，flush 侧 partition 是否仍保持 leaf-对齐需要重新审查；如果未来允许 partition 按 key-hash 或其他维度切，本 case 会真正触发，届时需要引入跨 worker 合并的 merge_substitutions。
2. §5.4 整轮 abort 的 `result.st` 取值：本文用 `unsupported_unimplemented` 占位，但语义上更接近 "flush_failed"。是否在本 step 引入新的 `flush_stage_status::flush_failed` enum 值，还是留给 frontier_switch step 引入。
3. §10.2 选项 X 的 `pending_writes` 表是否能直接合到 `active_rounds`（共用一个表，把 `combined_root` / `new_manifest` 等字段并进 `flush_round_state`）——这是表分裂还是合并的取舍，不影响算法正确性，但影响 round_state 字段密度。
4. §7.3 `update_superblock` handle 内的 read 路径：v1 是否应该 cache active superblock 的字节在 `tree_state` 里（避免每次都 NVMe read），还是每次 read。

---

## 14. 退出条件汇总

本 step 退出条件 = §12 所有 checklist 项都打钩 + 下列三项：

1. `cmake --build build --target inconel` 成功（`apps/inconel/` production target）。
2. Phase 7 三个失败测试（`test_candidate_build` / `test_flush_carriers` / `test_leaf_mapping`）维持失败状态，**不**在本 step 内修——它们留给端到端测试 step（§1.2）。
3. 顶层 `tree_local_flush(owner, req)` sender 类型推导通过（compile-time 验证）。

不要求"production 上跑过一轮真 flush 拿到 result"——那是端到端测试 step 的事；本 step 只对 production code 闭环负责。

---

## 15. §5.3 文档回写触发点

每个子节落地后立刻按下表回写 `design_doc/`（不要等到本 step 全部完成再 batch 写——`feedback_docs_every_step`）：

| 子节完成 | 立即更新 |
|---|---|
| §2 合并算法 | FF §3.4B / §3.5：worker 侧形态（已 Phase 7 完成 + Phase 9 owner 合并补完整）；FF §3 引言：tree-local flush 的 4 段 owner seam 对齐到 fold / leaf mapping (worker 内联) / candidate (= worker proposal) / merge |
| §3 tree_allocator | RSM §4.4：实装内容（head bump / push_back_bump / recycle / shared_heads）写满；RSM §4.3 不变 |
| §4 Plan | FF §3.5：Shadow Slot 选择细节同步 plan §4.2 case 1 / 2 / 2+ |
| §5 Writer | FF §3.9：device flush 单次；FF §8.2 sender 链同步 §11 形态 |
| §6 New manifest | FF §3.8：从"不全扫"角度补 §6.3.1 / §6.3.3 算法概要 |
| §7 Root-change | FF §6.2 / RSM §4.9 superblock_safe_lsn 推进时机表；RSM §4.2 加 `update_superblock` handle 完整签名 |
| §8 finish_flush_round | RSM §4.2 `tree_flush_result` 字段确认（已与 Phase 7 对齐） |
| §9 checkpoint_guard | core/checkpoint_guard.hh 注释 + RSM §4.6 / FF §4.1 / §5.1 / §5.2 把 G_K.retired 挂接路径写满（destructor 投递留 frontier_switch step） |
| §11 顶层 pipeline | FF §8.1 + flush_module_guide §2.4：sender 链终稿（写完后 flush_module_guide.md / flush_development_plan.md / 027 三份临时文档可一起删除——§5.3 决议） |

回写过程中如果发现 design_doc 与 production 仍有不一致，同时改两边——不允许 design_doc 滞后于 production。

---

## 16. 进度记录

| 日期 | 内容 |
|---|---|
| 2026-04-15 | 本文创建。覆盖 Phase 9 完整 owner 闭环：合并算法 / tree_allocator / Plan / Writer / new manifest / Root-change / finish / checkpoint_guard.retired / 顶层 pipeline。明确不做项含 frontier_switch 本体 / ~checkpoint_guard 投递 / reclaim_q consumer / release_gens / INC-040 / 端到端测试。设计缺口（§13）5 项标 codex review。文档回写（§15）按子节触发，不 batch。 |
| 2026-04-16 | Review 修正（对照 `029_owner_closure_review.md`）：<br>— **P1 §2.4.E**（owner_view 丢失多 slot 覆盖信息）：owner_view 从 "单槽位索引" 改为 "substitution_iv 区间模型"，显式保留 `start_old_slot..end_old_slot`，对齐 worker `candidate_build.hh:907-914` 的 `covered_paddrs` 语义；passthrough paddr 单独用 `passthrough_marker` 记录，避免和真 substitution 混计成冲突；跨 worker 同槽位冲突走 `panic_inconsistency` 而非 silent 选 unique_ptr。<br>— **P0-2 §6.3**（fence 依赖 last_key+1 + 空 leaf 语义冲突）：fence contract 拍板为 neighbor-based exclusive upper（`leaf[i].upper == leaf[i+1].lower`），中间位置不允许空 fence；新增 §6.3.2 "prune_empty_leaves" pass，在合并 §2 完成后、Plan §3 之前物理删除空 leaf 并 retire 其 old ranges；新 leaf 的 fence_lower/upper 由 in-order 邻居决定，不再用 last_key+1。<br>— **P0-1 §6.3.4**（reverse_topology 单值映射 split 失败）：rebuild 算法从"old_range_base → new_idx 单值映射"改为"按 combined_root 的 children list 精确决定每张 leaf 的 new parent"；未改 internal subtree 通过 Step B.2 枚举 base_topo DFS 全量重分配 new idx；承认 idx 全量重编号，不保留 base 原 idx；未改 leaves 在 Step D 按 new_lo.spans 线性填 parent_idx。<br>— **Obs-1 §6.3.5**（复杂度口径与"禁 O(tree) 全扫"冲突）：新增复杂度总表 + 明确解释"leaf_parent_idx O(L) 是输出下界、不是扫描"；给出 10 亿 KV 量级参照（~80ms CPU，远低于一次 NVMe flush 总时长）。<br>— **Obs-2 §11**（recovery_safe_lsn 留占位）：拍板在 `flush_fold_result` 加 `recovery_safe_lsn` 字段，顶层 sender 从 fold_result 读取；fold path 所有 4 个出口都填字段。<br>— §13 设计缺口列表同步更新：删除已收敛的 P0-1 反向索引条目，新增"combined_root 完全塌陷"的 new_manifest 合法形态条目（§6.3.2 引入）。 |
| 2026-04-16 | 原 §13 缺口 3 "combined_root 完全塌陷合法形态" 拍板：选**保留一张空 root leaf**（方案 2）。理由：让"空态 → 再次被填充"完全走已有 flush 路径，不引入空树 bootstrap flush 分支；磁盘代价 = 一个 shadow range (~16-64KB) 可忽略。§6.3.2 要点 5 补完塌陷合成规则（空 leaf root、`replaces_old_paddrs={}`、走 §3 Plan case 0 → §5 普通 write → §6 单条 span 首尾哨位 fence → §6.3.4 `leaf_parent_idx=[kInvalidInternalIdx]` → §7 root-change 路径）；要点 6 明确本 step 只覆盖 flush 产物，boot / format 初态的空树构造归另一个 step。§13 删掉第 3 条，剩 4 条缺口。 |
