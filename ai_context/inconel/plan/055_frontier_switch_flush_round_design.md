# 055 — Frontier Switch / flush_round_once 详细设计

> Step 1 of the background-lifecycle mainline（稳态后台环）。
> 对应 known_issues：INC-022（tree flush handle 整套）的 **coord 编排子任务**部分。
> 设计角色文档；实现阶段禁止读测试文件。

---

## 0. 一句话

把"已存在但只被测试 harness 手动驱动"的 `tree_local_flush` 接成生产可调用的
`flush_round_once` 操作：在 coord 上补 `capture_flush_frontier` / `frontier_switch` /
`end_flush_round` 三个 handle，把 flush 产出的 `new_manifest` 安装进新的 `CAT2`（让
live reader 经 guard 看到已 flush 的 tree），把 `retired` 挂到旧 guard `G0`，并 fan-out
`release_gens` 收掉前端 imms。**本 step 不做物理 reclaim**（TRIM / value recycle）。
seal 与 flush 通过统一 `catalog_update_in_progress_` 串行（§6）。

---

## 1. 范围（Scope）

### 1.1 本 step 做什么

1. coord 新增 `capture_flush_frontier()` handle —— pin 当前 tree guard + 读 durable_lsn 作为本轮 frontier；置位 `catalog_update_in_progress_`。
2. coord 新增 `frontier_switch(old_guard, new_manifest, retired, release_plan)` handle —— 构造 `G1 / PRS2 / CAT2` 并 `install_cat(CAT2)`，把本轮 `retired` append 到 `G0.retired`（阶段 A 构造／阶段 B 提交两段式，§5.3）。
3. coord 新增 `end_flush_round()` handle —— 清 `catalog_update_in_progress_`（在 `release_gens` 之后）。
4. **（B4）改 coord 既有 `handle_close_gate` / `handle_open_gate`**：分别取/放同一 `catalog_update_in_progress_`，使 seal 与 flush 互斥（§6.1b）。
5. 新增 `pipeline/flush_round.hh`：`flush_round_once(coord, fronts, tree)` 顶层 sender，对照 cross_doc §5 flush 路径编排：
   ```
   capture_flush_frontier
     → fan-out collect_eligible_gens → reduce
     → [无 eligible gen → end_flush_round (no-op, B3)]
     → tree_local_flush
     → frontier_switch (install CAT2)
     → fan-out release_gens → reduce
     → end_flush_round
   ```
6. coord_state 增加统一串行化字段 `bool catalog_update_in_progress_`（覆盖 flush-vs-flush + flush-vs-seal）。
7. core 新增 `flush_frontier` / `flush_release_plan` / `flush_noop` carrier（§5.1）。

### 1.2 本 step **明确不做**（fail-fast / 留后续 step）

| 不做项 | 理由 | 去向 |
|---|---|---|
| 物理 reclaim（`G0` 析构 → enqueue `reclaim_task` → TRIM 旧 slot/range + `value::reclaim_values`） | 依赖 `retired` 已正确挂上（本 step 完成）+ `recovery_safe_lsn` 的 WAL 维度 | **step 2** |
| `checkpoint_guard` 析构器 reclaim 投递 | 同上；本 step 只保证 `retired` 内容正确累积在 `G0`，析构器保持 default | **step 2** |
| `memtable_gen.loser_durable_refs` 的 drain / value 回收 | 同上 | **step 2** |
| `recovery_safe_lsn` 推进（RSM §4.9 的 `min(flush_max_lsn, superblock_safe_lsn, wal_frontier)`） | 推进点要接 `wal_space_sched` 的 sealed-segment frontier；本 step 无 reclaim，不需要推进 | **step 2** |
| INC-054 tree allocator push-floor / ENOSPC backpressure | reclaim 把 retired range 还给 allocator 后才有真实推进点 | **step 3** |
| flush pipelining（多轮并发 flush） | 本 step 端到端串行 flush round（见 §6）；pipelining 需 per-gen in-flight 追踪 | future opt |
| 背景自动触发策略（"何时该 flush"） | 本 step 把 `flush_round_once` 做成显式可调用操作；触发器是独立小步 | **step 1.5 / 随 step 2** |

**容量影响声明（10 亿 KV 硬指标对账）**：step 1 单独 land 后，每轮 flush 的旧
slot/range/value 盘空间**不回收**（只在内存里挂到 `G0.retired`，`G0` 析构时这些
`InlinedVector` 直接释放但不触发 TRIM/recycle）。因此 step 1 与 step 2 必须近距离
连续 land，且 step 1 的验证 harness **不得**跑"长时高 churn 期望盘空间稳定"的容量
压力测试（那是 step 2 的验收项）。这是**开发顺序切分**，不是 v1 ship 边界——按
INDEX "阶段拆分是开发顺序工具，不是 v1 ship 哪些的划分线"，step 1+2 合起来才是
frontier-switch+reclaim 这条能力的完整交付。

---

## 2. 背景对账（spec 完整度 + 当前代码缺口）

### 2.1 spec 已精确定义（本 step = 转写，不是自创）

- **RSM §2.3** 给出 `capture_flush_frontier` / `frontier_switch` 完整伪码。
- **cross_doc_contracts §1** 给出两 handle 签名：
  - `capture_flush_frontier()` → `flush_frontier { durable_lsn, old_guard }` [RSM §2.3, FF §2.2/8.1]
  - `frontier_switch(old_guard, new_manifest, retired, flushed_gens_by_front)` → `void` [RSM §2.3, FF §4.2/8.1]
- **cross_doc_contracts §5** 给出完整 flush 路径：
  ```
  tree_sched(check_trigger_conditions)
    → coord_sched(capture_flush_frontier)
    → fan-out front_scheds(collect_eligible_gens) → reduce
    → tree_sched(tree_flush)
    → tree_sched(update_flush_max_lsn)
    → coord_sched(frontier_switch)
    → fan-out front_scheds(release_gens)
  ```
- **FF §4.1/§4.2/§4.3** 给出 `G1 / PRS2 / CAT2` 构造与 release_gens 语义。
- **FF §5** 给出 retired 双 hook（不合并）。
- **FF §6** 给出 root-stable / root-change superblock 时机。
- **FF §7** 给出引用计数归零回收链（本 step 不实现，但 retired 累积必须为它铺好地基）。

### 2.2 当前代码现状（已核对）

