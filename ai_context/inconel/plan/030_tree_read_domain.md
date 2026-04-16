# Step 030 — `tree_read_domain` + `shard_partition_map` 立起来（INC-040 返工）

> 本文是**实现前**的设计文档，目的是在动手前把 scope、数据结构、所有权、启动顺序、回滚范围一次锁死。lookup 路由算法、scheduler 组合、cache ownership 都会被这步撬动；range 太广，靠改一处是收不住的。

---

## 1. 动机

### 1.1 INC-040 的实际修正方向

前一轮 INC-040 的修改（2026-04-16，已 merge 在本地工作树）把 `tree::lookup` 路由写成 `leaf_order.find_leaf_for_key(key) % K`（leaf ordinal modulo）。

用户随后指出这是**两个错误**：

1. **访问方式没统一**：lookup 和 flush worker 对"哪些 key 落到哪个 shard"必须用**同一个决策函数**。现状是
   - lookup：ordinal modulo（我刚写的）
   - worker（`memtable_fold.hh:240-245`）：range partition `leaf_idx * P / leaf_count`

   两套不一致，同 key 在读路径和 flush 路径可能落到不同 shard，共享 `tree_node` cache shard 的前提被破坏。

2. **路由必须按 key range，而非 hash / modulo**：modulo 会把相邻 leaf 散到不同 shard，丢失 range 局部性（batch lookup、range scan、flush 读连续 leaf 做 candidate build 都要用这块局部性）。

### 1.2 更深一层的约束

用户在后续讨论里还加了两条：

1. **路由决策不能每次都算 `find_leaf_for_key`**。per-key 二分走 `log2(leaf_count) ≈ 22` 次比较太贵；要求**一次二分**就拿到 shard_idx，不要再把 leaf 拉进来。

2. **代码里不能硬绑定"shard 边界必须和 leaf 对齐"**。当前 bootstrap 出来的 shard 边界**事实上**和 leaf 对齐（按 `leaf_idx * P / leaf_count` 切），但这只是初始构造策略。将来会根据访问热度 rebalance shard 边界，切点可能和 leaf 边界无关。路由代码必须只看 shard 边界，不看 leaf。

### 1.3 把欠债一次收完

步骤拆了会留下中间态（ordinal mod 撤了但 read_domain 还没立），风险大于一次做完。下一步要开端到端测试，再留中间态成本更高。

本步一次性收：

- **INC-040 返工**：路由从 ordinal modulo 撤成 shard_partition_map 一次二分
- **INC-003 收口**：`tree::lookup` 内部路由（已经在前一轮做了，本步只是把内部实现换一遍）
- **`tree_read_domain` struct 立起来**（step 022 D5 当时故意没建，现在有理由建了）
- **`tree_node` cache ownership 从 `tree_lookup_sched` 搬进 `tree_read_domain`**（RSM §4.7 本来就说 cache 属于 read_domain，是现在才物理落实）
- **memtable_fold 的 range 切分算法下线**，改成问 partition map

---

## 2. 数据结构

### 2.1 `shard_partition_map`（新文件 `core/shard_partition.hh`）

```cpp
namespace apps::inconel::core {

// Upper bound (exclusive) of one shard's covered key range.
// `fence_upper_len == 0` means +∞ — only the last shard is allowed
// to carry this sentinel, and the map builder must enforce it.
struct shard_partition {
    uint32_t fence_upper_off;
    uint16_t fence_upper_len;
    uint16_t _pad0;
    uint32_t shard_idx;        // target read_domain index
};
static_assert(sizeof(shard_partition) == 12);

struct shard_partition_map {
    std::string                  fence_pool;   // owns the fence bytes
    std::vector<shard_partition> shards;       // sorted by fence_upper
                                                // last shard's upper is empty (=+∞)

    bool     empty()      const noexcept { return shards.empty(); }
    uint32_t shard_count() const noexcept {
        // Highest shard_idx + 1. May differ from shards.size() if the
        // future rebalance ever overlaps multiple ranges onto the same
        // read_domain (not used in v1, but the type permits it).
        // v1 builder sets shard_idx == position-in-shards, so this is
        // `shards.size()` on the initial map.
        ...
    }

    uint32_t route(std::string_view key) const {
        // Single binary search on fence_upper.
        // Precondition (builder-enforced): last shard has empty upper
        // (= +∞), so every key falls into some shard — no coverage gap.
        auto lo = std::size_t{0}, hi = shards.size();
        while (lo < hi) {
            auto mid = lo + (hi - lo) / 2;
            auto upper = fence_upper_view(shards[mid]);
            // empty upper = +∞, always > key
            if (upper.empty() || key < upper) hi = mid;
            else                               lo = mid + 1;
        }
        if (lo >= shards.size()) {
            core::panic_inconsistency("shard_partition_map::route",
                "partition map has no +∞ sentinel (shards=%zu)",
                shards.size());
        }
        return shards[lo].shard_idx;
    }

private:
    std::string_view fence_upper_view(const shard_partition& s) const {
        // same offset/len bounds-check pattern as leaf_order_index::fence_slice;
        // panic on out-of-bounds
        ...
    }
};

}  // namespace apps::inconel::core
```

