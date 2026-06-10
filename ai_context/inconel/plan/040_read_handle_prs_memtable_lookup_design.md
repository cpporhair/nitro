# 040 — Read Handle / PRS / Memtable Lookup Carrier

> 本文是 `front_wal_development_plan.md` 里 M02 的详细设计文档。
> M02 只冻结 read-side carrier、CAT/PRS pin 链，以及基于 PRS snapshot 的
> front-local memtable lookup / scan CPU 语义。
> 本文不设计 coord scheduler、publish ready bitmap、publish/release gate、
> point_get / multiget / scan sender pipeline，也不设计 tree lookup/value read。

## 1. 范围

M02 承接 M01 的 `front_read_set`、`memtable_gen`、`memtable_lookup_result`，
把旧 `inconel` 分支 Step 8 / Step 11 已验证过的读侧 pin 链迁移到当前
`inconel.new` 架构：

1. Step 8：`lookup_memtable` / `scan_memtable` 在调用方传入的
   `front_read_set` snapshot 上选择 winner。
2. Step 11：`published_read_set` / `publish_catalog` / `read_handle` /
   CPU-only `catalog_store` 固定 reader 可见拓扑与 lifetime。

M02 落点：

1. `apps/inconel/core/memtable_lookup.hh`：
   `lookup_memtable(key, read_lsn, front_read_set)`、
   `scan_memtable(begin, end, read_lsn, front_read_set)`、
   `memtable_scan_item` / `memtable_scan_result`。
2. `apps/inconel/core/read_catalog.hh`：
   `published_read_set`、`publish_catalog`、`read_handle`、`catalog_store`。
3. M02 合约测试：
   active/imms winner、tombstone mask、scan sorted winners、CAT install 后旧
   handle pin 旧 PRS/guard/front gens。

M02 明确不做：

1. 不实现 `coord_state`、`assign_batch_lsn`、`publish_batch`、
   `release_batch`、ready bitmap 或 publish gate。
2. 不实现 front scheduler owner methods；M02 只提供后续 front owner 可调用的
   CPU helper。
3. 不实现 point_get / multiget / range_scan pipeline。
4. 不实现 tree lookup sender、value read sender 或 memtable-over-tree merge。
5. 不实现 `capture_flush_frontier` / `frontier_switch` / `release_gens`。

## 2. 已检查输入

旧分支证据：

1. `inconel:ai_context/inconel/plan/steps/step_08_design.md`
2. `inconel:ai_context/inconel/plan/steps/step_11_design.md`
3. `inconel:apps/inconel/runtime/front/read_set.hh`
4. `inconel:apps/inconel/runtime/front/state.hh`
5. `inconel:apps/inconel/runtime/coord/catalog.hh`
6. `inconel:apps/inconel/test/step_08_front_read_set_contract_test.cc`
7. `inconel:apps/inconel/test/step_11_publish_catalog_contract_test.cc`

当前分支证据：

1. `apps/inconel/core/memtable.hh`
2. `apps/inconel/core/checkpoint_guard.hh`
3. `apps/inconel/core/tree_manifest.hh`
4. `apps/inconel/core/batch_carrier.hh`
5. 当前分支尚无 `published_read_set` / `publish_catalog` / `read_handle`。

正式设计依据：

1. `ai_context/inconel/design_doc/code_modules.md`
2. `ai_context/inconel/design_doc/code_quality_standard.md`
3. `ai_context/inconel/design_doc/cross_doc_contracts.md`
4. `ai_context/inconel/design_doc/runtime_state_machine.md`
5. `ai_context/inconel/design_doc/runtime_memory_and_cache.md`
6. `ai_context/inconel/design_doc/read_api_and_pipeline.md`
7. `ai_context/inconel/design_doc/design_overview.md`

## 3. 语义来源对照表

