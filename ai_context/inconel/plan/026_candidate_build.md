# 026 — Candidate Build（Worker-Local Primitive）

> 实现第二十六步。落地 `flush_development_plan.md` Phase 6 的 **worker 侧 primitive**：merge 算法、candidate page 构造、multi-round process_candidates handle、以及封装这些能力的 `build_candidates_for_partition()` pipeline。
>
> **本 step 的范围是 worker-local candidate build primitive。** 顶层 `tree_local_flush()` pipeline 尚未把 candidate-build fanout/fan-in 接入；`flush_merge_request` 仍只携带 `mapping_results`（`flush_types.hh`）；`tree_sched::advance(merge)` 仍只做 `merge_lookup_leaf_groups()`（`owner_scheduler.hh`）。这些接线属于后续 step（组装完整 flush pipeline 时一起做）。

## 整体架构：worker 侧 primitive，尚未接入顶层 pipeline

Phase 5 建立了 fold → fanout(mapping) → fan-in(merge leaf groups) 的 sender 骨架。Phase 6 本 step 落地 worker 侧的 candidate build 能力，但**不改变顶层 pipeline 的接线**：

```
已落地（本 step）:
  candidate_build_state          — worker arm 内部 multi-round 状态
  process_candidate_groups()     — 两遍扫描算法（cache check + temp buffer + bounded reads）
  merge_and_build_leaf()         — sorted merge + tombstone compact + page image 构造
  build_candidates_for_partition() — 封装上述能力的 PUMP pipeline（sender.hh）
  tree_worker_sched<Cache>       — 模板化 worker，直接访问共享 cache
  flush_read_budget()            — 根据 lookup 负载动态调节 read budget

尚未接线（后续 step）:
  tree_local_flush()             — 仍标注 NOT YET IMPLEMENTED
  flush_merge_request            — 仍只携带 mapping_results
  tree_sched::advance(merge)     — 仍只做 merge_lookup_leaf_groups()
```

### worker handle 内部 async pipeline

读 old leaf page 可能 cache miss，需要异步 NVMe I/O。Worker 的 `advance()` 不能阻塞等 I/O。

解决方案复用 codebase 已有的 **decision 模式**（与 `tree_lookup_sched` 对称）：

```
sender.hh 封装:
  build_candidates_for_partition(worker, ...)
    = with_context(candidate_build_state)(
        loop(check_not_done)
          >> worker->process_candidates(state)
          >> visit()
          ├─ candidate_need_read → NVMe read（不做 cache submit）→ 下一轮
          └─ candidate_done → 跳出
        >> collect results
      )
```

Worker 在 process pass 中通过共享 cache 做 pin 检查（免费复用 lookup 的热页）。Cache miss 时分配 `candidate_build_state::page_bufs` 临时 buffer，NVMe 直接读入。读完后 worker 从临时 buffer 消费 page 内容，消费完立即释放。**不做 cache submit**——flush 读的 old leaf 即将被 frontier_switch 淘汰，进 cache 只会驱逐 lookup 热页。

## 本 step 覆盖的目标

| 目标 | 说明 |
|---|---|
| G1 | `candidate_build_state` — worker arm 内部的 multi-round 状态 |
| G2 | `tree_worker_sched` 拆分为 `tree_worker_sched_base` + `tree_worker_sched<Cache>`，新增 `process_candidates(state)` handle |
| G3 | Sorted merge 算法：old leaf records + memtable winners → merged stream（含 old tombstone compact） |
| G4 | Page-local compact：`tombstone && data_ver <= recovery_safe_lsn` → absent（对 old leaf 和 memtable winner 均适用） |
| G5 | `leaf_page_builder` 构造 candidate page image |
| G6 | `flush_leaf_candidate` 扩展，携带 candidate page image + retired old values |
| G7 | `sender.hh::build_candidates_for_partition()` — worker arm 内部 pipeline |
| G8 | `flush_candidate_batch` 扩展，携带 candidate images + retired values |
| G9 | `candidate_decision` 类型（`candidate_done` / `candidate_need_read`） |
| G10 | `flush_read_budget()` — 根据 paired lookup 的 `pending_lookups` 动态调节每轮 read 上限 |
| G11 | `tree_lookup_sched_base::pending_lookups` 计数器（plain uint32_t，同核同线程） |

## 本 step 不覆盖