**要点**：

- **数据结构自洽**。不 include `leaf_order.hh`，不 import `tree_manifest`。`route()` 除了自己的 `fence_pool` + `shards` 不读任何外部状态。
- **一次二分**：成本 `log2(shard_count)`，对 K ≈ 几十时是几次比较。
- **shard_idx 独立于 position**：类型允许将来同一个 read_domain 拥有不连续的多段 key range（热度 rebalance 时可能出现），但 v1 builder 保持 `shard_idx == 位置`。
- **边界契约**：最后一个 shard 的 `fence_upper_len` 必须为 0（+∞ sentinel）。builder 必须保证，`route` 用 panic 兜底。

### 2.2 `shard_partition_builder`（放 `core/shard_partition.hh` 同文件，或独立 `core/shard_partition_builder.hh`）

**唯一**依赖 `leaf_order` 的地方。将来动态 rebuild 会换成另一套 builder，`shard_partition_map` 本身接口不变。

```cpp
// Initial bootstrap builder: derive shard partitions from the current
// manifest's leaf_order using range partitioning.
//
// Policy:
//   - let L = leaf_order.size(), P = min(K, L)
//   - shard i covers leaves [i * L / P, (i+1) * L / P)
//   - fence_upper of shard i = fence_upper of its last leaf
//     (shard P-1 takes the +∞ sentinel)
//
// The leaf alignment is a *construction* decision — `shard_partition_map::route`
// never observes leaves. A future heat-driven rebuild can split at any
// key without touching the routing code.
shard_partition_map
build_initial_shard_partition_map(const leaf_order_index& leaf_order,
                                  uint32_t read_domain_count);
```

**注意**：

- `K == 0` 是 runtime 配置错误 → panic
- `leaf_order.empty()` → **返回单 shard 占位 map**（`shards = [{fence_upper_len=0, shard_idx=0}]`，即"(-∞, +∞) → shard 0"）。这是问题 7 决策 P：bootstrap 装占位 map，保证"有 tree 但 rebuild 未做"的过渡期任何 key 仍然能 `route()`（全部路由到 shard 0，K-1 个 shard 闲置但功能正确）。空树时 lookup 靠 `manifest.has_root()` 短路，占位 map 不会被真正使用。
- `L < K`：`P = L`，前 L 个 shard 各覆盖 1 leaf，剩余 K-L 个 shard 不被任何 shard_partition 指向；map 的 `shard_idx` 值域只含 `[0, P)`，registry `tree_read_domains.list` 仍有 K 个实例但其中 K-L 个闲置。

### 2.3 `tree_read_domain`（新文件 `core/tree_read_domain.hh`）

决策（问题 5 G1 + advance）：`tree_read_domain` **own** 两个 scheduler；它同时暴露 `advance()`，由 PUMP runtime tuple 注册并轮询——不再按 scheduler 类型分别注册 `tree_lookup_sched` / `tree_worker_sched`。

```cpp
// Physical runtime object per read domain. Step 022 D5 deliberately
// skipped this; step 030 lifts it because (a) INC-040 needs a
// non-leaf-bound routing carrier owned above the scheduler layer,
// (b) `tree_node` readonly_frame_cache semantically belongs here
// (RSM §4.7 / RMC §6.1), and (c) PUMP registers one advance entry
// per read_domain instead of two (lookup/worker) — cleaner ownership
// chain and single scheduling unit per core.

template <cache_concept Cache>
struct tree_read_domain {
    uint32_t                                      read_domain_index;

    // Shared global view of key → shard routing. `shared_ptr` so a
    // future heat-driven rebuild can swap the map atomically without
    // touching the read_domain struct itself (step 030 本步不触发
    // rebuild — 见 §2.8 bootstrap、问题 2 决策).
    std::shared_ptr<const shard_partition_map>    partitions;

    // `tree_node` domain frame cache for this shard. Moved out of
    // `tree_lookup_sched` into the read_domain it logically belongs to.
    // Shared by `lookup` and `worker` below, zero virtual call because
    // schedulers are template-specialized on the same `Cache`
    // (问题 1 决策 A).
    Cache                                         node_cache;

    // Owned schedulers. Constructed inline in the read_domain ctor,
    // destroyed in reverse order when the read_domain drops.
    std::unique_ptr<tree_lookup_sched<Cache>>     lookup;
    std::unique_ptr<tree_worker_sched<Cache>>     worker;

    tree_read_domain(uint32_t rdi,
                     std::shared_ptr<const shard_partition_map> parts,
                     Cache                                      cache,
                     const core::tree_geometry*                 geom,
                     /* scheduler 构造其余参数，如 queue_depth */);

    // PUMP advance entry. Both schedulers have bounded-per-advance
    // loops, fairness achieved by round-robin at call site.
    bool advance() {
        bool progress = lookup->advance();
        progress |= worker->advance();
        return progress;
    }

    template <typename Runtime>
    bool advance(Runtime& rt) { return advance(); }
};

// Non-template base — registry stores `tree_read_domain_base*` so it
// stays non-templated (matches the existing registry pattern).
struct tree_read_domain_base {
    uint32_t read_domain_index;
    // 模板化入口由派生类各自实现；base 只暴露 index 和 runtime 驱动
    // 所需的 advance 虚调用（advance 不在 hot path 上，是 per-core
    // 外层轮询，虚调用开销可忽略）.
    virtual bool advance() = 0;
    virtual ~tree_read_domain_base() = default;
};
```

