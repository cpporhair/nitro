# 023 — Flush Manifest Carrier 与 Round State

> 实现第二十三步。落地 `flush_development_plan.md` Phase 3：把 flush 必需的运行时 carrier 一次性立稳——`tree_manifest.leaf_order`、`tree_sched` skeleton、`tree_flush_request / tree_flush_result`、round-owned immutable arrays。
>
> 本 step 仍然是**设计 step**：只确定数据 carrier 的形状、生命周期与跨 scheduler 的 borrow 边界；不做 fold / leaf mapping / candidate build / writer 任何算法。Phase 4-7 在本 step 立下的 carrier 上接算法。
>
> Phase 0（设计同步）由 step 020 完成；Phase 1（front input / memtable common carrier）由 step 021 完成；Phase 2（geometry + read-domain pairing seam + worker skeleton）由 step 022 完成。

## 本 step 覆盖的目标

| 目标 | 说明 |
|---|---|
| G1 | `tree_manifest` 扩出 per-manifest immutable `leaf_order`，包含 fence bytes pool + sorted `leaf_span[]`，10 亿 KV 工作集可以装得下 |
| G2 | 落地 `tree_flush_request / tree_flush_result`，字段语义对齐 RSM §4.2（含 sealed_gens 的 owning 修订与 status 字段的修订，见 §与 RSM 偏差） |
| G3 | 落地 `flush_round_state`：所有跨 stage 的 borrowed span / view 必须 borrow 自这一处，绝不直接 borrow 调用方栈或 pipeline 临时对象 |
| G4 | 落地 `tree_sched` skeleton（`apps/inconel/tree/owner_scheduler.hh`），`tree_state` 字段与 RSM §4.1 一一对应；`handle_tree_flush` 在 Phase 3 走 value-path 直接返回 `unsupported_unimplemented`，不偷做 fold / lookup / worker / writer 任意一段 |
| G5 | `core::registry` 暴露 `tree_sched` 单例 helper；`runtime::builder` 把 `tree_sched` 加进 PUMP runtime tuple，安装在 cores[0] |
| G6 | `flush_lookup_req / flush_worker_req` 重新挂上 borrowed payload（022 review M-3 之前临时拿掉的那条），但 owning side 锚定在 `flush_round_state`，跨 `per_core::queue` 安全 |
| G7 | `core/checkpoint_guard.hh` 最小落地（只持 `shared_ptr<const tree_manifest>`），让 `tree_flush_request.base_guard` 有真实可 pin 的对象 |

## 本 step 不覆盖

| 不做项 | 归属阶段 |
|---|---|
| memtable fold / sorted `flush_key_group[]` 构造 | Phase 4 |
| `keys_to_leaf_groups()` 算法与 lookup fanout sender | Phase 5 |
| `build_leaf_candidates()` 真实 old leaf read / merge / compact | Phase 6 |
| `plan_tree_delta_from_candidates()` / bounded writes / device flush / `finish_flush_round` | Phase 7 |
| leaf split / parent rewrite / root change / consolidation | Phase 8 |
| `frontier_switch / install_cat / release_gens` 对 `tree_flush_result` 的消费 | Phase 3 后续 frontier switch step |
| `checkpoint_guard::~checkpoint_guard()` 投递 reclaim_task；`tree_state.reclaim_q` 的真实 producer 与 consumer | 同上 |
| `tree_allocator::allocate / recycle / data_area_heads` 实际接线 | Phase 7 |
| `update_superblock` request 与 `superblock_safe_lsn` 推进 | root-change writer 后置 step |
| cache / frame pool / inflight 从 lookup 向最终 `tree_read_domain` ownership 迁移 | Phase 5 / Phase 6 前置专门 step |
| recovery 文档同步到 tree-domain 新拓扑 | 邻接独立 step（020/022 review 已经提醒过两次） |
| 任何 production 写入 / 读取测试 | 对应阶段的实现 step |

> 反面规则同 022 §"反面规则"：实现 Phase 3 的过程中如果发现"必须顺便做一点 fold/写盘/frontier switch 才能跑通"，那是越界信号——停下来把它挪到对应 step，不要在本 step 中偷带。

## 容量估算（per-manifest leaf_order）

按 INDEX.md 顶部硬约束（10 亿 KV、`RocksDB × 5` 性能取向）做 per-manifest 内存预算。32 字节 key、相邻 leaf 共享 fence、leaf_span metadata 紧凑布局：

| Page size | 1B KV 的 leaf 数 | leaf_span metadata（24 B / leaf） | fence pool（去重，~32 B / leaf） | 单 manifest 总占用 | flush 期峰值（新+旧 manifest） |
|---|---:|---:|---:|---:|---:|
| 16 KiB | ~3.86 M | ~93 MB | ~124 MB | **~217 MB** | ~434 MB |
| 4 KiB  | ~15.6 M | ~375 MB | ~500 MB | **~875 MB** | ~1.75 GB |

口径与方法：

1. leaf 数取自 INDEX.md "容量快速校准参考" 表，按 32 字节 key + (4K | 16K) tree page 给出。
2. `leaf_span` 一条 24 字节：`fence_lower_off:4 + fence_upper_off:4 + fence_lower_len:2 + fence_upper_len:2 + leaf_range_base:10 + 2 字节对齐` = 24。
3. fence pool 按"相邻 leaf 共享 fence"估算：第 `i` 条 leaf 的 `upper_bound_exclusive` 与第 `i+1` 条 leaf 的 `lower_bound_inclusive` 复用同一段 offset，所以总 fence bytes ≈ `(num_leaves + 1) × key_len ≈ num_leaves × key_len`，不是 `2 × num_leaves × key_len`。
4. 没有按 leaf 单独 heap allocation：`fence_pool` 是单段 contiguous storage（详见 §详细设计 §1）。

可读结论：

1. 16K page 是 v1 容量模型下默认 page size 的支撑论据；4K page 在 1B KV 量级已经接近 GB 级 manifest 工作集，仍然可用，但代价更高。
2. fence dedupe（相邻 leaf 共享 offset）不是优化，而是 spec：未去重时 4K page 工作集会翻倍到 ~1.7 GB，明显伤 INDEX.md 顶部硬约束，**Phase 3 必须冻结**。
3. `leaf_span` 字段宽度被 `uint32_t` offset / `uint16_t` len 卡住：`fence_pool` 单段最大 4 GiB、单条 fence 最大 64 KiB。两条都明显宽于 32 B key + 1B KV 的实际需要，留出充足 headroom。
4. flush 期"新+旧 manifest 同时活"是必然的——旧 reader 仍 pin 旧 guard，本轮 flush 要构造新 manifest——所以预算要按 2× 计。`new_manifest.leaf_order` 的构造由 Phase 7 实现，但内存上限**现在就要拍板**。

## 文件结构