| 项目 | 旧 `inconel` 证据 | 当前 `inconel.new` 状态 | 正式设计依据 | 040 决议 |
|---|---|---|---|---|
| `front_read_set` | Step 8 定义 `active + imms`，用 `intrusive_ptr<memtable_gen>` pin gens。 | M01 已定义 `std::shared_ptr<memtable_gen> active` + `std::vector<std::shared_ptr<memtable_gen>> imms`。 | RSM/RMC 明确 `std::shared_ptr<memtable_gen>` 是 pin 链唯一跨线程 atomic gate。 | 沿用 M01 形态，不引入 intrusive refcount；`imms` 语义仍为 newest -> oldest，但 correctness 不依赖该顺序。 |
| `lookup_memtable` | Step 8 搜索 `active + imms`，取 `data_ver <= read_lsn` 的最大版本；旧 result 带 `data_ver` 和 hot value。 | M01 只有单 gen `lookup_visible()`，result 是 value_ref/tombstone/miss。 | Cross-doc 要求 `(key, read_lsn, front_read_set) -> variant<value_ref, tombstone, miss>`；INC-055 要求 value_ref-only。 | 新增跨 gen `lookup_memtable()`，返回 M01 `memtable_lookup_result`，不暴露 value body，也不把 `data_ver` 塞回 point lookup API。 |
| `scan_memtable` | Step 8 返回按 key 排序的 winners，并保留 tombstone；旧 item 使用旧 lookup result。 | 当前无 scan helper。 | RAP §6.3 要求返回 sorted `[(key, memtable_entry)]`，merge 阶段 tombstone 参与遮蔽。 | 新增 `memtable_scan_item { key, data_ver, kind, vh }` 和 `memtable_scan_result`。它保留 winner metadata，但 value body 仍不进入结果。 |
| active 命中是否提前返回 | Step 8 明确禁止；测试覆盖 active 命中但 imm 有更大 visible version。 | M01 单 gen helper 无跨 gen逻辑。 | RSM §3.7/RAP §4.3 明确 topology order 不等于版本顺序。 | `lookup_memtable` 和 `scan_memtable` 都必须比较所有 candidate 的 `data_ver`，不能按 active/imms 顺序短路。 |
| tombstone 语义 | Step 8 tombstone winner 不回退旧 value，scan 保留 tombstone。 | M01 result 有 `memtable_tombstone`。 | OV/RAP：只要 memtable 命中 tombstone，就对上层 not found；range scan merge 后再过滤 tombstone。 | point lookup 返回 `memtable_tombstone`；scan item 保留 `kind=tombstone`，由后续 API formatting/merge 过滤。 |
| `published_read_set` | Step 11 定义 `tree_guard`、`fronts`、`epoch`；`fronts` 用 shared_ptr 持有 vector。 | 当前有 `checkpoint_guard` 真实类型，但无 PRS。 | Cross-doc/RMC 定义 read_handle -> CAT -> PRS -> fronts/tree_guard pin 链。 | 在 `core/read_catalog.hh` 定义 PRS；`fronts` 使用 `std::shared_ptr<const std::vector<front_read_set>>`，避免 acquire 时复制所有 fronts。 |
| `checkpoint_guard` | Step 11 只是空 stub，足以被 shared_ptr pin。 | 当前已有 `core::checkpoint_guard { manifest, retired }`。 | RMC/FF 要求 guard pin manifest，并承载 retired objects。 | M02 直接使用当前 `core::checkpoint_guard`，不定义 parallel stub。PRS 持有 `std::shared_ptr<checkpoint_guard>`，因为后续 frontier_switch 会向旧 guard 挂 retired。 |
| `publish_catalog` | Step 11 定义 `prs`、atomic `durable_lsn`、`epoch`。 | 当前无 CAT。 | RSM §2.3/RAP §2.1 要求 acquire 时 atomic load CAT 后 acquire-load durable_lsn。 | 定义 `publish_catalog` 同字段；`durable_lsn` 是 atomic。CAT 通过 shared_ptr 发布，不按值复制。 |
| `read_handle` | Step 11 定义 `{cat, read_lsn}`；旧 handle pin 旧 CAT。 | 当前无 read handle。 | RAP §2：整次 GET/MultiGet/Scan 共享一个 read_handle。 | 定义 `read_handle { std::shared_ptr<const publish_catalog> cat; uint64_t read_lsn; }`。read_lsn 是 acquire 瞬间 snapshot。 |
| `catalog_store` | Step 11 用 CPU-only store 测试 atomic load/install，不实现 coord_state。 | 当前无 current CAT。 | RSM 中真正 owner 是 coord_sched.current_cat。 | M02 提供 CPU-only `catalog_store` 作为 core test helper / 后续 coord 可复用组件；它不是 coord scheduler，不推进 durable_lsn，不生成 epoch。 |
| `batch_lookup` | 旧 `front_state` 有 batch lookup helper。 | 当前无 front owner。 | RSM §3.7 把 batch_lookup 作为 front-side handle。 | 排除出 M02。M02 只提供 `lookup_memtable` helper；front owner 的 batch handle 在 M05/M10 设计。 |