**所有权关系**：

```
runtime (PUMP tuple) owns
  └── unique_ptr<tree_read_domain<Cache>>    (K 个，每 core 一个)
        │
        ├── shared_ptr<const shard_partition_map> partitions
        │        ↑ 所有 read_domain 共享同一个指针目标
        │
        ├── Cache node_cache                   (per-domain shard)
        │
        ├── unique_ptr<tree_lookup_sched<Cache>> lookup
        │        └── tree_read_domain<Cache>*  ← back-ref (raw)
        │
        └── unique_ptr<tree_worker_sched<Cache>> worker
                 └── tree_read_domain<Cache>*  ← back-ref (raw)
```

- `tree_read_domain` 模板化 on `Cache`；registry 存 `tree_read_domain_base*`（非模板）。
- scheduler 从 PUMP tuple 中**消失**——tuple 只注册 `tree_read_domain<Cache>`，由它的 advance 代驱两个 scheduler。测试访问 scheduler 走 `rt->get_by_core<tree_read_domain<Cache>>(c)->lookup.get()`（问题 4 决策 F2、问题 5 决策 G1）。

### 2.4 `tree_lookup_sched` / `tree_worker_sched` 改造

决策（问题 1 A + 问题 6 I1）：两个 scheduler **仍然模板化 on `Cache`**，但 `Cache` 的语义从"我持有此 cache"退化为"我访问的 read_domain 持有此 cache"。

- **模板签名保留**：`template <cache_concept Cache> struct tree_lookup_sched` / `tree_worker_sched`。
- **持有** `tree_read_domain<Cache>*`（非 `base*`，因为需要编译期类型访问 `node_cache`，零虚调用）。
- `tree_lookup_sched` 里的 `page_cache_` 成员**删掉**；`process()` 里所有 `page_cache_.pin / put` 替换成 `read_domain_->node_cache.pin / put`（模板化直接调用，不走虚函数）。
- `inflight_reads_` 保留（per-lookup-shard 的 single-flight state）。
- `all_frames_` / `free_frames_` 保留在 lookup_sched 里（属于该 shard 的 frame pool）。
- 构造函数参数：`tree_lookup_sched(tree_read_domain<Cache>*, const tree_geometry*, queue_depth)`，cache 参数消失——cache 由 `read_domain->node_cache` 提供。
- `tree_worker_sched` 同构，额外的 `paired_lookup` 指针从"构造时独立传入"改成"通过 `read_domain_->lookup.get()` 读取"（back-ref，无双维护）。
- **`read_domain_index` 字段从 scheduler base 删除**（问题 6 I1）：scheduler 要编号时走 `read_domain_->read_domain_index`；base class `tree_lookup_sched_base` / `tree_worker_sched_base` 瘦一层。

说明：cache ownership 的 buffer 层重构（INC-016：DMA 内存、buffer pool 模型）**不在本步范围**，等 nvme/。本步只是把 cache struct 的**持有者**从 lookup_sched 换成 read_domain——物理挪位，语义不变。

### 2.5 `memtable_fold` 改造

当前（`apps/inconel/tree/memtable_fold.hh:220-271`）：

```cpp
const auto leaf_count = static_cast<uint32_t>(lo.size());
const uint32_t P = std::min(worker_count, leaf_count);
for (const auto& k : rs.workset) {
    auto leaf_idx = lo.find_leaf_for_key(k.key);
    const uint32_t p = static_cast<uint32_t>(
        (static_cast<uint64_t>(leaf_idx) * P) / leaf_count);
    buckets[p].push_back(k);
}
```

改成：