```text
apps/inconel/
├── core/
│   ├── tree_manifest.hh           — 更新：加入 leaf_order_index 字段与最小访问器
│   ├── leaf_order.hh              — 新增：leaf_order_index / leaf_span / key_fence 视图
│   ├── checkpoint_guard.hh        — 新增：minimal checkpoint_guard（只持 shared_ptr<const tree_manifest>）
│   ├── retired_objects.hh         — 新增：retired_objects 聚合体（old_slots / old_ranges / old_tree_values）
│   └── registry.hh                — 更新：加入 tree_sched 单例 + helper
├── tree/
│   ├── flush_types.hh             — 更新：新增 tree_flush_request / tree_flush_result / flush_error；扩展 lookup_req / worker_req 加回 borrowed payload
│   ├── flush_round_state.hh       — 新增：flush_round_state，所有跨 stage borrow 的 owning side
│   ├── owner_scheduler.hh         — 新增：tree_state + tree_sched skeleton + _tree_flush op/sender + advance() 拆段
│   ├── scheduler.hh               — 更新：include owner_scheduler.hh
│   └── sender.hh                  — 更新：暴露 tree_flush(tree_sched*, tree_flush_request) facade
└── runtime/
    └── builder.hh                 — 更新：把 tree_sched 加进 inconel_runtime_t tuple，安装在 cores[0]，destroy 顺序对齐
```

显式**不**在本 step 出现的文件：

```text
apps/inconel/tree/flush_pipeline.hh    // tree-local flush sender 编排是 Phase 4-7 的事
apps/inconel/coord/                    // frontier_switch / install_cat 后置 step
apps/inconel/recovery/                 // recovery 同步 step 单独走
```

## 设计目标

1. **不在 Phase 3 落任何 flush 算法**。Phase 3 只产 carrier 与 owner skeleton；fold / leaf mapping / candidate build / writer 都明确留给 Phase 4-7 各自的 step。
2. **borrowed view 必须有 stable owner**。022 review M-3 已经踩过"`flush_worker_req` 内 `std::span<flush_leaf_group>` 借调用方栈"的坑：sender op 把 req copy 进 PUMP queue，worker 在另一个 advance tick 才消费，调用方的 backing storage 早已不存在。Phase 3 把所有跨 scheduler borrow 锚定到 `flush_round_state`——这是 tree_sched 在 round 期间唯一的 owning side。
3. **`tree_state` 与 RSM §4.1 一一对应**。skeleton 不允许引入"临时字段"绕过 RSM；任何与 §4.1 字段名/语义偏离的，必须在本文 §与 RSM 偏差 节里挂明。
4. **leaf_order 容量布局必须能扛 INDEX.md 顶部硬约束**。Phase 3 不构造它，但 layout 必须现在就过 1B KV 的 per-manifest 估算（见 §容量估算）。
5. **carrier 不持 cache、不持 frame pool、不跨 core**。`tree_state` 永远 owner-local 单线程；`flush_round_state` 由 tree_sched 单线程持有，lookup / worker 拿到的是只读 span。Phase 3 不偷带 cache ownership migration。

## 设计决策

| #   | 决策点 | 结果 | 说明 |
|---|---|---|---|
| `D1` | 本 step 性质 | **设计 step（carrier-only）** | 与 022 同样收紧：只立 carrier、owner skeleton 与 borrow 边界；fold / lookup / worker / writer 全部留给 Phase 4-7 |
| `D2` | `tree_manifest.leaf_order` 形态 | **manifest-owned `leaf_order_index { fence_pool, spans }`，spans 引用 fence_pool 内的 offset/len** | 1B KV 量级单 manifest 已 GB 级，禁止每 leaf 一次 small allocation |
| `D3` | leaf fence 是否相邻去重 | **必须去重：`leaf[i].upper_off == leaf[i+1].lower_off`** | 见 §容量估算结论 2；不是优化是 spec |
| `D4` | `leaf_span` 字段宽度 | **`fence_*_off: uint32_t, fence_*_len: uint16_t, leaf_range_base: paddr`，紧凑 24 B** | 4 GiB pool 上限 + 64 KiB 单条 fence 上限，明显宽于 1B KV / 32 B key 需要 |
| `D5` | empty bootstrap manifest 的 leaf_order | **`fence_pool` 为空、`spans` 为空；`tree_manifest::empty(geom)` 直接构造空 leaf_order** | 空树合法情形；reader 在空树上 lookup 走 `!has_root()` 短路（Phase 2 已实现），与 leaf_order 无关 |
| `D6` | leaf_order 是否单独持久化 | **不持久化** | RSM §4.5 已冻结：runtime-only immutable，recovery 重建 runtime manifest 时一并重建。Phase 3 不动 recovery |
| `D7` | leaf_order 是否在原地修改 | **不允许；新 flush 构造新 manifest + 新 leaf_order** | RSM §4.5 / OV §4.4 / FF §3.8 已冻结 |
| `D8` | `tree_flush_request.sealed_gens` 容器 | **`absl::InlinedVector<std::shared_ptr<memtable_gen>, 8>`（owning），不是 `std::span`** | 偏离 RSM §4.2 原文；理由同 022 review M-3：req 进 `per_core::queue` 后调用方栈不可依赖。InlinedVector 在 Phase 4 的常见 round 大小下零分配 |
| `D9` | `tree_flush_request.base_guard` | **`std::shared_ptr<const checkpoint_guard>`** | 与 RSM §4.2 一致；checkpoint_guard 在 Phase 3 最小落地 |
| `D10` | `tree_flush_result` 状态字段 | **统一用 `flush_stage_status st`，不沿用 RSM §4.2 的 `bool ok + flush_error error`** | 与 022 D11 / `flush_candidate_batch.st` / `flush_leaf_group_result.st` 一致；偏离 RSM 原文，进 §偏差 表 |
| `D11` | `flush_error` 是否单独存在 | **不引入；状态全部走 `flush_stage_status`** | 避免 result 字段同时出现两层 status |
| `D12` | `tree_flush_result.flushed_max_lsn` 字段 | **保留，但 Phase 3 始终回 0**（Phase 7 才有真实 fold winner） | RSM §4.2 字段名一致；Phase 3 不偷算它 |
| `D13` | `flush_round_state` 落哪 | **`apps/inconel/tree/flush_round_state.hh`，独立头文件** | 跨 stage carrier，预计每个 phase 都会扩字段；放进 flush_types.hh 会让那个文件膨胀 |
| `D14` | `flush_round_state` 生命周期 | **`std::unique_ptr<flush_round_state>`，由 `tree_sched.handle_tree_flush` 在 round 开始时分配，round 结束时释放** | Phase 3 的 handle 立即 unsupported 并立即释放；Phase 4 起把 round_state 移进 `tree_state.active_rounds` 供后续 stage 使用，本 step 不预创建该 map |
| `D15` | round_state 内的中间数组形态 | **owning 容器（`std::vector` / `absl::InlinedVector`），跨 stage 暴露 `std::span` view** | "round-owned immutable arrays + borrowed spans" 模式（dev plan §4.4 明确） |
| `D16` | `flush_lookup_req / flush_worker_req` 是否重新挂载 borrowed payload | **是；payload 是 `std::span<...>` 借自 round_state，文档中明确 owner 是 `flush_round_state`** | 022 临时移除是因为 owning side 不存在；Phase 3 owning side 落地后这条约束解除 |
| `D17` | `tree_sched` 实例数 | **单实例（singleton）** | RSM §1 / OV §1.7 已冻结 |
| `D18` | `tree_sched` 安装位置 | **cores[0]，与 `value_alloc_sched` 一致** | 单 singleton；其他 core 在 PUMP per-core tuple 里挂 nullptr 占位 |
| `D19` | `tree_state` 字段 | **严格对齐 RSM §4.1：`alloc / flush_max_lsn / superblock_safe_lsn / recovery_safe_lsn / reclaim_q`** | 不允许"先放一半字段、其他后补"——RSM §4.1 已经是冻结 spec |
| `D20` | `tree_allocator` 实际接线 | **不在 Phase 3 实装；本 step 只放 placeholder struct 占住字段名 `alloc`** | 真正的 `allocate / recycle / data_area_heads` 在 Phase 7 才有写盘需求；提前接线会把 value 侧的 `data_area_heads` 也牵进来 |
| `D21` | `tree_state.reclaim_q` 在 Phase 3 的行为 | **存在但 Phase 3 advance() 不 drain；无 producer、无 consumer** | 真实 producer 是 `~checkpoint_guard()`，留给 frontier_switch step；真实 consumer 是 TRIM dispatch，留给 Phase 8+ |
| `D22` | `tree_sched.handle_tree_flush` Phase 3 行为 | **校验 base_guard / sealed_gens 非空 → 直接 cb(`tree_flush_result { st = unsupported_unimplemented, ... }`)；不分配 round_state** | 一旦 Phase 3 在 handle 里偷偷分配 round_state、然后立即释放，会同时考验"分配/释放"路径却又验不到任何后续 stage——不增加证据，反而模糊 Phase 3/4 的分界。Phase 4 才是 round_state 真正进入 `active_rounds` 的地方 |
| `D23` | sender 路径 | **`tree::tree_flush(tree_sched*, tree_flush_request)` 暴露在 `tree/sender.hh`，op/sender/`op_pusher`/`compute_sender_type` 特化与 worker 一样跟着 `tree/owner_scheduler.hh` 走** | facade 一致；外部模块仍只 include `tree/sender.hh` |
| `D24` | `core::registry::tree_sched` | **新增 inline 单例指针 + `tree_sched_singleton()` accessor，与 `value_sched()` 同形** | per-core helper 不需要——单例直接通过 singleton accessor 拿 |
| `D25` | runtime tuple 加 tree_sched 的位置 | **加到 tuple 末尾**（`mock_nvme, lookup, worker, value, tree_sched`） | 与 value 一起作为 singleton 挂尾；advance 顺序不影响 flush 正确性，因为 tree_sched 是 round owner，input 经由 queue 进入 |
| `D26` | destroy 顺序 | **tree_sched → tree_worker → tree_lookup → value → nvme** | tree_sched 无下游资源，先删；其余沿用 022 §8 顺序 |
| `D27` | `checkpoint_guard` Phase 3 形态 | **只含 `std::shared_ptr<const tree_manifest> manifest;` 一个字段** | `retired` 字段留给 frontier_switch step；OV §5.2 明确 destructor 会投递 reclaim_task，但那条链路 Phase 3 不需要 |
| `D28` | `retired_objects` 放哪 | **`apps/inconel/core/retired_objects.hh`，与 checkpoint_guard 解耦** | tree_flush_result 直接持有 `retired_objects` by-value，不依赖 checkpoint_guard；frontier_switch step 再把它挂到旧 guard 上 |
| `D29` | `retired_value_ref` 复用情况 | **复用 `core/memtable.hh` 已有定义**（Phase 1 D14） | 不重复定义 |
| `D30` | flush_round_state 是否预留 active_rounds map | **不预留** | Phase 3 的 handle 不存 round_state；Phase 4 加 fold 时自然会引入 `tree_state.active_rounds`，把 carrier 改动留给那一步更准确 |

