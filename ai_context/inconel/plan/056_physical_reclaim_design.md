# 056 — 物理 Reclaim（稳态后台环 step 2）详细设计

> 稳态后台环 step 2，对应 known_issues INC-023（tree reclaim handle）+ INC-022 剩余项 + INC-052 可观测。
> 依赖 step 1（055，已 land）：retired 已正确挂 G0、gen.loser_durable_refs 由 fold 填好。
> 设计角色文档；实现阶段禁止读测试文件。

---

## 0. 一句话

把 step 1 累积但永不回收的盘空间真正回收：`checkpoint_guard` / `memtable_gen` 析构时把
退役对象 post 到 `tree_sched.reclaim_q`；tree_sched 起 reclaim consumer——跨 shards
`tree_node` invalidate barrier → TRIM 旧 slot/range → `tree_allocator.recycle`；
按 `data_ver ≤ recovery_safe_lsn` 判定 value 回收（不够则进 deferred 队列），调
已就位的 `value::reclaim_values`；并把 `recovery_safe_lsn` 补上缺失的 `wal_frontier` 维度。

---

## 1. 范围

### 1.1 本 step 做什么

| # | 内容 | 落点 |
|---|---|---|
| 0 | **WAL reclaim 闭环（B4，codex 顺出）**：当前 `wal::reclaim_check` **无 production caller**；step 2 接上——flush_durable_frontier 推进后驱动 `reclaim_check(flush_durable_frontier)` → wal 回收段 + 更新 frontier cell → tree recompute（§5.8.2） | `tree/sender.hh`、`wal/*` |
| 1 | **`recovery_safe_lsn` 两个 frontier（B3）**：WAL 段回收用 `flush_durable_frontier=min(flush_max_lsn,superblock_safe_lsn)`；value/tombstone GC 用 `recovery_safe_lsn=min(那个, wal_frontier)`，`wal_frontier=wal_global_min_unreclaimed_lsn-1`（含 active 段、跨流保守、单调）。wal 发布 `global_min_unreclaimed_lsn` 到 core atomic cell（§5.4） | `tree/owner_scheduler.hh`、`wal/scheduler.hh`、`core/wal_reclaim_frontier` |
| 2 | **`reclaim_task` 定义**（当前仅前向声明） + **`core::reclaim_sink` 抽象接口**（atomic 发布，destructor 跨层 reach）；**ingress 队列 `mpmc::queue`**（非 per_core，B1） | `core/`、`tree/owner_scheduler.hh` |
| 3 | **`checkpoint_guard` 析构器**：retired 非空时 post `reclaim_task` 到 sink（FF §4.6/§7.1） | `core/checkpoint_guard.hh` |
| 4 | **`memtable_gen` 析构器**：loser_durable_refs 非空时 post gen-loser 到 sink（FF §5.3 gate 1=gen 释放） | `core/memtable.hh` |
| 5 | **tree_sched reclaim consumer**：advance() drain `reclaim_q` → `reclaim_round` pipeline | `tree/owner_scheduler.hh` + `tree/sender.hh` |
| 6 | **跨 shards `tree_node` invalidate barrier**：old_range 进 allocator 前 fan-out 各 read_domain 失效 leaf cache + owner 失效 non_leaf cache，wait-all-acks（RSM §4.4/§4.7） | `core/tree_read_domain.hh`、`tree/*` |
| 7 | **TRIM**：range 内旧 slot（保留 current）→ 整个旧 range（RW §8.2），经 nvme | `tree/sender.hh` |
| 8 | **value 回收判定 + deferred 队列**：`data_ver ≤ recovery_safe_lsn` → dead set → `value::reclaim_values`；否则 `deferred_value_reclaim`，周期扫描（FF §7.2/§7.3） | `tree/owner_scheduler.hh` |
| 9 | **`tree_allocator.recycle` 接 invalidate barrier**（当前直接 try_enqueue，缺 barrier） | `tree/owner_scheduler.hh` |
| 10 | **INC-052 可观测**：`value_alloc_sched` reclaim_stats atomic counters + inspector | `value/scheduler.hh` |

### 1.2 明确不做（fail-fast / 留后续）

| 不做项 | 去向 |
|---|---|
| INC-054 tree allocator push-floor / ENOSPC backpressure（撞墙仍 panic） | step 3 |
| INC-053 `free_ranges` coalescing extent map（本 step 仍用现有 `local::queue{4096}`） | 独立 issue，可任意时刻 |
| boot recovery / format_disk / install_recovered_state 调用方 | step 4 |
| B5 root-change CAT2/superblock reorder | 随 INC-024 |
| `reclaim_values` 本体（已就位，本 step 只调用） | — |
| value 侧 liveness 校验防御代码（INC-052 明确不加，只加 counter） | — |

### 1.3 容量影响

本 step land 后稳态盘空间回收闭环（退役 slot/range/value 真正 TRIM + recycle，
gen/guard 析构链触发）。step 1 的"盘空间无界增长"在此消除。INC-054（撞墙 graceful
ENOSPC 而非 panic）仍是 step 3——本 step 撞墙仍 panic（约束 A：声明的已知限制）。

---

## 2. 背景对账

### 2.1 spec 已精确定义（本 step = 转写 + impl 机制裁决）

