# 024 — Phase 4 Production Code Review

> Review 对象：当前 worktree 上已落地的 step 024 production 代码 + 测试（Codex 实现）。
>
> 覆盖文件：
>
> 1. `apps/inconel/core/memtable.hh`
> 2. `apps/inconel/tree/flush_types.hh`
> 3. `apps/inconel/tree/flush_round_state.hh`
> 4. `apps/inconel/tree/memtable_fold.hh`（新文件）
> 5. `apps/inconel/tree/owner_scheduler.hh`
> 6. `apps/inconel/test/test_flush_carriers.cc`

---

## 1. 结论

**1 条 high，2 条 medium，2 条 low。**

算法主体正确：K-way merge fold、D16 staged losers 零副作用、D17 sentinel + D18 provenance、D12 fail-fast、empty-workset ok 路径的 D19 `new_manifest` 回传——核心设计全部落地。关键问题在 fold 产出的 `flush_key_group.key` 来源违反了三处文档的 pin 契约，以及两个 ok 路径的 `new_manifest` 不一致。

### 1.1 验证执行

1. `cmake --build build -j$(nproc)` — 编译通过，无 warning / error
2. `./build/inconel_test_flush_carriers` — 45 项全部 OK（Phase 3 原有 + Phase 4 新增：fold 10 case、partition 3 case、handle 6 case、panic 7 case、SFINAE 12 条 static_assert）
3. `./build/inconel_test_runtime` — clock + slru 全部 OK
4. `./build/inconel_test_tree_value` — 5 case 全部 OK
5. `./build/inconel_test_tree_lookup` — 400 key + eviction stress 全部 OK

---

## 2. Findings

### H-1: `flush_key_group.key` 可能指向非 winner gen 的 arena

**位置**：`memtable_fold.hh:137`

```cpp
rs.workset.push_back(flush_key_group{
    .key = min_key,   // ← 问题在这里
    ...
});
```

`min_key` 在 step (a)（行 77-86）中从第一个碰到最小 key 的 cursor 取得。step (b)（行 100-118）收集同 key 全部 entries 并选 winner（max data_ver），但选出的 winner 不一定与 step (a) 取 `min_key` 的那个 gen 是同一个 gen。

例：gen0 和 gen1 都有 key "abc"。step (a) 遍历 cursors 先碰到 gen0，`min_key` 指向 gen0 的 arena。step (b) 发现 gen1 的 data_ver 更大，winner 是 gen1。此时 `flush_key_group { .key = min_key(gen0), .winner_gen = gen1 }`——key 指向 gen0 的 arena，winner_gen 是 gen1。

三处文档明确说 key 必须指向 winner gen 的 arena：

| 出处 | 原文 |
|------|------|
| `flush_types.hh:73` | "key is a std::string_view into the winner gen's kv_arena" |
| `flush_and_frontier_switch.md:135` | "key, // string_view into winner's owning gen kv_arena" |
| `024 plan §4.1 行 220` | "key: winner entry 的 key（string_view 进 winner gen kv_arena）" |

**当前阶段是否 UAF**：不会。Phase 4 内 `round_state.pinned_gens` 保住全部 gen 的 arena，workset 生命周期 ≤ round_state。但 `flush_key_group` 的 carrier 契约写的是"key 由 `winner_gen` shared_ptr pin"。Phase 5-7 消费方如果只持有 `winner_gen` 做 pin 保护（这正是 carrier 注释鼓励的理解），而 key 实际指向另一个 gen 的 arena——那个 gen 的最后一根 pin 掉了，key 就悬空。

**修复方式**：`entry_ref` 加一个 `std::string_view key` 字段，step (b) 收集 entries 时一并记录该 gen 的 btree_map key。step (d) 用 `winner.key` 替代 `min_key`。改动约 5 行。

### M-1: 两个 ok 路径的 `new_manifest` 语义不一致

**位置**：`owner_scheduler.hh:275-280`（sealed_gens 为空）vs `owner_scheduler.hh:336-338`（workset 为空）