```cpp
auto partitions = core::registry::current_shard_partitions();
if (!partitions || partitions->empty()) {
    // bootstrap 还装了占位 map，partitions 不会真的 empty；
    // 走到这里说明 install 顺序错了，panic 而非 silent fallback。
    core::panic_inconsistency("memtable_fold",
        "shard_partition_map not installed");
}
const uint32_t K = partitions->shard_count();
std::vector<std::vector<flush_key_group>> buckets(K);
for (const auto& k : rs.workset) {
    uint32_t shard = partitions->route(k.key);    // 一次二分
    buckets[shard].push_back(k);
}
...
rs.partitions.push_back(flush_key_partition{
    .read_domain_index = shard,                   // == map.shard_idx
    ...
});
```

说明：
- `flush_fold_req` **不加** `partitions` snapshot 字段。本步不做 rebuild（问题 2 决策：rebuild trigger 等 coord_sched），map 从 bootstrap install 起就稳定，`current_shard_partitions()` 实时读和 snapshot 等价。
- 将来 rebuild 协议落地时，`flush_fold_req` 再加 `shared_ptr<const shard_partition_map>` 字段，保证 flush 开始时 tree_sched freeze 一份、fold + worker 共享同一 snapshot；届时 `current_shard_partitions()` 只用于读路径。
- `leaf_count`-key 越界的 panic 不再需要（map 的 `+∞` sentinel 保证覆盖）。bootstrap 装的占位 map 是 single shard，所有 key route 到 shard 0，fold 的结果就是"只产 1 个 partition"——功能正确，性能不均。

### 2.6 `tree::lookup` sender 改造

决策（问题 3 C1）：`tree::lookup(keys, manifest)` 签名**保留 manifest**——manifest 是"lookup 看到的 tree 版本 snapshot"，下降阶段每一步用（`root_slot` / `resolve(child_base)`），不能从 context 反查。

**前一轮 INC-040 引入的 `_lookup_impl::build_route_plan` 重写**：

```cpp
inline lookup_route_plan
build_route_plan(key_range, const core::tree_manifest* manifest) {
    if (!manifest->has_root()) {
        // 空树 → all-absent 短路，连 route 都不调
        return { .total_keys = count_keys(keys), .groups = {} };
    }
    auto partitions = core::registry::current_shard_partitions();
    if (!partitions || partitions->empty()) {
        // bootstrap 已装占位 map，走到这里是 install 顺序错误
        core::panic_inconsistency("tree::lookup",
            "has_root() but no shard_partition_map installed");
    }
    // 每 key 做一次 partitions->route(key)，按返回的 shard_idx 分 bucket；
    // 下降阶段仍然用 manifest（下面的 shard_lookup 内部）
}
```

`tree::lookup(keys, manifest)` 对外签名不变，内部 routing 从 "`leaf_order.find_leaf_for_key(key) % K`" 换成 "`current_shard_partitions()->route(key)`"。下降阶段 (`_lookup_impl::shard_lookup`) 完全保留，只是 scheduler 从列表拿的方式换：`registry::tree_read_domains.list[shard_idx]->lookup.get()`。

### 2.7 `core/registry.hh` 改造

**去掉**：

```cpp
// 前一轮 INC-040 加的路由 wrapper —— 撤掉
inline tree::tree_lookup_sched_base*
route_tree_lookup_for_key(const tree_manifest*, std::string_view);
```

**加**：

```cpp
// Global shard partition map. Atomic-load / atomic-store via
// shared_ptr for the future heat-driven rebuild path (see issue 2
// 决策: rebuild trigger 等 coord_sched 完成). 本步只由 builder 在
// startup 装一次占位 map.
inline std::shared_ptr<const shard_partition_map> current_shard_partitions_ptr;

inline std::shared_ptr<const shard_partition_map>
current_shard_partitions() {
    return current_shard_partitions_ptr;      // cheap copy of shared_ptr
}

inline void
install_shard_partitions(std::shared_ptr<const shard_partition_map> m) {
    current_shard_partitions_ptr = std::move(m);
    // 本步仅 builder 调；将来 coord_sched 发起 frontier switch 时由
    // tree_sched 调。注入接口保持稳定.
}

// Read domain list — the only registry维度 for tree read access.
// `tree_read_domains.list[idx]` 就是 shard_idx 到 read_domain 的直接
// 查找；scheduler 通过 `read_domains[idx]->lookup.get()` /
// `->worker.get()` 访问（问题 4 决策 F2：不再维护独立的
// `tree_lookup_scheds` / `tree_worker_scheds` 列表）.
struct tree_read_domain_list {
    std::vector<tree_read_domain_base*>  list;
    std::vector<tree_read_domain_base*>  by_core;
};
inline tree_read_domain_list tree_read_domains;

inline tree_read_domain_base*
tree_read_domain_at(uint32_t shard_idx) {
    return tree_read_domains.list[shard_idx % tree_read_domains.list.size()];
}

inline tree_read_domain_base*
local_tree_read_domain() {
    auto* rd = tree_read_domains.by_core[pump::core::this_core_id];
    assert(rd);
    return rd;
}
```