- **FF §7.1** 回收触发链（逐字）：CAT2 install → 旧 CAT refs→0 → PRS → fronts/gen → **G0 析构 → G0.retired 投递 tree_sched** → old_slots TRIM / old_ranges TRIM+barrier+recycle / old_tree_values value 回收判定。
- **FF §4.6** `checkpoint_guard` 析构器 spec：`if (!retired.empty()) tree_sched->enqueue_reclaim(std::move(retired))`；**析构器不直接发 NVMe**（OV §3.1 约束 5）。
- **FF §7.2/§7.3** value 回收：`data_ver ≤ recovery_safe_lsn` 立即 `recycle_value_slot`（收 dead_value_refs → `reclaim_values`）；否则 `deferred_value_reclaim.push`；tree_sched 周期 `while front().data_ver ≤ recovery_safe_lsn: pop + recycle`。
- **FF §5.3** memtable-only loser gate：**gen 释放 AND data_ver ≤ recovery_safe_lsn** 两条都满足 → 经 tree_sched TRIM 后投 value_alloc。
- **RSM §4.9** `recovery_safe_lsn = min(flush_max_lsn, superblock_safe_lsn, wal_frontier)`；`wal_frontier`：无未回收 sealed segs → `flush_max_lsn`，否则 `min(sealed segs.min_lsn) - 1`。
- **RSM §4.4** `recycle(range_ref r)`：`invalidate_tree_node_range_on_all_shards(r)`（wait-all-acks；pinned frame = 生命周期 bug）→ `free_ranges.try_enqueue(r)`。
- **RSM §4.7** old range 进 `free_ranges` 前必须完成跨 shards `tree_node` invalidate barrier（裸 frame_id 复用安全性依此）。
- **RW §8.2** TRIM 顺序：每 range 保留 `clean_manifest.slot_map` 的 current slot、TRIM 其余旧 slot；再 TRIM 不在 slot_map 的整个旧 range。

### 2.2 当前代码现状（grounding 已核对）

| 组件 | 现状 | file:line |
|---|---|---|
| `checkpoint_guard` | `{manifest, retired}`，**default 析构**（注释明确"析构器 + reclaim_task 投递 + reclaim pipeline 属后续 step"） | `core/checkpoint_guard.hh:44-51` |
| `retired_objects` | `{old_slots[32], old_ranges[8], old_tree_values[64]}`，无析构 | `core/retired_objects.hh:41-58` |
| `retire_list::drain(f)` | 遍历 `f(move(item))` + `clear()`；已就位 | `core/memtable.hh:117-144` |
| `memtable_gen.loser_durable_refs` | `retire_list<retired_value_ref>`；gen **无析构 hook** | `core/memtable.hh:218` |
| `tree_state` | `alloc, flush_max_lsn, superblock_safe_lsn, recovery_safe_lsn, active_superblock_slot, non_leaf_page_cache, reclaim_q{256}, active_rounds, active_merge, next_round_id` | `owner_scheduler.hh:250-275` |
| `reclaim_q` | `per_core::queue<reclaim_task*>{256}`；**`reclaim_task` 仅前向声明、无 enqueue、advance() 无 drain = 完整 stub** | `owner_scheduler.hh:264` / `:52` |
| `tree_allocator.recycle` | `free_ranges.try_enqueue(r)`，**无 invalidate barrier**；`free_ranges: local::queue{4096}` | `owner_scheduler.hh:118-120` |
| `recompute_recovery_safe_lsn` | `min(flush_max_lsn, superblock_safe_lsn)`，**缺 wal_frontier** | `owner_scheduler.hh:2318-2320` |
| `value::reclaim_values` | `(span<const value_ref>) → _value_reclaim::sender`；handle 按 page_base 分组、dirty 延迟、quiescent immediate release + cache refresh；**已就位** | `value/scheduler.hh:487/1558-1589` |
| wal sealed segs | `sealed_segments_: vector<sealed_segment_info{id,gen,min_lsn,max_lsn}>` **private**；`reclaim_segments(recovery_safe_lsn)` public（回收 max_lsn≤rsl 的段）；**无 min_lsn accessor** | `wal/scheduler.hh:476/369-405`，`core/wal_stream.hh:48-53` |

**缺口结论**：value reclaim 本体已就位、retire 累积已就位（step 1）、retire_list.drain 已就位；缺的是**触发链（guard/gen 析构 → reclaim_q）+ tree_sched consumer + invalidate barrier + TRIM + wal_frontier 接入 + counter**。

---

## 3. 三阶段文档检查

### 3.1 实现前（已做）
读 INDEX 定位（"资源回收/GC→flush §5,§7 + runtime_state §8；recovery §8-9 tombstone"）；完整读 FF §5/§7、RSM §4.4/§4.7/§4.9/§5.4/§8、RW §8.2/§12。

### 3.2 实现中回查
reclaim handle 签名→cross_doc §1（`reclaim_values` / `reclaim_check`）；retired 字段→cross_doc §2；invalidate barrier→RSM §4.4/§4.7 + RMC §10.2；pipeline 路径→cross_doc §5 + RW reclaim 路径。

### 3.3 实现后验收
对照 cross_doc：reclaim_values precondition（INC-052 红线候选）、§5 pipeline 无遗漏、三红线。

### 3.4 三条红线复核
- **读路径红线**：reclaim 全在 tree_sched + read_domain owner 内，不让读路径访问可变状态——✅。invalidate barrier 是 tree_sched→read_domain 的 fan-out（写 owner-local cache），不在读路径上。
- **tree 运行时红线**：不扫 slot 选最新；recycle 不要求"每 range 单 slot"；TRIM 不把 flush 理解为重写 slot 0——✅。
- **recovery 红线**：本 step 不碰 recovery 路径——✅ 不适用（但 recovery_safe_lsn 的正确性是 recovery 安全前提，本 step 修正它）。

### 3.5 Shadow CoW A/B
本 step 是回收路径，不在 tree 写侧（不 cascade/reformat/assign_paddr），不影响 A/B。invalidate barrier 失效的是退役 range 的 cache 条目，不动 live manifest。

---

## 4. 顶层编排

### 4.1 触发链（被动，shared_ptr 析构驱动）