| ok 路径 | `new_manifest` | `flushed_gens_by_front` |
|---------|---------------|------------------------|
| sealed_gens 为空（行 276-279） | `nullptr`（default 未赋值） | `{}`（空） |
| workset 为空（行 336-338） | `base_guard->manifest`（D19） | populated |

D19 的设计意图是"outer flow 走统一 success 路径"。如果 consumer 对所有 ok result 统一做 `install_cat(res.new_manifest)`，sealed_gens 为空路径会传 `nullptr` 进去。

虽然 sealed_gens 为空时 "没有 gen 需要 release" 看似可以跳过 frontier_switch，但依赖 consumer 去区分两种 ok（一种 new_manifest 有值、一种没有）本身就是脆弱的——后续 phase 加的 ok 路径越多，consumer 分支越难维护。

**建议**：sealed_gens 为空路径也赋 `new_manifest = r->args.base_guard->manifest`，让所有 ok 路径的 consumer 无条件信任 `new_manifest != nullptr`。改动 1 行。

### M-2: 缺少 D17 `front_owner_index == UINT32_MAX` sentinel panic 的 forked 测试

**位置**：`test_flush_carriers.cc` — 7 个 forked panic 测试中无此 case

plan D17 定义了 `UINT32_MAX` 为 invalid sentinel，`build_flushed_gens_by_front()` 函数体（`owner_scheduler.hh:158-160`）已实现遇 sentinel 直接 `panic_inconsistency`。但测试没有 forked child 触发这条路径。现有测试全部通过 `make_dummy_sealed_gen(id, front_idx)` 显式赋值 0 或 1，sentinel 路径零覆盖。

**修复**：新增一个 forked child，构造 `front_owner_index = UINT32_MAX` 的 gen，提交到 tree_sched，走 empty-workset ok 路径触发 `build_flushed_gens_by_front` panic。约 20 行。

### L-1: `owner_scheduler.hh` 顶部文件注释仍是 Phase 3 措辞

行 12-13 写 "Phase 3 intentionally does NOT implement any flush algorithm"，但文件现在包含完整的 Phase 4 fold + partition + round_state 生命周期。

### L-2: `K` 类型 `size_t` 与循环变量 `uint32_t` 的 narrowing 比较

`memtable_fold.hh:51` `const auto K = rs.pinned_gens.size();` 推导为 `size_t`（64 bit），`for (uint32_t gi = 0; gi < K; ++gi)` 做 narrowing 比较。实际 K ≤ 8 无运行时风险，但 `-Wsign-compare` / `-Wconversion` 可能 warn。建议 `const auto K = static_cast<uint32_t>(rs.pinned_gens.size());`。

---

## 3. 逐文件确认（无 finding 部分）

### 3.1 `core/memtable.hh`

`uint32_t front_owner_index = UINT32_MAX;` 位于 `st` 之后、`min_lsn` 之前。sentinel 默认值与 plan D17 一致。

### 3.2 `tree/flush_types.hh`

`flush_key_partition { read_domain_index, groups }` 位于 `flush_key_group` 之后、`flush_lookup_req` 之前。SFINAE 在测试中锁住字段类型。

### 3.3 `tree/flush_round_state.hh`

- `partitions` + `staged_loser_entry` + `staged_memtable_losers` 按 phase 顺序追加在 workset 之后。
- `staged_loser_entry { uint32_t pinned_gen_index; retired_value_ref rvr; }` 与 plan D16 收敛形态一致（用 pinned_gens 下标，不用 gen_id map，不持额外 shared_ptr）。
- SFINAE 在测试中锁住新字段存在性。

### 3.4 `tree/memtable_fold.hh`（H-1 之外）

- `inline` 标记，ODR 安全。
- K-way merge 主循环结构正确：cursor 初始化 → 找最小 key → 收集 entries → 选 winner → 暂存 losers → 产出 flush_key_group → advance cursor。
- loser 暂存到 `rs.staged_memtable_losers`，不触碰 `gen.loser_durable_refs`——D16 零副作用保证成立。
- tombstone loser 跳过——正确。
- winner 通过 pointer equality `ref.entry == winner.entry` 排除——在同一遍遍历中指针唯一，正确。
- `build_key_partitions` 等量分区，`uint64_t` 中间计算防溢出，span 指向 workset 内存——正确。

