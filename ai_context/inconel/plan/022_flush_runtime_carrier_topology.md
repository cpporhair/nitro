# 022 — Flush Runtime Carrier 与 Topology

> 实现第二十二步。落地 `flush_development_plan.md` Phase 2，但按 `022_flush_runtime_carrier_topology_review.md` 的 blocker 修订后，**收窄本 step 的实现边界**：
>
> 1. 先冻结 tree geometry 的 single source 和 runtime carrier；
> 2. 先冻结 read-domain pairing seam；
> 3. 先立 `tree_worker_sched` skeleton 和最小 flush shell types；
> 4. `tree_sched`、cache/frame ownership 迁移、lookup flush sender 一律后移。
>
> 这份文档的目标不是“把最终 tree-local flush runtime 一次写完”，而是把**现在就能稳定落地**、且不会在 Phase 3-6 第一次接算法时立刻重构的那一层先拍板。

## 本 step 覆盖的目标

| 目标 | 说明 |
|---|---|
| G1 | 冻结 Phase 2 可落地的 read-domain pairing：同 core 的 `lookup + worker` 配对，加上稳定的 `read_domain_index` |
| G2 | 冻结 tree geometry 的 bootstrap single source 和 runtime carrier |
| G3 | 冻结 `tree_worker_sched` skeleton 的边界、sender surface 和 fail-fast 语义 |
| G4 | 冻结 `flush_round_id / flush_lookup_req / flush_worker_req` 等 shared shell types，但不提前绑定 round-owned array ownership |
| G5 | 冻结 `core::registry` / `runtime::builder` 在 Phase 2 应如何接 lookup shards、worker shards 与 read-domain pairing |

## 为什么要收窄 Phase 2

`022_flush_runtime_carrier_topology_review.md` 提出的 4 个 blocker 里，真正的问题不是“022 想得太多”，而是其中有几项在当前阶段**还没有可稳定冻结的前提**：

1. `tree_read_domain<Cache>` 会和现有 non-templated `tree_lookup_sched_base` / registry 基础设施硬冲突；
2. `tree_sched` 如果现在立 skeleton，很容易绕开 RSM §4.1 的正式 owner 字段，变成自造 spec；
3. cache / frame pool / inflight 三者如果只迁两项，Phase 6 的 worker read 一落地就要推翻；
4. `keys_to_leaf_groups()` 的 req payload 和 round-owned array ownership 还没冻结，现在立 sender surface 是伪冻结。

所以 Phase 2 现在的正确边界应该是：

```text
tree geometry single source
-> read-domain pairing seam
-> worker skeleton
-> registry / builder wiring
-> flush shell types
```

而不是把 `tree_sched`、cache owner 迁移、lookup flush sender 一起塞进来。

## 本 step 不覆盖

| 不做项 | 归属阶段 |
|---|---|
| `tree_sched` runtime skeleton / owner state | Phase 3（与正式 round carrier 一起落） |
| `tree_manifest.leaf_order` | Phase 3 |
| `tree_flush_request / tree_flush_result` 完整 carrier | Phase 3 |
| round-owned immutable arrays / borrowed spans 的 owning side | Phase 3 |
| memtable fold / winner-loser 分类 | Phase 4 |
| `keys_to_leaf_groups()` sender surface 与 mapping 算法 | Phase 5 |
| cache / frame pool / inflight 从 lookup 向 final `tree_read_domain` owner 迁移 | Phase 5 前置或 Phase 6 前置专门 step |
| `build_leaf_candidates()` 的 old-leaf read / decode / merge / compact 算法 | Phase 6 |
| tree delta / bounded writes / device flush | Phase 7 |
| split / consolidation / root change | Phase 8 |
| recovery 文档与 tree 域新拓扑的同步 | 邻接独立 step |

## 目标文件结构

```text
apps/inconel/
├── format/
│   └── format_profile.hh            — 更新：bootstrap profile 补 tree geometry 字段
├── core/
│   ├── tree_geometry.hh             — 新增：tree geometry runtime carrier
│   ├── tree_manifest.hh             — 更新：reader-side manifest 改引用 `tree_geometry`
│   └── registry.hh                  — 更新：注册 lookup / worker shards
├── tree/
│   ├── lookup_scheduler.hh          — 现有 lookup scheduler 拆出为独立角色
│   ├── worker_scheduler.hh          — 新增：tree_worker_sched skeleton
│   ├── flush_types.hh               — 新增：flush shared shell types
│   ├── sender.hh                    — 更新：继续作为模块对外 sender 入口
│   └── scheduler.hh                 — 迁移期 umbrella shim，只聚合子头文件
└── runtime/
    ├── builder.hh                   — 更新：lookup / worker / read-domain wiring
    └── start.hh                     — 更新：runtime tuple 扩容后的入口封装
```