```
（reader 释放 read_handle / seal 推进 / flush 装 CAT2）
  → 旧 CAT refs→0 → PRS refs→0
      ├── fronts → 各 shared_ptr<memtable_gen> use_count--
      │     └── gen refs→0 → ~memtable_gen()：loser_durable_refs 非空
      │           → sink->post_gen_losers(drain 出的 refs)        [gate 1 已满足]
      └── tree_guard(G0) refs→0 → ~checkpoint_guard()：retired 非空
            → sink->post_retired(std::move(retired))
  → 两者都 enqueue reclaim_task 到 tree_sched.reclaim_q（per_core::queue，cross-core 安全）
```

### 4.2 消费链（主动，tree_sched 驱动）

```
tree_sched.advance():
  drain reclaim_q → 累积进 pending_reclaim（owner-local）
  if !reclaim_round_in_flight && (pending 非空 || deferred 可推进):
      submit reclaim_round pipeline:
        ── 1. 聚合 pending 的 old_slots / old_ranges / {retired + gen} value refs
        ── 2. old_ranges：fan-out 各 read_domain.invalidate_range + owner 失效 non_leaf
                 → reduce（wait-all-acks）
        ── 3. nvme：TRIM（range 内非 current slot + 整个退役 range + old_slots）bounded
        ── 4. old_ranges → tree_allocator.recycle（barrier 已过）
        ── 5. value refs：data_ver ≤ recovery_safe_lsn → dead set；否则 deferred_value_reclaim.push
        ── 6. dead set → value::reclaim_values(dead) bounded
        ── 7. finish：清 in_flight；若 recovery_safe_lsn 本轮推进过，扫 deferred 队列头
```

reclaim_round 与 flush_round 一样**串行**（tree_sched 单实例，且 invalidate barrier /
allocator recycle 不能并发）；用 `reclaim_round_in_flight` 标志（owner-local，无需 coord 标志）。

---

## 5. 详细设计

### 5.1 裁决 D1 — `reclaim_task` + `core::reclaim_sink`（析构器跨层 reach）

**问题**：`checkpoint_guard` / `memtable_gen` 在 `core/`（L0），`reclaim_q` 在 `tree_sched`（L2）。
析构器在**任意线程**跑（谁 drop 最后一个 ref），不能让 core 依赖 tree。

**裁决**：core 定义抽象 sink，tree 实现，runtime 注册。

```cpp
// core/reclaim_sink.hh（新）
namespace apps::inconel::core {
    struct reclaim_task;   // 见下，定义也放 core（含的都是 core 类型）

    struct reclaim_sink {
        virtual ~reclaim_sink() = default;
        virtual void post_retired(retired_objects&& r) noexcept = 0;
        virtual void post_gen_losers(
            absl::InlinedVector<retired_value_ref, 16>&& losers) noexcept = 0;
    };

    // 进程级活跃 sink，**atomic 发布**（B2）：build 时 tree_sched 注册，
    // teardown 时**先**置空（见下）。store/load 用 release/acquire。
    std::atomic<reclaim_sink*>& reclaim_sink_cell() noexcept;   // 单一 atomic
    inline void set_reclaim_sink(reclaim_sink* s) noexcept {
        reclaim_sink_cell().store(s, std::memory_order_release);
    }
    inline reclaim_sink* active_reclaim_sink() noexcept {
        return reclaim_sink_cell().load(std::memory_order_acquire);  // teardown 后为 null
    }
}
```

> **B2 — sink 生命周期（codex review 修正）**：`destroy_runtime`（`runtime/builder.hh:986-1019`）当前删除序是
> coord → fronts → wal → **tree_sched** → read_domain → value。删 coord 会 drop 当前 CAT → 级联触发
> guard/gen 析构 → post 到 sink（此刻 tree_sched 还在，OK）；但删 tree_sched 后若还有 guard/gen 析构（被别处
> 持有的 read_handle / gen），sink 指向已 delete 的 tree_sched → **dangling**。
> **裁决**：`destroy_runtime` **第一步**就 `core::set_reclaim_sink(nullptr)`（release），此后所有 guard/gen 析构
> 走 sink==null 分支跳过 post（teardown 时丢弃 reclaim 无害——进程退出，盘空间由下次 boot recovery 重建）。
> tree_sched 析构前 drain 一次 reclaim_q（释放未处理 task 的内存，非必须但干净）。production 运行期 sink 恒非 null。

`reclaim_task` 定义（core，因为 retired_objects / retired_value_ref 都是 core 类型）：
```cpp
struct reclaim_task {
    enum class kind : uint8_t { guard_retired, gen_losers } k;
    retired_objects                              retired;        // k==guard_retired
    absl::InlinedVector<retired_value_ref, 16>   gen_losers;     // k==gen_losers
};
```

tree_sched 侧实现 sink，post_* 把 `new reclaim_task{...}` 入 reclaim ingress 队列：
```cpp
void post_retired(core::retired_objects&& r) noexcept override {
    reclaim_q.try_enqueue(new core::reclaim_task{
        .k = kind::guard_retired, .retired = std::move(r)});   // mpmc，任意线程安全
}
```

> **B1 — ingress 队列必须 `mpmc::queue`，不能 `per_core::queue`（codex review 修正）**：
> `per_core::queue::try_enqueue` 按 thread-local `this_core_id` 选 SPSC lane（`lock_free_queue.hh`），
> 而 `this_core_id` **只在 runtime run loop 设置**（`share_nothing.hh:32`）。`read_handle` / `shared_ptr<memtable_gen>`
> 是公开 carrier，最后一个 ref 可能在**任意应用线程**释放 → 析构器 post 时 `this_core_id` 可能是默认 0
> （多外部线程共写同一 SPSC lane = 数据竞争）或越界（>127）。**裁决**：把 `tree_state.reclaim_q` 从
> `per_core::queue<reclaim_task*>{256}` 改为 **`mpmc::queue<reclaim_task*>`**（CAS 多生产者，`lock_free_queue.hh`
> 已提供），任意线程 post 安全；tree_sched.advance() 单消费者 drain。