### 3.5 `tree/owner_scheduler.hh`（M-1 之外）

- `tree_state` 新增 `active_rounds`（flat_hash_map）+ `next_round_id`。
- `build_flushed_gens_by_front` 标记 `static inline`，含 UINT32_MAX sentinel panic。
- advance() 六阶段流程与 plan §5 一致：validate → park → fold → empty-workset fast path → build_partitions → unpark/cb。
- `sealed_gens.empty()` 降级为 ok（D8）。
- per-gen 校验 null / sealed / dup gen_id（D9/D10/D11）。
- `tree_lookup_count() == 0` panic（D12）。
- forward-declare `registry::tree_lookup_count()` 打破 include cycle——合理的工程决策，注释说明完备。
- unsupported 路径 `active_rounds.erase` 在 cb 之前——round_state 销毁释放 staged losers。
- panic 路径全部走 `[[noreturn]]` 的 `panic_inconsistency` → `abort()`，不需要 `delete r`。

### 3.6 测试

- SFINAE：翻转 `active_rounds` / `next_round_id` 为 positive assert（023 review §3 交接信号）。新增 `flush_key_partition`、`staged_memtable_losers`、`front_owner_index` 检查。
- Fold 10 case：单 gen 单 key、单 gen 两 entry、两 gen 同 key、两 gen 不相交、tombstone winner、tombstone loser 不暂存、value loser + tombstone winner、全空 gen、单 gen 多 entry 同 key、零副作用综合——每条都验 `loser_durable_refs.size() == 0`。
- Partition 3 case：N=100/K=4、N=2/K=4、span validity + coverage。
- Panic 7 case：Phase 3 原有 3 条 + D9/D10/D11/D12。D12 构造了带真实 key 的 gen + `registry::clear()`。
- Handle 6 case：empty tables ok（D18 multi-front + D19 new_manifest）、non-empty unsupported（零副作用）、round-id 单调、pin chain release、multi-front flushed_gens_by_front。
- `make_dummy_sealed_gen(id, front_idx)` 显式赋 `front_owner_index`，不靠 sentinel。
- `test_handle_nonempty_workset_unsupported` 用 `fake_lookup_ptr` + 事后 `list.clear()` 隔离 registry 状态。

---

## 4. 修复优先级

| # | 级别 | 改动量 | 说明 |
|---|------|-------|------|
| H-1 | high | ~5 行 | `entry_ref` 加 `key` 字段，step (d) 用 `winner.key` 替代 `min_key` |
| M-1 | medium | 1 行 | sealed_gens 为空 ok 路径补 `.new_manifest = r->args.base_guard->manifest` |
| M-2 | medium | ~20 行 | 补 forked panic：`front_owner_index = UINT32_MAX` → `build_flushed_gens_by_front` panic |
| L-1 | low | 文本 | 更新 `owner_scheduler.hh` 顶部注释到 Phase 4 |
| L-2 | low | 1 行 | `const auto K = static_cast<uint32_t>(rs.pinned_gens.size());` |

**已确认 Codex 在第二轮提交中修复了 H-1、M-1、L-1、L-2。** 当前 worktree 上这四条已不复存在。M-2（D17 sentinel panic 测试）状态需重新确认。以下第二轮 review 基于 Codex 最新代码。

---

## 5. 第二轮 Review — 效率 / 容量校准 / 收窄简化

> 校准标尺：INDEX.md 硬约束——10 亿 KV 起步，所有容器选型 / 算法复杂度 / 热路径成本按此基线。flush 是后台路径，不是前台热路径，但单次 fold 处理的 key 数量级是 1M-10M（gen 大小 1M-10M，K ≤ 8）。

### 5.1 Codex 第二轮做对了什么

对照前一轮 review，Codex 做了四项值得肯定的改进：