## 详细设计

### 1. `core/leaf_order.hh` — `leaf_order_index / leaf_span / key_fence`

```cpp
namespace apps::inconel::core {

    using format::paddr;

    // ── leaf_span ──
    //
    // Per-leaf positional record inside the immutable leaf_order
    // (RSM §4.5). Fence bytes are NOT carried inline; the index owns
    // a single contiguous `fence_pool` and each span addresses its
    // bounds via {offset, length}. Adjacent leaves share fence
    // storage (D3): for any i, the bytes at
    // `[upper_off[i], upper_off[i] + upper_len[i])` are identical to
    // `[lower_off[i+1], lower_off[i+1] + lower_len[i+1])`, so we
    // store them once and let both leaves point at the same offset.
    //
    // Field widths (D4):
    //   uint32 offsets cap fence_pool at 4 GiB
    //   uint16 lengths cap a single fence at 64 KiB
    // Both ceilings sit far above the 1B KV / 32B key working set —
    // a 1B KV / 4K-page tree fence pool sits at ~500 MB total (see
    // §容量估算).
    //
    // The 24 B layout below assumes 2 bytes of natural padding after
    // the lengths; static_assert keeps it locked.
    struct leaf_span {
        uint32_t fence_lower_off;
        uint32_t fence_upper_off;
        uint16_t fence_lower_len;
        uint16_t fence_upper_len;
        paddr    leaf_range_base;
    };
    static_assert(sizeof(leaf_span) == 24);

    // ── leaf_order_index ──
    //
    // Per-manifest immutable leaf order (RSM §4.5 / FF §3.4 / OV
    // §4.4). Owns the fence bytes and the sorted span vector by
    // value: when the owning tree_manifest snapshot drops its last
    // shared_ptr, every fence byte and every span retire at the
    // same time. There is no other pin path.
    //
    // The index is read-only after construction. Construction happens
    // in two places:
    //   1. Bootstrap empty path — `leaf_order_index{}` (D5).
    //   2. Phase 7 root-stable writer — rebuilds the new index from
    //      the old one + write_plan + allocations + consolidations
    //      (FF §3.8 `rebuild_leaf_order_from_tree_delta`). Phase 3
    //      does not implement that.
    //
    // The two `fence_*_view(span)` accessors return string_view into
    // the pool. They are valid for the same lifetime as the owning
    // `leaf_order_index` — i.e. for as long as the consuming
    // `tree_manifest` is pinned by some `checkpoint_guard`.
    struct leaf_order_index {
        std::string             fence_pool;   // owning bytes (D2)
        std::vector<leaf_span>  spans;        // sorted by lower_bound

        bool empty() const noexcept { return spans.empty(); }
        std::size_t size() const noexcept { return spans.size(); }

        std::string_view
        fence_lower(const leaf_span& s) const noexcept {
            return std::string_view(fence_pool).substr(
                s.fence_lower_off, s.fence_lower_len);
        }
        std::string_view
        fence_upper(const leaf_span& s) const noexcept {
            return std::string_view(fence_pool).substr(
                s.fence_upper_off, s.fence_upper_len);
        }
    };

}  // namespace apps::inconel::core
```

约束：

1. `leaf_order_index` 是 by-value 嵌进 `tree_manifest`（不是 pointer），随 manifest snapshot 一起 deep copy / move / drop。
2. `fence_pool` 用单段 `std::string`（heap 一次分配）；不允许 leaf 数级别的 small allocation。本 step 不冻结具体 builder 接口，那是 Phase 7 的事；本 step 只冻结 `leaf_order_index` 的字段、构造空表的能力，以及 `fence_lower / fence_upper` 视图语义。
3. `leaf_span` 必须 `static_assert(sizeof == 24)` 锁住布局，避免后续提议加字段时悄悄涨到 32+ B。
4. **Phase 3 不引入 leaf_order builder**。空 leaf_order 通过默认构造得到；populated leaf_order 的构造形状属于 Phase 7 的 root-stable writer step。

### 2. `core/tree_manifest.hh` — 引入 leaf_order