| 组件 | 现状 | file:line |
|---|---|---|
| `coord_sched` handles | assign / publish / release / read / close_gate / install_cat / open_gate / enter_memtable_phase；**无任何 flush/frontier/capture handle** | `coord/scheduler.hh:272-297` |
| `catalog_store cats_` | `current_cat()` / `install_cat()` / `acquire_read_handle()`；atomic `shared_ptr<const publish_catalog>` | `core/read_catalog.hh:65-97` |
| `publish_catalog` | `{prs, atomic durable_lsn, epoch}` | `core/read_catalog.hh:26-58` |
| `published_read_set` | `{tree_guard: shared_ptr<checkpoint_guard>, fronts: shared_ptr<const vector<front_read_set>>, epoch}` | `core/read_catalog.hh:20-24` |
| `checkpoint_guard` | `{manifest: shared_ptr<const tree_manifest>, retired: retired_objects}`；**default 析构，retired 字段已就位** | `core/checkpoint_guard.hh:44-51` |
| `retired_objects` | `{old_slots[32], old_ranges[8], old_tree_values[64]}`（InlinedVector） | `core/retired_objects.hh:41-58` |
| `memtable_gen` | `{gen_id, st, front_owner_index, min_lsn, max_lsn, kv_arena, table, loser_durable_refs}`；**无 in-flight 标志** | `core/memtable.hh:198-219` |
| front `seal_active()` | → `_front_seal::sender`（返回 `front_read_set`） | `front/scheduler.hh:282` |
| front `collect_eligible_gens(uint64_t durable_lsn)` | → `_front_collect::sender`（返回 eligible `shared_ptr<memtable_gen>[]`） | `front/scheduler.hh:285` |
| front `release_gens(std::vector<uint64_t> gen_ids)` | → `_front_release::sender` | `front/scheduler.hh:288` |
| `tree_local_flush(tree_flush_request)` | 已实现完整 pipeline：fold → worker → merge → 写盘 → device flush →（root_change 时异步 superblock）→ `rebuild_and_publish_shard_partitions` → 返回 `tree_flush_result` | `tree/sender.hh:780` |
| `tree_flush_request` | `{base_guard, sealed_gens: span<shared_ptr<memtable_gen>>, recovery_safe_lsn}` | （RSM §4.2 / 代码） |
| `tree_flush_result` | `{st, new_manifest, retired, flushed_gens_by_front: flat_hash_map<uint32_t, InlinedVector<shared_ptr<memtable_gen>,8>>, flushed_max_lsn}` | `tree/flush_types.hh:270-279` |

**缺口结论**：tree 侧 flush body（含 manifest / shard-partition publish / root-change
superblock）已就位；front 侧 collect/release 已就位；缺的只有 **coord 三个 handle
（capture_flush_frontier / frontier_switch / end_flush_round）+ 顶层 `flush_round_once`
编排 + 统一串行化标志 `catalog_update_in_progress_`（含对 seal 的 close_gate/open_gate
两个既有 handle 的小改动，§6.1b）**。这正是本 step 的全部范围。

### 2.3 与既有 `seal_round.hh` 的对称性

frontier_switch 的 CAT2 构造与 seal 的 `build_cat1`（`seal_round.hh:68-90`）同构，但有三处关键差异：

| 维度 | seal `build_cat1` | flush `frontier_switch` |
|---|---|---|
| `tree_guard` | **不变**（沿用 `old_cat->prs->tree_guard`） | **切到 G1**（new_manifest） |
| fronts | active→sealed、加新 active（seal_active 产出） | active 不变、imms **移除**已 flush gens |
| `durable_lsn` 来源 | gate **关闭**期间读 old_cat（安全，发布暂停） | gate **不关**，必须在 coord 线程**安装瞬间**读当前 cat 的 D1（见 §5.3） |

---

## 3. 三阶段文档检查（按 nitro 规则强制）

### 3.1 实现前（已做）

- 读 INDEX.md 定位表："Seal 流程→runtime_state §2.5/§3.6"、"Flush 流程→flush 全文 + runtime_state §4"。
- 完整读 `flush_and_frontier_switch.md`（§3-§9）、`runtime_state_machine.md`（§2/§3/§4/§9/§10）、`design_overview.md`（§6/§9）。

### 3.2 实现中（回查点）

- handle 签名 → 回查 cross_doc §1（capture_flush_frontier / frontier_switch / collect_eligible_gens / release_gens / install_cat 行）。
- CAT2/PRS2/G1 struct 字段 → 回查 `core/read_catalog.hh` + `core/checkpoint_guard.hh` 实际定义，**不漂移**。
- pipeline 路径 → 回查 cross_doc §5 flush 路径，逐跳对齐。
- LSN 语义（D1 安装瞬间读 / recovery_safe_lsn 不推进）→ 回查 OV §6 + RSM §4.9。

### 3.3 实现后（验收对照 cross_doc_contracts）

- §1 handle 签名一致。
- §2 struct 字段一致（publish_catalog / published_read_set / checkpoint_guard / retired_objects）。
- §5 flush pipeline 路径无遗漏跳转。
- **三条红线复核**（§4）。

### 3.4 三条红线复核

- **读路径红线**：frontier_switch 只换 `CAT` 指针（`install_cat` 原子 store），新 reader 经 `acquire_read_handle` 拿 CAT2 snapshot，旧 reader 继续持 CAT1 snapshot。**不**让读路径访问 coord/front/tree 的当前可变状态——✅ 不违反。
- **tree 运行时红线**：本 step 不碰 tree slot 扫描 / 不改 "每 range 多 slot" 语义 / 不把 flush 理解为"重写回 slot 0"。frontier_switch 只消费 `tree_local_flush` 产出的 `new_manifest`，不做 tree 结构动作——✅ 不违反。
- **recovery 红线**：本 step 不碰 recovery——✅ 不适用。

### 3.5 Shadow CoW 顶层不变量（A / B）复核

frontier_switch / flush_round_once **不在 tree 写侧**（不做 cascade / reformat /
assign_paddr），只消费 `tree_local_flush` 已产出的 `new_manifest`。不变量 A（不级联）
/ B（child_base 是 range_base）由 `tree_local_flush` 内部保证，本 step 不触及。
**与 A/B 的兼容性**：本 step 零 tree 写侧改动，A/B 不受影响。

---

## 4. 顶层 pipeline：`flush_round_once`

落点 `apps/inconel/pipeline/flush_round.hh`，结构对照 `seal_round.hh` / `point_get.hh`。