## 4. Memtable Lookup / Scan 设计

### 4.1 Point Lookup

接口：

```cpp
[[nodiscard]] memtable_lookup_result
lookup_memtable(std::string_view key,
                uint64_t read_lsn,
                const front_read_set& frs);
```

语义：

1. `frs.active` 可以为空；`frs.imms` 可以为空。
2. 在 `frs.active` 和每个 `frs.imms` 中查找 `key`。
3. 对所有命中的 version entries，只考虑 `entry.data_ver <= read_lsn`。
4. winner 是最大 `data_ver` 的 entry。
5. 无 winner 返回 `memtable_miss`。
6. winner 是 value 返回 `memtable_value_hit{winner.vh.durable}`。
7. winner 是 tombstone 返回 `memtable_tombstone`。

禁止事项：

1. 不能因为 active 命中就提前返回。
2. 不能依赖 `imms` newest -> oldest 顺序替代 `data_ver` 比较。
3. 不能返回 value body、`value_view`、`hot_blob` 或指向 client batch 的 view。
4. 不能读取 front scheduler 当前 active/imms；只能使用调用方传入的 `frs`。

实现策略：

1. 复用 M01 的 `find_visible_entry(gen, key, read_lsn)` 做单 gen winner。
2. 在跨 gen 层只比较返回 entry 的 `data_ver`。
3. point lookup 不分配 heap，不复制 key，不复制 value bytes。

### 4.2 Scan Result

M02 冻结 front-local scan item：

```cpp
struct memtable_scan_item {
    std::string_view       key;
    uint64_t               data_ver;
    memtable_entry::kind   kind;
    value_handle           vh;       // 仅 kind == value 时有效
};

using memtable_scan_result = std::vector<memtable_scan_item>;
```

说明：

1. `key` 是指向某个 pinned `memtable_gen::kv_arena` 的 view。
2. `data_ver/kind/vh` 是 winner entry 的 by-value metadata copy。
3. tombstone item 保留 `data_ver` 和 `kind=tombstone`，`vh` 无意义。
4. scan result 不持有 gens；调用方必须保证持有对应 `front_read_set` / `read_handle`
   直到结果消费完成。后续若需要跨异步边界保存 scan result，必须同时保存
   `read_handle` 或显式 pin carrier，不能单独保存 key views。

### 4.3 Scan

接口：

```cpp
[[nodiscard]] memtable_scan_result
scan_memtable(std::string_view begin,
              std::string_view end,
              uint64_t read_lsn,
              const front_read_set& frs);
```

范围语义：

1. 扫描半开区间 `[begin, end)`。
2. 若 `begin >= end`，返回空结果。
3. key bytes 按 `std::string_view` 字节序比较；不解释 key 格式。

winner 语义：

1. 对 `active + imms` 中每个 gen，取 `[begin, end)` 的有序 map range。
2. 多 gen 做 k-way merge。
3. 同一 key 只输出一个 winner。
4. winner 规则与 point lookup 完全一致：最大 visible `data_ver`。
5. 输出按 key 升序。
6. 保留 tombstone winner；后续 memtable-over-tree merge/API formatting 再过滤。

成本约束：

1. `scan_memtable` 可以为结果 vector 和 gen ranges 分配。
2. 不复制 value bytes。
3. 不为每个 key 复制 owning key string；输出 key view。
4. 不构造 per-key heap object。

### 4.4 线程安全边界

`lookup_memtable` / `scan_memtable` 是 CPU-only helpers，不自己加锁。
production 调用必须发生在 owning front scheduler 上，原因是 PRS 中的 active
gen 可能仍在被同一个 front owner 写入。sealed gens 是 immutable 的；active
gen 的读写串行性由 front scheduler 队列保证。

M02 测试可以直接调用 helper，因为测试中没有并发 writer。

## 5. CAT / PRS / Read Handle 设计

### 5.1 `published_read_set`

```cpp
struct published_read_set {
    std::shared_ptr<checkpoint_guard> tree_guard;
    std::shared_ptr<const std::vector<front_read_set>> fronts;
    uint64_t epoch = 0;
};
```