**为什么不存 sink* 进每个 guard/gen**（否决）：要在 frontier_switch / bootstrap / 每个
memtable_gen 创建点都 thread 指针进去，构造点多、易漏；进程级 atomic 单例更稳且匹配 registry 模式。
**实现期回查**：若 `core::registry` 已能提供线程安全的 tree reclaim hook，直接用它，不另起 cell。

> `reclaim_q`（mpmc）容量耗尽时 `try_enqueue` 返回 false → **本 step fail-fast panic**（约束 A：退役对象丢弃 = 盘 leak，不接受 silent drop）。容量评估见 §8。

### 5.2 `~checkpoint_guard()`（FF §4.6）

```cpp
~checkpoint_guard() {
    if (retired.empty()) return;                 // bootstrap / 无变化轮：常态空
    if (auto* sink = core::active_reclaim_sink()) {
        sink->post_retired(std::move(retired));
    }
    // sink==null（未装 runtime）：retired 随 vector 析构释放内存，不 TRIM——
    // 仅出现在无 runtime 的纯构造场景，production 必有 sink。
}
```
- `retired_objects` 加一个 `empty()` helper（`old_slots.empty() && old_ranges.empty() && old_tree_values.empty()`）。
- noexcept：析构器内不可抛（post 失败走 §5.1 panic）。

### 5.3 `~memtable_gen()` + loser drain（FF §5.3 gate 1）

```cpp
~memtable_gen() {
    if (loser_durable_refs.size() == 0) return;
    absl::InlinedVector<retired_value_ref, 16> out;
    loser_durable_refs.drain([&](retired_value_ref&& r){ out.push_back(std::move(r)); });
    if (auto* sink = core::active_reclaim_sink()) {
        sink->post_gen_losers(std::move(out));
    }
}
```
- gen 析构 = FF §5.3 **gate 1（gen 释放）已满足**；gate 2（`data_ver ≤ recovery_safe_lsn`）由 tree consumer 施加（§5.7）。
- lifetime：析构 body 执行时所有成员都还活着（不依赖声明顺序）；`loser_durable_refs` 只存 `{value_ref,data_ver}`，不引用 table/kv_arena。
- flush fold 不会与析构竞争：flush 持 gen 的 shared_ptr，fold 期间 gen ref>0 不析构；`clear()`/`drain()` 是 fold 内显式调用，与析构（ref→0）不同时发生。
- **production 不可静默丢 loser**（codex 建议）：运行期 sink 恒非 null（§5.1 B2，只 teardown 置空）；若析构期 sink==null 出现在非 teardown 场景 = 不变量破坏。本 step 析构走 sink==null 跳过仅限 teardown；production 若需更强保证可在析构 assert sink 非 null（除 teardown flag）——留实现期裁。

### 5.4 裁决 D2（重写）— 两个 frontier，打破循环依赖（B3，codex review 顺出）

**问题（原稿两处错）**：
1. **循环死锁**：原稿把含 wal_frontier 的 `recovery_safe_lsn` 喂回 WAL 段回收（当前唯一 API `reclaim_check(recovery_safe_lsn)`，`wal/scheduler.hh:541`）。若 `recovery_safe_lsn ≤ wal_frontier = ms-1`，而最旧未回收段 `min_lsn = ms` 故 `max_lsn ≥ ms > recovery_safe_lsn` → 永不满足 `max_lsn ≤ recovery_safe_lsn` → **段永不回收 → ms 冻结 → recovery_safe_lsn 卡死 → WAL 池耗尽**。
2. **非单调**：原稿假设 `min(sealed min_lsn)` 单调非降。**错**——WAL 多 front/stream，`record_sealed_segment` 仅 per-stream push_back、无全局 LSN 序（`wal/scheduler.hh:268-313`）；慢 stream 的低 LSN 段可能晚 seal → 只看 sealed 的 min 会回落 → `recovery_safe_lsn` 回落 → 已回收的值被"重新需要" → **corruption**。

**裁决：拆成两个独立 frontier。**

- **A. WAL 段回收 eligibility frontier**（RW §12.3）：
  ```
  flush_durable_frontier = min(flush_max_lsn, superblock_safe_lsn)
  ```
  一个段的 WAL 一旦其 entries 全部 durable 进 tree（`max_lsn ≤ flush_durable_frontier`）即可回收——**与 wal_frontier 无关**，故无循环。`wal::reclaim_check` 的 caller（§5.8 B4）传 `flush_durable_frontier`，**不**传 recovery_safe_lsn。（当前代码 `recovery_safe_lsn = min(flush_max_lsn, superblock_safe_lsn)` 恰等于它，所以现状未暴露循环；本 step 给 recovery_safe_lsn 加 wal 维度后二者分叉，必须改 caller 传 A。）

- **B. value/tombstone GC frontier = `recovery_safe_lsn`**：
  ```
  recovery_safe_lsn = min(flush_max_lsn, superblock_safe_lsn, wal_frontier)
  wal_frontier      = wal_global_min_unreclaimed_lsn - 1   (无未回收 WAL → flush_durable_frontier)
  ```
  **`wal_global_min_unreclaimed_lsn` = 跨所有 stream、所有未回收 WAL 输入（sealed-未回收 + active 非空 + pending-seal）的最低 LSN**（保守）。
  - 为什么含 active：慢 stream 的低 LSN 可能还在 active 段没 seal；只看 sealed 会漏，导致 recovery_safe_lsn 偏高 → premature reclaim。把 active 段 min_lsn 纳入，frontier 由**最慢 stream**钳住。
  - **单调性**：每个 stream 的"最低未回收 LSN"随它 flush+回收旧段单调上升；全局 min 由最慢 stream 决定、只前进。→ `wal_global_min_unreclaimed_lsn` 单调非降 → `recovery_safe_lsn` 单调非降（不回落，安全）。