```cpp
// 伪 PUMP 编排（最终以实现为准；context struct 持 round 状态）
[[nodiscard]] inline auto
flush_round_once(coord::coord_sched& coord_sched,
                 std::span<front::front_sched* const> fronts,
                 tree::tree_sched& tree_sched /* 或 rt::owner() 句柄 */) {
    return coord::capture_flush_frontier(coord_sched)
        // frontier = { durable_lsn, old_guard }；catalog_update_in_progress 已被置位
        >> push_result_to_context()
        >> get_context<core::flush_frontier>()
        >> flat_map([fronts, &coord_sched](core::flush_frontier& fr) {
            return just()
                // ── 1. fan-out 收 eligible gens（按 frontier.durable_lsn）──
                >> loop(fronts.size()) >> concurrent(fronts.size())
                >> flat_map([fronts, &fr](std::size_t i) {
                    return front::collect_eligible_gens(
                               *fronts[i], fr.durable_lsn);
                })
                >> to_vector< /* per-front eligible gen lists */ >()
                // ── 2. B3：无 eligible gen → no-op 分支；否则建 request ──
                >> then([&fr](auto&& per_front_gens)
                        -> std::variant<flush_noop, core::tree_flush_request> {
                    if (total_eligible(per_front_gens) == 0)
                        return flush_noop{};                      // 没东西可 flush
                    return build_tree_flush_request(fr, per_front_gens);
                    // base_guard=fr.old_guard; sealed_gens=扁平化;
                    // recovery_safe_lsn=0（保守，见 §8）
                })
                >> visit()
                >> flat_map([fronts, &coord_sched, &fr]<typename R>(R&& r) {
                    if constexpr (std::is_same_v<std::decay_t<R>, flush_noop>) {
                        // B3：no-op round —— 不跑 tree flush、不装 CAT2、不动 imms，
                        // 仅清串行标志。绝不把 null new_manifest 送进 frontier_switch。
                        return coord::end_flush_round(coord_sched)
                            >> then([]() {
                                return flush_round_result{/* noop */};
                            });
                    } else {
                        return tree::tree_local_flush(std::move(r))
                            >> then([](tree::tree_flush_result&& tr) {
                                if (tr.st != flush_stage_status::ok)
                                    throw flush_round_error{tr.st};   // §7
                                if (tr.new_manifest == nullptr)        // B3 防御
                                    throw flush_round_error{
                                        flush_stage_status::
                                            unsupported_unimplemented};
                                return std::move(tr);
                            })
                            // ── 3. frontier switch（B1：release_plan by value）──
                            >> flat_map([&coord_sched, &fr](
                                            tree::tree_flush_result&& tr) {
                                // 先从 result 提 release_plan（by value carrier：
                                // front_idx → vector<gen_id>），再 move 其余字段；
                                // 不把 tree_flush_result 的引用塞进跨域 sender。
                                auto rplan = extract_release_plan(
                                    tr.flushed_gens_by_front);
                                return coord::frontier_switch(
                                           coord_sched, fr.old_guard,
                                           std::move(tr.new_manifest),
                                           std::move(tr.retired),
                                           rplan)            // by value（内部 subtract 用）
                                    >> forward_value(std::move(rplan));  // 传给 release_gens
                            })
                            // ── 4. fan-out release_gens（收前端本地 imms）──
                            >> flat_map([fronts](core::flush_release_plan&& rp) {
                                return just() >> loop(fronts.size())
                                    >> concurrent(fronts.size())
                                    >> flat_map([fronts, &rp](std::size_t i) {
                                        return front::release_gens(
                                                   *fronts[i], rp.gen_ids_for(i));
                                    })
                                    >> reduce();
                            })
                            // ── 5. 清串行标志 ──
                            >> flat_map([&coord_sched]() {
                                return coord::end_flush_round(coord_sched);
                            })
                            >> then([]() { return flush_round_result{}; });
                    }
                })
                >> flat();
        })
        >> any_exception_into_end_round(coord_sched)   // §7：异常也必须 end_flush_round
        >> pop_context();
}
```

> B1：`frontier_switch` / `release_gens` 都吃 `flush_release_plan`（by value 的
> `front_idx → vector<gen_id>` carrier），**不**借用 `tree_flush_result.flushed_gens_by_front`
> 这个 pipeline 局部对象的引用——跨 scheduler sender 不能持局部引用（`feedback_use_context_not_pointer`）。
> B3：无 eligible gen 走 `flush_noop` 分支（`variant + visit + if constexpr`，对齐 `point_get.hh`
> 模式），不进 tree flush / frontier_switch；非空但 `new_manifest==null` 时 fail-fast，
> 绝不把 null manifest 送进 `frontier_switch` 构造 G1。

**编排要点**：
- `capture_flush_frontier` 是起点（coord handle，返回值），不是 bind_back，符合 `feedback_bind_back_needs_prev`。
- fan-out 两处（collect / release）都 `concurrent(fronts.size()) + reduce`，扇出必扇入。
- `tree_local_flush` 原样复用，不改其内部（它已做 shard-partition publish + root-change superblock；root-change 的 superblock/CAT2 时序见 §8.B5 标注的现有 gap）。
- round 状态走 context struct（`flush_frontier` + 中间产物），不裸指针捕获（`feedback_use_context_not_pointer`）。
- **异常路径**必须保证 `end_flush_round` 一定执行（清 `catalog_update_in_progress_`），否则后续 seal/flush 永久被串行标志锁死（§7）。

---

## 5. coord 两个核心 handle 详细设计

新增 namespace（对齐现有 `_coord_*` 模式）：
```cpp
namespace _coord_capture_frontier { struct req; struct sender; }
namespace _coord_frontier_switch  { struct req; struct sender; }
namespace _coord_end_flush_round  { struct req; struct sender; }
```
public sender 方法（对齐 `coord/scheduler.hh:272-297` 风格）：
```cpp
[[nodiscard]] _coord_capture_frontier::sender capture_flush_frontier();
[[nodiscard]] _coord_frontier_switch::sender  frontier_switch(
    std::shared_ptr<core::checkpoint_guard>    old_guard,
    std::shared_ptr<const core::tree_manifest> new_manifest,
    core::retired_objects                      retired,        // by value/move
    core::flush_release_plan                   release_plan);  // by value（B1）
[[nodiscard]] _coord_end_flush_round::sender end_flush_round();
```
> **B1**：第 4 参数从 `const tree::flushed_gens_by_front_map&`（借用 tree 局部对象）改为
> `core::flush_release_plan` by value。`frontier_switch` 用它做 `subtract`（需要 per-front 的
> gen_id 集合），`release_gens` 也用它——同一 carrier，不持 `tree_flush_result` 引用。

coord_state 新增字段（**B4**）：
```cpp
// 统一串行化标志：seal 与 flush 都是 coord 上的 CAT 安装操作，必须互斥。
// capture_flush_frontier / close_gate 任一置位则另一方 fail-fast；
// end_flush_round / open_gate 清位。覆盖 flush-vs-flush + flush-vs-seal + seal-vs-seal。
bool catalog_update_in_progress_ = false;
```

### 5.1 carrier（core 新增）

落 `core/`（与 publish_catalog 同域）：
```cpp
struct flush_frontier {
    uint64_t                                 durable_lsn;   // 捕获瞬间发布前沿（定 eligibility）
    std::shared_ptr<core::checkpoint_guard>  old_guard;     // pin 住本轮 base tree snapshot
};

// B1：跨域 sender 的 by-value carrier，frontier_switch（subtract）与 release_gens 共用。
// 不持 tree_flush_result 引用。front_idx 与 fronts[] 下标一致（= memtable_gen.front_owner_index）。
struct flush_release_plan {
    std::vector<std::vector<uint64_t>> gen_ids_by_front;     // [front_idx] → gen_id 列表
    std::span<const uint64_t> gen_ids_for(std::size_t i) const {
        return i < gen_ids_by_front.size()
                 ? std::span<const uint64_t>(gen_ids_by_front[i]) : std::span<const uint64_t>{};
    }
};

// B3：no-op round 标记（无 eligible gen），走 variant 分支不进 tree flush。
struct flush_noop {};
```
> `old_guard` 用 `shared_ptr` pin，确保 fold / 写盘 / frontier_switch 全程旧 manifest
> 不被提前释放（FF §2.2）。`extract_release_plan(flushed_gens_by_front)` 从
> `map<front_idx, InlinedVector<shared_ptr<memtable_gen>>>` 提 `gen->gen_id` 构造此 plan。