**去掉的接口**（问题 4 F2）：

- `tree_lookup_scheds` / `tree_worker_scheds` 两个 list 以及 `by_core` 映射整体移除
- `tree_lookup_at(idx)` / `tree_worker_at(idx)` 整体移除 —— caller 改走 `tree_read_domain_at(idx)->lookup.get()` / `->worker.get()`
- `local_tree_lookup()` / `local_tree_worker()` —— 改走 `local_tree_read_domain()->lookup.get()` / `->worker.get()`
- `route_tree_lookup_for_key(manifest, key)` 前一轮 INC-040 的 wrapper 整段删除

### 2.8 `builder.hh` 启动顺序

原来：

```
1. 建 nvme_scheds
2. 各 core 建 tree_lookup_sched<Cache>(rdi, geom, Cache(...))   ← cache 参数在这里
3. 各 core 建 tree_worker_sched<Cache>(rdi, ..., lookup*)
4. 建 value_alloc_sched
5. 装 registry
```

新的（决策 G1 + P + F2）：

```
1. 建 nvme_scheds
2. 建 value_alloc_sched
3. 建 bootstrap manifest（leaf_order 由 FormatProfile / recovery 决定；
   当前 step 27+ bootstrap 仍是空树，leaf_order 为空）
4. 构造 bootstrap shard_partition_map (占位 map):
      auto m = build_initial_shard_partition_map(manifest.leaf_order, K);
      //   leaf_order 空 → 单 shard 占位 { upper=+∞, idx=0 }（问题 7 P）
      //   leaf_order 非空 → 按 range 切（recovery 路径将来会走这条）
   registry::install_shard_partitions(make_shared<const shard_partition_map>(move(m)))
5. 对每个 core i（共 K 个，K = read_domain_count = cores.size()）：
   a. 构造 tree_read_domain<Cache>(rdi=i,
                                  partitions=current_shard_partitions(),
                                  Cache(...),
                                  geom, queue_depth)
      —— read_domain 构造函数内部 new 两个 scheduler、传 `this` 进去、
         把 unique_ptr 塞到自己的 lookup / worker 字段（G1 决策）
   b. 注册 registry::tree_read_domains.list.push_back(rd.get())
          registry::tree_read_domains.by_core[core_id] = rd.get()
      —— 不再注册 tree_lookup_scheds / tree_worker_scheds（F2 决策）
6. PUMP runtime tuple 按 `tree_read_domain<Cache>` 注册 per-core 实例；
   advance loop 调 read_domain->advance() 代驱两个 scheduler
```

bootstrap 空树时：`build_initial_shard_partition_map` 产出单 shard 占位 map → `route(key)` 永远返回 0；`tree::lookup` 仍靠 `manifest.has_root()==false` 短路，不会真的调 route。**第一次 flush 之后（有 leaf 但占位 map 未更新）**到 coord_sched 完成之间的窗口期：lookup / fold 调 route 都返回 shard 0，功能正确，K-1 个 read_domain 闲置（可接受的过渡态）。

---

## 3. 回滚清单：前一轮 INC-040 的改动哪些撤、哪些留

本地工作树里已经 commit 了 INC-040 第一轮的改动（尚未被 pop，但 user 说 "这次可以读 / 改测试代码"，所以仍在工作树里）。需要分类：

### 撤掉

| 位置 | 撤掉内容 | 原因 |
|---|---|---|
| `core/registry.hh` | `route_tree_lookup_for_key(manifest, key)` wrapper 整段删 | ordinal mod 算法作废 |
| `tree/sender.hh` | `_lookup_impl::build_route_plan` 里的 `find_leaf_for_key % K` 逻辑 | 改成 `shard_partition_map::route` |

### 保留（shape 正确，只改内部实现）

| 位置 | 保留 | 替换为 |
|---|---|---|
| `tree/sender.hh` | 公开 API `tree::lookup(keys, manifest)` + `_lookup_impl::shard_lookup` + fan-out + scatter 结构 | 内部路由调用点换成 `shard_partition_map::route` |
| tests | `build_leaf_order_8leaves()` fixture、调用点去掉 sched 指针 | 仍然有用——tests 仍需 leaf_order（manifest 的一部分），调用点无 sched 是最终形态 |

### 需要扩充