1. **`flush_key_group.winner_gen` 从 `shared_ptr` 改成 `uint32_t winner_pinned_gen_index`**（H-1 修复同时解决了效率问题）。旧版 shared_ptr 每条 workset entry 要一次 atomic refcount bump（`shared_ptr` 拷贝），10M entries = 10M 次 atomic 操作 ≈ 100-200ms。新版 uint32_t 零原子操作。这是 carrier 形态变更，但方向完全正确——workset 是纯 borrowed-view carrier，round_state.pinned_gens 已持住全部 gen 生命周期，额外持 shared_ptr 是冗余开销。
2. **`cursors` 从 `std::vector` 改为 `absl::InlinedVector<gen_cursor, 8>`**——K ≤ 8 时零 heap allocation。
3. **`entry_ref` 加 `std::string_view key`**——修 H-1 同时不增加额外 allocation（string_view 是 16B POD）。
4. **`staged_memtable_losers` 末尾 `std::sort` 保证 per-gen 的 data_ver 升序**——为 FF §7.3 `deferred_value_reclaim` 的 FIFO front-stop scan 预做保序。推理链成立：全局按 data_ver 排序 → 按 gen 过滤的子序列保持升序 → Phase 7 commit 按遍历顺序 push → gen 释放时 drain 保持 push 顺序 → 进入 deferred_value_reclaim 时有序 → front-stop 不 stall。

### 5.2 Findings — 效率 / 容量

#### E-1（medium）: `workset` 和 `staged_memtable_losers` 缺 `reserve()`

**位置**：`memtable_fold.hh:49-162`，整个 `fold_pinned_gens` 函数

两个 vector 在 fold 期间做纯 push_back，无 reserve。容量估算：

| 场景 | workset 大小 | 每条 ~64B | 峰值内存（含 realloc 2×）| staged losers（最坏 N×(K-1)）|
|------|-------------|-----------|--------------------------|------------------------------|
| 典型：1M keys, K=2 | 1M | ~64 MB | ~128 MB | ~32 MB |
| 压力：10M keys, K=4 | 10M | ~640 MB | ~1.28 GB | ~960 MB |
| 极端：10M keys, K=8 | 10M | ~640 MB | ~1.28 GB | ~2.24 GB |

vector 无 reserve 时 amortized doubling 导致：
- ~23 次重分配（log₂(10M)）
- 最后一次 realloc 复制 ~640 MB 的 `flush_key_group`（现在无 shared_ptr，是 trivially relocatable，纯 memcpy）
- `staged_memtable_losers` 的最后一次 realloc 复制 ~1-2 GB
- 峰值内存是最终大小的 2×（旧 + 新同时存在）

**修复**：fold 开头加两行 O(K) 上界估算：

```cpp
std::size_t total_entries = 0;
for (const auto& g : rs.pinned_gens)
    total_entries += g->table.size();
rs.workset.reserve(total_entries);
rs.staged_memtable_losers.reserve(total_entries);  // pessimistic but safe
```

`total_entries` 是唯一 key 数的上界（跨 gen 有 overlap 时实际更少），对 workset 绰绰有余。对 staged_losers 偏悲观（大部分 key 只出现在一个 gen 里，没有 loser），但 reserve 只分配不初始化，多分配的只是虚拟地址空间。

这不是"优化"——是容量校准。10M-key flush round 在无 reserve 时的 peak = 2× final size + sort 临时空间，可能逼近 4-5 GB 单纯用于 vector realloc；加 reserve 后 peak = final size，~1.5 GB。差距在 10 亿 KV 基线下不可忽略。

#### E-2（medium）: `std::sort` 的 O(N·K·log(N·K)) 代价需要标注为"已知成本"

**位置**：`memtable_fold.hh:150-161`

sort 推理链成立（§5.1 第 4 点已确认），但代价不容忽视：

| 场景 | staged losers 数 | sort 比较次数 | 估算耗时（~1ns/compare）|
|------|-------------------|---------------|------------------------|
| 典型：1M keys, K=2, 50% overlap → 500K losers | 500K | ~10M | ~10 ms |
| 压力：10M keys, K=4, 50% overlap → 15M losers | 15M | ~360M | ~360 ms |
| 极端：10M keys, K=8, 100% overlap → 70M losers | 70M | ~1.8G | ~1.8 s |