显式不在本 step 出现的文件：

```text
apps/inconel/tree/owner_scheduler.hh  // Phase 2 不立 tree_sched
```

## 设计目标

1. **先冻结不会反复返工的 seam**。Phase 2 结束后，geometry source、lookup/worker pairing、worker skeleton 和 registry/builder wiring 应该已经稳定。
2. **不在缺前提时强行冻结 sender shape**。`keys_to_leaf_groups()` 和 `tree_flush()` 都属于当前前提不够的项，必须后移。
3. **不在 Phase 2 偷做 cache ownership migration**。最终正式设计仍然是 `tree_read_domain` 拥有 node cache，但当前实现步先不迁，避免 template/base 与 inflight 冲突。
4. **所有未实现 surface 一律 value-path fail-fast**。这里统一走 result/status，不走 exception pattern。
5. **对外 sender 入口仍然只有 `tree/sender.hh`**。内部头文件拆分不意味着外部模块可以直接 include 子 scheduler 头。

## 设计决策

| # | 决策点 | 结果 | 说明 |
|---|---|---|---|
| `D1` | 本 step 的性质 | **设计 step，且 scope 收窄** | 先把 blocker 清掉，不硬推“完整 Phase 2 runtime” |
| `D2` | bootstrap source of truth | **扩 `format::kBootstrapFormatProfile`，补 `tree_page_size / shadow_slots_per_range`** | 复用 step 017 已经确立的 bring-up single source |
| `D3` | tree geometry 的 runtime carrier | **新增 `core::tree_geometry`，由 builder 从 format_profile 构造** | 统一 reader / worker / writer 未来共用的 geometry 类型 |
| `D4` | `tree_manifest` 怎么持有 geometry | **持 `const tree_geometry*`，不按值复制 full geometry** | 避免 reader-side snapshot 按值携带 writer-only 字段 |
| `D5` | Phase 2 的 read-domain 表达 | **不落具名 `tree_read_domain` struct；pairing 用 `read_domain_index + same-core lookup/worker install` 表达** | 避免一个立即会在 Phase 5/6 改形状的 named runtime object |
| `D6` | lookup scheduler 在 Phase 2 的职责 | **继续独享 node cache / frame pool / inflight，并保留 point lookup** | 不在 Phase 2 破坏已有读路径 |
| `D7` | worker scheduler 在 Phase 2 的职责 | **新增 non-templated skeleton，暴露 `build_leaf_candidates(flush_worker_req)` sender，统一返回 `unsupported_unimplemented`** | 让 worker 不是“死 scheduler”，但不偷带算法 |
| `D8` | `keys_to_leaf_groups()` sender | **不在 Phase 2 落代码；只保留 type shell** | Phase 5 再冻结真正 sender surface |
| `D9` | `tree_sched` | **整体延后到 Phase 3，与正式 round carrier / owner state 一起落** | 避免自造 `tree_state` shape |
| `D10` | `flush_lookup_req` / `flush_worker_req` 的字段精度 | **只冻结顶层 identity 和当前稳定 payload；不引入 `flush_key_group_ref` 这类 ownership-biased indirection** | 防止 Phase 3 因 round-owned array 拍板而推翻 Phase 2 shell |
| `D11` | sender fail-fast 方式 | **统一 value-path：`result.st = unsupported_unimplemented`** | 避免 future flush sender 同时出现 value-path 和 exception-path 两套 pattern |
| `D12` | 头文件拆分后的 PUMP 特化归属 | **op / sender / `op_pusher` / `compute_sender_type` 特化跟着定义它们的子头文件走；`tree/scheduler.hh` 只做 include 聚合** | 防止拆头文件后编译期特化不可见 |

## 详细设计

### 1. `format/format_profile.hh` 与 `core/tree_geometry.hh`

Phase 2 先把 bootstrap tree geometry 的来源钉死：

```cpp
struct format_profile {
    uint32_t lba_size;
    ...
    uint32_t tree_page_size;
    uint32_t shadow_slots_per_range;
    ...
};
```

`profile_is_self_consistent()` 和 `runtime::validate_build_inputs()` 都同步扩这两个字段的校验。

builder 再从 profile 构造 runtime-visible carrier：