- tests 里除 leaf_order 外，还要构造 / 注入 `shard_partition_map`（用 `build_initial_shard_partition_map(leaf_order, K)` helper）
- tests 里原来直接 `tree_lookup_sched<Cache>` 构造的，改成先建 `tree_read_domain<Cache>` 再建 lookup + worker

---

## 4. 文档同步清单

**返工**（上一轮已改，这次再改一版）：

| 文档 | 位置 | 本轮动作 |
|---|---|---|
| `design_doc/runtime_state_machine.md` | §1 路由表 + §4.7 | 从 `leaf_order.find_leaf_for_key(key) % K` 改成 `partitions.route(key)`；§4.7 加 `tree_read_domain` struct 定义（spec 原来就声明了但没 physicalize） |
| `design_doc/design_overview.md` | §8.1、§14.2 point_get 伪码、读路径 diagram | routing 描述改 |
| `design_doc/read_api_and_pipeline.md` | §4 point GET §5 MultiGet §5.4 tree batch lookup | 改 |
| `design_doc/code_modules.md` | routing helper 表 | `route_tree_lookup_for_key` 换成 `current_shard_partitions().route(key)` |
| `design_doc/INDEX.md` | scheduler 总览 + 新增 `tree_read_domain` 条目？ | 加 |
| `audit/tree.md` | F7 resolved 注释 | 从"INC-040 第一轮"改成"step 030 最终形态" |
| `known_issues.md` | Resolved 区 INC-040 / INC-003 描述 | 重写 |

**新增**：

- `design_doc/runtime_state_machine.md` / `runtime_memory_and_cache.md`：加 `shard_partition_map` 的字节级定义 + owner 说明。RMC 里说明 `tree_node` cache 现在物理归 `tree_read_domain`。
- `cross_doc_contracts.md`：加 `tree_read_domain` / `shard_partition_map` 的字段表，加 routing handle 契约。

---

## 5. 测试 fixture 改动

### 现有 fixtures（step 030 要改）

| Test | 当前构造 | 新构造 |
|---|---|---|
| `test_tree_lookup.cc` `struct test_env` | `tree_lookup_sched<Cache> tree_sched(rdi, geom, Cache(...))` | `tree_read_domain<Cache> rd(rdi, partitions_ptr, Cache(...))`，`tree_lookup_sched tree_sched(&rd, geom, ...)`，`tree_worker_sched worker(&rd, ...)` |
| `test_tree_lookup_multicore.cc` `core_schedulers` | 同上，两个 core 各一套 | 同上，两个 core 各一个 read_domain，共享同一个 shared_ptr<shard_partition_map> |
| `test_runtime.cc` `build_runtime` | 走 builder | builder 改了，测试调用点不变，但 `runtime::build_options` 可能要加 partitions 注入口；倾向让 builder 自己从 `format::kBootstrapFormatProfile` 或直接 `leaf_order` 构造 |
| `test_tree_value.cc` | 单 leaf tree | 同样 read_domain 壳子，partitions 只有一个 shard 覆盖 +∞ |

### 已破（pre-existing, 非本步范围）

- `test_tree_value.cc` / `test_candidate_build.cc` / `test_flush_carriers.cc`：`value_alloc_sched` 7-arg 构造器 mismatch
- `test_leaf_mapping.cc`：include 不存在的 `tree/leaf_mapping.hh`

本步**不**修它们，但 `test_tree_value` 的 read_domain 构造会跟着改；它的 compile 仍然因 pre-existing value 错误而失败，维持现状。

---

## 6. 决策记录（动手前所有问题均已拍板）

所有决策点及其结论（2026-04-16 讨论确定）：

### 6.1 scheduler 模板化方式 → **决策 A**

`tree_lookup_sched<Cache>` / `tree_worker_sched<Cache>` 仍是模板类，`Cache` 参数的语义从"我持有此 cache"退化成"我访问的 read_domain 持有此 cache"。scheduler 持 `tree_read_domain<Cache>*` 直接调 `node_cache.pin/put`，**零虚调用**。对应 "RocksDB × 5" 级别的性能取向，hot path 不上虚函数。

### 6.2 `shard_partition_map` 重建时机 → **B1 作为目标设计，本步不实现**

**目标设计 B1**：每次 flush 结束由 tree_sched `build_initial_shard_partition_map(new_leaf_order, K)` + `install_shard_partitions()`；flush 期间 fold / worker 用 freeze 的 snapshot，读路径用 `current_shard_partitions()` 实时读。**本步范围**：rebuild trigger 及其 freeze 语义**不做**，留给 coord_sched 完成时与 frontier switch 一起设计。`install_shard_partitions()` API 先准备好，本步仅 builder bootstrap 一次调用。

### 6.3 `tree::lookup` 是否保留 manifest 参数 → **C1 保留**