| 不做项 | 归属阶段 | 说明 |
|---|---|---|
| `tree_local_flush()` 接入 candidate-build fanout/fan-in | 后续 step | 顶层 pipeline 仍标注 NOT YET IMPLEMENTED |
| `flush_merge_request` 改为携带 candidate results | 后续 step | 仍只携带 `mapping_results` |
| `tree_sched::advance(merge)` 收集 candidate results | 后续 step | 仍只做 `merge_lookup_leaf_groups()` |
| tree delta planning / shadow slot 选择 | Phase 7 | tree_sched merge handle 扩展 |
| NVMe 地址分配 / bounded writes / device flush | Phase 7 | |
| new_manifest 构造 / leaf_order rebuild | Phase 7 | |
| leaf split / parent rewrite / root change | Phase 8 | worker handle 再扩展 |

## 设计决策

| # | 决策点 | 结果 | 说明 |
|---|---|---|---|
| D1 | worker handle 内部 async 模式 | batch-decision 模式（与 lookup 对称） | 每轮扫全部 group：cache hit + 有 buffer 的当场处理，cache miss 的收集成一个 batch 返回。pipeline NVMe read 后重入 worker |
| D2 | `candidate_build_state` 位置 | PUMP context stack（`with_context`） | 与 lookup_state 同模式 |
| D3 | candidate page image 所有权 | `std::vector<char>` owned by `flush_leaf_candidate` | worker 分配 buffer，构造 candidate，move 进 result。tree_sched 后续用这个 buffer 做 NVMe write |
| D4 | old leaf read 的 cache 复用 | 通过 read_domain 共享的 cache（`tree_worker_sched<Cache>` 持有 `Cache*`） | worker 和 lookup 持有同一个 `Cache*`，调同一个 `cache->pin(fid)`。零类型擦除，零函数指针 |
| D5 | worker 与 cache 的交互 | **只 check，不 insert** | worker 在 process pass 中通过共享 cache 做 pin 检查（免费复用 lookup 的热页）。cache miss 时读进 `state.page_bufs` 临时 buffer，**不做 cache submit**。理由：flush 读的 old leaf 即将被 frontier_switch 淘汰，insert 进 cache 只会驱逐 lookup 的热页 |
| D6 | merge 算法 | 双指针 sorted merge（old records iterator + new keys iterator） | 两边都已排序，O(old_count + new_count)。同 key 时 memtable winner 覆盖 old |
| D7 | candidate overflow（merge 后放不下一页） | 返回 `unsupported_shape_change` | Phase 8 split 才处理 |
| D8 | tombstone compact 条件 | `kind == tombstone && data_ver <= recovery_safe_lsn` | FF §3.4B 冻结。对 candidate image 中的所有记录适用（old leaf 和 memtable winner 均检查）。物理删除安全因为低于 recovery_safe_lsn 的 WAL 已全部回收 |
| D9 | retired old tree values 收集 | worker 在 merge 过程中收集被覆盖的 old leaf record 的 value_ref | 挂到 flush_leaf_candidate.retired_old_values，后续 merge handle 汇总到 round_state |
| D10 | worker 读 old leaf 的方式 | 每轮两遍扫描：pass 1 处理所有有 page data 的 group（shared cache hit 或 temp buffer），pass 2 为 miss group 分配 `state.page_bufs` 临时 buffer 并收集 read_desc（受 budget 限制）。NVMe 直接读入临时 buffer，不做 cache submit | 与 lookup 的 process_entries + prepare_reads 两遍扫描同构。worker 不需要多级 descend——每个 leaf 只读一页 |
| D11 | 临时 buffer 管理 | `state.page_bufs[i]` = `unique_ptr<char[]>`，NVMe read 直接写入，pass 1 消费后立即 `reset()` | 不进 cache，不被驱逐，不需要 frame pool / free_frames。buffer 生命周期完全由 state 管理 |
| D12 | 空 leaf group 处理 | 跳过——leaf_group.keys 不可能为空（Phase 5 保证） | keys_to_leaf_groups 只 emit 非空 group |
| D13 | worker 多 leaf group 的处理顺序 | 每轮扫全部未完成 group，不保证顺序 | cache hit 的先处理，miss 的等 NVMe read 后下轮处理。tree_sched 不依赖 worker 输出的 leaf 顺序 |
| D14 | 每轮 NVMe read 上限 | `max_reads_per_round`（构造时传入，默认 256），运行时受 `flush_read_budget()` 动态调节 | 峰值临时 buffer = `budget × page_size`。Budget 根据 paired lookup 的 `pending_lookups` 线性退让，最低保证 1（保证前进，不会热循环） |
| D15 | worker 模板化 | `tree_worker_sched_base` + `tree_worker_sched<Cache>`，与 lookup 对称 | Cache 模板参数从 builder 流入，worker 和 lookup 持有同一个 `Cache*`，调同一个 `cache->pin(fid)`。Registry 存 `tree_worker_sched_base*` |