```cpp
struct tree_geometry {
    uint32_t lba_size;
    uint32_t tree_page_size;
    uint32_t shadow_slots_per_range;

    uint32_t page_lbas() const noexcept;
    uint64_t range_lbas() const noexcept;
    range_ref range_ref_from_base(paddr base) const noexcept;
    paddr slot_paddr(paddr range_base, uint32_t slot_index) const noexcept;
};
```

约束：

1. tree runtime 代码以后只读 `tree_geometry`，不再散落裸三元组。
2. bootstrap 值只从 `kBootstrapFormatProfile` 来，不允许再在 `builder.hh` 里手写另一个 `constexpr tree_geometry k{...}`。
3. `range_ref_from_base()` 用显式命名，避免 `whole_range()` 这种模糊名词。

### 2. `core/tree_manifest.hh`

Phase 2 不碰 `leaf_order`，但把 manifest 的 geometry 访问口改成引用 runtime-owned immutable geometry：

```cpp
struct tree_manifest {
    paddr root_slot;
    flat_hash_map<paddr, uint32_t> slot_map;
    const tree_geometry* geom;

    bool has_root() const;
    paddr resolve(paddr range_base) const;
    uint32_t page_lbas() const noexcept { return geom->page_lbas(); }
};
```

选择 pointer 而不是按值复制的原因：

1. reader path 只需要 `lba_size / tree_page_size` 来算 slot 偏移，不需要把 writer-only 参数作为 snapshot 字段复制多份；
2. geometry 在 bring-up runtime 生命周期内天然长于 manifest snapshot；
3. 未来即使 tree owner / recovery 也读同一份 immutable geometry，对齐 single source。

### 3. Phase 2 的 read-domain pairing seam

Phase 2 **不落具名 `tree_read_domain` runtime object**。当前阶段真正需要冻结的只有两件事：

1. 每个 lookup shard 有一个稳定的 `read_domain_index`；
2. 每个 worker shard 也有同一个 `read_domain_index`，并和 lookup 通过 **same-core install** 配对。

建议形状：

```cpp
struct tree_lookup_sched_base {
    uint32_t read_domain_index;
    ...
};

struct tree_worker_sched {
    uint32_t read_domain_index;
    ...
};
```

pairing 关系不通过具名 domain 对象表达，而通过 builder 安装约定表达：

```text
core X 上安装的 tree_lookup_sched_base
<-> core X 上安装的 tree_worker_sched
拥有相同的 read_domain_index
```

Phase 2 明确不做：

1. 不把 `node_cache` 挪进 domain；
2. 不把 `all_frames / free_frames` 挪进 domain；
3. 不把 `inflight_reads_` 升成 domain 级协议；
4. 不引入一个只含 `read_domain_index + lookup + worker` 三个字段、且 Phase 5/6 前必然要改形状的 named struct。

这条收窄的含义要说清楚：

1. **最终正式设计**仍然是 lookup/worker 共享 `tree_read_domain` cache shard；
2. **但当前实现阶段**先只冻结 pairing seam，cache ownership migration 单独留给 worker 真正开始读 old leaf 之前；
3. pairing 关系从 `registry::tree_lookup_scheds.by_core[core]` 和 `registry::tree_worker_scheds.by_core[core]` 推导，不再额外造一层 Phase 2 专用 runtime object。

### 4. `tree/lookup_scheduler.hh`

当前 `apps/inconel/tree/scheduler.hh` 的 lookup 内容拆到 `lookup_scheduler.hh`，但 Phase 2 对 lookup 的改动只限于：

1. 头文件归位；
2. builder / registry 仍然能拿到 non-templated `tree_lookup_sched_base*`；
3. point lookup sender 语义保持不变；
4. node cache / frame pool / inflight 继续由 lookup 自己持有。

Phase 2 不新增：

```cpp
keys_to_leaf_groups(flush_lookup_req)
```

原因不是这个 handle 永远不需要，而是：

1. `flush_lookup_req` 的 payload ownership 还没冻结；
2. 现在立 sender 只会形成一次 req/op/queue 的伪冻结；
3. Phase 5 再落这条 sender，更符合 `flush_development_plan.md` 的 seam 切法。

### 5. `tree/worker_scheduler.hh`

Phase 2 新增 non-templated worker skeleton。因为 cache 还没迁过来，worker 现在不需要模板参数。

建议形状：

```cpp
struct tree_worker_sched {
    pump::core::per_core::queue<build_leaf_candidates_req*> build_q;
    uint32_t read_domain_index;

    void schedule_build(build_leaf_candidates_req* r);
    bool advance();
};
```