极端 case 的 1.8s sort 占 fold 总耗时（plan 估计 100ms-1s merge）的 1-2 倍。这不是 bug（sort 语义上正确），但必须在注释或 plan 里标注为已知成本，否则后续 Phase 7 做性能调优时会误以为 fold 本身慢。

如果将来 Phase 7 对 loser 的消费方式不需要全局排序（比如按 gen 分组 drain 时本地排序即可），可以把 sort 推迟到 Phase 7 commit，或者改为 per-gen 稳定顺序（fold 过程中 key 升序 → gen 内 loser 天然按 key 序 → data_ver 不保序但可以在 commit 时 per-gen sort，规模缩小 K 倍）。当前做法不 wrong，但在注释里应标注代价和替代路径。

#### E-3（low）: `flush_key_group` carrier 形态变更需要 doc-sync

**位置**：`flush_types.hh:87-93`

`shared_ptr<memtable_gen> winner_gen` → `uint32_t winner_pinned_gen_index` 是效率正确的改动（§5.1 第 1 点），但这是 **Phase 3 冻结的 carrier 的形态变更**。023 设计文档 §5 行 446 说"Phase 3 不实现 fold；只保证 shape 落地后 Phase 4 不需要再改"。三处文档仍引用旧形态：

| 出处 | 旧文本 |
|------|--------|
| `flush_types.hh:73`（注释已更新） | ✓ 已更新 |
| `flush_and_frontier_switch.md:134-138` | `owning_gen` — **未更新** |
| `024 plan §4.1 行 220` | `winner_gen: 所属 gen 的 shared_ptr 副本` — **未更新** |

这些文档引用 `winner_gen` 作为 shared_ptr 的地方需要改成 `winner_pinned_gen_index` + "resolve via `round_state.pinned_gens[idx]`"。

### 5.3 确认无收窄简化

逐条对照 CLAUDE.md 约束 A/B/C：

| 约束 | 检查 | 结论 |
|------|------|------|
| **A — 收窄必须显式 fail-fast** | fold 不假设固定 gen 数、固定 key 分布、固定 entry 数。无 hardcoded 深度/宽度上限。空 gen、空 workset 都有显式路径。| ✓ 无收窄 |
| **B — 通用命名对应通用语义** | `fold_pinned_gens` 处理任意 K、任意 N、任意 value/tombstone 组合。`build_key_partitions` 处理任意 N 和 lookup_count 组合。| ✓ 命名与语义匹配 |
| **C — 设计缺口禁止自行补 spec** | sort 的依据是 FF §7.3（设计文档已冻结）。carrier 形态变更有明确效率理由。无"凭直觉补 spec"的痕迹。| ✓ 无自行补 spec |

### 5.4 确认无 10 亿 KV 下的结构性缺陷

| 检查项 | 结论 |
|--------|------|
| 算法复杂度 | O(N×K) merge + O(L·logL) sort，L ≤ N×(K-1)。无 O(N²) 或 per-key heap allocation。|
| 容器选型 | workset/staged_losers 用 `std::vector`（连续内存，cache 友好）。cursor 用 `InlinedVector<8>`（零 alloc）。all_entries 用 `InlinedVector<4>`（99%+ 零 alloc）。|
| 热路径干扰 | fold 在 `tree_sched::advance()` 内同步执行。tree_sched 是 singleton on cores[0]，与 value_alloc_sched 同核。10M-key fold 耗时 ~100ms-1s 会阻塞 cores[0] 上的其他 scheduler advance。这是 Phase 4 的已知约束——Phase 5+ 将 fold 后的 fanout 变为异步，但 fold 本身目前是同步的。|
| 内存峰值 | 无 reserve 时 ~2× final（E-1）。加 reserve 后 ~1× final。10M-key round 约 1.5 GB workset + losers + partition spans。可接受：flush 是后台任务，10B KV 系统的内存预算远大于此。|

---

## 6. 最终修复优先级（合并四轮）