约束：

1. `tree_guard` 必须非空。它 pin 当前 reader 可见 tree manifest。
2. `fronts` 必须非空；`fronts->size()` 是当前 runtime 的 front count。
3. `fronts` 使用 shared_ptr 持有 immutable vector。读 handle 获取时不复制所有
   `front_read_set`。
4. `fronts[i]` 中的 `shared_ptr<memtable_gen>` 负责 pin 该 front 的 active/imms。
5. `epoch` 用于诊断和测试，不参与 winner 选择。

`published_read_set` 本身不推进 durable_lsn，不修改 front gens，不参与 recovery
输入。

### 5.2 `publish_catalog`

```cpp
struct publish_catalog {
    std::shared_ptr<const published_read_set> prs;
    mutable std::atomic<uint64_t> durable_lsn;
    uint64_t epoch = 0;
};
```

约束：

1. `prs` 必须非空。
2. `durable_lsn` 是该 CAT 上已经连续发布的最大 batch LSN。
3. `durable_lsn` 可以在 CAT 仍是 current CAT 时由后续 coord scheduler 推进。
4. 旧 CAT 被新 CAT 替换后，旧 CAT 的 `durable_lsn` 不再前进。
5. `epoch` 必须与 `prs->epoch` 一致；构造 helper 应 fail-fast 检查。

M02 不实现 durable_lsn 推进逻辑；那属于 M03。

### 5.3 `read_handle`

```cpp
struct read_handle {
    std::shared_ptr<const publish_catalog> cat;
    uint64_t read_lsn = 0;
};
```

语义：

1. `cat` pin 住 acquire 时看到的 CAT。
2. `read_lsn` 是 acquire 时从 `cat->durable_lsn` acquire-load 得到的 snapshot。
3. 后续即使 current CAT 切换，或新 CAT 的 durable_lsn 前进，旧 handle 的
   `cat/read_lsn` 都不变。
4. 整次 GET/MultiGet/Scan 必须共享同一个 `read_handle`。

### 5.4 `catalog_store`

M02 提供 CPU-only helper：

```cpp
class catalog_store {
public:
    explicit catalog_store(std::shared_ptr<const publish_catalog> initial_cat);

    [[nodiscard]] read_handle acquire_read_handle() const noexcept;
    void install_cat(std::shared_ptr<const publish_catalog> new_cat);
    [[nodiscard]] std::shared_ptr<const publish_catalog> current_cat() const noexcept;
};
```

实现要求：

1. 内部使用 `std::atomic<std::shared_ptr<const publish_catalog>>`，或等价的
   `atomic_load` / `atomic_store` shared_ptr API。
2. constructor 和 `install_cat` 必须拒绝 null CAT；这是配置/调用错误。
3. `acquire_read_handle()` 顺序必须是：
   - acquire-load current CAT shared_ptr
   - acquire-load `cat->durable_lsn`
   - 返回 `read_handle { cat, read_lsn }`
4. acquire hot path 不做 heap allocation，不复制 PRS/fronts。
5. `install_cat()` 只 store 新 CAT，不生成 epoch，不推进 durable_lsn，不释放旧 CAT；
   旧 CAT 的释放由 shared_ptr 引用计数自然决定。

`catalog_store` 是 M02 的测试壳和后续 coord 可复用组件，不是 `coord_state`。
M03 可以把它嵌进 coord owner，也可以直接按同样语义实现 current CAT；不能改变
M02 冻结的 acquire/install 语义。

## 6. Pin 链与生命周期

读路径 correctness owner 链：

```text
read_handle
  -> publish_catalog
     -> published_read_set
        -> fronts vector
           -> front_read_set
              -> shared_ptr<memtable_gen>
                 -> kv_arena key bytes
                 -> table / memtable_entry / value_handle.durable
        -> checkpoint_guard
           -> tree_manifest
           -> retired_objects
```

必须保证：

1. 旧 handle 存活时，旧 CAT 不释放。
2. 旧 CAT 存活时，旧 PRS 不释放。
3. 旧 PRS 存活时，旧 fronts vector、每个 front_read_set pin 的 memtable gens、
   tree_guard 都不释放。
4. `frontier_switch` 后从新 PRS 中移除的 memtable gens，只要旧 read_handle 仍在，
   仍由旧 PRS pin 住。
