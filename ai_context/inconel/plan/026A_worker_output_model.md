# 026A — Worker 输出模型重做

## Scope

在 026 candidate build 基础上，将 worker 输出从 leaf-only 的 `flush_candidate_batch` 改为 manifest overlay 的 `flush_worker_result`。Worker 做完整 consolidation cascade（含 root），通过现有多轮异步协议（`candidate_done / candidate_need_read`）异步读取 internal page。

不改上游（fold / mapping / dedup），不改 plan 阶段（027 实现）。

---

## 目标

- **G1**: 新增 `flush_changed_node`、`flush_worker_result` 类型
- **G2**: `tree_manifest` 新增 `slot_index()` / `slot_exhausted()` 便捷方法 + `root_range_base` 字段
- **G3**: 新增 `tree_reverse_topology` carrier（leaf→root 反向索引），by-value 挂在 `tree_manifest`
- **G4**: `flush_leaf_group` 新增 `leaf_span_idx` 字段；`keys_to_leaf_groups` 填充
- **G5**: `candidate_build_state` 改为 worklist 形态（leaf_ready / leaf_need_read / leaf_inflight / cascade_ready / cascade_waiting）
- **G6**: `process_candidate_groups()` 用 worklist 状态机，cascade 用 reverse_topology 反向 climb 替代 root-down trace
- **G7**: 调整 `flush_round_state` 存储类型（`candidates` → `worker_results`）

---

## 设计决策

### D1: flush_changed_node 定义

```cpp
struct flush_changed_node {
    paddr range_base;               // 标识哪个 node
    uint32_t level;                 // 0 = leaf, 1 = leaf 的 parent, ...
    bool needs_new_range;           // slot 用尽，需要分配新 range
    std::vector<char> page_content; // leaf: merge 后的完整候选页
                                    // internal: 旧页内容副本
};
```

**不含 child_base_changes**：plan 阶段分配完新 range 后自己知道哪些 child 的 range_base 变了，拿旧 internal page 副本直接 patch。worker 不预报。

### D2: flush_worker_result 定义

```cpp
struct flush_worker_result {
    flush_round_id round_id;
    uint32_t read_domain_index;
    flush_stage_status st;

    const core::tree_manifest* base;

    absl::flat_hash_map<paddr, flush_changed_node> changed_nodes;

    absl::InlinedVector<core::retired_value_ref, 64> retired_old_values;
};
```

替代 `flush_candidate_batch`。`base` 是共享引用（来自 `flush_worker_req.base_manifest`），生命周期由 `flush_round_state.pinned_base_guard` 保证。

`read_domain_index` 保留（保持和 `flush_candidate_batch` 一致的 identity 字段，fan-in 时 tree_sched 用于 assert 路由正确）。

### D3a: tree_reverse_topology — 反向拓扑 carrier

manifest by-value 挂 `tree_reverse_topology`，让 worker cascade 从 leaf
向 root climb，不再需要 root-down descent：

```cpp
// core/tree_reverse_topology.hh
using internal_idx = uint32_t;
inline constexpr internal_idx kInvalidInternalIdx = UINT32_MAX;

struct __attribute__((packed)) internal_node_entry {
    paddr         range_base;   // 10 bytes
    internal_idx  parent_idx;   // 4 bytes; kInvalidInternalIdx = root
};
static_assert(sizeof(internal_node_entry) == 14);

struct tree_reverse_topology {
    std::vector<internal_idx>        leaf_parent_idx;  // parallel to leaf_order.spans
    std::vector<internal_node_entry> internal_nodes;
};
```

**容量口径**（1B KV baseline）：
- 16 KiB page：~3.86M leaves × 4B + ~10.8K internal × 14B ≈ 16 MB/manifest
- 4 KiB page：~15.6M leaves × 4B + ~247K internal × 14B ≈ 65 MB/manifest

远小于 leaf_order 的 ~217 MB / ~875 MB。

**语义**：
- `leaf_parent_idx[leaf_span_idx]` → 该 leaf 的 direct parent 在 internal_nodes 的索引
- `internal_nodes[idx].parent_idx` → 该 internal node 的 parent 索引，root 为 sentinel