| # | 级别 | 状态 | 说明 |
|---|------|------|------|
| H-1 | high | **✓ 已修复** | key 来源 + carrier 形态改为 index |
| M-1 | medium | **✓ 已修复** | sealed_gens 为空 ok 路径 new_manifest |
| M-2 | medium | **✓ 已修复** | D17 sentinel panic forked 测试 |
| E-1 | medium | **✓ 已修复** | workset 加 `reserve(total_entries)` |
| E-2 | high | **✓ 已修复** | 删除 staging + sort，loser 直推 gen + fold 开头 clear 保幂等 |
| E-3 | low | **✓ 部分修复** | FF §3.3 伪码已更新；**残留 4 处过期注释引用 staged_memtable_losers（见 §9 R-1）** |
| E-4 | medium | **✓ 已修复** | back() = gen-local winner，前 n-1 条 = intra-gen loser，消除 all_entries |
| L-1 | low | **✓ 已修复** | 顶部注释 |
| L-2 | low | **✓ 已修复** | uint32_t cast |

---

## 9. 第四轮 Review — E-2/E-4 落地后全量确认

### 9.1 验证执行

1. `cmake --build build -j$(nproc)` — 编译通过
2. `./build/inconel_test_flush_carriers` — 全部通过，含：
   - fold 11 case（新增 direct push + idempotency re-fold）
   - panic 8 case（新增 D17 sentinel front_owner_index）
   - SFINAE **负向断言** `staged_memtable_losers` 不存在（确认 E-2 删除干净）
3. `inconel_test_runtime` / `inconel_test_tree_value` / `inconel_test_tree_lookup` — 不退化

### 9.2 E-2 落地确认

`memtable_fold.hh` 完全重写：

- **staging 删除**：`flush_round_state` 无 `staged_loser_entry`、无 `staged_memtable_losers`。测试用 SFINAE 负向断言锁住。
- **sort 删除**：无 `std::sort`、无 `#include <algorithm>`。
- **直推 gen**：loser 在 fold 循环内直接 `gen.loser_durable_refs.push(rvr)`。
- **幂等 clear**：fold 开头 `gen->loser_durable_refs.clear()` 对每个 pinned gen。
- **retire_list::clear()**：`core/memtable.hh` 新增 `clear() noexcept` 方法。
- **reserve**：fold 开头 `workset.reserve(total_entries)` 消除 vector realloc。

### 9.3 E-4 落地确认

- **`all_entries` InlinedVector 消除**：替换为 `candidates` InlinedVector<gen_candidate, 8>（栈固定，循环外分配，循环内 clear）。
- **back() = gen-local winner**：`candidates.push_back({ gi, &entries_vec.back(), gen_key })`，O(1)。
- **intra-gen losers 直推**：`for i in [0, n-1)` 直接 push value loser 到 gen，零比较。
- **跨 gen 比较只在 candidates 之间**：O(K) per key，K ≤ 8。

### 9.4 Finding

#### R-1（low）: 4 处注释/文档仍引用已删除的 `staged_memtable_losers`

代码中已无 `staged_memtable_losers` 的实际使用（SFINAE 负向断言确认），但以下注释/文档未同步：

| 文件 | 行 | 过期内容 |
|------|---|---------|
| `owner_scheduler.hh` | 17 | "Losers staged on `round_state.staged_memtable_losers` (D16)" |
| `owner_scheduler.hh` | 347 | "destruction discards staged_memtable_losers — zero external side effect" |
| `flush_types.hh` | 84 | "`round_state.staged_memtable_losers` by the fold step (Phase 4, D16)" |
| `flush_and_frontier_switch.md` | 133, 140, 150 | "stage → round_state.staged_memtable_losers"、"Phase 7 commit: staged_memtable_losers → each gen's loser_durable_refs" |

**修复**：将这些引用改为描述直推行为——"Losers pushed directly into each gen's `loser_durable_refs` during fold; `clear()` at fold start ensures idempotency. See review E-2 §7."

`flush_and_frontier_switch.md` 的伪码（已部分更新为 gen-local winner 算法）需要把 "stage → round_state.staged_memtable_losers" 改为 "push → gen.loser_durable_refs"，并删除 "Phase 7 commit: staged_memtable_losers → ..." 这行。