## 详细设计

### 1. `flush_leaf_candidate` 扩展（D3, D9）

```cpp
struct flush_leaf_candidate {
    paddr              leaf_range_base;
    paddr              old_slot_paddr;
    flush_stage_status st;

    // Phase 6 新增：
    std::vector<char>  candidate_page;         // 完整 page image，tree_sched 用于 NVMe write
    absl::InlinedVector<core::retired_value_ref, 16>
                       retired_old_values;     // merge 中被覆盖的 old leaf value_refs
    uint16_t           record_count = 0;       // candidate page 的记录数
};
```

### 2. `candidate_build_state`（D1, D2, D11）

Worker arm 内部的 multi-round 状态，放在 PUMP context stack 上。

```cpp
struct candidate_build_state {
    // 输入（构造时填入，不变）
    std::span<const flush_leaf_group> leaf_groups;
    const core::tree_manifest*        base_manifest;
    uint64_t                          recovery_safe_lsn;
    uint32_t                          page_size;
    uint32_t                          page_lbas;
    uint32_t                          max_reads_per_round;  // 可配，默认 256

    // per-group 处理状态
    std::vector<bool>                          processed;
    std::vector<std::unique_ptr<char[]>>       page_bufs;  // 临时 buffer，不进 cache
    bool                                       all_done = false;

    // 输出
    flush_candidate_batch                      result;
};
```

没有 `current_group` 游标。每一轮扫**全部**未完成的 group。pass 1 对每个 group 按优先级查找 page 内容：shared cache hit > page_bufs[i] > miss。hit 或有 buffer 的当场处理；miss 的在 pass 2 分配 `page_bufs` 临时 buffer 并收集 read_desc。

保证每个 page 最多读一次 NVMe：首轮 miss 的分配 buffer + NVMe read → buffer 跨轮存活在 state 上 → 二轮 pass 1 从 buffer 消费。不进 cache，不会被驱逐，不需要再读。

**内存控制**：pass 1 消费完 `page_bufs[i]` 后立刻 `reset()` 释放。pass 2 每轮受 `flush_read_budget()` 动态调节（最低 1，最高 `max_reads_per_round`）。峰值临时 buffer = `budget × page_size`，可按实际内存预算调整。

### 3. `process_candidate_groups<Cache>(state, paired_lookup, cache)` handle（D10）

每次调用做两件事：
1. **process 阶段**：扫全部未完成的 group，shared cache hit 或有 temp buffer 的立即完成 merge+build
2. **prepare_reads 阶段**：扫全部仍未完成的 group，分配 `state.page_bufs` 临时 buffer，收集 read_desc（受 budget 限制）

```
process_candidate_groups(state, paired_lookup, cache):

  // ── pass 1: process all available groups ──
  for i in 0..leaf_groups.size():
    if processed[i]: continue

    leaf = leaf_groups[i]
    const char* page_data = nullptr

    // 优先查共享 cache（同一个 Cache*，同 cache->pin(fid)）
    if cache:
      fid = make_tree_frame_id(leaf.old_slot_paddr, page_lbas)
      pin = cache->pin(fid)
      if pin.frame:
        page_data = pin.frame->buf

    // 次查临时 buffer（上轮 NVMe read 的结果）
    if !page_data && page_bufs[i]:
      page_data = page_bufs[i].get()

    if !page_data: continue  // 两路都 miss，等 pass 2

    // 有 page → merge + build + emit
    candidate = merge_and_build_leaf(page_data, ...)
    result.leaves.push_back(candidate)
    processed[i] = true
    page_bufs[i].reset()  // 立即释放临时 buffer

  // ── check: all done? ──
  if all processed:
    all_done = true
    return candidate_done {}

  // ── pass 2: collect miss pages（budget 限制）──
  pending = paired_lookup ? paired_lookup->pending_lookups : 0
  budget = flush_read_budget(pending, max_reads_per_round)  // >= 1

  read_descs = []
  reads_this_round = 0

  for i in 0..leaf_groups.size():
    if processed[i]: continue
    if page_bufs[i]: continue  // 已有 buffer，等 pass 1 消费
    if reads_this_round >= budget: break

    page_bufs[i] = make_unique<char[]>(page_size)  // 临时 buffer，不进 cache
    read_descs.push({ .lba, .buf = page_bufs[i].get(), .num_lbas })
    reads_this_round++

  return candidate_need_read { read_descs }
```