**为什么是 026A 边界内**：cascade 是 worker 的核心操作。root-down trace 不仅要每轮重新从 root 搜索（async read 恢复后 re-trace），还会读经过的非 cascade 页面（transit reads）。反向索引让 climb 变成 O(H) 常数时间查找 + 只读真正参与 cascade 的页面。延后补是工程债，不是优化。

### D3b: tree_manifest 便捷方法

```cpp
// 返回 range_base 当前的 slot_index（0-based）
// 如果 range_base 不存在于 slot_map → panic_inconsistency
uint32_t slot_index(paddr range_base) const;

// slot_index(range_base) + 1 >= geom->shadow_slots_per_range
bool slot_exhausted(paddr range_base) const;
```

纯查询，不修改 manifest。`slot_index()` 实现和 `resolve()` 共享查找逻辑，区别是返回 index 而非 computed paddr。

### D4: flush_leaf_group 扩展 — leaf_span_idx

新增字段，keys_to_leaf_groups 填充：

```cpp
struct flush_leaf_group {
    paddr leaf_range_base;
    paddr old_slot_paddr;
    uint32_t leaf_span_idx;  // 026A: index into base_manifest->leaf_order.spans
                             // → used as key into reverse_topology.leaf_parent_idx
    std::span<const flush_key_group> keys;
};
```

keys_to_leaf_groups 在遍历 leaf_order.spans 时记录当前 span 的 `li`，填到输出的 `leaf_span_idx`。下游 worker cascade 直接拿 idx 查 `reverse_topology.leaf_parent_idx[idx]`，不需要 O(log L) 的 find_leaf_for_key。

### D5: candidate_build_state 改为 worklist 形态

用显式队列状态机，每轮只消费 ready work，不再扫 leaf_groups[]：

```cpp
struct candidate_build_state {
    // inputs (immutable)
    std::span<const flush_leaf_group> leaf_groups;
    const core::tree_manifest*        base_manifest;
    uint64_t                          recovery_safe_lsn;
    uint32_t                          page_size;
    uint32_t                          page_lbas;
    uint32_t                          max_reads_per_round;

    bool initialized = false;  // first-entry classification flag

    // ── leaf worklists ──
    std::vector<uint32_t> leaf_ready;      // 页数据就绪，待 merge
    std::vector<uint32_t> leaf_need_read;  // miss，未分配 buffer
    std::vector<uint32_t> leaf_inflight;   // buffer 分配，read 进行中
    std::vector<std::unique_ptr<char[]>> page_bufs;  // sparse

    // ── cascade worklists ──
    std::vector<uint32_t> cascade_ready;
    absl::flat_hash_map<paddr, std::vector<uint32_t>> cascade_waiting;  // key=slot_paddr
    absl::flat_hash_map<paddr, std::unique_ptr<char[]>> internal_page_bufs;

    bool all_done = false;
    flush_worker_result result;
};
```

**关键设计**：
- internal_page_bufs 与 cascade_waiting 用同一 key（slot_paddr）：cascade_waiting[p] 存在且 internal_page_bufs[p] 存在 ⇒ read 已完成
- cascade_waiting 按 paddr 聚合 leaf —— 多个 leaf 等同一 page 自然共享一次读
- leaf_inflight 在 re-entry 时整体晋升到 leaf_ready（read 已完成的协议保证）

### D6: process_candidate_groups — worklist 状态机

每轮只消费 ready work，不全量扫 leaf_groups[]：

```
Phase 1（首次入口分类 / re-entry completion 推进）:
  首次:
    for i in 0..N-1:
      if cache_hit(leaf_groups[i].old_slot_paddr): leaf_ready.push(i)
      else: leaf_need_read.push(i)
    initialized = true
  re-entry:
    leaf_inflight → leaf_ready 全部晋升
    cascade_waiting 中 paddr 在 internal_page_bufs 的 → 对应 leaves → cascade_ready，
      erase entry

Phase 2（drain leaf_ready, merge）:
  for i in leaf_ready:
    merge_and_build_leaf(page_data, ..., candidate)
    exhausted = slot_exhausted(leaf_range_base)
    changed_nodes[leaf_range_base] = {level=0, needs_new_range=exhausted, page_content=move}
    retired_old_values 合并
    if exhausted: cascade_ready.push(i)
  leaf_ready.clear()

Phase 3（drain cascade_ready, climb）:
  for i in cascade_ready:
    outcome = cascade_climb_one_leaf(i, state, cache)    // D7
    if outcome.status == need_read:
      cascade_waiting[outcome.missing_slot_paddr].push(i)
  cascade_ready.clear()

Phase 4（done? 三队列空 → candidate_done）

Phase 5（pass 2: 分配 read buffers）:
  leaf 读: leaf_need_read → leaf_inflight，抽 budget 个
  internal 读: cascade_waiting 中 paddr 未在 internal_page_bufs 的 → 分配 + read_desc
```