### 5.2 `handle_capture_flush_frontier`

```text
1. if (catalog_update_in_progress_):                  // B4：seal 或 flush 在飞都拦
       cb(std::unexpected(catalog_update_in_progress)) // fail-fast，不 silent skip
       return
2. catalog_update_in_progress_ = true
3. cat = cats_.current_cat()
4. frontier = flush_frontier {
       durable_lsn = cat->durable_lsn.load(acquire),
       old_guard   = cat->prs->tree_guard,
   }
5. cb(frontier)
```
- **失败语义（B4）**：seal 或另一 flush 在飞 → 返回 `catalog_update_in_progress` 错误（串行 driver + seal/flush 不重叠则永不触发；重叠则 fail-fast 暴露契约违反，符合约束 A）。
- 注意：`cat->prs->tree_guard` 类型是 `shared_ptr<checkpoint_guard>`（拷贝即 +1 引用），pin 住 G0。

### 5.3 `handle_frontier_switch`（核心，对照 RSM §2.3）

```text
输入: old_guard, new_manifest, retired(by value/move), release_plan(by value)

// ── 阶段 A：只读 + 构造（可抛；此阶段不改任何已发布状态）──
1. cat = cats_.current_cat()                       // 安装瞬间当前 cat（publish 可能已推进）

2. 防御性校验（B4 串行兜底 + stale-guard）:
       if (cat->prs->tree_guard.get() != old_guard.get()):
           panic_inconsistency("frontier_switch: stale base guard; "
                               "seal/flush must be serialized")
   // catalog_update_in_progress_ 串行保证此处恒成立；panic 防未来状态机改动

3. D1        = cat->durable_lsn.load(acquire)       // 继承安装瞬间最新发布前沿（§5.3 裁决）
   new_epoch = cat->epoch + 1                        // 单调；与 install_cat 的 epoch 校验一致

4. G1 = make_shared<checkpoint_guard>{ manifest=new_manifest, retired={} }       // 可抛
   // PRS2.fronts：当前 cat 的 fronts 快照上移除已 flush gens（per-front gen_id 来自 release_plan）
   new_fronts = subtract_flushed_gens(cat->prs->fronts, release_plan)            // 可抛（建新 vector）
   PRS2 = make_shared<published_read_set>{ tree_guard=G1, fronts=new_fronts, epoch=new_epoch }  // 可抛
   CAT2 = make_shared<publish_catalog>{ prs=PRS2, durable_lsn=D1, epoch=new_epoch }             // 可抛

5. // B2：预留 G0.retired 三个 vector 的容量，使阶段 B 的 append 不再分配
   old_guard->retired.{old_slots,old_ranges,old_tree_values}.reserve(各自 size+本轮增量)  // 可抛(仅扩容)

// ── 阶段 B：提交（noexcept；reserve 后 append 不分配，install 是 atomic store）──
6. old_guard->retired.old_slots.append(retired.old_slots)        // 挂旧 G0（FF §4.1/§5.1/§5.2，唯一挂接点）
   old_guard->retired.old_ranges.append(retired.old_ranges)
   old_guard->retired.old_tree_values.append(retired.old_tree_values)
7. cats_.install_cat(CAT2)                          // 原子 store；旧 CAT 经 shared_ptr 延迟释放
   cat_epoch_ = new_epoch
8. cb()
```

> **B2（异常原子性）**：所有可抛构造（G1 / new_fronts / PRS2 / CAT2 / reserve）集中在阶段 A，
> 该阶段**不碰 `G0.retired`、不 install**。阶段 B 仅 append + install：reserve 后 append 不分配、
> `install_cat` 是 atomic store，二者 noexcept。于是"构造失败 → 不挂 retired、不装 CAT2、CAT
> 仍是 CAT1"，满足 FF 异常语义"失败不 install、不改 retire list"。coord 单线程，handler 内无
> 并发观察者，阶段 A/B 间无需中间态保护，只需保证异常不留半挂。

**§5.3 关键裁决点**：

- **D1 必须在 coord 线程、安装瞬间读**（步骤 5），不能在 pipeline lambda 里提前读。
  原因：flush **不关 publish gate**，capture 到 install 之间 `publish_batch` 持续推进
  `durable_lsn`。若 CAT2 继承 capture 时的旧 durable_lsn，会让已发布可见的 batch 在
  CAT2 下"消失"（可见性回退，违反 OV §8 "永不回退"）。继承当前最新 D1 才正确。
  （这也是 frontier_switch 必须是 coord handle、在 coord 单线程内 build+install 的根因，
  区别于 seal 的 build_cat1 可在 pipeline 里 build——seal 期间 gate 关、durable 不动。）

- **`subtract_flushed_gens(cur_fronts, release_plan)`**：对每个 front index，
  从 `cur_fronts[i].imms` 复制出移除了 `release_plan.gen_ids_for(i)` 里 gen_id 的新 imms，
  `active` 原样保留。产出新的 `vector<front_read_set>`（包进 `shared_ptr<const ...>`）。
  - 用**当前** cat 的 fronts（不是 capture 时的），因为 capture→install 间可能插入过 seal
    （seal 只**加**新 gen 到 imms，不删已 flush 的），已 flush gens 仍在当前 imms 中可被移除。
  - 这是 **PRS 级**移除（新 reader 不再经 imms 看到这些 gen，改经 G1 tree 看到）。
    front **本地** imms 的移除由后续 `release_gens` 完成——两者都需要（FF §4.3 + RSM §2.3
    "frontier_switch 不直接修改各 front_sched 本地 imms"）。

- **retired append 到 G0**（阶段 B 步骤 6）：`G0`（`old_guard`）此刻仍被 CAT1 的 PRS pin 着、
  可能也被在飞 reader pin 着。append 是在 coord 单线程内对 `old_guard->retired` 的写——
  `checkpoint_guard.retired` 在 guard "活跃发布期"只被 frontier_switch 写一次（本轮），
  之后 guard 进入只读退役期等析构。本 step 析构器仍 default（retired 内容随 G0 析构被
  释放，但**不** TRIM/recycle）；step 2 给析构器加 enqueue。

### 5.4 `handle_end_flush_round`

```text
1. catalog_update_in_progress_ = false
2. cb()
```
- 必须在 `release_gens` 完成之后执行（见 §6.2 为何不能在 frontier_switch 里清）。
- 异常路径也必须执行（§7）。

---