### 9.5 总结

E-2 / E-4 落地后的 `fold_pinned_gens` 干净且高效：

- **零额外数据结构**：无 staging carrier、无 sort、无 all_entries 临时缓冲
- **直推 + clear 幂等**：loser 处理从 4 个组件缩成 2 行
- **利用 InlinedVector 有序性**：intra-gen loser 零比较，跨 gen 比较 O(K)
- **workset reserve**：消除 10M-key 场景的 ~23 次 vector realloc

唯一残留是 R-1（4 处过期注释），low severity，不影响正确性。

---

## 7. E-2 升级 — 删除 staging，loser 直推 gen + fold 开头 clear

### 7.1 现状

当前 loser 处理涉及四个组件：

1. `flush_round_state::staged_loser_entry` struct（carrier 定义）
2. `flush_round_state::staged_memtable_losers` vector（staging 容器）
3. `std::sort(staged_memtable_losers, by data_ver)`（fold 末尾）
4. Phase 7 `finish_flush_round` 遍历 staging → commit 到各 gen 的 `loser_durable_refs`（尚未实现）

### 7.2 三条 sort 无效的理由

1. **`loser_durable_refs` 是 `retire_list`（append + drain）**，对推入顺序无要求。gen 释放时 drain 逐条做 §7.2 判定，不做 front-stop。
2. **`deferred_value_reclaim` 队列（FF §7.3）的 front-stop scan 需要全局 data_ver 有序**，但该队列接收来自多个 gen 释放、多个 flush round 的 entries。即使每个 gen 的 losers 内部完美排序，**跨 gen 释放的时间交错会打破全局有序**——fold 阶段排序保证不了下游队列有序。
3. **如果 §7.3 真需要全局有序**，正确做法是把 `deferred_value_reclaim` 改成 min-heap（按 data_ver），这是 §7.3 的设计问题，不是 fold 能用 sort 兜的。

### 7.3 staging 无必要的理由

D16 staging 的设计前提是"flush 失败时 loser 不应推入 gen，防止 double-push"。但实际上：

1. **flush 失败时 gen 不释放**。frontier_switch 不发生 → gen 的 shared_ptr 不归零 → `loser_durable_refs` 不 drain → 推了也不会触发回收 → 无害。
2. **生产环境 flush 失败 = 引擎重启**（NVMe I/O 失败是 fatal）。recovery 从 WAL + tree 重建一切，旧进程的内存对象（包括 gen 及其 loser_durable_refs）随进程消失。不存在"同一 gen 被提交给第二个 flush round"的场景。
3. **即使理论上同一 gen 被重新 fold**（例如开发阶段 `unsupported_unimplemented` 后重试），只要 fold 开头 clear 该 gen 的 `loser_durable_refs`，就能保证幂等——新 fold 的 losers 覆盖旧的，不会 double-push。

### 7.4 改进方案

```text
fold 开头（在主循环之前）：
    for each gen in pinned_gens:
        gen->loser_durable_refs.clear()          ← 幂等保证

fold 主循环中识别 loser 时：
    gen->loser_durable_refs.push(rvr)            ← 直推，一行
```

**删除**：
- `flush_round_state::staged_loser_entry` struct
- `flush_round_state::staged_memtable_losers` vector
- `std::sort(staged_memtable_losers, ...)`
- Plan 024 中 D16 staging 设计及所有引用
- Phase 7 `finish_flush_round` 中的 staging commit 逻辑（尚未实现，不需要实现了）

**保留**：
- `retire_list<retired_value_ref> loser_durable_refs`（gen 上已有，Phase 1 落的）
- fold 对 loser 的识别逻辑（不变）
- gen 释放时 drain + §7.2 判定（不变）

### 7.5 效果