`tree_manifest` 在 Phase 2 已经持 `const tree_geometry*`。Phase 3 加一个 by-value `leaf_order_index leaf_order;` 字段：

```cpp
struct tree_manifest {
    paddr root_slot;
    absl::flat_hash_map<paddr, uint32_t> slot_map;
    const tree_geometry* geom;
    leaf_order_index leaf_order;       // NEW (Phase 3 G1)

    bool has_root() const { return root_slot.lba != 0; }

    paddr resolve(paddr range_base) const { /* unchanged */ }
    uint32_t page_lbas() const { return geom->page_lbas(); }

    static tree_manifest empty(const tree_geometry* g) {
        if (g == nullptr) panic_inconsistency(...);
        return {
            .root_slot = {0, 0},
            .slot_map  = {},
            .geom      = g,
            .leaf_order = {},          // empty index (D5)
        };
    }
};
```

约束：

1. `leaf_order` **by-value**——manifest 自己拥有它，不允许"manifest 持指针，pool 在外部"这种 split ownership。
2. `tree_manifest::empty()` 仍然只产空 manifest；Phase 3 不引入"非空 leaf_order 的工厂"。
3. `tree_manifest::resolve()` 与 `page_lbas()` 在 Phase 3 不变，避免破坏 Phase 2 的 lookup 路径（022 H-1 / M-2 / M-3 已经收敛）。

### 3. `core/checkpoint_guard.hh` — minimal guard

Phase 3 让 `checkpoint_guard` 第一次以可用形态出现：

```cpp
namespace apps::inconel::core {

    // ── checkpoint_guard ── (Phase 3 minimal form)
    //
    // OV §5.2 / RSM §4.6 give checkpoint_guard two responsibilities:
    //
    //   1. Pin the immutable tree_manifest snapshot used by readers
    //      that captured this guard.
    //   2. Carry the retired_objects bag whose destructor posts a
    //      reclaim_task to tree_sched.
    //
    // Phase 3 only needs (1) — `tree_flush_request.base_guard` must
    // outlive the entire round, and `tree_flush_result.new_manifest`
    // must be wrappable into a fresh guard later. The `retired`
    // member, the destructor, and the reclaim_task posting all
    // belong to the frontier_switch step (next-but-one). Splitting
    // it now would force Phase 3 to also bring in `reclaim_task`,
    // `data_area_heads`, and the TRIM dispatch — all of which are
    // expressly outside this step's scope.
    //
    // When the frontier_switch step lands, it MUST extend this
    // struct in place rather than introducing a parallel
    // `checkpoint_guard_with_retired` type.
    struct checkpoint_guard {
        std::shared_ptr<const tree_manifest> manifest;
    };

}  // namespace apps::inconel::core
```

约束：

1. **不在 Phase 3 加 destructor**。Phase 3 没有 reclaim_q producer，加了等于死代码 + 误导。
2. **不在 Phase 3 加 `retired` 字段**。frontier_switch step 会原地扩 `checkpoint_guard`，不允许造一个 `checkpoint_guard_v2`。
3. `manifest` 用 `shared_ptr<const ...>`：guard 拥有 manifest，guard 自身被 reader 用 `shared_ptr<const checkpoint_guard>` 持有。

### 4. `core/retired_objects.hh`

```cpp
namespace apps::inconel::core {

    // ── retired_objects ──
    //
    // Aggregate of everything a single flush round retires (RSM §4.2,
    // FF §5). Used by:
    //
    //   - tree_flush_result (this step) — by-value field, populated
    //     by Phase 7 writer in later steps.
    //   - frontier_switch step — appended onto the OLD checkpoint_guard's
    //     retire list (FF §4.1, §5.1, §5.2).
    //
    // Phase 3 only fixes the layout. Producers and consumers are out
    // of scope.
    struct retired_objects {
        absl::InlinedVector<format::paddr, 32>          old_slots;
        absl::InlinedVector<format::range_ref, 8>       old_ranges;
        absl::InlinedVector<retired_value_ref, 64>      old_tree_values;
    };

}  // namespace apps::inconel::core
```

约束：

1. `retired_value_ref` 复用 `core/memtable.hh` Phase 1 已有定义（D29）；不重复定义。
2. inline capacity（32 / 8 / 64）与 RSM §4.2 一致；后续如果发现 1B KV 工作集下需要更大 inline capacity，再统一调整。
3. 字段名必须与 RSM §4.2 中 `retired_objects` 完全一致（`old_slots / old_ranges / old_tree_values`），方便 cross_doc §2 校对。

### 5. `tree/flush_types.hh` — 扩展

Phase 2 落的内容继续保留（`flush_round_id / flush_stage_status / flush_lookup_req / flush_leaf_group / flush_leaf_group_result / flush_worker_req / flush_leaf_candidate / flush_candidate_batch`）。Phase 3 在同一头文件追加：

```cpp
namespace apps::inconel::tree {

    // ── tree_flush_request ──
    //
    // Owns its inputs by value. RSM §4.2 declares sealed_gens as a
    // span<>, but a borrowed span across the tree_sched ingress
    // queue would repeat the M-3 bug (022 review §1): the op copies
    // args into a heap req, the caller's stack frame returns, and
    // tree_sched.advance() consumes the req in a different scheduler
    // tick — by which time the original storage is gone. So Phase 3
    // promotes sealed_gens to an owning InlinedVector. The
    // shared_ptr copies bump refcount on the caller's core (correct
    // ownership transfer); tree_sched only sees fully pinned gens.
    //
    // Deviation logged in §与 RSM §4.1/§4.2 的偏差.
    struct tree_flush_request {
        std::shared_ptr<const core::checkpoint_guard>            base_guard;
        absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8> sealed_gens;
        uint64_t                                                  recovery_safe_lsn;
    };

    // ── tree_flush_result ──
    //
    // Status field is `flush_stage_status` (D10), not the original
    // `bool ok + flush_error error` shape. Aligned with worker /
    // lookup result statuses already in this header.
    //
    // In Phase 3, every successful path is impossible: the only
    // value tree_sched ever returns is
    //   { st = unsupported_unimplemented,
    //     new_manifest = nullptr,
    //     retired = {},
    //     flushed_gens_by_front = {},
    //     memtable_losers = {},
    //     flushed_max_lsn = 0 }.
    // Phase 4-7 progressively replace that with real content
    // without changing the field layout.
    struct tree_flush_result {
        flush_stage_status                                      st;
        std::shared_ptr<const core::tree_manifest>              new_manifest;
        core::retired_objects                                   retired;
        absl::flat_hash_map<
            uint32_t,
            absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8>>
                                                                flushed_gens_by_front;
        absl::InlinedVector<core::retired_value_ref, 64>        memtable_losers;
        uint64_t                                                flushed_max_lsn;
    };

    // ── flush_lookup_req — Phase 3 re-attaches borrowed payload ──
    //
    // The borrowed `groups` span points into the round-owned
    // `flush_round_state` arrays (see flush_round_state.hh).
    // tree_sched MUST not allow `flush_lookup_req` to outlive the
    // round_state it borrows from; Phase 5 will land the actual
    // dispatch logic that enforces it. Phase 3 only freezes the
    // shape so Phase 5 doesn't have to refloat the carrier.
    struct flush_lookup_req {
        flush_round_id                          round_id;
        uint32_t                                read_domain_index;
        const core::tree_manifest*              base_manifest;
        std::span<const flush_key_group>        groups;   // borrows from round_state
    };

    // ── flush_worker_req — same Phase 3 re-attachment ──
    //
    // The borrowed `leaf_groups` span points into round_state's
    // owning `leaf_groups` array. 022 review M-3 removed this field
    // because there was no stable owning side; Phase 3 establishes
    // one (flush_round_state, see §detailed design §6) and the
    // borrow becomes safe.
    struct flush_worker_req {
        flush_round_id                          round_id;
        uint32_t                                read_domain_index;
        const core::tree_manifest*              base_manifest;
        uint64_t                                recovery_safe_lsn;
        std::span<const flush_leaf_group>       leaf_groups; // borrows from round_state
    };

    // ── flush_key_group ──
    //
    // Per-logical-key fold result. Phase 3 only freezes the shape;
    // Phase 4 fold step populates it. memtable_winner / memtable_losers
    // are the references the rest of the pipeline carries through.
    //
    // None of the bytes are owned here:
    //
    //   - `key` is a std::string_view into the winner gen's kv_arena
    //     (RSM §3.2). The pin chain
    //     `tree_flush_request.sealed_gens[*]` keeps that arena alive
    //     for the round.
    //   - `winner_value` is a `value_handle` (POD). Both halves
    //     follow the same pin chain.
    //
    // Phase 3 forbids any "owning" variant — anything that would
    // require copying value bytes through the round_state belongs
    // outside flush.
    struct flush_key_group {
        std::string_view                                 key;
        uint64_t                                         winner_data_ver;
        core::memtable_entry::kind                       winner_kind;
        core::value_handle                               winner_value;  // valid iff winner_kind == value
        std::shared_ptr<core::memtable_gen>              winner_gen;
        // memtable losers are pushed onto winner_gen->loser_durable_refs
        // by the fold step itself (Phase 4); they are not carried on
        // this struct.
    };

}  // namespace apps::inconel::tree
```