## 6. 串行化：catalog 更新互斥（flush-vs-flush + flush-vs-seal）

> coord 是**唯一**的 CAT 安装者：seal 装 CAT1、frontier_switch 装 CAT2。两类操作都
> 改 `cats_` + `cat_epoch_`，必须互斥。本 step 用统一标志 `catalog_update_in_progress_`
> 串行 **flush-vs-flush + flush-vs-seal + seal-vs-seal**。

### 6.1 flush-vs-flush（spec 缺口与裁决）

agent 核对：design_overview §9 **未显式冻结** "flush round 必须串行"；RSM §10.3 只说
tree_sched 单线程（tree 工作天然串行），但 fold→write→switch 跨 tree/front/nvme/coord
多域异步，**两个 `flush_round_once` pipeline 可被并发 submit**。同时 FF §5.5/§3.4 明确
**同一 gen 不得进入两个 in-flight round**（否则 `loser_durable_refs` 被两轮 clear/rebuild 竞争）。

**裁决（约束 C：spec 缺口显式定 + 写回）**：**本 step flush round 端到端串行**，
机制 = coord `catalog_update_in_progress_` 标志，`capture_flush_frontier` 置位（已置位则
fail-fast），`end_flush_round`（在 `release_gens` 之后）清位。

### 6.1b flush-vs-seal（B4：codex review 顺出的硬缺口）

**冲突链（已对代码验证）**：seal_round 在 `close_gate` 处捕获当时的 `old_cat`，经异步
`seal_active` fan-out 后才 `install_cat(CAT1')`，而 `build_cat1` 用的是**捕获时的
old_cat**（`seal_round.hh:69` `.tree_guard=old_cat->prs->tree_guard` / `old_cat->durable_lsn`）。
coord 单线程在 seal 的 `close_gate→install_cat` **异步窗口内会处理其它 event**——若一个
`frontier_switch` 插进来先装了 CAT2（`cat_epoch_` 推到 E+1），seal 的 `install_cat(CAT1')`
随后到达：`CAT1'.epoch = old_cat.epoch+1 = E+1 ≤ cat_epoch_` → `validate_replacement_cat`
（`coord/scheduler.hh:446`）抛 `"replacement CAT epoch must advance"` → seal 异常逃逸，
**gate 可能停在 closed（open_gate 没跑到）→ 前台写入永久阻塞**。

**裁决**：用同一个 `catalog_update_in_progress_` 串行 seal 与 flush。要求对 seal 既有
handle 做**小改动**（本 step 范围内、必须做，否则 flush 上线即破坏 seal）：

- `handle_close_gate`（seal 起点）：开头加 `if (catalog_update_in_progress_) → fail-fast`；
  成功则置位 `catalog_update_in_progress_ = true`（与现有 `gate_.close()` 并列）。
- `handle_open_gate`（seal 终点）：清位 `catalog_update_in_progress_ = false`（与 `gate_.open()` 并列）。

这样 seal 在飞时 `capture_flush_frontier` fail-fast、flush 在飞时 `close_gate` fail-fast，
两类 CAT 安装严格互斥；`frontier_switch` 永不插进 seal 窗口。seal-vs-seal 也顺带被它兜住
（原本只靠"close 已关的 gate 抛异常"兜）。

### 6.2 为什么清位必须在 release_gens 之后（不能在 frontier_switch 里清）

双重折叠风险链：
```
round N: frontier_switch 装 CAT2（gen 从 PRS imms 移除）→ [若此时清 catalog_update_in_progress]
round N+1: capture → collect_eligible_gens 读 front 本地 imms
           ← 但 round N 的 release_gens 还没跑完，gen 仍在 front 本地 imms
           → max_lsn ≤ durable_lsn 仍命中 → 同一 gen 被 round N+1 再次 fold
```
后果：同一 gen 被两轮 fold——非 UB（gen 已从 PRS 移除、readers 经 G1 tree 读，重折叠
产出近似空 delta），但浪费 + 语义混乱 + `loser_durable_refs` 被两轮 clear/rebuild 竞争。
**清位放在 `release_gens` 之后**（`end_flush_round`），保证下一轮 `collect_eligible_gens`
看到的 front 本地 imms 已剔除本轮 gens，杜绝重折叠。

### 6.3 driver 契约

本 step `flush_round_once` 是**显式可调用操作**，调用方（验收 harness / 后续触发器）
**必须串行调用**（await 上一轮完整 result 再发下一轮），且 seal 与 flush 不重叠发起。
`catalog_update_in_progress_` 是**防御兜底**——正常 driver 下永不阻塞；若 flush 之间、
或 flush 与 seal 重叠，则 `capture_flush_frontier` / `close_gate` fail-fast 暴露契约违反。

### 6.4 pipelined flush（future opt，非本 step）

放开串行需 per-gen in-flight 追踪（gen 上加 `flushing_by_round` 标志，collect 跳过在飞
gen）+ frontier_switch 的 stale-guard 校验升级为多轮 epoch 比较。本 step 不做，记为
future optimization。

---

## 7. 异常处理分层

按 CODING_GUIDE §6.5（操作级 → 子流程级 → 事务级 → 全局兜底）+ "资源释放在异常屏蔽之后"。

**最高约束：除 process-fatal panic 外，无论成功失败 `catalog_update_in_progress_` 必须被清**
（否则后续 seal/flush 永久锁死）。

| 失败点 | 分类 | 处理 |
|---|---|---|
| `capture_flush_frontier` 返回 `catalog_update_in_progress` | 操作级 | 没拿到 round（未置位）；driver 收到错误稍后重试，无需清位 |
| `collect_eligible_gens` 失败 | 事务级 | 已置位 → 经 `any_exception_into_end_round` 清位；本轮 abort，不装 CAT2 |
| `tree_local_flush` 返回非 ok / 抛异常 | 事务级 | 同上：不装 CAT2、不 retire、清位。**tree 侧已自回滚**已分配 range（owner rollback），coord 侧无副作用（未碰 CAT） |
| `frontier_switch` 内部异常 | 事务级 | **B2 已保证原子（§5.3）**：阶段 A（构造）异常 → 不碰 `G0.retired`、不 install → CAT 仍 CAT1，经清位 abort；阶段 B（append+install）是 noexcept，不会半挂 |
| `release_gens` 失败 | **process-fatal** | CAT2 已装但 front 本地 imms 与 PRS 不一致 = 不变量破坏 → **panic**（约束 A，不 silent 降级）。release_gens 是本地 imms 删除（无 I/O），正常不会失败；失败即 bug。进程重启，标志随进程消失 |
| capture..release_gens 之间其它 recoverable 异常 | 全局兜底 | `any_exception_into_end_round`：屏蔽异常后**强制**调 `end_flush_round` 清位，再 rethrow 给 driver |

> `end_flush_round`（清 `catalog_update_in_progress_`）必须在异常屏蔽之后（类 RAII finish），
> 对所有 recoverable 异常确保一定清位——这是 §7 的核心。`release_gens` 失败走 panic（process-fatal），
> 是唯一不经 `end_flush_round` 的路径，但进程已死，标志无意义。措辞统一：不存在"记录错误继续"的降级路径。