**wal 侧发布**：`wal_space_sched` 维护 `wal_global_min_unreclaimed_lsn`（遍历各 stream 的最旧未回收段 / active 段 min_lsn 取全局 min；无任何未回收 WAL → `UINT64_MAX`），在 seal / reclaim / active 段推进改变它时 store-release 到共享 atomic（新增 `core::wal_reclaim_frontier { std::atomic<uint64_t> global_min_unreclaimed_lsn }`）。

**tree 侧消费**（`recompute_recovery_safe_lsn`）：
```cpp
uint64_t recompute_recovery_safe_lsn() const {
    const uint64_t fd = std::min(state.flush_max_lsn, state.superblock_safe_lsn);
    const uint64_t gm = wal_frontier_cell_->global_min_unreclaimed_lsn.load(acquire);
    const uint64_t wal_frontier = (gm == UINT64_MAX) ? fd : (gm - 1);
    return std::min(fd, wal_frontier);
}
```

**为什么 atomic 不走 sender**（否决）：单调 frontier 的简单读，与 `data_area_heads` 同模式，避免 per-recompute cross-sched round-trip。
**实现期**：cell 指针在 build 时交 tree_sched + wal_space_sched（与 `data_area_heads` 注入同路径）。wal 计算 global min 需遍历 streams 的 active+sealed 段——`sealed_segments_` 已在，active 段 min_lsn 需从 per-stream active segment 取（现有结构应可暴露，实现期确认）。

### 5.5 裁决 D3 — 跨 shards `tree_node` invalidate barrier（RSM §4.4/§4.7）

old_range 进 `tree_allocator.recycle` **前**，必须确保该 range 的 tree page frames 不再被任何
cache 持有（裸 `frame_id{paddr}` 复用安全性的前提）。INC-046 后：
- read_domain 只 cache **leaf** page（`node_cache`）。
- tree_sched 持 **non-leaf** page（`non_leaf_page_cache`，owner-local）。

**机制**：reclaim_round 对每个 old_range：
```
fan-out 各 tree_read_domain: invalidate_range(range_ref)   # 失效 node_cache 中落在该 range 的 leaf frame
  >> reduce(wait-all-acks)
owner 本地: 失效 non_leaf_page_cache 中落在该 range 的条目
```
- read_domain 新增 `invalidate_range(range_ref)` handle（在 read_domain owner 线程执行，遍历/按 range 失效其 leaf `node_cache`）。发现 `pin_count>0` 的 frame → `panic_inconsistency`（生命周期 bug：退役 range 仍被 reader pin）。
- 这是 tree_sched → 所有 read_domain 的 PUMP fan-out（`for_each(read_domains) >> concurrent >> on(read_domain) >> invalidate >> reduce`），不是 advance 内直接 enqueue（守 `feedback_cross_sched_pump_only`）。

> invalidate 粒度：按 range（一个 range = `shadow_slots × page` 的 LBA 区间）。read_domain 的 `node_cache` key 是 `frame_id{slot_paddr,...}`；失效 = 移除该 range 所有 slot 的 cache 条目。具体遍历策略（全扫 vs range 索引）实现期定，但**禁止**因此引入"cache miss 扫 shadow range 多 slot"（红线）——这是写侧失效，不是读侧多 slot 扫描。

### 5.6 TRIM（RW §8.2，经 nvme）

reclaim_round 的 TRIM 阶段（bounded，经 nvme sender）：
- **old_slots**（来自 retired.old_slots，shadow-slot rewrite 退役的单 slot）：**只 TRIM 这些具体 slot 的 LBA**，**绝不**整段 TRIM 其所在 range——该 range 的 current slot 仍 live（RW §8.2：range 内保留 current slot、TRIM 其余旧 slot）。
- **old_ranges**（consolidation 整 range 退役、不在 new manifest.slot_map）：invalidate barrier 后 TRIM 整个 range 的 LBA 区间。
- 顺序：先 invalidate barrier（§5.5）→ 再 TRIM → 再 recycle（§5.9）。recycle（逻辑归还 allocator）**不得早于** barrier/TRIM。
- bounded：复用 value_io_policy 风格的 bounded inflight，参考 M07 的 bounded sender。

### 5.7 value 回收判定 + deferred 队列（FF §7.2/§7.3）

tree_state 新增 `deferred_value_reclaim`（按 data_ver 有序，或 FIFO + 周期全扫；FF §7.3 用 `while front().data_ver ≤ rsl` 暗示有序）。本 step 用 **min-heap / 有序结构**（按 data_ver）以支持 §7.3 的 while-pop。

```
对每个 retired_value_ref rvr（来自 guard_retired.old_tree_values + gen_losers）:
    if rvr.data_ver <= state.recovery_safe_lsn:  dead.push(rvr.vr)
    else:                                        deferred_value_reclaim.push(rvr)
dead 非空 → value::reclaim_values(dead)（bounded）

周期扫描（reclaim_round 末 + recovery_safe_lsn 推进后）:
    while !deferred.empty() && deferred.min().data_ver <= state.recovery_safe_lsn:
        dead2.push(deferred.pop().vr)
    dead2 非空 → value::reclaim_values(dead2)
```