约束：

1. `flush_key_group` 字段对齐 FF §3.3 fold 算法的输出形状。Phase 3 不实现 fold；只保证 shape 落地后 Phase 4 不需要再改。
2. `flush_lookup_req.groups` 与 `flush_worker_req.leaf_groups` 的 span 必须明确文档化为"borrows from `flush_round_state`"。Phase 3 的 worker handle 仍然只看 `round_id / read_domain_index / base_manifest / recovery_safe_lsn` 字段，不 deref `leaf_groups`——但字段必须存在，避免 Phase 6 又一次改 carrier。
3. `tree_flush_request` 与 `tree_flush_result` 字段名必须能直接对应 RSM §4.2 cross_doc §2 中 `tree_flush_request / tree_flush_result` 那一行（除了 §与 RSM 偏差 节中标出的两点）。

### 6. `tree/flush_round_state.hh`

```cpp
namespace apps::inconel::tree {

    // ── flush_round_state ──
    //
    // Owning side for everything a tree-local flush round produces
    // and consumes between stages. Borrowed views inside
    // `flush_lookup_req` / `flush_worker_req` (and any future flush
    // sub-stage carrier) point into this struct's owning vectors.
    //
    // Lifetime contract (Phase 3 freeze; Phase 4 starts using it):
    //
    //   1. `tree_sched` allocates `std::unique_ptr<flush_round_state>`
    //      when a `tree_flush_request` enters its handle and the
    //      round actually proceeds (Phase 3: no rounds proceed —
    //      handle returns unsupported_unimplemented before allocation).
    //   2. `tree_state.active_rounds` (added in Phase 4) keys the
    //      live round_state by `round_id` so each subsequent stage
    //      handle on tree_sched can `find()` it without going through
    //      the queue payload again.
    //   3. Each lookup_req / worker_req carries a `round_id` plus
    //      borrowed spans into the round_state. tree_sched MUST not
    //      free the round_state until every fan-out has fanned in.
    //   4. `finish_flush_round` (Phase 7) drops the round_state from
    //      `active_rounds` and frees it; the request callback fires
    //      with `tree_flush_result` populated from the round_state.
    //
    // Phase 3 only freezes the field layout below. Per-stage methods
    // (build sorted workset, install lookup result, install candidate
    // batch, etc.) are added by Phase 4-7 in their own steps.
    //
    // Important: `pinned_gens` / `pinned_base_guard` are the ONLY
    // strong references to the input that survive past the request
    // callback's argument scope. Once the request `tree_flush_request`
    // payload has been moved into round_state, the request struct
    // is no longer used.
    struct flush_round_state {
        flush_round_id                                                  round_id;
        std::shared_ptr<const core::checkpoint_guard>                   pinned_base_guard;
        absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8>     pinned_gens;
        uint64_t                                                        recovery_safe_lsn;

        // ── populated by Phase 4 (memtable fold) ──
        // Sorted by `key` ascending. Borrowed by lookup_req.groups.
        std::vector<flush_key_group>                                    workset;

        // ── populated by Phase 5 (lookup fanout merge) ──
        // Sorted by `leaf_range_base` ascending after the per-shard
        // fan-in dedupe runs back on tree_sched.
        std::vector<flush_leaf_group>                                   leaf_groups;

        // ── populated by Phase 6 (worker fanout) ──
        std::vector<flush_leaf_candidate>                               candidates;

        // ── populated by Phase 7 (writer + finish_flush_round) ──
        // Final result fields; mirror **every** tree_flush_result
        // field (including `st` and `new_manifest`) so finish_round
        // can hand them out without restructuring. See 023 review
        // H-1: if tree_flush_result gains a field, this section must
        // gain it too — anything less forces Phase 7 to stash result
        // state off to the side.
        flush_stage_status                                              st = flush_stage_status::ok;
        std::shared_ptr<const core::tree_manifest>                      new_manifest;
        core::retired_objects                                           retired;
        absl::flat_hash_map<
            uint32_t,
            absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8>> flushed_gens_by_front;
        absl::InlinedVector<core::retired_value_ref, 64>                memtable_losers;
        uint64_t                                                        flushed_max_lsn = 0;
    };

}  // namespace apps::inconel::tree
```

约束：

1. **Phase 3 不在任何 handle 里实例化 `flush_round_state`**（D22）。本头文件只定义类型，让 Phase 4 在 fold step 里填 `workset` 时不需要再加字段。
2. round_state 是 owning side：每一段中间数据用 `std::vector` / `absl::InlinedVector` 持有；跨 scheduler 的 borrow 全部通过 `std::span` 视图。Phase 4-7 在各自 step 文档里说明它们用的是哪个字段、span 起止规则。
3. round_state 不持 raw pointer；所有强引用走 shared_ptr。tree_sched 单线程持有 unique_ptr，不需要内部锁。
4. 字段顺序按 phase 推进给出，方便 review 时看到"哪段属于哪个 phase 的责任"。
5. **mirror 是硬合同**（023 review H-1）：`tree_flush_result` 的每一条字段——包括 `st`、`new_manifest`——都必须在本 struct 的 Phase 7 段出现，否则 `finish_flush_round` 就做不到 "不 restructuring 直接交付 result"，`flush_round_state` 也就不算 carrier freeze 完成。Phase 4-7 后续如果给 `tree_flush_result` 加字段，必须同步往这里加。

### 7. `tree/owner_scheduler.hh` — `tree_state` + `tree_sched` skeleton