manifest 是"这次 lookup 看到的 tree 版本 snapshot"，下降阶段必用（`root_slot` / `resolve(child_base)`），且不同 read_handle 可能 snapshot 不同，不能从 context 反查。签名显式更符合 `cross_doc_contracts §4` 的数据源断言。

### 6.4 `tree_lookup_scheds` / `tree_worker_scheds` 双列表 → **F2 整体去掉**

registry 只维护一个 `tree_read_domains.list` / `by_core`。应用层通过 `tree_read_domain_at(idx)->lookup.get()` / `->worker.get()` 访问 scheduler。`tree_lookup_at` / `local_tree_lookup` 等旧 API 整体移除，所有调用点切到 read_domain 维度。避免双维护漂移。

### 6.5 `tree_read_domain` 与 scheduler 所有权 → **G1 + advance()**

`tree_read_domain<Cache>` **own** 两个 scheduler（`unique_ptr`）。read_domain 自己提供 `advance()`，PUMP runtime tuple 注册 `tree_read_domain<Cache>` 代替两个 scheduler，advance loop 一轮驱两个。`scheduler` 的构造参数（`geom`, `queue_depth` 等）通过 read_domain 构造函数透传。不破坏引用层级：read_domain 单向 own scheduler；scheduler back-ref 用 raw 指针。

### 6.6 `read_domain_index` 字段 → **I1 保留，上移到 read_domain**

字段从 `tree_lookup_sched_base` / `tree_worker_sched_base` 移除，放到 `tree_read_domain` 上。语义从 "lookup-worker pairing 身份证" 降级成 "数值编号"（= `shard_partition.shard_idx`），仅用于诊断和 registry list 查找。scheduler 诊断时走 `read_domain_->read_domain_index`。

### 6.7 bootstrap `shard_partition_map` 形态 → **P 占位单 shard map**

builder startup 时调 `build_initial_shard_partition_map(leaf_order, K)`；`leaf_order` 空 → 返回单 shard 占位 map `{ shards=[{upper=+∞, idx=0}] }`。`route()` 永远能返回合法 `shard_idx`，空树时 lookup 仍靠 `manifest.has_root()` 短路避免真正调用。bootstrap 到 coord_sched 落地之间的开发过渡期：所有 key 路由到 shard 0，K-1 个 read_domain 闲置，功能正确、性能不均，可接受。测试用同一个 `build_initial_shard_partition_map` helper + 自己构造的 `leaf_order` 产出 map，不算"手选"。

---

## 7. 工作量估算 & 顺序

做完本步约 = 前一轮 INC-040 × 4～5 倍的代码量。G1 决策（read_domain own scheduler）让构造顺序必须先 read_domain 再 scheduler，需要调整顺序避免循环依赖。建议顺序：

1. **新文件 `core/shard_partition.hh`**：`shard_partition` + `shard_partition_map` + `build_initial_shard_partition_map`（纯数据结构，可 isolated 编译）
2. **新文件 `core/tree_read_domain.hh`**：`tree_read_domain_base`（非模板基类）+ `tree_read_domain<Cache>` 模板定义**骨架**。此时 scheduler 类型还没改，先用 forward decl：
   ```cpp
   namespace apps::inconel::tree { template <cache_concept> struct tree_lookup_sched; ... }
   ```
   构造函数、advance() 留空实现或仅占位。
3. **`tree_lookup_sched` / `tree_worker_sched` 重构**：模板签名保留，cache 成员删，持 `tree_read_domain<Cache>*`，`read_domain_index` 字段从 base 移除。此时 read_domain 骨架已有，scheduler 能 compile。
4. **回头填 `tree_read_domain` 的 ctor/advance 实现**：`new` 两个 scheduler、绑 back-ref、实现 advance。
5. **`memtable_fold` 改路由来源**：走 `current_shard_partitions()->route(key)`，去掉 `leaf_idx * P / leaf_count` 逻辑。
6. **`tree/sender.hh` `build_route_plan` 换实现**：routing 调用 `current_shard_partitions()->route(key)`，fan-out 目标从 `tree_lookup_at(idx)` 换成 `tree_read_domain_at(idx)->lookup.get()`。
7. **`core/registry.hh` 改**：删 `route_tree_lookup_for_key` wrapper；加 `current_shard_partitions` / `install_shard_partitions` / `tree_read_domains` / `tree_read_domain_at` / `local_tree_read_domain`；删 `tree_lookup_scheds` / `tree_worker_scheds` 及相关 at/by_core API。
8. **`runtime/builder.hh` 改启动顺序**：先 install_shard_partitions 装占位 map，再 per-core 构造 read_domain（own scheduler），注册到 registry；PUMP tuple 类型列表从 `tree_lookup_sched<Cache>, tree_worker_sched<Cache>` 换成 `tree_read_domain<Cache>`。
9. **tests 改 fixture**：
   - `test_tree_lookup.cc` / `test_tree_lookup_multicore.cc` / `test_tree_value.cc` / `test_runtime.cc` 构造路径：`build_initial_shard_partition_map(leaf_order, K)` → `install_shard_partitions` → per-core `tree_read_domain<Cache>`。
   - `test_runtime.cc` 里 `rt->get_by_core<tree_lookup_sched<Cache>>(c)` 换成 `rt->get_by_core<tree_read_domain<Cache>>(c)->lookup.get()`。