公开 sender surface 通过 `tree/sender.hh` 暴露：

```cpp
build_leaf_candidates(tree_worker_sched*, flush_worker_req)
  -> flush_candidate_batch
```

Phase 2 的 handle 行为固定为：

```text
收到 req
-> 不做任何 old-leaf read / merge / compact
-> 正常 cb 一个 `flush_candidate_batch { st = unsupported_unimplemented }`
```

这条 sender 现在可以存在，原因是它不要求提前冻结 round-owned arrays 的解引用方式；它的 payload 只依赖 `flush_leaf_group[]` shell，本 step 可以稳定定义。

### 6. `tree/flush_types.hh`

Phase 2 只定义最小 shell types：

```cpp
struct flush_round_id {
    uint64_t v = 0;
};

enum class flush_stage_status : uint8_t {
    ok,
    unsupported_unimplemented,
    unsupported_shape_change,
};

struct flush_lookup_req {
    flush_round_id             round_id;
    uint32_t                   read_domain_index;
    const core::tree_manifest* base_manifest;
};

struct flush_leaf_group {
    paddr leaf_range_base;
    paddr old_slot_paddr;
};

struct flush_leaf_group_result {
    flush_round_id                         round_id;
    uint32_t                               read_domain_index;
    flush_stage_status                     st;
    absl::InlinedVector<flush_leaf_group, 8> leaf_groups;
};

struct flush_worker_req {
    flush_round_id                         round_id;
    uint32_t                               read_domain_index;
    const core::tree_manifest*             base_manifest;
    uint64_t                               recovery_safe_lsn;
    std::span<const flush_leaf_group>      leaf_groups;
};

struct flush_leaf_candidate {
    paddr              leaf_range_base;
    paddr              old_slot_paddr;
    flush_stage_status st;
};

struct flush_candidate_batch {
    flush_round_id                              round_id;
    uint32_t                                    read_domain_index;
    flush_stage_status                          st;
    absl::InlinedVector<flush_leaf_candidate, 8> leaves;
};
```

这里刻意**不**定义：

1. `flush_key_group_ref`
2. `round_index` 之类的 indirection 字段
3. 任何“索引 round-owned array”的 payload 引用关系

这些都要等 Phase 3 把 round-owned immutable arrays / borrowed spans 的 owning side 拍板之后再定。

### 7. `core::registry`

Phase 2 的 registry 只扩到 lookup/worker pairing：

```cpp
struct tree_worker_list {
    std::vector<tree::tree_worker_sched*> list;
    std::vector<tree::tree_worker_sched*> by_core;
};
inline tree_worker_list tree_worker_scheds;
```

并新增 helper：

```cpp
tree::tree_worker_sched* local_tree_worker();
tree::tree_worker_sched* tree_worker_at(uint32_t idx);
uint32_t tree_worker_count();
```

Phase 2 明确不加：

1. `tree_sched` registry slot
2. `home_tree_lookup_for_front()`
3. `tree_read_domain` 对外暴露 helper

原因：

1. `tree_sched` 已后移到 Phase 3；
2. front 路由 helper 仍然依赖 front topology step；
3. pairing 关系通过 lookup/worker 的 `by_core` 安装位置和相同的 `read_domain_index` 表达，不需要额外的 read-domain runtime object。

### 8. `runtime/builder.hh` / `runtime/start.hh`

builder 在 Phase 2 只扩到：

```text
for core in opts.cores:
  build nvme(core)
  build tree_lookup(core, read_domain_index = next_idx)
  build tree_worker(core, read_domain_index = same next_idx)

on first core only:
  build value_sched
```

runtime tuple 从当前 3 类 scheduler 扩成 4 类：

```text
mock_nvme
tree_lookup
tree_worker
value_alloc_sched
```

对应的 runtime type alias 应明确写成：

```cpp
template <core::cache_concept TreeCache, core::cache_concept ValueCache>
using inconel_runtime_t = pump::env::runtime::global_runtime_t<
    mock_nvme::scheduler,
    tree::tree_lookup_sched<TreeCache>,
    tree::tree_worker_sched,
    value::value_alloc_sched<ValueCache>
>;
```

没有 `tree_sched`。

原因：

1. 避免把“没有正式 owner state 的 tree owner”提前塞进 runtime；
2. lookup/worker 的 pairing seam 现在就能稳定落地；
3. worker 有可观察的 unsupported sender，不是 dead scheduler。

destroy 路径也要写清：

1. 先删 worker；
2. 再删 lookup；
3. 再删 value / nvme；
4. 最后清 registry。