---

## 8. LSN 处理边界（landmine 2/3）

| LSN | 本 step 处理 | 依据 |
|---|---|---|
| `durable_lsn`（capture） | 读当前 cat，作为 `collect_eligible_gens` 阈值 | RSM §2.3 |
| `D1`（install） | 安装瞬间读当前 cat，继承进 CAT2，**可能 > capture durable_lsn** | §5.3 / OV §8 永不回退 |
| `flushed_max_lsn` | `tree_flush_result` 字段（本轮覆盖的 max gen.max_lsn） | FF §8.1 |
| `flush_max_lsn` | **`tree_local_flush` 已内部推进**：`state.flush_max_lsn = max(flush_max_lsn, flushed_max_lsn)`（`owner_scheduler.hh:2963-2964`）。**flush_round_once 无需独立 `update_flush_max_lsn` 跳**——cross_doc §5 路径里的该跳在当前实现中已由 `tree_local_flush` 内联完成 | RSM §4.1 / owner_scheduler.hh:2963 |
| `superblock_safe_lsn` | **`tree_local_flush` 已内部推进**：`state.superblock_safe_lsn = max(..., flushed_max_lsn)`（`owner_scheduler.hh:2965-2966`）。root-change 的 superblock FUA 也由 tree_local_flush 内部做——但**时序见 §8.B5**。本 step coord 侧不碰 | FF §6.1/§6.2 / owner_scheduler.hh:2965 |
| `recovery_safe_lsn` | **裁决修正（回查 owner_scheduler.hh 后）**：(1) `flush_round_once` 在 `tree_flush_request` 传 **`recovery_safe_lsn = 0`**（保守：fold 不物理删任何 tombstone，全部保留为 tombstone record，安全）；(2) **但 `tree_local_flush` 内部每轮成功后仍会 recompute `tree_state.recovery_safe_lsn = min(flush_max_lsn, superblock_safe_lsn)`（`owner_scheduler.hh:2318/2967`）——该 recompute 缺 RSM §4.9 的 `wal_frontier` 维度，是 pre-existing gap**。step 1 不依赖 `tree_state.recovery_safe_lsn`（无 reclaim、无 recovery），所以不咬；但 **step 2 接 reclaim/recovery 前必须先修 `recompute_recovery_safe_lsn` 加 `wal_frontier`**，否则 premature tombstone compaction → 崩溃后 recovery 复活旧值的 latent bug | RSM §4.9 / owner_scheduler.hh:2318 |

**已回查确认（owner_scheduler.hh:2963-2967）**：`tree_local_flush` 在 flush finalize
内一次性推进 `flush_max_lsn` / `superblock_safe_lsn`，并 recompute
`recovery_safe_lsn = min(flush_max_lsn, superblock_safe_lsn)`。所以：
1. §4 pipeline **不含**独立 `update_flush_max_lsn` 跳（tree_local_flush 内联）。
2. `recovery_safe_lsn` 的 `wal_frontier` 维度（RSM §4.9）当前缺失 = **pre-existing gap**，
   不是本 step 引入；step 1 因不依赖 `tree_state.recovery_safe_lsn` 而不咬，step 2 必修。

`recovery_safe_lsn=0` 是 **step-1 临时欠实现**（temporary under-implementation），最终形态仍按 RSM §4.9。

### 8.B5 root-change：CAT2 与 superblock 的时序（B5，现有代码 gap，非 conformance）

**事实（已对代码验证）**：现有 `tree_local_flush` 对 root-change 走
`finalize_root_change`（`tree/sender.hh`）= `begin_update_superblock → perform_superblock_io
(FUA) → finalize_flush_round`，即 **superblock FUA 在 `tree_flush_result` 返回之前完成**。
而本 step 的 `frontier_switch`（装 CAT2）在 `tree_local_flush` 返回**之后**。所以实际时序是
**superblock FUA → CAT2 install**。

**与 spec 的偏离**：FF §6.2 / RSM 时序要求 **"install CAT2 立即，不等 superblock；superblock
异步在 CAT2 之后"**。现有实现把次序反过来了。

**裁决**：本 step **不修这个反序**（修它要改 `tree_local_flush` 内部编排，超出 step 1 范围），
而是**显式标记为现有代码 gap，不声称这条 conformant**：

- **正确性安全**：superblock 只是 recovery hint 不是 correctness source（FF §6.2）；CAT2 是
  in-memory，崩溃必丢，恢复读 superblock root + WAL replay 补齐。superblock-先于-CAT2 只是
  让 root-change 轮的 reader 可见性多等一次 superblock FUA——root-change 罕见（树升层），
  延迟代价可接受。
- **去向**：把 "root-change 时 CAT2 install 应先于 superblock FUA（superblock 异步）" 列入
  §13 的后续优化（属 `tree_local_flush` 编排重构，可与 step 2/INC-024 一并做）。
- 因此 §3.4 / §4 的 "复用 tree_local_flush" 仅就 root-**stable** 路径声称时序 conformant；
  root-change 路径标注为已知 gap。

---

## 9. retired 累积正确性（为 step 2 reclaim 铺地基）

本 step 必须保证 step 2 的回收链有正确输入：

1. **old tree value / slot / range** → 经 `frontier_switch` step 3 append 到 `old_guard(G0).retired`。
   - 这是 FF §5.1/§5.2 的唯一挂接点。**禁止**挂 memtable_gen / 全局结构（红线 / 顶层 invariant）。
2. **memtable-only loser** → `tree_local_flush` 的 fold 阶段已直推各 owning `gen.loser_durable_refs`（FF §5.3，本 step 不碰，已由现有 fold 实现）。
   - **禁止**和 G0.retired 合并（两条可观测路径经不同 shared_ptr 节点：gen vs G0）。
3. **double hook 不变量复核**：本 step 只对 G0.retired 做**一次** append（frontier_switch 内），
   不重复挂；gen loser 由 fold 单遍历，不跨 gen。符合 FF §5.5。

step 2 将：给 `checkpoint_guard` 析构器加 "投递 retired 到 tree_sched.reclaim_q"，
给 `memtable_gen` 析构（或 gen ref→0）加 loser drain hook，并接 `recovery_safe_lsn`
WAL 维度 + value reclaim 判定。本 step 把数据备好，析构器/consumer 不动。

---

## 10. 容量与性能（热路径预算）

flush 是**后台路径**，不在前台 write/read 热路径上，但仍按 10 亿 KV 校准 per-round 成本：

