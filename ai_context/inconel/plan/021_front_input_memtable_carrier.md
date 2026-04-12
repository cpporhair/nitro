# 021 — Front Input / Memtable Common Carrier

> 实现第二十一步。落地 `flush_development_plan.md` Phase 1：把 flush 要消费的前端输入对象的**类型契约**立住，消掉"计划默认 front/memtable/gen snapshot 已存在"的隐含前提。
>
> 本 step 只定义共享类型（契约），**不实现任何 runtime**。Phase 1 的原话是"只做 flush 所需的**最小输入契约**"与"`collect_eligible_gens() / release_gens()` 相关的**最小输入输出 shape**"——shape 与 contract，不是工作中的 scheduler。front scheduler 骨架、WAL stream、seal、builder、registry 注册位都留给后续 phase。
>
> Phase 0（设计文档同步）由 step 020 完成。

## 本 step 覆盖的目标

| 目标 | 说明 |
|---|---|
| G1 | 落地 `memtable_entry / memtable_gen / value_handle / value_view / gen_arena / retired_value_ref / retire_list` 的共享类型定义 |
| G2 | 落地 `front_read_set` 作为 sealed/pinned gen 的 readonly view（OV §5.3 的字段形状） |
| G3 | 冻结 `collect_eligible_gens / release_gens / flushed_gens_by_front` 的输入输出 shape（不实现 handle；只固定它们将使用的现成容器形态） |
| G4 | 让后续 flush phase 的 step 文档可以直接 `#include "core/memtable.hh"` 引用上述类型，不再绕开 |

## 本 step 不覆盖

| 不做项 | 归属阶段 |
|---|---|
| `front::scheduler / front_state / advance() / op_pusher` | 后续 front skeleton / 写路径 / seal 系列 step |
| `front/memtable_builder.hh`、直构造入口 | recovery / seal / 写路径 / end-to-end harness 任一更合适的 step |
| `front/sender.hh`、`collect_eligible_gens / release_gens` 的 production sender 实现 | 与 front scheduler 骨架同一 step |
| `core/registry.hh` 的 `front_list / route_to_front` 注册位 | runtime builder 接纳 front 实例时的 step |
| `canonical_entry / fragment / batch_ctx / wal_stream_state` | 写路径 step |
| `tree_manifest.leaf_order` / tree domain topology | Phase 2 Runtime Carrier 与 Topology |
| 任何 tree-local flush 算法 | Phase 3–7 |
| `loser_durable_refs.drain()` 的消费端、value 物理回收 | flush retire 链路 |

> 反面规则：如果本 step 的审查或实现过程中发现"必须顺便做一点 front scheduler / builder / registry"，那是越界信号——停下来把它挪到后续 step，不要在本 step 中偷带。

## 文件结构

```text
apps/inconel/
└── core/
    └── memtable.hh                  — 新增：本 step 的唯一产物
```

不改任何现有文件。`core/registry.hh`、`front/` 目录、`runtime/builder.hh` 一律不动。

## 设计目标