因为 Phase 2 没迁 cache ownership，所以 frame 析构仍然留在 lookup scheduler，不发生析构职责搬迁。

### 9. 头文件拆分后的 PUMP 特化归属

Phase 2 明确规定：

1. `_tree_lookup::*` 的 op/sender 及其 `op_pusher` / `compute_sender_type` 特化跟着 `lookup_scheduler.hh`；
2. worker 新增的 req/op/sender 及其特化跟着 `worker_scheduler.hh`；
3. `tree/scheduler.hh` 只 include 这些子头文件，作为过渡 umbrella；
4. `tree/sender.hh` 实现上是 `#include "./lookup_scheduler.hh"` + `#include "./worker_scheduler.hh"` 的 facade 头文件，把两个子头文件里定义的 public sender 和对应的 PUMP 特化整合成模块对外的单一入口；
5. **外部模块**仍然只 include `tree/sender.hh`；
6. builder/runtime 可以 include 内部子头文件，不违反 `code_modules.md`。

### 10. `unsupported_unimplemented` 的统一语义

Phase 2 开始，flush 侧暂未实现的 sender surface 统一走 **value-path**：

```text
cb(result { st = unsupported_unimplemented })
```

不走 exception path。

原因：

1. `flush_stage_status` 已经是 shared shell type；
2. 后续 Phase 5-7 的 fan-in 更适合按 result/status 聚合；
3. 避免 worker 和 future lookup sender 一边抛异常、一边返回 status 的双模式并存。

## 与正式设计文档的关系

本 step 只是在实现层收窄顺序，不推翻 Phase 0 已冻结的最终职责边界：

1. 最终正式语义仍然是 `tree_read_domain` 拥有 shared node cache；
2. 最终仍然需要 `tree_sched / tree_lookup_sched / tree_worker_sched` 三域；
3. Phase 2 只是把其中“当前能稳定落代码的部分”先落下。

这意味着有一个必须主动提醒的相邻项：

1. **在 Phase 6 worker 真正开始读 old leaf 之前，必须补一个专门的 cache ownership migration step**：
   - lookup-local `node_cache / frame pool / inflight`
   - 迁到 final `tree_read_domain` ownership
2. **在 Phase 3 前，必须让 `tree_sched` 和正式 owner state 一起落**，不能再用临时字段绕过 RSM §4.1。

## 明确不做的内容

- 不在 Phase 2 立 `tree_sched`
- 不在 Phase 2 把 `tree_manifest` 扩到 `leaf_order`
- 不在 Phase 2 落 `keys_to_leaf_groups()` sender
- 不在 Phase 2 决定 round-owned arrays 的 owning side
- 不在 Phase 2 迁 cache / frame pool / inflight 的 owner
- 不在 Phase 2 实现 worker 的 old-leaf read / merge / compact
- 不在 Phase 2 改 recovery 文档

## 最少验证范围

本 step 的验收仍然是结构性验证，但要比“纯静态结构”更具体：

1. geometry source 已经唯一：bootstrap tree 参数只从 `format_profile` 来。
2. lookup/worker pairing 已经稳定：registry/builder 能按 core 找到两者，且 `read_domain_index` 一致。
3. worker sender 已经有可观察行为：调用后返回 `flush_candidate_batch { st = unsupported_unimplemented }`。
4. 没有任何一个 Phase 2 类型提前把 round-owned array ownership 写死。
5. lookup 现有 point read 没因为头文件拆分或 wiring 改动而退化。

## 下一步建议

这一步之后，最合理的顺序是：

1. **补 Phase 2 实现 step**
   - `format_profile` tree 字段
   - `tree_geometry`
   - `lookup_scheduler.hh` / `worker_scheduler.hh`
   - registry / builder / runtime tuple 扩容
   - worker unsupported sender
2. **进入 Phase 3 — Manifest Carrier 与 Round State**
   - `tree_sched`
   - `tree_manifest.leaf_order`
   - `tree_flush_request / tree_flush_result`
   - round-owned immutable arrays / views
3. **在 Phase 5 或 Phase 6 前插一个相邻 step**
   - 把 final `tree_read_domain` cache ownership migration 收掉

另一个必须提前提醒的相邻项：

1. `020_tree_local_flush_design_sync_review.md` 里标过 recovery 文档还没跟 tree-domain 新拓扑同步；
2. 如果后面打算让 recovery 复用同一套 lookup/worker/read-domain 结构，最好在 Phase 3 或 Phase 4 前单独补一个 recovery 同步 step。