- gen-loser 与 guard-retired 的 old_tree_values **同一判定**（都是 `retired_value_ref{vr,data_ver}`）。gate 差异（gen-loser 需 gen 释放）已由"gen 析构才 post"在源头满足——到了 tree consumer 只剩 data_ver gate。
- **INC-052 trust boundary**：`reclaim_values` 对输入 blind trust（`value/space_manager.hh` 明确不做 liveness defense）；本 step 是 caller，必须保证投进去的 vr 对 tree + outstanding read_handle 皆 dead。**三条 provenance + gate（缺一不可，codex 修正）**：
  1. **provenance**：该 vr 必须是 flush/fold 阶段判定为"被本轮 winner 覆盖（或被 tombstone 取代）的旧 tree-visible value / memtable loser"——即它已**不在** new manifest / 任何活跃 PRS。这是它进 `retired.old_tree_values` / `gen.loser_durable_refs` 的前提（FF §5.1/§5.3 的挂接条件），不是 reclaim 阶段重新推导。
  2. **guard/gen 释放**：guard_retired 来自 G0 refs→0（无 reader 可达旧 manifest）；gen-loser 来自 gen refs→0（无 PRS 可达该 gen）。
  3. **data_ver gate**：`data_ver ≤ recovery_safe_lsn`（无 recovery 翻盘，§5.4 B frontier 含 wal）。
  三者皆满足才 dead。仅"G0 refs→0 + data_ver≤rsl"**不足以**单独证明任意输入 dead——必须叠加 provenance（1）。**不加 liveness 校验**（INC-052 裁决），只加 counter（§5.10）把推导错降级为可报警。
- **deferred 扫描触发点**：reclaim_round 末 + **每次 recovery_safe_lsn 推进后**（flush 推进 flush_durable_frontier、或 WAL reclaim 推进 wal_frontier，§5.8 都会 recompute → 触发扫描），不只挂普通 reclaim_round 末尾。

### 5.8 reclaim consumer pipeline + WAL reclaim 闭环（裁决 D4，含 B4/B5）

#### 5.8.1 reclaim_round pipeline

`tree/sender.hh` 组 `reclaim_round`（参照 `tree_local_flush` 的 owner-driven 分步），tree_sched advance 在
`持有 tree mutation token && (pending 非空 || frontier 推进过)` 时 submit：
1. `submit_reclaim_collect`：drain `reclaim_q`（mpmc）→ 累积 {old_slots, old_ranges, value_refs(retired+gen-loser)}。
2. old_ranges：invalidate fan-out（§5.5）→ reduce。
3. TRIM bounded（§5.6）→ reduce。
4. old_ranges → `tree_allocator.recycle`（§5.9，barrier 已过）。
5. value_refs：data_ver gate（§5.7）→ dead → `value::reclaim_values`（bounded）；不足 → deferred。
6. **WAL reclaim（B4，见 §5.8.2）** → recompute `recovery_safe_lsn` → 扫 deferred。
7. finish：释放 token。

#### 5.8.2 WAL reclaim 闭环（B4，codex review 顺出的漏环）

**现状**：`wal::reclaim_check` 只有 sender/handle，**无任何 production caller**（`wal/sender.hh:18`）——sealed_segments 永不缩、frontier 永不推进、recovery_safe_lsn + deferred 永久卡住。step 2 必须把这条闭环接上。

**驱动**：每当 `flush_durable_frontier = min(flush_max_lsn, superblock_safe_lsn)` 推进（flush_round 完成后，或 reclaim_round 内）：
```
tree_sched: just() >> on(wal_space_sched) >> wal::reclaim_check(flush_durable_frontier)
  → wal 回收 max_lsn ≤ flush_durable_frontier 的 sealed 段、更新 global_min_unreclaimed_lsn cell
  → 回 tree_sched: recompute_recovery_safe_lsn()（§5.4 B）→ 扫 deferred_value_reclaim
```
- 这是 tree_sched → wal_space_sched 的 PUMP sender（守 `feedback_cross_sched_pump_only`），不是 advance 内直接调。
- 触发点：(a) flush_round_once（055）末尾追加一跳触发 WAL reclaim + recompute；(b) reclaim_round step 6。本 step 在 flush_round 之外**新增独立 reclaim/maintenance round**驱动，避免改 055 的 flush_round（除非 055 末尾加触发更简——实现期裁，优先独立 round）。

#### 5.8.3 互斥 gate（裁决 D4 / B5，FIFO 公平）

flush_round 的 tree 阶段（`tree_local_flush`）与 reclaim_round 都改 `tree_allocator` + owner cache，必须互斥。
**裁决：owner-local `tree_mutation_gate`（FIFO token），不是会失败的 bool。**
- flush 与 reclaim 各自在进入 tree-mutation 步骤前 acquire token，结束 release；waiter FIFO 排队（不忙等、不失败）。
- **不能在 coord 已置 `catalog_update_in_progress_` 后再阻塞**（否则 seal 也被堵）：flush 的 tree 阶段 token 在 `tree_local_flush` 内部 acquire（tree_sched 域内），与 coord 的 capture/frontier_switch 解耦——coord 侧 capture 只设 catalog gate，tree 阶段排队等 token 期间 catalog gate 已持有但 reclaim_round 短（TRIM + 几个 op），可接受；**且 reclaim_round 不在 flush 的 tree 阶段窗口内启动**（acquire 失败即排后），所以 flush 等的至多是一个已在飞的短 reclaim_round。
- **公平**：reclaim/flush finish 时 FIFO 唤醒下一个 waiter（bounded alternation），防持续 flush 饿死 reclaim。
- 与 coord `catalog_update_in_progress`（055，串 CAT 安装）**正交**：那个串 coord 的 CAT 安装，这个串 tree 的 mutation；两者不同 owner、不同对象。

### 5.9 `tree_allocator.recycle` + barrier（RSM §4.4）

```cpp
void recycle(format::range_ref r) {
    // barrier 已由 reclaim_round 在调用前完成（§5.5）；此处只做逻辑归还。
    free_ranges.try_enqueue(std::move(r));   // 失败处理见下
}
```
- **裁决**：invalidate barrier 放在 **reclaim_round pipeline**（§5.8），不放进 `recycle` 内部——因为 barrier 是异步 fan-out（PUMP sender），不能在同步 `recycle` 里 wait。`recycle` 保持同步、只逻辑归还。RSM §4.4 伪码把 barrier 写进 recycle 是**语义**表达；实现上 barrier 在 pipeline 完成后才调 recycle，等价且符合 PUMP（不在同步函数里阻塞等 ack）。
- `free_ranges.try_enqueue` 满（4096）→ 本 step **panic**（与 INC-053 正交；INC-053 改 coalescing map 后此约束放宽，本 step 不做 INC-053）。