5. value body 不在该 pin 链中；memtable hit 后调用方必须用 `value_ref` 走 value
   scheduler 读 body。

## 7. 测试要求

M02 实现必须新增合约测试，例如
`apps/inconel/test/test_m02_read_handle_prs_memtable_lookup.cc`，并接入 CMake。

必须覆盖：

1. `lookup_memtable_prefers_highest_visible_version_across_active_and_imms`：
   active 命中但 imm 有更大 visible `data_ver` 时，返回 imm winner。
2. `lookup_memtable_respects_read_lsn`：
   `data_ver > read_lsn` 不可见；低 read_lsn 可返回旧版本或 miss。
3. `lookup_memtable_tombstone_masks_older_value`：
   tombstone winner 返回 `memtable_tombstone`，不回退旧 value。
4. `empty_front_read_set_returns_miss_and_empty_scan`。
5. `scan_memtable_merges_winners_sorted_by_key`：
   active + 多个 imm 的结果按 key 排序，每个 key 只输出一个 winner。
6. `scan_memtable_respects_range_bounds_and_keeps_tombstones`。
7. `scan_memtable_does_not_copy_value_body`：
   result 只含 `value_ref`/metadata，不含 value bytes。
8. `acquire_read_handle_snapshots_cat_and_read_lsn`：
   acquire 后原 CAT durable_lsn 改变，旧 handle.read_lsn 不变。
9. `install_cat_switches_only_new_readers`：
   install 新 CAT 后，新 reader 看到新 CAT，旧 reader 仍 pin 旧 CAT。
10. `old_handle_pins_old_prs_guard_and_front_gens`：
    用 `weak_ptr` 验证旧 handle 释放前旧 CAT/PRS/guard/front gens 不析构。
11. `catalog_store_rejects_null_cat`：
    constructor / install null CAT 失败。

旧 Step 8 中断言 hot value body 的测试必须改成 value_ref-only。旧 Step 11 中基于
intrusive refcount 的断言必须改成 `std::weak_ptr` / `shared_ptr::use_count`
语义，不能重新引入 `memtable_gen::ref_count`。

## 8. 冲突与决议

1. **旧 lookup result 带 `data_ver` vs 正式 value_ref/tombstone/miss variant**  
   旧 Step 8 的 `memtable_lookup_result` 同时服务测试和 scan，携带 `data_ver`。
   039 和正式 cross-doc 已把 point lookup result 冻结为
   `variant<value_ref, tombstone, miss>`。040 选择正式 result。需要 winner
   metadata 的 scan 使用独立 `memtable_scan_item`。

2. **旧 intrusive_ptr vs 当前 shared_ptr**  
   旧 Step 8/11 用 `intrusive_ptr<memtable_gen>` 并在测试里读 refcount。
   当前正式设计明确 memtable_gen 不嵌 refcount，生命周期由
   `std::shared_ptr<memtable_gen>` 管理。040 不迁移 intrusive refcount。

3. **Step 11 空 `checkpoint_guard` vs 当前真实 guard**  
   旧 Step 11 只需要空 stub。当前分支已经有 `checkpoint_guard { manifest, retired }`。
   040 直接使用当前类型，避免后续 frontier_switch 再做类型替换。

4. **`catalog_store` 是否属于 coord**  
   旧 Step 11 用它测试 current CAT atomic load/store；正式 RSM 中 owner 是
   `coord_sched.current_cat`。040 保留 CPU-only helper，但明确它不是 coord scheduler。
   M03 才能决定是否复用该 helper。

## 9. 相邻事项

M03 应直接消费 `read_catalog.hh` 的 CAT/read_handle 语义来实现 coord scheduler 的
`acquire_read_handle`、`install_cat`、`publish_batch`、`release_batch` 和 gate。
M03 不能改变 M02 的 acquire 顺序和 old-handle pin 链。

M05 front scheduler owner 应把 `lookup_memtable` / `scan_memtable` 包成 owner
handle，并保证在 front owner 线程上执行，避免 active gen 并发读写。

M10 point_get / MultiGet / range scan pipeline 才负责把 memtable result 接到
tree lookup 和 value read；M02 不返回 value body，也不做 API 级 tombstone
filtering。

M12/flush frontier switch 使用 `published_read_set` 和 `checkpoint_guard` 时，必须
保持旧 PRS 对 flushed gens 的 pin 链，直到旧 read_handle 全部释放。