| | 当前 | 改进后 |
|---|---|---|
| carrier | `staged_loser_entry` + flat vector | 无新增（复用 gen 已有的 `retire_list`） |
| fold loser 推入 | `staged_memtable_losers.push_back({idx, rvr})` | `gen->loser_durable_refs.push(rvr)` |
| sort | O(L log L) | **删除** |
| Phase 7 commit | 遍历 staging → dispatch to gen | **删除**（已经在 gen 里了） |
| 幂等性 | 靠 staging 丢弃保证 | 靠 fold 开头 clear 保证 |
| flush 失败安全性 | staging 随 round_state 销毁 | gen 不释放 → losers 不 drain → 无害 |
| 10M losers 内存 | ~320 MB (staged_loser_entry × 10M) | **0**（loser 直接在 gen 的 retire_list 里，无额外分配） |

---

## 8. E-4 详述 — fold 未利用 gen 内 entry 有序性

### 8.1 背景

每个 gen 的 `btree_map<key, InlinedVector<memtable_entry, 1>>` 有两层有序保证：

1. btree_map 按 key 升序——fold 的 K-way merge 已经利用了这一层。
2. **InlinedVector 内同 key 的 entries 按 data_ver 严格递增**——fold 完全没利用这一层。

### 8.2 现状

`memtable_fold.hh:100-120` 对每个 gen 的 `entries_vec` 做全量遍历：

```cpp
const auto& entries_vec = cursors[gi].it->second;
for (const auto& e : entries_vec) {           // 遍历全部 M 条
    entry_ref ref{ .gen_index = gi, .entry = &e, .key = gen_key };
    all_entries.push_back(ref);               // 全部收集
    if (...) winner = ref;                    // 逐条比较
}
```

然后在 step (c) 再遍历 `all_entries`，逐条和 winner 指针比对来判定 loser：

```cpp
for (const auto& ref : all_entries) {
    if (ref.entry == winner.entry) continue;  // 指针比较
    if (ref.entry->k == kind::value) { ... }
}
```

一个 key 如果在单 gen 内有 M 条 entry（hot key 被同一 gen 的 M 个 batch 更新），当前代码做 M 次 winner 比较 + M 条 entry_ref 收集 + M 次 loser 判定 = 3M 次操作。

### 8.3 可利用的不变量

InlinedVector 内 data_ver 严格递增意味着：

- **`entries_vec.back()` 一定是该 gen 在该 key 上的 local winner。** 不需要扫描。
- **`entries_vec[0..size()-2]` 一定全是 intra-gen loser。** 不需要和任何 winner 比较就能确定——它们在 gen 内就已经输了。

### 8.4 改进后的 fold 内循环（结合 E-2 直推）

```text
对每个持有 min_key 的 gen gi：
    entries_vec = cursors[gi].it->second
    gen = pinned_gens[gi]

    // (1) intra-gen losers：前 n-1 条直推 gen，O(n-1)，零比较
    for i in [0, entries_vec.size() - 1):
        if entries_vec[i].kind == value:
            gen->loser_durable_refs.push({ entries_vec[i].vh.durable, entries_vec[i].data_ver })

    // (2) gen-local winner：back()，O(1)
    gen_candidates.push_back({ gi, &entries_vec.back(), gen_key })

    ++cursors[gi].it

// (3) 跨 gen 比较：只在 K 个 gen-local winner 之间选，O(K)
global_winner = max_by_data_ver(gen_candidates)

// (4) 输掉跨 gen 比较的 gen-local winner 是 inter-gen loser，直推对应 gen
for candidate != global_winner:
    if candidate.kind == value:
        pinned_gens[candidate.gi]->loser_durable_refs.push(candidate 的 rvr)

// (5) 产出 flush_key_group
```

### 8.5 效果

| 场景 | 当前 | 改进后 |
|------|------|--------|
| hot key 100 entries/gen, K=4 → M=400 | 400 次 winner 比较 + 400 条 entry_ref + 400 次 loser 判定 | **4 次**跨 gen 比较，396 条 loser 直推（零比较） |
| 典型 key 1 entry/gen, K=4 → M=4 | 4 次比较 | 4 次比较（相同） |
| `all_entries` InlinedVector | 每个 key 构造+析构 | **消除** |
| loser 推入 | 收集到 staging → 末尾 sort → Phase 7 commit | **fold 内直推 gen，一步到位** |