10. **cmake build 全过一遍**，跑 `inconel_test_tree_lookup` / `inconel_test_tree_lookup_multicore` / `inconel_test_runtime` pass。
11. **spec / audit / known_issues 同步更新**：RSM §1 / §4.7 / §4.8，design_overview §8.1 / §14.2，read_api_and_pipeline.md §4 / §5 / §5.4，code_modules.md（加 `tree_read_domain` / `shard_partition_map` 域对象、registry API 表），INDEX.md，cross_doc_contracts.md（加 struct 字段、routing handle 契约），audit/tree.md F7 resolved 描述，known_issues INC-040 / INC-003 描述。

每一步都 keep-compilable（步骤 2-4 之间可能需要临时 forward decl 和过渡类型，具体碰到再处理）。每完成一步构建一次，避免 C++ 模板错误雪崩。

---

## 8. 不在本步范围

明确**不做**的事，避免 scope creep：

- INC-016（cache buffer ownership → DMA pool）：等 nvme/
- INC-002（mock_nvme 硬耦合）：等 nvme/ + device abstraction
- 热度驱动的 `shard_partition_map` 动态 rebalance：本步只做静态 bootstrap/flush 重建
- value cache ownership 搬家：value 模块不动
- pre-existing 测试崩溃（`test_tree_value` / `test_candidate_build` / `test_flush_carriers` / `test_leaf_mapping`）：不修

---

## 9. 验收标准

- `core/shard_partition.hh` 的 `route()` 单元层级可推理：
  - 空 map（shards.size() == 0）→ 不允许被 `route` 调用（caller 负责短路）；本步 bootstrap **不会装** empty map，`build_initial_shard_partition_map` 对空 leaf_order 返回单 shard 占位
  - 非空 map：任何 key 都映射到一个合法 `shard_idx`
  - 最后一个 shard 必须是 +∞ sentinel（`fence_upper_len == 0`）
- `tree_read_domain` 实例数 K == `registry::tree_read_domains.list.size()`；registry 不再维护独立的 `tree_lookup_scheds` / `tree_worker_scheds` list
- `tree::lookup(keys, manifest)` 公开 API shape 和前一轮一致（无 sched 指针、返回 `vector<lookup_result>` 按输入顺序）
- `memtable_fold` 产生的 `flush_key_partition.read_domain_index` 值域 ⊆ 当前 `shard_partition_map` 的 `shard_idx` 值域；fan-out 到 `tree_read_domain_at(idx)->worker.get()` 后，worker **读取的 page 和 lookup 看到的 cache 在同一个 read_domain 的 node_cache shard 上**——这是本步 "cache 命中率保证" 的验收点
- `inconel_test_tree_lookup` / `inconel_test_tree_lookup_multicore` / `inconel_test_runtime` 全部 pass
- 所有 spec / audit / known_issues 同步更新

---

## 10. Review Checklist

- [x] `shard_partition_map` 接口形状（§2.1）
- [x] `tree_read_domain` 字段 / 所有权：G1 — read_domain own scheduler + advance() 代驱（§2.3 / §6.5）
- [x] cache 搬家策略：整个 `Cache` 成员从 `tree_lookup_sched` 迁到 `tree_read_domain`（§2.3 / §2.4）
- [x] scheduler 模板化 vs 虚调用：A — scheduler 模板化 on `Cache`，持 `tree_read_domain<Cache>*`，零虚调用（§6.1 / §2.4）
- [x] `shard_partition_map` 重建时机：B1 为目标，本步不做（§6.2）；bootstrap 装占位 map（§6.7 / §2.2 / §2.8）
- [x] `tree::lookup` 保留 manifest 参数：C1（§6.3 / §2.6）
- [x] registry 双列表冗余：F2 — 去掉 `tree_lookup_scheds` / `tree_worker_scheds`（§6.4 / §2.7）
- [x] `read_domain_index` 字段：I1 — 保留，上移到 `tree_read_domain`（§6.6 / §2.4）
- [x] 回滚前一轮 INC-040 的范围（§3）
- [x] 不在本步范围的清单（§8）

**所有决策已拍板。动手按 §7 的 11 步顺序执行。**