| 成本项 | 量级 | 说明 |
|---|---|---|
| `subtract_flushed_gens` 重建 fronts | O(front_count × imms_len) | front_count 小（分片数），imms_len 受 seal/flush 节奏控制（稳态 ≤ 个位数）。每轮重建一个 `vector<front_read_set>`（shared_ptr 包裹），仅指针/小结构拷贝，无 value/key body 拷贝 |
| retired append 到 G0 | O(retired_count) | InlinedVector append；retired_count = 本轮 changed slots/ranges/values，与 dirty leaf 数同量级，远小于全树 |
| CAT2 / PRS2 / G1 构造 | O(1) + 上面的 fronts 重建 | 3 个 make_shared |
| `install_cat` | O(1) | atomic shared_ptr store |
| capture/switch/end 三 handle | coord 单线程串行 | 每轮 3 次 coord 入队，相比一轮 flush 的 NVMe I/O 可忽略 |

**无 per-request / per-KV 成本**：所有成本按"每轮 flush"摊销，与单轮 flush 覆盖的 batch
数无关地小。10 亿 KV 下 flush 轮数多但每轮 coord 侧成本恒定小，不构成瓶颈。
RocksDB×5 性能锚点：本 step 不引入前台热路径成本，不影响该取向。

**容量**：见 §1.2 声明——step 1 单独 land 盘空间不回收（step 2 闭），内存侧 gen 经
release_gens + PRS 移除后由 shared_ptr ref→0 正常释放（CAT1 退役链），无内存无界泄漏；
**盘空间**无界增长直到 step 2。

---

## 11. 验证计划（production harness 形态，禁读测试文件）

验收 harness 是 production 代码形态（如 `inconel_test_*` target，但本设计阶段不写/不读其内容；下列是**意图**，实现期由实现者按意图写）：

1. **单轮 flush 可见性**：写若干 PUT → seal → `flush_round_once` → 用**新** read_handle
   point_get，命中 tree path（经 G1 manifest）读回 value；老 read_handle（flush 前 acquire）
   仍经 CAT1 + memtable 读回同值（snapshot 隔离）。
2. **imms 收敛**：flush 后 PRS2.fronts 的 imms 不含已 flush gen；front 本地 imms 经
   release_gens 也不含。
3. **durable_lsn 不回退**：capture 后、frontier_switch 前再发布若干 batch（推高 durable_lsn）
   → CAT2.durable_lsn == 安装瞬间最新 durable_lsn（≥ capture 值），新 reader 能看到这些 batch。
4. **串行化（flush-vs-flush）**：并发发两个 `flush_round_once` → 第二个 capture 拿
   `catalog_update_in_progress`；串行发则两轮都成功且第二轮 collect 不重选第一轮的 gen。
5. **retired 累积**：flush 后检查 `G0.retired.{old_slots,old_ranges,old_tree_values}` 内容
   与本轮 changed 集合一致（白盒 inspector）；确认 gen.loser_durable_refs 由 fold 填好。
6. **root-stable / root-change 混合**：多轮 flush 触发 root-change（树长高）后，新 reader 经
   CAT2 仍正确读回；superblock 更新由 `tree_local_flush` 内部完成（本 step 不验 superblock 字节，
   归 tree-local flush 既有验收）。
7. **异常清位**：注入 `tree_local_flush` 失败 → 本轮不装 CAT2、`catalog_update_in_progress_` 被清、
   下一轮可正常发起。
8. **多轮累积读一致**：连续多轮 flush（含 overwrite / tombstone）后逐 key 读回 winner 正确。
9. **B3 no-op round**：在无 eligible gen 时发 `flush_round_once` → 不装 CAT2（CAT epoch 不变）、
   不动 imms、`catalog_update_in_progress_` 正常清；不得构造 null-manifest guard。
10. **B4 flush-vs-seal 互斥**：seal 进行中发 `flush_round_once` → capture fail-fast；flush
    进行中发 seal → close_gate fail-fast。穿插发起 seal 与 flush（串行）→ CAT epoch 单调、
    gate 不卡死、两者各自正确完成。
11. **B2 frontier_switch 原子性**：注入 CAT2 构造失败（阶段 A）→ `G0.retired` 未被改动、
    CAT 仍 CAT1、`catalog_update_in_progress_` 被清。

> 验收覆盖 cross_doc §5 flush 路径每一跳；不跑长时容量压力（step 2 验收）。

---

## 12. 文档回写清单

本 step 落地后同步（spec 已大体定义，回写主要是"实现已对齐"+ 串行化裁决）：

- **cross_doc_contracts §1**：确认 `capture_flush_frontier` / `frontier_switch`（第 4 参改 `flush_release_plan`）/ `end_flush_round` 签名与实现一致（`end_flush_round` 是本 step 新增串行化 seam，spec 未列，补一行 + 注明"catalog 更新串行兜底"）。
- **cross_doc_contracts §5**：flush 路径补 `end_flush_round` 末跳（capture…release_gens → **end_flush_round**）。
- **runtime_state_machine §2**：coord_state 补 `catalog_update_in_progress_` 字段；§2.3 补 `capture_flush_frontier` / `frontier_switch` / `end_flush_round` 语义 + 串行化裁决；**补 close_gate/open_gate 也取/放该标志（B4）**。
- **runtime_state_machine §10.3**：把"seal 与 flush 的 CAT 安装互斥、flush round 端到端串行"从隐含升级为**显式不变量**（引用本 §6 裁决）。
- **flush_and_frontier_switch §4**：补 "D1 安装瞬间读、可能 > capture durable_lsn、CAT2 不回退可见性"（§5.3）+ stale-guard 防御 panic；**§6.2 补 root-change 下 CAT2-先于-superblock 的目标时序，并注明当前 `tree_local_flush` 是反序（§8.B5 现有 gap）**。
- **core 新增** `flush_frontier` / `flush_release_plan` carrier 在 cross_doc §2 struct 表登记。
- **known_issues**：本 step land 后，给 INC-022 标注"coord frontier_switch 编排子任务（step 1）已完成"；登记 B5（root-change CAT2/superblock 反序）为新 issue 或并入 INC-024。

---

## 13. 后续 step 路线（本 step 之后，供排期）

| step | 内容 | 解锁 / 依赖 |
|---|---|---|
| **1（本文 055）** | frontier_switch 可见拓扑切换 + release_gens 闭环 | 依赖：tree_local_flush（已存在） |
| **2** | 物理 reclaim：`checkpoint_guard` 析构 enqueue `reclaim_task` → tree_sched reclaim consumer（TRIM 旧 slot/range + `value::reclaim_values`）；`memtable_gen` loser drain hook；**修 `owner_scheduler.hh:2318 recompute_recovery_safe_lsn` 加 `wal_frontier` 维度（RSM §4.9）**；value 回收判定（data_ver ≤ recovery_safe_lsn）+ 延迟队列（FF §7.2/§7.3） | 依赖：step 1（retired 已正确挂 G0 / gen） |
| **3** | INC-054：tree allocator push-floor + ENOSPC backpressure（reclaim 把 retired range 还 allocator 后才有真实推进点） | 依赖：step 2（reclaim consumer 接通 allocator recycle） |
| **4** | boot recovery pipeline + format/recovery runtime wiring（INC-035 format_disk / INC-020 install_recovered_state 调用方 / INC-024 update_superblock）。value 侧 `install_recovered_state` / `install_recovered_value_space` helper 已存在，tree root-change superblock seam 已存在；缺 boot pipeline + wiring | 依赖：step 1-3（稳态 on-disk 不变量 / recovery_safe_lsn / retire 顺序落定后，recovery = flush-like merge 可复用机器） |