```cpp
namespace apps::inconel::tree {

    // ── tree_allocator (Phase 3 placeholder) ──
    //
    // RSM §4.1 / §4.4 freeze the allocator shape. Phase 3 only needs
    // the field name to exist on tree_state so Phase 7 (root-stable
    // writer) can wire allocate / recycle / data_area_heads in place
    // without renaming. None of the methods are usable until then.
    //
    // The placeholder MUST stay non-functional until Phase 7 — any
    // call site that tries to use it before then is a phase-creep
    // bug and should be caught at code review.
    struct tree_allocator {
        format::paddr                       head{};         // populated by Phase 7
        std::vector<format::range_ref>      free_ranges{};  // populated by Phase 7
        // data_area_heads* shared_heads;   // wired by Phase 7
    };

    // ── tree_state ──
    //
    // RSM §4.1 owner state. Field names mirror the spec one-for-one.
    // Phase 3 lands every field but only `next_round_id` ever moves;
    // the LSN cursors stay 0 until later phases write to them, and
    // `reclaim_q` has no producer or consumer in Phase 3 (D21).
    struct tree_state {
        tree_allocator                                          alloc;
        uint64_t                                                flush_max_lsn = 0;
        uint64_t                                                superblock_safe_lsn = 0;
        uint64_t                                                recovery_safe_lsn = 0;
        pump::core::per_core::queue<reclaim_task*>              reclaim_q;          // Phase 3: no producer / no consumer
        // Phase 4 will add: absl::flat_hash_map<flush_round_id,
        //                       std::unique_ptr<flush_round_state>> active_rounds;
        // Phase 4 will add: uint64_t next_round_id = 1;
    };

    struct tree_sched;

    // ── PUMP req / op / sender for tree_flush ──
    namespace _tree_flush {

        struct req {
            tree_flush_request args;
            std::move_only_function<void(tree_flush_result&&)> cb;
        };

        struct op {
            constexpr static bool tree_flush_op = true;
            tree_sched*         sched;
            tree_flush_request  args;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_sched*        sched;
            tree_flush_request args;

            auto make_op() {
                return op{ .sched = sched, .args = std::move(args) };
            }

            template <typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };

    }  // namespace _tree_flush

    // ── tree_sched ──
    //
    // Singleton flush owner. Holds tree_state. In Phase 3 the only
    // ingress that ever drains is `flush_q`; reclaim_q exists for
    // structural fidelity to RSM §4.1 but is unused.
    //
    // Phase 3 handle behavior (D22):
    //
    //   1. Validate base_guard non-null.
    //   2. Validate sealed_gens non-empty (empty round is allowed by
    //      FF §2.3 but is currently produced by callers — Phase 4
    //      will handle the empty fast path; Phase 3 keeps it strict
    //      so call sites can't silently rely on the stub returning ok).
    //   3. Fire cb with
    //        tree_flush_result {
    //            .st = flush_stage_status::unsupported_unimplemented,
    //            .new_manifest = nullptr, ...,
    //        }
    //   4. delete the req.
    //
    // Phase 4 will replace step 3 with: allocate flush_round_state,
    // park into active_rounds, fan out to fold sub-pipeline.
    struct tree_sched {
        static constexpr uint32_t kMaxFlushOpsPerAdvance = 8;

        tree_state                                          state;
        pump::core::per_core::queue<_tree_flush::req*>      flush_q;

        explicit tree_sched(std::size_t flush_q_depth = 256)
            : flush_q(flush_q_depth) {}

        void schedule_flush(_tree_flush::req* r) {
            flush_q.try_enqueue(r);
        }

        // Sender factory; called from tree::tree_flush() facade.
        auto submit_flush(tree_flush_request args) {
            return _tree_flush::sender{ this, std::move(args) };
        }

        // Phase 3 advance(): drain at most kMaxFlushOpsPerAdvance
        // flush requests per tick. reclaim_q is intentionally NOT
        // drained (D21).
        bool advance();

        template <typename runtime_t>
        bool advance(runtime_t&) { return advance(); }
    };

    // PUMP op::start: same shape as tree_worker_sched.
    template <uint32_t pos, typename ctx_t, typename scope_t>
    void _tree_flush::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_flush(new _tree_flush::req{
            std::move(args),
            [ctx = ctx, scope = scope](tree_flush_result&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
        });
    }

}  // namespace apps::inconel::tree
```

`advance()` 的 Phase 3 实现要点（写在头文件 inline 处或对应 .hh 内联）：

```cpp
bool tree_sched::advance() {
    bool progress = false;
    for (uint32_t i = 0; i < kMaxFlushOpsPerAdvance; ++i) {
        auto item = flush_q.try_dequeue();
        if (!item) break;
        auto* r = *item;

        // Phase 3 fail-fast: any request whose payload doesn't satisfy
        // the carrier contract is a caller-side bug. Phase 4 will
        // relax to a structured invalid_request flush_stage_status,
        // but Phase 3 prefers panic so the failure surfaces near
        // the producer instead of being masked as
        // `unsupported_unimplemented`.
        if (r->args.base_guard == nullptr) {
            core::panic_inconsistency(
                "tree::tree_sched::advance",
                "tree_flush_request.base_guard is null");
        }
        if (r->args.sealed_gens.empty()) {
            core::panic_inconsistency(
                "tree::tree_sched::advance",
                "tree_flush_request.sealed_gens is empty");
        }

        // Phase 3 stub: do not allocate flush_round_state, do not
        // touch tree_state, do not advance any LSN cursor. Just
        // return the unsupported result through the value path
        // (022 D11 / Phase 3 D22).
        tree_flush_result res{
            .st                    = flush_stage_status::unsupported_unimplemented,
            .new_manifest          = nullptr,
            .retired               = {},
            .flushed_gens_by_front = {},
            .memtable_losers       = {},
            .flushed_max_lsn       = 0,
        };
        r->cb(std::move(res));
        delete r;
        progress = true;
    }
    return progress;
}
```

PUMP 特化（与 worker_scheduler.hh 同形）放在同一个头文件末尾：

```cpp
namespace pump::core {

    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::tree_flush_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template <typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template <typename ctx_t>
    struct compute_sender_type<ctx_t, apps::inconel::tree::_tree_flush::sender> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<apps::inconel::tree::tree_flush_result>{};
        }
    };

}  // namespace pump::core
```

约束：

1. handle 严格遵守 D22：不分配 round_state，不动 `tree_state`，不推进任何 LSN。
2. 输入校验直接 `panic_inconsistency`（与 worker_scheduler.hh M-4 一致），不退化成 silent fallback。
3. `kMaxFlushOpsPerAdvance = 8`：flush request 远比 worker request 稀疏（singleton handle，1 round / 数十 ms 数量级），8 足够；与 worker 64 区分。
4. `reclaim_task` 是 forward-declared placeholder，定义留给 frontier_switch step；本 step 只让 `pump::core::per_core::queue<reclaim_task*>` 能编译过，不真正使用。可以采用 `struct reclaim_task;`（incomplete type）+ 队列里走指针。

### 8. `core/registry.hh` — `tree_sched` 单例

在现有 registry 末尾追加（与 `value_alloc_sched` 同形）：

```cpp
inline tree::tree_sched* tree_sched_singleton_ptr = nullptr;

inline tree::tree_sched*
tree_sched_singleton() {
    assert(tree_sched_singleton_ptr && "tree::tree_sched not registered");
    return tree_sched_singleton_ptr;
}
```