### 5.10 裁决 D5 — INC-052 可观测 counter

`value_alloc_sched` 加 atomic `reclaim_stats`（INC-052 方向，逐字）：
```cpp
struct reclaim_stats {
    std::atomic<uint64_t> reclaim_total_refs{0};
    std::atomic<uint64_t> partial_into_dirty{0}, partial_into_open{0},
        partial_into_allocatable{0}, partial_into_cache{0},
        partial_into_hole{0}, partial_into_untracked{0};
    std::atomic<uint64_t> whole_into_dirty{0}, whole_clears_existing{0},
        whole_already_pending{0}, dropped_freed_mask_zero{0};
};
```
- 在 `handle_reclaim` 的各分支 bump 对应 counter；facade 暴露 inspector。
- 稳态 `partial_into_untracked ≈ 0`；持续非零 = 上层 dead set 推导出错的一手信号（把 silent corruption 降级为可报警）。
- **不加 liveness 校验防御**（INC-052 明确）。

---

## 6. 关键正确性点 / landmine

| # | 点 | 处理 |
|---|---|---|
| L1 | 析构器在任意线程跑 → 不能 inline 干活/发 NVMe | 只 post reclaim_task 到 per_core::queue（cross-core 安全），work 在 tree_sched consumer |
| L2 | old range recycle 前必须 invalidate barrier（否则裸 frame_id 复用读到旧页） | §5.5 fan-out + wait-all-acks；pin_count>0 = panic |
| L3 | value reclaim 必须 gate `data_ver ≤ recovery_safe_lsn`，且 recovery_safe_lsn 必须含 wal_frontier | §5.4 修公式；§5.7 gate + deferred |
| L4 | reclaim 与 flush 都改 tree_allocator → 必须互斥 | §5.8 `tree_mutation_in_flight` |
| L5 | gen-loser 的 gate 1（gen 释放）必须在源头满足 | §5.3 只在 gen 析构 post |
| L6 | reclaim_q 满 / free_ranges 满 = 盘 leak，不能 silent | §5.1/§5.9 panic（约束 A） |
| L7 | deferred_value_reclaim 必须按 data_ver 有序支持 while-pop | §5.7 有序结构 |
| L8 | INC-052：reclaim_values blind trust，dead set 推导错=silent corruption | §5.7 正确性论据 + §5.10 counter（不加校验） |

---

## 7. 异常处理

- reclaim_round 任一阶段失败（invalidate / TRIM / reclaim_values 抛）：本 step **fail-fast panic**。理由：reclaim 是后台 GC，部分失败会留下不一致的回收状态（barrier 过了但没 TRIM、或 TRIM 了没 recycle），没有干净的中间态恢复；且这些操作（owner-local cache 失效、nvme TRIM、value release）正常不该失败，失败即不变量破坏。panic process-fatal，重启走 recovery（step 4）重建。
- 唯一非 panic：value `data_ver > recovery_safe_lsn` → 进 deferred（正常流，不是异常）。
- `tree_mutation_gate` token 不依赖 panic 前清理（panic process-fatal，gate 随进程消失）。

---

## 8. 容量与性能

- **reclaim 是后台路径**，不在前台 write/read 热路径。
- reclaim_q（mpmc）：每个 reclaim_task = 一个退役 guard/gen。稳态下 guard/gen 析构频率 ≈ flush/seal 频率（每轮 flush 退役 ~1 guard、每轮 seal 退役 gen 随 release）；容量取足够大常数，远超在飞退役数；撞上限 = 异常堆积（panic 暴露）。
- invalidate barrier：每 reclaim_round 对 old_ranges fan-out K 个 read_domain，每个失效该 range 的 leaf cache 条目（O(slots_per_range) 或 O(cache size) 取决遍历）。range 数 = 本轮退役 range，远小于全树。
- TRIM / reclaim_values bounded inflight，不打爆 nvme/value queue。
- 10 亿 KV 稳态：reclaim 轮数随 flush 轮数，每轮成本 ∝ 本轮退役对象数（与 dirty leaf 同量级），不随总量爆炸。
- deferred_value_reclaim 内存：暂不回收的 value ref 数 = recovery_safe_lsn 滞后窗口内的退役 value；有序结构 O(n log n)，n 受 recovery_safe_lsn 推进节奏约束。

---

## 9. 验证计划（production harness 形态）

1. **基本回收闭环**：write→seal→flush（退役旧 slot/value）→ 释放所有 read_handle + 触发 gen/guard 析构 → 驱动 tree_sched advance → 断言 reclaim_q 被 drain、TRIM 调用、`reclaim_values` 被调、退役 range 回 free_ranges。
2. **invalidate barrier**：退役 range 仍被 reader pin 时 reclaim → panic（构造 pin 不释放）；正常释放后 invalidate 成功、ack 全收。
3. **data_ver gate**：`data_ver > recovery_safe_lsn` 的 value → 进 deferred 不回收；推进 recovery_safe_lsn 后周期扫描回收。
4. **wal_frontier**：未回收 sealed segment 压低 recovery_safe_lsn → tombstone/value 不被 premature 回收；segment 回收后 recovery_safe_lsn 推进。
5. **gen-loser 路径**：overwrite 产生 memtable-only loser → gen 释放后 loser 经 reclaim 回收（gate 1+2）。
6. **reclaim/flush 互斥**：flush in-flight 时 reclaim 不启动，反之；串行交替都正确。
7. **多轮稳态**：连续 write/seal/flush/reclaim 多轮，断言盘空间（free_ranges 复用 + TRIM 计数）稳定、无泄漏、读一致。
8. **INC-052 counter**：正常路径 `partial_into_untracked == 0`。
9. **容量回归**（step 1 §1.2 的反向）：长跑下 tree_allocator head 不再单调爆涨（退役 range 被复用）。