1. **只定义类型，不定义 runtime**。本 step 落地后，`apps/inconel/front/` 仍然只有 `.gitkeep`，`core/registry.hh` 仍然不认识 front。
2. **字段形状严格对齐 design_doc/**：任何与 OV §5.1 / §5.3、RSM §3.1-3.3、cross_doc §2 不一致的字段都算不一致，需先修 design_doc 再回改本 step。
3. **不引入 Phase 1 专属 carrier**。`collect_eligible_gens / release_gens / flushed_gens_by_front` 的 shape 用现成容器表达（`absl::InlinedVector<std::shared_ptr<memtable_gen>, 8>` / `std::span<const uint64_t>` / `absl::flat_hash_map<uint32_t, small_vec<...>>`），不发明新 POD。
4. **单一存储 owner：per-gen `kv_arena`**。每个 `memtable_gen` 持一个 bump-allocated arena 同时承载 key bytes 和 value bytes；`memtable_entry` 里的 key 和 value 都只是 view，不做单独 heap 分配。`std::shared_ptr<memtable_gen>` 的 control block refcount 是整条 memtable 对象图的**唯一**跨线程 atomic gate——arena、key view、value view 全部随 gen 一起生灭。
5. **POD-friendly**：因为 value 改成 view，`value_handle` / `memtable_entry` 都退化成 trivially copyable POD，去掉原本因 `unique_ptr<hot_blob>` 引入的 move-only 约束和自定义 deleter。

## 设计决策

| # | 决策点 | 结果 | 说明 |
|---|---|---|---|
| `D1` | 本 step scope | **只写 `core/memtable.hh` 一个头文件** | scheduler / builder / registry / sender 一律不做 |
| `D2` | 类型放置 | **全部放 `core/memtable.hh`** | 对齐 `code_modules.md` §5 "跨模块共享的运行期对象在 `core/`" |
| `D3` | key/value 存储机制 | **`gen_arena`：per-`memtable_gen` bump allocator，同时承载 key bytes 与 value bytes** | 单次 `allocate(src, len)` 返回 `string_view` 指向 arena 切片；所有 key 和 value 都走这个接口，没有例外。arena 是 `memtable_gen` 的 by-value 字段，随 gen 一起生灭，由 `std::shared_ptr<memtable_gen>` 控制整条链 |
| `D3a` | arena chunk 策略 | **默认 64 KB chunk；单个超长 allocation 独占 `max(len, kChunkBytes)` chunk** | 典型 workload tail waste < 1%；超长 value 对应原 `new hot_blob{big}` 的等价物 |
| `D3b` | arena chunk 数据结构 | **`std::vector<std::unique_ptr<char[]>>`** | 析构时 vector 析构级联释放所有 chunk，一次 sweep |
| `D4` | `memtable_gen::table` 容器 | **`absl::btree_map<std::string_view, absl::InlinedVector<memtable_entry, 1>>`** | key 类型是 `std::string_view`，指向本 gen 的 `kv_arena`；`logical_key` typedef **删除**（跨模块只认 `std::string_view`） |
| `D5` | `value_handle` 形态 | **POD `{ value_ref durable; value_view hot; }`** | `hot` 是 `{const char*, uint32_t}`，指向本 gen `kv_arena` 中的 value 切片；无任何 heap owner；trivially copyable |
| `D5a` | `hot_blob` 类型 | **删除** | 原来的 `struct hot_blob { len; data[] }` 及其 `hot_blob_deleter` 一并删除——arena 切片就是 value bytes，没有独立命名对象 |
| `D6` | `memtable_entry` 可拷贝性 | **trivially copyable POD** | 不再含 `unique_ptr`；`absl::InlinedVector<memtable_entry, 1>` 的 relocate 走 trivial `memcpy` 路径 |
| `D6a` | `memtable_gen` 字段声明顺序 | **`kv_arena` 声明于 `table` 之前（防御性语义）** | 保证反向析构顺序中 table 先于 arena 析构；`table` 析构不 deref arena 切片，但声明顺序让依赖关系显式 |
| `D7` | `memtable_gen` 的引用计数 | **`std::shared_ptr<memtable_gen>` 的 control block refcount**；`memtable_gen` 本体**不**嵌 `ref_count` 字段 | gen 量级小（每 front 几到十几个），shared_ptr 16B pointer + 融合 control block 的开销可以忽略；收益是不需要 `intrusive_ptr_add_ref / intrusive_ptr_release` 的 custom glue。构造走 `std::make_shared<memtable_gen>(...)`。所有跨 scheduler 引用（PRS / front_state.imms / flush round state）都用 `std::shared_ptr<memtable_gen>` |
| `D7a` | `lookup_memtable` return 形态 | **`variant<value_view, tombstone, miss>`** | 命中 value 时直接 `cb(winner.vh.hot)`，`vh.hot` 本身就是 `value_view`，无需再构造；调用方在 read_handle 作用域内使用（详见 RSM §3.7 的生命周期契约） |
| `D8` | `retire_list<T>` 形态 | **薄 wrapper：内部 `absl::InlinedVector<T, 16>` + `push` / `drain(F)` / `size()`** | 与 RSM §3.3 的命名一致；未来若 fold 量变大可换 chunk list，不影响 caller API |
| `D9` | `front_read_set` 字段顺序 | **`active` 在前，`imms` 为 newest→oldest** | 与 OV §5.3 / RSM §3.1 一致；构造时逐一拷贝 `std::shared_ptr<memtable_gen>`（`use_count++`） |
| `D10` | `collect_eligible_gens` 返回 shape | **`absl::InlinedVector<std::shared_ptr<memtable_gen>, 8>`** | 现成容器，不引入专属 carrier；reducer 端按 front owner_id 作 key 汇总 |
| `D11` | `release_gens` 输入 shape | **`std::span<const uint64_t>`** | gen_id 列表，只读借用；handle 实现是后续 step 的事 |
| `D12` | `flushed_gens_by_front` shape | **`absl::flat_hash_map<uint32_t, absl::InlinedVector<std::shared_ptr<memtable_gen>, 8>>`** | key = front owner_id；与 RSM §4.2 `tree_flush_result` 字段一致 |
| `D13` | 约束 A（收窄显式声明） | **本 step 只定义类型，编译期没有未覆盖的 tree 形态 / 路径需要 fail-fast** | 本 step 不引入任何运行期逻辑，因此不会产生 silent fallback 风险 |
| `D14` | 是否定义 `handle_*_req / _res` 结构 | **不定义** | handle 不存在时的 req/res 是空壳；shape 已在 D10-D12 用现成容器表达 |

## 详细设计

### `core/memtable.hh` 大纲

```cpp
#ifndef APPS_INCONEL_CORE_MEMTABLE_HH
#define APPS_INCONEL_CORE_MEMTABLE_HH

// 跨模块共享的 memtable 运行期类型。只定义类型，不引入任何
// scheduler / handle / factory。修改前对照：
//   - design_overview.md §5.1, §5.3
//   - runtime_state_machine.md §3.1-3.3, §3.5
//   - runtime_memory_and_cache.md §2, §3, §9.3, §9.4
//   - cross_doc_contracts.md §2
//   - read_api_and_pipeline.md §4.4

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/btree_map.h>
#include <absl/container/inlined_vector.h>

#include "../format/types.hh"

namespace apps::inconel::core {

    using format::value_ref;

    // ── gen_arena ───────────────────────────────────────────────
    // Per-memtable_gen bump allocator. Holds BOTH key bytes and
    // value bytes for this gen. Single-writer: only the owning
    // front_sched mutates while the gen is active; frozen once
    // sealed. When the last shared_ptr<memtable_gen> drops, the
    // arena (and every chunk) frees atomically along with the gen.
    //
    // Chunk size is 64 KB by default. Oversized entries (value
    // bigger than a chunk) get a dedicated chunk of size
    // max(len, kChunkBytes). No internal locking.
    struct gen_arena {
        static constexpr std::size_t kChunkBytes = 64 * 1024;

        std::vector<std::unique_ptr<char[]>> chunks;
        char*                                bump_next = nullptr;
        char*                                bump_end  = nullptr;

        std::string_view allocate(const char* src, std::size_t len);
    };

    // ── value_view ──────────────────────────────────────────────
    // {pointer, length} into a gen's kv_arena. Used as the "hot"
    // half of value_handle and as the return type of
    // lookup_memtable on value hit. Valid while the owning gen's
    // shared_ptr is held (RSM §3.7, RMC §9.3).
    struct value_view {
        const char* data;
        uint32_t    len;
    };

    // ── value_handle ────────────────────────────────────────────
    // POD payload of a memtable PUT entry (OV §5.1, RSM §3.3).
    // `durable` is the stable on-disk location; `hot` is a zero-
    // copy view into the owning gen's kv_arena.
    struct value_handle {
        value_ref  durable;
        value_view hot;
    };

    // ── retired_value_ref ───────────────────────────────────────
    struct retired_value_ref {
        value_ref vr;
        uint64_t  data_ver;   // compared against recovery_safe_lsn
    };

    // ── retire_list<T> ──────────────────────────────────────────
    // Single-writer / single-reader container for
    // memtable_gen::loser_durable_refs. Normal lifecycle is
    // push -> drain; tree flush fold may also clear+rebuild the
    // list when retrying an unfinished round on the same sealed gen.
    template <typename T>
    struct retire_list {
        void push(T v);
        template <typename F> void drain(F&& f);
        std::size_t size() const noexcept;
        void clear() noexcept;
      private:
        absl::InlinedVector<T, 16> items_;
    };

    // ── memtable_entry ──────────────────────────────────────────
    // Trivially copyable POD. data_ver is semantically equivalent
    // to batch_lsn (OV §6). vh is valid iff k == value.
    struct memtable_entry {
        uint64_t     data_ver;
        enum class kind : uint8_t { value, tombstone } k;
        value_handle vh;
    };

    // ── memtable_gen ────────────────────────────────────────────
    // kv_arena holds ALL gen-local bytes (keys + values). The
    // btree_map's key is a std::string_view into kv_arena;
    // memtable_entry.vh.hot is a value_view into the same arena.
    // When the last shared_ptr<memtable_gen> drops, the arena
    // sweeps every chunk, retiring keys / values / hot data
    // together.
    //
    // kv_arena is declared BEFORE table so reverse-order
    // destruction destroys table first (defensive, though table
    // destruction does not dereference arena slices).
    //
    // No embedded refcount: lifetime is managed entirely by
    // std::shared_ptr<memtable_gen>. The shared_ptr control block
    // is the only cross-thread atomic gate for the whole memtable
    // object graph.
    struct memtable_gen {
        uint64_t gen_id;
        enum class state : uint8_t { active, sealed } st;
        uint32_t front_owner_index = UINT32_MAX;
        uint64_t min_lsn = UINT64_MAX;
        uint64_t max_lsn = 0;

        gen_arena                                       kv_arena;
        absl::btree_map<std::string_view,
                        absl::InlinedVector<memtable_entry, 1>>
                                                        table;
        retire_list<retired_value_ref>                  loser_durable_refs;
    };

    // ── front_read_set ──────────────────────────────────────────
    // Readonly snapshot of a single front's active + imms chain,
    // captured at PRS construction time (OV §5.3, RSM §3.1).
    struct front_read_set {
        std::shared_ptr<memtable_gen>                         active;
        absl::InlinedVector<std::shared_ptr<memtable_gen>, 8> imms;  // newest → oldest
    };

    // ── Invariants enforced at compile time ────────────────────
    static_assert(std::is_trivially_copyable_v<value_view>);
    static_assert(std::is_trivially_copyable_v<value_handle>);
    static_assert(std::is_trivially_copyable_v<memtable_entry>);
    static_assert(sizeof(value_view) <= 16);

}  // namespace apps::inconel::core

#endif
```

> 注：本 step 不需要任何定制 smart pointer glue——`memtable_gen` 用 `std::shared_ptr`（`std::make_shared<memtable_gen>(...)` 单次分配 object + control block），kv bytes 都走 arena，entry 本体是 POD。`hot_blob` 和 `unique_ptr` 都不再需要。

### 未由本 step 定义的 shape（只在文档中冻结）

下列 shape 后续 step 直接引用现成容器，不新增类型：

| 名字 | Shape | 引用点 |
|---|---|---|
| `collect_eligible_gens` 返回 | `absl::InlinedVector<std::shared_ptr<memtable_gen>, 8>` | RSM §3.8 |
| `release_gens` 输入 | `std::span<const uint64_t>`（gen_id 列表） | RSM §3.8 |
| `flushed_gens_by_front` | `absl::flat_hash_map<uint32_t, absl::InlinedVector<std::shared_ptr<memtable_gen>, 8>>` | RSM §4.2, FF §4.3 |

这些条目只在本文档列出作为后续 step 的冻结引用，不写进 `core/memtable.hh`。

## 跨文档一致性

本 step 不改任何 design_doc/ 文件；若实现阶段发现 `core/memtable.hh` 字段与下列引用点不一致，必须先修 design_doc 再回改本 step（不能反向反推 spec）：

| 校对点 | 对照文档章节 |
|---|---|
| `memtable_gen / memtable_entry / value_handle` 字段 | OV §5.1, RSM §3.2-3.3, cross_doc §2 |
| `gen_arena` 存在、语义与生命周期 | RSM §3.2, RMC §9.3/§9.4 |
| `value_view` 定义与生命周期 | RSM §3.3/§3.7, RAP §4.4 |
| `retired_value_ref / retire_list` 命名与形态 | RSM §3.3, FF §5.1 |
| `front_read_set` 字段与 imms 顺序 | OV §5.3, RSM §3.1 |
| `flushed_gens_by_front` shape | RSM §4.2, FF §4.3 |
| **反向断言**：文档不应再出现 `hot_blob` / `intrusive_ptr<hot_blob>` / `unique_ptr<hot_blob>` / `logical_key` | 全部 design_doc |

## 明确不做的内容

- 任何 scheduler（front / tree / coord / value / wal）骨架或 advance 循环
- 任何 handle 实现（`collect_eligible_gens / release_gens / lookup_memtable / insert_memtable_entries / seal_active / write_wal_entries / ...`）
- 任何 sender 层（`front/sender.hh` 不创建）
- 任何 builder（`front/memtable_builder.hh` 不创建）
- `core/registry.hh` 的 `front_list / route_to_front / init_capacity / clear` 扩展
- `gen_arena::allocate` 的 caller（insert path）——本 step 只定义类型与内联接口，实际写入路径在未来 phase
- `loser_durable_refs.drain()` 的消费端
- 任何测试 harness——Phase 1 的验证只看类型契约本身，不依赖 insert/WAL/scheduler 路径
- `canonical_entry / fragment / batch_ctx / wal_stream_state / tree_manifest.leaf_order`

任何一项如果在实现时"顺手做一下"，必须停下来立新 step；不要在本 step 夹带。

## 实施顺序

1. 新建 `apps/inconel/core/memtable.hh`，按本文 "§详细设计/`core/memtable.hh` 大纲" 落字段。
2. 实现 `gen_arena::allocate`（内联，单次 `memcpy` + bump，超长 allocation 走独占 chunk）。
3. 实现 `retire_list<T>::push / drain / size`（内联即可）。
4. 确认头文件自包含：下游只 `#include "core/memtable.hh"` 即可编译。
5. 用独立 driver 链 absl 运行一次 `gen_arena` / `memtable_gen` / `front_read_set` / `retire_list` 的最小路径断言。

不创建任何 `.cc` 文件；本 step 全部逻辑都是类型定义 + 内联函数。

## 验证

Phase 1 的验证只围绕类型契约本身，不依赖任何 scheduler / handle / pipeline 路径。范围严格有限：

1. **编译自包含**：
   - `core/memtable.hh` 单独被 `#include` 时不依赖任何尚未实现的头。
   - 下游模块（后续 phase）可以直接用其中的类型做前向声明，不报错。
2. **字段对齐**：
   - `gen_arena / memtable_gen / memtable_entry / value_handle / value_view / retired_value_ref / front_read_set` 的字段顺序、类型、命名与 OV §5.1/§5.3、RSM §3.1-3.3、cross_doc §2 完全一致。
   - `value_handle::hot` 是 `value_view`（POD），不是 `unique_ptr<hot_blob>`。
   - `memtable_gen::kv_arena` 声明在 `table` **之前**。
3. **`gen_arena::allocate` 正确性**：
   - 小于 chunk 的 allocation 走当前 chunk bump；bytes 和 `src` 逐字节一致。
   - 当前 chunk 剩余不够时分新 chunk，旧 chunk 的 tail 放弃。
   - 超过 `kChunkBytes` 的 allocation 获得独占 chunk，size = `len`。
   - `len == 0` 的 allocation 合法（例如 tombstone 的 empty value case 或 empty key），返回的 view 也是空。
4. **`memtable_entry` / `value_handle` POD 语义**：
   - `static_assert(std::is_trivially_copyable_v<memtable_entry>)`
   - `static_assert(std::is_trivially_copyable_v<value_handle>)`
   - `static_assert(std::is_trivially_copyable_v<value_view>)`
   - `static_assert(sizeof(value_view) <= 16)`
5. **`shared_ptr<memtable_gen>` 生命周期**：
   - `std::make_shared<memtable_gen>(...)` 构造后 `use_count() == 1`。
   - 复制/共享时 `use_count` 单调增；最后一个引用释放时 gen 析构。
   - 析构时 `table`、`kv_arena`、`loser_durable_refs` 按**反向声明顺序**依次释放（table 先，arena 后），所有 chunk 一次 sweep。
   - `memtable_gen` 本体没有 refcount 字段。
6. **`retire_list<T>` 语义**：
   - `push` 后 `size` 增；`drain(F)` 把所有元素逐一传给 `F` 并清空；再 `size()` 为 0。
   - 单 writer / 单 reader 约束由 caller 维护，本 step 不加锁。
7. **`front_read_set` 语义**：
   - 构造时对 `active` 与 `imms[i]` 逐一走 shared_ptr 拷贝（`use_count++`）；析构时逐一 release。
   - 禁止裸 `memtable_gen*` 泄露——所有跨域引用必须经由 `std::shared_ptr<memtable_gen>`。
8. **反向断言**（编译器 + 人工对照）：
   - 文件内没有 `hot_blob` / `hot_blob_deleter` / `unique_ptr<hot_blob>` / `logical_key` 这些符号。

验证的唯一形式是 seam 级单元测试（类型 + 生命周期），**不**构造 end-to-end 场景，**不**模拟 sealed gen 安装，**不**触碰 flush fold。后者都属于引入 scheduler / builder 的 step。