**跟进优化（非独立 step，随相邻 step 收）**：
- **B5 reorder**（§8.B5）：把 `tree_local_flush` root-change 改成 "CAT2 install 先、superblock FUA 异步在后"（FF §6.2 目标时序）。属 `tree_local_flush` 编排重构，随 step 2 或 INC-024（update_superblock 正式化）一并做。step 1 先按现有反序运行（correctness-safe）。

---

## 14. 冲突与裁决记录（三方对照）

| # | 冲突 / 缺口 | spec | 当前代码 | 裁决 |
|---|---|---|---|---|
| 1 | flush round 是否必须串行 | OV §9 未冻结；RSM §10.3 只说 tree 单线程；FF §5.5 暗示同 gen 不并发折叠 | 无 flush 编排 | **本 step 端到端串行**（`catalog_update_in_progress_` + `end_flush_round` 在 release_gens 后清）。pipelined flush 记为 future opt。写回 RSM §10.3 升级为显式不变量（§6） |
| 2 | frontier_switch 是否校验 current guard == old_guard | FF §4 未写；RSM §2.3 伪码未含 | 无 | **加防御性 panic**（§5.3 步骤 2）；串行 driver 下恒成立，防未来状态机改动（§6.3） |
| 3 | CAT2.durable_lsn 继承哪个 durable_lsn | RSM §2.3 "读安装瞬间当前 cat"；FF §4.2 "原样继承" | seal 用 build_cat1（gate 关时读，安全） | **D1 必须 coord 线程安装瞬间读**（§5.3）；明确 flush 不关 gate，D1 可 > capture 值，CAT2 不回退可见性。写回 FF §4 |
| 4 | `flushed_max_lsn` 推进点（pipeline 是否需独立 update_flush_max_lsn 跳） | cross_doc §5 路径含 `tree_sched(update_flush_max_lsn)` | **已回查 `owner_scheduler.hh:2963-2966`：`tree_local_flush` 已内联推进 `flush_max_lsn` + `superblock_safe_lsn`** | **省该跳**——flush_round_once 不加 `update_flush_max_lsn`；cross_doc §5 的该跳在当前实现中由 tree_local_flush 内联完成（已解决，非 open） |
| 5 | `recovery_safe_lsn` 本 step 如何处理 | RSM §4.9 推进公式 = `min(flush_max_lsn, superblock_safe_lsn, wal_frontier)` | **已回查 `owner_scheduler.hh:2318`：`recompute_recovery_safe_lsn = min(flush_max_lsn, superblock_safe_lsn)`，缺 `wal_frontier`**；每轮 flush finalize 调用（:2967） | **两面裁决**：(1) flush_round_once 请求传 `recovery_safe_lsn = 0`（保守，不 premature 删 tombstone）；(2) tree_local_flush 内部 recompute 缺 `wal_frontier` 是 **pre-existing gap**，step 1 不依赖故不咬，**step 2 接 reclaim/recovery 前必修**（否则 latent recovery 复活旧值）（§8） |
| 6 | `end_flush_round` 是否需要独立 handle（spec 未列） | spec 无 | 无 | **新增**（串行化兜底 seam）；写回 cross_doc §1/§5 + RSM §2.3（§12） |

**以下 7-11 为 2026-06-16 codex spec-conformance review 顺出（均已对代码验证）：**

| # | 冲突 / 缺口 | spec | 当前代码 | 裁决 |
|---|---|---|---|---|
| 7 (B1) | `frontier_switch` 第 4 参 `flushed_gens_by_front` 设成 `const&`，借用 pipeline 局部 `tree_flush_result` | PUMP 跨域 sender 不持局部引用（`feedback_use_context_not_pointer`） | — | 改 `core::flush_release_plan` **by value**；`frontier_switch`（subtract）与 `release_gens` 共用；move 前从 result 提取（§5/§4/§5.1） |
| 8 (B2) | §5.3 先 append retired 再构造 CAT2，构造抛异常则 retired 脏但未 install | FF §"失败不 install、不改 retire list" | — | **阶段 A（构造+reserve，可抛）／阶段 B（append+install，noexcept）两段式**；阶段 A 不碰 G0、不 install（§5.3 重排） |
| 9 (B3) | 空 round 的 null `new_manifest` 被送进 frontier_switch → 构造 null-manifest guard | — | `owner_scheduler.hh:2422` 空 sealed_gens 返回 null manifest | **no-op 分支**（`variant<flush_noop,...> + visit`，对齐 point_get）：无 eligible gen 短路到 end_flush_round；非空但 null manifest fail-fast（§4/§5.1） |
| 10 (B4) | §6 只串行 flush-vs-flush，**漏 flush-vs-seal** | FF/RSM 无显式 seal-vs-flush 互斥 | `seal_round build_cat1` 用 close-time old_cat；`coord:446` epoch 校验抛 | **统一 `catalog_update_in_progress_` 串行 seal+flush**；seal 的 close_gate/open_gate 也取/放该标志（§6.1b）。否则 frontier_switch 插进 seal 窗口 → seal install epoch 校验抛 → gate 卡死 |
| 11 (B5) | 复用 tree_local_flush 在 root-change 下 superblock FUA 先于 CAT2 | FF §6.2 "CAT2 立即装、superblock 异步在后" | `tree/sender.hh finalize_root_change` superblock 先于 return | **标为现有代码 gap，不声称 conformant**（correctness-safe：superblock 是 recovery hint，crash 丢内存 CAT2 无妨；root-change 罕见）；reorder 延后（§8.B5，随 step 2/INC-024） |

---

## 15. 开工 checklist（B 类，按 flush_development_plan）

- [x] 目标明确（§0/§1）
- [x] 输入输出 carrier 明确（§4/§5.1：flush_frontier / flush_release_plan / flush_noop / CAT2 / PRS2 / G1）
- [x] 涉及 scheduler owner 明确（coord 新 3 handle + 改 close_gate/open_gate；front collect/release 复用；tree_local_flush 复用）
- [x] 本阶段**不做**什么明确（§1.2）
- [x] 哪些 path 必须 fail-fast 明确（capture/close_gate 互斥拦截、no-op round 短路、null manifest、release_gens panic、tree flush 失败，§7）
- [x] 最少验证范围明确（§11，含 B2/B3/B4 验证项）
- [x] spec 缺口已显式裁决并计划写回（§14/§12，约束 C；含 codex review 的 B1-B5）
- [x] 三条红线 + Shadow CoW A/B 复核（§3.4/§3.5）；root-change CAT2/superblock 时序标为现有 gap（§8.B5）