`clear()` / `init_capacity()` 同步处理：

```cpp
inline void clear() {
    // ... existing ...
    tree_sched_singleton_ptr = nullptr;
}
```

约束：

1. 不引入 per-core helper：tree_sched 是 singleton，调用方都通过 `tree_sched_singleton()` 拿。
2. 字段名 `tree_sched_singleton_ptr` 故意不叫 `tree_sched`，避免与命名空间 `tree::` 重名歧义。
3. registry 不 own 任何对象——builder 负责 new/delete，registry 只持指针（与 Phase 2 完全一致）。

### 9. `runtime/builder.hh` — runtime tuple + 安装顺序

runtime tuple 从 4 元扩成 5 元：

```cpp
template <core::cache_concept TreeCache, core::cache_concept ValueCache>
using inconel_runtime_t = pump::env::runtime::global_runtime_t<
    mock_nvme::scheduler,
    tree::tree_lookup_sched<TreeCache>,
    tree::tree_worker_sched,
    value::value_alloc_sched<ValueCache>,
    tree::tree_sched
>;
```

build_runtime per-core 循环增加 tree_sched 安装（singleton on cores[0]）：

```cpp
tree::tree_sched* tsched = nullptr;
if (first) {
    tsched = new tree::tree_sched();
    core::registry::tree_sched_singleton_ptr = tsched;
}
rt->add_core_schedulers(core, nvme, tlookup, tworker, value_sched, tsched);
```

destroy 顺序按 D26：

```cpp
delete static_cast<tree::tree_sched*>(core::registry::tree_sched_singleton_ptr);
for (auto* s : core::registry::tree_worker_scheds.list) delete s;
for (auto* s : core::registry::tree_lookup_scheds.list) {
    delete static_cast<tree::tree_lookup_sched<TreeCache>*>(s);
}
if (core::registry::value_alloc_sched) { /* 不变 */ }
for (auto* s : core::registry::nvme_scheds.list) delete s;
core::registry::clear();
delete rt;
```

约束：

1. tree_sched 永远在 `cores[0]` 上构造，与 `value_alloc_sched` 同核——避免跨核 pinning，且 RSM §4.1 单线程不变量天然满足。
2. 其他 core 在 PUMP per-core tuple 里挂 nullptr 占位（与 value 一致），share_nothing.run 跳过空 slot。
3. destroy 顺序保证 tree_sched 在 worker / lookup 之前 free——Phase 3 tree_sched 不持任何 cache / frame，析构是 trivial 的；先删它能让后续 phase 加入 round_state 后保持顺序稳定。

### 10. `tree/sender.hh` / `tree/scheduler.hh` — facade 更新

`tree/sender.hh` 末尾追加 facade wrapper（与 `build_leaf_candidates` 同形）：

```cpp
inline auto
tree_flush(tree_sched* sched, tree_flush_request req) {
    return sched->submit_flush(std::move(req));
}
```

`tree/scheduler.hh` umbrella shim 增加一行 include：

```cpp
#include "./lookup_scheduler.hh"
#include "./worker_scheduler.hh"
#include "./owner_scheduler.hh"   // NEW
```

约束：

1. 外部模块仍然只 `#include "tree/sender.hh"`。
2. owner_scheduler.hh 内的 `_tree_flush` op/sender + `op_pusher` / `compute_sender_type` 特化跟着头文件走，与 022 D12 一致；`tree/scheduler.hh` 只做 include 聚合。
3. Phase 3 不对 `lookup_scheduler.hh` 做任何修改：lookup 现有 point-read 路径必须保持原样，避免破坏 022 已经收敛的 H-1/M-2/M-3 修订。

## 与 RSM §4.1/§4.2 的偏差（设计 follow-up）

Phase 3 在 carrier 上对 RSM 有两条偏差，必须在本 step 后由邻接独立 doc-sync step 同步回 `runtime_state_machine.md` 与 `cross_doc_contracts.md`：

| # | 位置 | RSM 原文 | Phase 3 实际形态 | 理由 |
|---|---|---|---|---|
| `Δ-1` | `tree_flush_request.sealed_gens` | `span<std::shared_ptr<memtable_gen>> sealed_gens;` | `absl::InlinedVector<std::shared_ptr<memtable_gen>, 8> sealed_gens;`（owning） | 022 review M-3 同款问题：req 跨 `per_core::queue` 后调用方栈不可依赖。owning 容器在 Phase 4 常见 round 大小下零分配 |
| `Δ-2` | `tree_flush_result` 状态字段 | `bool ok; ... flush_error error;` | `flush_stage_status st;`（统一） | 与 022 D11 / `flush_candidate_batch.st` / `flush_leaf_group_result.st` 一致；避免 result 字段同时出现 `bool ok` 和 `flush_error` 两层 status |

后置 doc-sync step 必须更新的位置：

1. `runtime_state_machine.md` §4.2 `tree_flush_request / tree_flush_result` 代码块
2. `cross_doc_contracts.md` §1 `tree_flush` handle 行
3. `flush_and_frontier_switch.md` §3.1 `tree_flush_request / tree_flush_result` 文本块

Phase 3 的 production code 落地后，doc-sync step **不能跳过**——三处文档与代码长期不一致会让后续 Phase 4-7 的 review 反复绕。

## 跨文档一致性

本 step 除了 §与 RSM 偏差 那两条外，必须与下列引用点字段一致；如果实现阶段发现不一致，必须先修 design_doc 再回改本 step（永远不能反向反推 spec）：

| 校对点 | 对照文档章节 |
|---|---|
| `tree_state.alloc / flush_max_lsn / superblock_safe_lsn / recovery_safe_lsn / reclaim_q` 字段 | RSM §4.1 |
| `tree_manifest.leaf_order` 存在与 immutable 语义 | OV §4.4, OV §5.2, RSM §4.5, FF §3.4/§3.8, cross_doc §2 |
| `leaf_order` 覆盖性 / 严格有序 / 不重叠 不变量 | RSM §4.5 leaf_order 语义冻结条目 1-5 |
| `leaf_order` 不持久化 | OV §4.4 / RSM §4.5 |
| `tree_flush_request.base_guard` 类型 | RSM §4.2, FF §3.1 |
| `tree_flush_result.flushed_gens_by_front` shape | RSM §4.2, FF §4.3, cross_doc §2 (Phase 1 D12) |
| `retired_objects.{old_slots, old_ranges, old_tree_values}` 字段名 | RSM §4.2, FF §5 |
| `retired_value_ref` 字段 | RSM §3.3, Phase 1 §021 |
| `checkpoint_guard.manifest` 字段 | OV §5.2, RSM §4.6 |
| `flush_lookup_req / flush_worker_req` 字段 | RSM §4.2/§4.7/§4.8, FF §3.4 |
| `flush_key_group` shape | FF §3.3 fold algorithm |
| `tree_sched` singleton + cores[0] 安装策略 | RSM §1, OV §1.7, code_modules §L2.tree |
| owner 边界（tree_sched 不持 cache） | cross_doc §3 行 "tree node read-only frame cache" |

## 明确不做的内容