### 4. Sorted Merge 算法（D6, D8）

```
merge_and_build_leaf(page_data, page_size, keys, recovery_safe_lsn, out):
  reader.parse(page_data, page_size)
  builder.init(candidate_buf, page_size)

  oi = 0  // old record index
  ni = 0  // new key index

  while oi < old_count || ni < new_count:
    ...双指针 merge...

    same key → memtable winner 覆盖 old → old value_ref 进 retired
    old-only → emit_old (含 tombstone compact)
    new-only → emit_winner (含 tombstone compact)

  builder.finalize()

emit_old(record, recovery_safe_lsn):
  if tombstone && data_ver <= recovery_safe_lsn: skip  // compact
  else: builder.add_*()

emit_winner(key_group, recovery_safe_lsn):
  if tombstone && data_ver <= recovery_safe_lsn: skip  // compact
  else: builder.add_*()
```

Tombstone compact 对 candidate image 中的**所有记录**适用——old leaf 和 memtable winner 均检查。

### 5. `sender.hh::build_candidates_for_partition()`（D1, G7）

```cpp
inline auto
build_candidates_for_partition(
    tree_worker_sched_base* worker,
    std::span<const flush_leaf_group> leaf_groups,
    const core::tree_manifest* base_manifest,
    uint64_t recovery_safe_lsn,
    flush_round_id round_id,
    uint32_t read_domain_index,
    uint32_t page_size,
    uint32_t page_lbas)
{
    auto state = make_candidate_build_state(...);
    return with_context(std::move(state))(
        [worker]() {
            return get_context<candidate_build_state>()
                >> flat_map([worker](candidate_build_state& state) {
                    return just()
                        >> for_each(check_candidates_not_done(state))
                        >> flat_map([worker, &state](bool) {
                            return worker->submit_process_candidates(&state);
                        })
                        >> visit()
                        >> flat_map([]<typename D>(D&& d) {
                            if constexpr (candidate_need_read)
                                return on_candidate_need_read(__fwd__(d));
                                // NVMe read，不做 cache submit
                            else
                                return just(true);
                        })
                        >> all()
                        >> then([&state](bool) -> flush_candidate_batch {
                            return std::move(state.result);
                        });
                });
        });
}
```

**注意**：此函数已定义并可用，但**顶层 `tree_local_flush()` 尚未调用它**。当前 `tree_local_flush()` 仍标注 NOT YET IMPLEMENTED。后续 step 组装完整 flush pipeline 时，会把 fanout arm 从 `submit_leaf_mapping` 替换为 `build_candidates_for_partition`，并相应扩展 `flush_merge_request` 和 merge handle。

### 6. `flush_read_budget()`（D14, G10）

```cpp
inline uint32_t
flush_read_budget(uint32_t pending_lookups,
                  uint32_t max_reads,
                  uint32_t throttle_threshold = 32)
{
    if (pending_lookups == 0) return max_reads;
    if (pending_lookups >= throttle_threshold) return 1;
    uint32_t scaled = max_reads * (throttle_threshold - pending_lookups)
                    / throttle_threshold;
    return (scaled > 0) ? scaled : 1;
}
```

最低保证返回 1（保证前进，不会热循环）。`pending_lookups` 是 plain uint32_t（同核同线程，不需要 atomic）。

### 7. `tree_worker_sched` 模板化（D15）

```
tree_worker_sched_base       — 非模板：queue, schedule, submit, PUMP ops
tree_worker_sched<Cache>     — 模板：Cache* cache_, advance()

Builder 构造时：
  auto* tlookup = new tree_lookup_sched<Cache>(...);
  auto* tworker = new tree_worker_sched<Cache>(
      rdi, &tlookup->page_cache_, tlookup);
```

Worker 和 lookup 持有同一个 `Cache*`，调同一个 `cache->pin(fid)`。Registry 存 `tree_worker_sched_base*`。

## Decision 类型

```cpp
struct candidate_done {};

struct candidate_need_read {
    std::vector<format::read_desc> read_descs;
    // 没有 frames 字段。
    // buffer 由 state.page_bufs 持有，read_desc.buf 指向
    // page_bufs[i].get()，NVMe 直接写入。不做 cache submit。
};

using candidate_decision = std::variant<candidate_done, candidate_need_read>;
```

## Fail-Fast 矩阵