### D7: cascade_climb_one_leaf — 基于 reverse_topology 的 climb

替代老的 root-down trace，直接从 leaf 向 root 跳：

```
cascade_climb_one_leaf(leaf_idx, state, cache) -> {status, missing_slot_paddr}:
  lg = leaf_groups[leaf_idx]
  pidx = reverse_topology.leaf_parent_idx[lg.leaf_span_idx]
  level = 1

  while pidx != kInvalidInternalIdx:
    node = reverse_topology.internal_nodes[pidx]

    if node.range_base 已在 changed_nodes:
      // 其他 leaf 的 climb（或本 leaf 上一轮）已经物化
      existing = changed_nodes[node.range_base]
      if !existing.needs_new_range: return complete  // cascade 终止
      pidx = node.parent_idx; level++; continue      // 复用决策，继续向上

    parent_slot = manifest.resolve(node.range_base)
    page_data = try cache / try internal_page_bufs
    if !page_data: return need_read{missing = parent_slot}

    exhausted = slot_exhausted(node.range_base)
    changed_nodes[node.range_base] = {level, needs_new_range=exhausted, page_content=copy}
    if !exhausted: return complete

    pidx = node.parent_idx; level++

  return complete  // 走到 sentinel = root 已 cascade 完
```

**重点**：
- "已在 changed_nodes" 不无脑 break —— 根据 existing.needs_new_range 决定是否继续（防止 interrupted climb 再次进入时漏掉上层）
- 不读 leaf 页（旧 trace 读 leaf 页来判断 type==leaf 终止；现在用 `leaf_parent_idx[leaf_span_idx]` 直接跳过 leaf）
- 只读 cascade path 上真正需要 materialize 到 changed_nodes 的 internal 页，没有 transit reads

### D8: 不保留 trace_root_to_parent

旧的 root-down descent 实现删除。reverse_topology 是 026A 边界内的必选数据结构。

### D10: _build_leaf_candidates PUMP surface 调整

当前 `_build_leaf_candidates` 的 cb 类型是 `flush_candidate_batch`，这是 Phase 2 stub 的遗留。026A 不改这个 surface——它是 Phase 2 stub（advance 中 build_q 仍返回 unsupported_unimplemented），实际的 candidate build 走 `_process_candidates` surface。

`_process_candidates` 的 cb 类型是 `candidate_decision`（不变），state 内的 `result` 字段从 `flush_candidate_batch` 改为 `flush_worker_result`。pipeline 在 `candidate_done` 后从 state 中取出 result。

所以 PUMP surface 签名不变，只是 state 内部的输出类型变了。

### D11: flush_round_state 调整

```cpp
// 旧:
std::vector<flush_leaf_candidate> candidates;

// 新:
std::vector<flush_worker_result> worker_results;
```

fan-in 后 tree_sched 将各 worker 的 result 收集到这里。027 的 plan 从这里取。

### D12: flush_leaf_candidate 保留

`flush_leaf_candidate` 类型保留不删。`merge_and_build_leaf()` 的签名不变——它仍然输出 `flush_leaf_candidate`。pass 1A 中 merge 完成后，从 `flush_leaf_candidate` 提取 page_content 和 retired_old_values 转入 `flush_changed_node` 和 `flush_worker_result`。

这样 merge_and_build_leaf 是纯算法函数，不需要知道 manifest overlay 模型。

### D13: changed_nodes 中 internal page 只收需要写入的