- 不在本 step 实现 memtable fold / sorted workset 构造。
- 不在本 step 实现 `keys_to_leaf_groups()` 或任何 lookup fanout sender。
- 不在本 step 实现 worker 真实 old leaf read / merge / compact（worker 仍然返回 `unsupported_unimplemented`，与 022 完全一致）。
- 不在本 step 写任何 tree slot / 发任何 NVMe FLUSH。
- 不在本 step 引入 leaf_order builder / `rebuild_leaf_order_from_tree_delta()`。
- 不在本 step 加 `tree_state.active_rounds` 或 `next_round_id`（Phase 4 自然添加）。
- 不在本 step 实现 `~checkpoint_guard()` / `reclaim_task` / `tree_state.reclaim_q` 的 producer 与 consumer。
- 不在本 step 接线 `tree_allocator.allocate / recycle / data_area_heads`。
- 不在本 step 暴露 `update_superblock` 请求或推进 `superblock_safe_lsn`。
- 不在本 step 迁移 cache / frame pool / inflight 的 owner。
- 不在本 step 改 recovery 文档；recovery 文档与新 tree-domain 拓扑的同步留给独立 step（020/022 review 已经提醒过）。
- 不在本 step 写测试 harness——验证范围按 §最少验证范围 走，仅做结构性 / 编译期 / 微 smoke 验证。

任一项如果在实现时"顺手做一下"，必须停下来立新 step；不要在本 step 夹带。

## 最少验证范围

Phase 3 仍然只做结构性验证 + 微 smoke。不构造端到端 flush 用例（那是 Phase 7 闭环之后的事）。

1. **carrier 编译自包含**
   - `core/leaf_order.hh / core/checkpoint_guard.hh / core/retired_objects.hh / tree/flush_round_state.hh / tree/owner_scheduler.hh` 单独被 `#include` 时不依赖任何尚未实现的头。
   - `static_assert(sizeof(leaf_span) == 24);`
   - `static_assert(std::is_trivially_copyable_v<leaf_span>);`

2. **leaf_order 视图基线**
   - 默认构造 `leaf_order_index{}` 后 `empty() == true`、`size() == 0`。
   - 手工构造一个 2-leaf 小例：`fence_pool = "akz"`（共享 fence "k"），spans = `[ {0,1,1,1, base0}, {1,2,1,1, base1} ]`，验证 `fence_lower / fence_upper` 返回正确 string_view，且相邻 leaf 的 upper 与下一条 leaf 的 lower 字节地址相同（dedupe 生效）。

3. **tree_manifest leaf_order 集成**
   - `tree_manifest::empty(geom)` 后 `manifest.leaf_order.empty() == true`。
   - 现有 lookup 路径（022 落地的 `make_lookup_state` / `process()`）在空 leaf_order 上仍然只走 `!has_root()` 短路，**不退化**——Phase 2 已有的 `inconel_test_runtime` / `inconel_test_tree_value` 必须继续通过。

4. **tree_flush_request / tree_flush_result POD 与字段对齐**
   - `tree_flush_result` 默认构造后 `st == flush_stage_status::ok`、所有容器为空、`flushed_max_lsn == 0`。
   - 字段顺序与名字与 cross_doc §2 / RSM §4.2（含 §与 RSM 偏差 标注）一致——人工对照即可。

5. **tree_sched skeleton smoke**
   - 在测试 driver 里构造一个 build_runtime + 在 cores[0] 上拿到 `tree_sched_singleton()`。
   - 用 `tree::tree_flush(tsched, req)` 投递一个最小 request：`base_guard = make_shared<checkpoint_guard>({manifest = empty_manifest})`、`sealed_gens = {one shared_ptr<memtable_gen>}`、`recovery_safe_lsn = 0`。
   - 等待 advance() 消费一次。
   - 断言 cb 收到的 `tree_flush_result.st == unsupported_unimplemented`。
   - 断言 base_guard / sealed_gens 在 cb 回到测试 driver 之后 `use_count() == 1`（说明 tree_sched 的 req delete 已经释放掉它的引用）。
   - 同时构造一个 base_guard 为 nullptr 的 request，验证 `tree_sched::advance()` panic（白盒断言：用单独 process 跑 + 期望 abort，或用专用 panic interceptor）。

6. **runtime tuple 与 destroy 顺序**
   - build_runtime 后 `core::registry::tree_sched_singleton()` 返回非空。
   - destroy_runtime 后 `core::registry::tree_sched_singleton_ptr == nullptr`。
   - 按 D26 顺序删除：tree_sched → worker → lookup → value → nvme，destroy 完不泄漏 frame、不留挂尾 sched。

7. **borrowed view 静态结构**
   - 编译期断言 `flush_lookup_req` 与 `flush_worker_req` 内的 span 字段类型为 `std::span<const flush_key_group>` / `std::span<const flush_leaf_group>`。
   - **不做** runtime borrow 安全测试——Phase 3 的 worker handle 仍然不 deref `leaf_groups`，runtime 实际 borrow 是 Phase 5/6 的事。

8. **反向断言**
   - `flush_round_state` 默认构造后所有 owning 容器为空、`flushed_max_lsn == 0`。
   - 整个 Phase 3 source 中没有任何对 `tree_state.alloc.allocate / recycle / shared_heads` 的调用——可通过 grep / 静态检查确认。
   - `tree_state.reclaim_q` 在 Phase 3 advance() 中没有被 drain（grep `reclaim_q.try_dequeue` 应只出现在未来的 frontier_switch step）。

验证的唯一形式是 seam-level 单元测试 + 现有 `inconel_test_runtime` / `inconel_test_tree_value` 不退化。**不**构造 fold / lookup mapping / candidate build / writer 任何端到端场景。

## 下一步建议

按 `flush_development_plan.md` 节奏，Phase 3 之后最合理的顺序是：

1. **补 Phase 3 实现 step**（落本 step 文档下的代码）
   - `core/leaf_order.hh / core/tree_manifest.hh` 扩 leaf_order
   - `core/checkpoint_guard.hh / core/retired_objects.hh`
   - `tree/flush_types.hh` 扩 request/result + lookup/worker req borrowed payload
   - `tree/flush_round_state.hh`
   - `tree/owner_scheduler.hh / tree/scheduler.hh / tree/sender.hh`
   - `core/registry.hh` 加 tree_sched 单例
   - `runtime/builder.hh` 加 tree_sched 到 tuple + 安装 + destroy

2. **doc-sync follow-up step**（必须紧接，不可跳过）
   - RSM §4.1 / §4.2 同步偏差 Δ-1 / Δ-2
   - cross_doc §1 `tree_flush` handle 行同步
   - FF §3.1 `tree_flush_request / tree_flush_result` 文本块同步

3. **进入 Phase 4 — Memtable Fold / Workset**
   - 在 Phase 3 carrier 上接 fold 算法
   - 第一次需要 `tree_state.active_rounds` 与 `next_round_id`，那时新增字段
   - 构造 `flush_round_state.workset` 并把它作为 lookup_req 的 borrow source

另一个必须主动提醒的相邻项（与 022 §下一步 提醒同样紧急，且本 step 已是第三次提及）：

- recovery 文档仍然没有跟新 tree-domain 拓扑同步。如果 Phase 4 之前不补上 recovery 同步 step，Phase 7 / Phase 8 的 writer 与 frontier switch 一旦触及 `tree_manifest.leaf_order` 的 recovery 重建路径，会立刻发现正式设计还停在旧形态。
- cache ownership migration 仍然必须保留为独立 step，不要在 Phase 3 收尾 commit 里偷带。