| 检查点 | 条件 | 动作 |
|---|---|---|
| `process_candidates` | old leaf page CRC 校验失败 | `panic_inconsistency` |
| `process_candidates` | old leaf page 不是 leaf 类型 | `panic_inconsistency` |
| merge | builder.add_value/add_tombstone 返回 false（overflow） | `unsupported_shape_change` |
| `build_candidates_for_partition` | page_size / page_lbas 与 manifest->geom 不匹配 | `panic_inconsistency` |
| `flush_read_budget` | budget 最低保证 1 | 防止 budget=0 导致热循环 |

## 偏差记录

| # | 偏差 | 说明 |
|---|---|---|
| Δ-8 | `_leaf_mapping` sender surface 保留 | Phase 5 遗留，只用于独立测试。顶层 pipeline 接线时可能清理 |
| Δ-9 | `flush_leaf_candidate` 从 POD shell 扩展为携带 page image + retired values | Phase 2 冻结的 shell 只有 status，Phase 6 填入真内容 |
| Δ-10 | worker 只 check cache 不 insert | 与 lookup pipeline 的关键区别：lookup 读完做 `submit_cache`；worker 不做。flush 读的页即将作废，进 cache 只会驱逐热页 |
| Δ-11 | 顶层 pipeline 尚未接线 | `tree_local_flush()` 仍标注 NOT YET IMPLEMENTED；`flush_merge_request` 仍只携带 `mapping_results`；`tree_sched::advance(merge)` 仍只做 `merge_lookup_leaf_groups()`。后续 step 统一接线 |

## 与现有代码的交互

### 直接复用（不修改）
- `leaf_page_reader` — decode old leaf
- `leaf_page_builder` — 构造 candidate page image
- `keys_to_leaf_groups()` — mapping 算法
- `make_tree_frame_id()` — frame id 构造
- `tree_page_validate()` — CRC 校验

### 已修改
- `tree/flush_types.hh` — `flush_leaf_candidate` / `flush_candidate_batch` 扩展 + `candidate_decision` 类型
- `tree/worker_scheduler.hh` — 拆分为 `tree_worker_sched_base` + `tree_worker_sched<Cache>`，新增 `_process_candidates` PUMP sender + `candidates_q`
- `tree/sender.hh` — `build_candidates_for_partition()` + `on_candidate_need_read()`
- `tree/lookup_scheduler.hh` — `pending_lookups` 计数器（plain uint32_t）
- `core/registry.hh` — `tree_worker_list` 改为存 `tree_worker_sched_base*`
- `runtime/builder.hh` — worker 构造改为 `tree_worker_sched<TreeCache>`，传入 `&tlookup->page_cache_`

### 未修改
- `tree/owner_scheduler.hh` — merge handle 不变
- `tree/flush_types.hh` 中的 `flush_merge_request` — 仍只携带 `mapping_results`

### 新增文件
- `tree/candidate_build.hh` — `candidate_build_state` + `merge_and_build_leaf()` + `process_candidate_groups<Cache>()` + `flush_read_budget()`
- `test/test_candidate_build.cc` — 9 个测试覆盖 merge/compact/cache-hit/temp-buffer/bounded-reads/budget

## 容量估算

10 亿 KV / 16K page 场景：
- 一次 flush 影响的 leaf 数 = flush 覆盖的 unique keys / ~259 keys per leaf
- 假设一次 flush 10M keys → ~38.6K affected leaves
- 每个 candidate page image = 16K → ~38.6K × 16K = ~617 MB 峰值
- 分 K = 4 workers → 每 worker ~154 MB
- 临时 buffer 峰值 = min(miss_count, budget) × page_size，budget 默认 256 → 最多 4MB

candidate image 的累积是真实需求——tree_sched 需要拿去写盘。Phase 7 的 bounded writes 会限制一次 flush 的规模。

## 验证计划（已实现）

### 算法层
1. sorted merge：3 old + 2 new, 1 overlap → 4 records, retired 正确
2. tombstone compact (new)：memtable tombstone data_ver <= recovery_safe_lsn → 删除
3. tombstone compact (old)：tree 里旧 tombstone data_ver <= recovery_safe_lsn → 在 rewrite 时删除
4. tombstone preserved：data_ver > recovery_safe_lsn → 保留

### Process 层
5. two-pass temp buffer：pass 1 miss → pass 2 allocate → NVMe read → pass 1 consume → done
6. cache hit：page 在共享 cache 中 → 一轮完成
7. bounded reads：max=2, 5 groups → 4 rounds
8. flush_read_budget：idle/half/full/never-zero

### 边界
9. empty groups → immediate done