遍历路径上经过但不需要变更的 internal page 不进 changed_nodes。只有 child 的 range_base 会变（因为 child needs_new_range）的 internal page 才加入。cascade 向上直到：

- 遇到 slot 未满的层 → 该层加入 changed_nodes（needs_new_range=false），cascade 终止
- 到达 root → root 加入 changed_nodes（needs_new_range 按 slot 判定），cascade 终止

---

## 文件变更

### flush_types.hh

新增（在 `flush_candidate_batch` 之后）：

```cpp
struct flush_changed_node {
    paddr              range_base;
    uint32_t           level;           // 0 = leaf
    bool               needs_new_range;
    std::vector<char>  page_content;
};

struct flush_worker_result {
    flush_round_id                     round_id;
    uint32_t                           read_domain_index;
    flush_stage_status                 st;
    const core::tree_manifest*         base;
    absl::flat_hash_map<paddr, flush_changed_node>
                                       changed_nodes;
    absl::InlinedVector<core::retired_value_ref, 64>
                                       retired_old_values;
};
```

保留 `flush_leaf_candidate`、`flush_candidate_batch`（Phase 2 stub 仍引用）、`candidate_decision`（不变）。

### core/tree_reverse_topology.hh（新文件）

参见 D3a 的类型定义。

### tree_manifest.hh

- 新增 `root_range_base` 字段
- 新增 `reverse_topology` 字段（by-value，跟随 manifest 生命周期）
- 新增 `slot_index()` / `slot_exhausted()` 便捷方法
- `empty()` 工厂更新，reverse_topology 初始化为空

### flush_types.hh

- 新增 `flush_changed_node` / `flush_worker_result`
- `flush_leaf_group` 加 `leaf_span_idx` 字段
- 保留 `flush_leaf_candidate` / `flush_candidate_batch`（Phase 2 stub 仍引用）、`candidate_decision`

### leaf_mapping.hh

`keys_to_leaf_groups` 填充 `leaf_span_idx`（当前 li 值）。

### candidate_build.hh

- `candidate_build_state` 改为 worklist 形态（D5）
- 新增 `cascade_climb_one_leaf()`（D7，基于 reverse_topology）
- 删除 `trace_root_to_parent` / `apply_cascade_path` / `trace_path_entry` / `trace_result`
- `process_candidate_groups()` 重写为 Phase 1-5 worklist 状态机（D6）

### flush_round_state.hh

`candidates`（`flush_leaf_candidate[]`）→ `worker_results`（`flush_worker_result[]`）。

### worker_scheduler.hh

不改 PUMP surface 签名（D10）。advance() 中 build_q drain 不变（仍是 Phase 2 stub）。candidates_q drain 不变（process_candidate_groups 签名未改，返回 candidate_decision）。

---

## 实现顺序

1. **core/tree_reverse_topology.hh**: 新文件，定义 `tree_reverse_topology` 和 `internal_node_entry`
2. **core/tree_manifest.hh**: 加 `slot_index()` / `slot_exhausted()` / `root_range_base` / `reverse_topology` 字段
3. **tree/flush_types.hh**: 新增 `flush_changed_node` / `flush_worker_result`；`flush_leaf_group` 加 `leaf_span_idx`
4. **tree/leaf_mapping.hh**: `keys_to_leaf_groups` 填充 `leaf_span_idx`
5. **tree/flush_round_state.hh**: `candidates` → `worker_results`
6. **tree/candidate_build.hh**: state 改 worklist、新增 cascade climb、重写 process_candidate_groups、删除 trace 相关函数
7. **tree/sender.hh**: pipeline 返回类型 `flush_candidate_batch` → `flush_worker_result`
8. 更新测试：leaf_span_idx 字段、cascade 测试里的 reverse_topology 填充
9. 全量编译 + 测试

---

## 不做的事

- 不改 fold / mapping / dedup pipeline（上游不变）
- 不改 `_build_leaf_candidates` / `_leaf_mapping` PUMP surface
- 不改 `merge_and_build_leaf()` 签名和算法
- 不实现 plan 阶段（027 的事）
- 不实现 split（page overflow 仍返回 `unsupported_shape_change`）
- 不改 owner_scheduler.hh（tree_sched handle 不变）