> 注入点（barrier panic / fake TRIM / recovery_safe_lsn 控制）尽量用 test 侧 fixture，不给 production 加 hook；不可干净注入的报告并靠 review 覆盖。

## 10. 文档回写清单（land 后）

- cross_doc §1：`reclaim`（tree_sched handle）签名 + `invalidate_range`（read_domain）；`recovery_safe_lsn` 公式引用。
- cross_doc §2：`reclaim_task`、`reclaim_sink`、`deferred_value_reclaim`、`reclaim_stats`、`wal_reclaim_frontier` carrier。
- cross_doc §5：新增 Reclaim pipeline 路径（guard/gen 析构 → reclaim_q → invalidate fan-out → TRIM → recycle / value reclaim）。
- cross_doc §6（红线）：补 INC-052 `reclaim_values` precondition（caller 必须保证 vr 对 tree+read_handle 皆 dead）。
- RSM §4.4：`recycle` 的 barrier 语义 vs 实现（barrier 在 pipeline、recycle 同步）；§4.9 recovery_safe_lsn 实现已含 wal_frontier；§8 对象生命周期补 reclaim consumer。
- FF §7：实现已对齐链；§5.3 gen-loser drain 实现位置。
- known_issues：INC-023 resolved；INC-022 reclaim 项完成；INC-052 counter 部分落地（trust boundary spec 项随红线）；INC-053 仍 open（本 step 未做 coalescing）；INC-054 仍 blocked（step 3）。

## 11. 后续 step
- **step 3**：INC-054 tree allocator push-floor + ENOSPC（撞墙 graceful 而非 panic）。本 step reclaim 把 retired range 还 allocator 后，floor 协议才有真实推进点。
- **step 4**：boot recovery（recovery = flush-like merge，复用 step1-3 落定的 manifest/retire/recovery_safe_lsn 机器）。

## 12. 冲突与裁决记录

| # | 缺口 | spec | 裁决 |
|---|---|---|---|
| D1 | 析构器（core L0）如何 reach tree_sched.reclaim_q（L2） | FF §4.6 写 `tree_sched->enqueue_reclaim`，未说跨层机制 | core 定义 `reclaim_sink` 抽象 + 进程级 **atomic** 活跃 sink；tree 实现；否决"每 guard/gen 存 sink*"（§5.1）。实现期若 registry 已有线程安全 hook 则复用 |
| D2（重写） | wal_frontier 如何传 + 是否单调 + 与 WAL 段回收的循环 | RSM §4.9 公式，未说 cross-sched 机制、未点多流非单调 | **两个 frontier**：WAL 段回收用 `flush_durable_frontier`（破循环）；value GC 用 `recovery_safe_lsn=min(那个, wal_frontier)`，`wal_frontier` 基于**跨流所有未回收 WAL（含 active）的全局最低 LSN**（保守、单调）；wal 发布到 atomic cell，tree acquire 读（§5.4）。**原稿"只看 sealed min 单调"被否**（codex：多流 push_back 无序，会回落） |
| D3 | invalidate barrier 的实现机制 | RSM §4.4 写进 recycle 伪码（同步语义） | barrier 是 reclaim_round 的 PUMP fan-out（tree→read_domains leaf cache `take()` 检 pin + owner 本地清 non_leaf），完成后才 recycle（§5.5/§5.9）——等价 spec 语义、符合 PUMP 不阻塞同步函数 |
| D4（强化） | reclaim_round 与 flush_round 互斥 | spec 未显式 | owner-local **FIFO `tree_mutation_gate`**（非会失败的 bool）：waiter 排队、finish FIFO 唤醒（bounded alternation 防 flush 饿死 reclaim）；不在 coord 已置 `catalog_update_in_progress_` 后阻塞 seal（§5.8.3）。与 coord catalog gate 正交 |
| D5 | reclaim 失败语义 | spec 未明确 | fail-fast panic（后台 GC 部分失败无干净中间态）（§7） |
| D6 | reclaim_q / free_ranges 满 | spec 未明确 | panic（退役对象丢弃=盘 leak，不 silent；约束 A）（§5.1/§5.9） |
| **B1** | ingress 队列承接任意析构线程 | — | **`mpmc::queue`**，非 `per_core::queue`（后者按 this_core_id 选 SPSC lane，非 runtime 线程 post 会竞争/越界，codex 顺出）（§5.1） |
| **B2** | sink 生命周期 / teardown | — | atomic 发布；`destroy_runtime` **首步** null sink（之后析构跳过 post），再删 tree_sched（§5.1） |
| **B4** | WAL reclaim 闭环无 production caller | — | step 2 接上：flush_durable_frontier 推进 → `reclaim_check` → wal 更新 frontier → tree recompute（§5.8.2，codex 顺出） |

## 13. 开工 checklist
- [x] 目标 / 范围 / 不做项明确（§0/§1）
- [x] 输入输出 carrier 明确（§5.1 reclaim_task/sink、deferred、stats、wal frontier cell）
- [x] scheduler owner 明确（析构器 post；tree_sched consumer；read_domain invalidate；value reclaim_values 复用；wal frontier 发布）
- [x] fail-fast 路径明确（barrier pin / reclaim 失败 / 队列满，§7/§5.1/§5.9）
- [x] 最小验证范围（§9，含 barrier panic / data_ver gate / wal_frontier / 互斥 / 容量回归）
- [x] spec 缺口显式裁决（§12 D1-D6，约束 C）
- [x] 三红线 + A/B 复核（§3.4/§3.5）+ INC-052 trust boundary（§5.7/§5.10）
