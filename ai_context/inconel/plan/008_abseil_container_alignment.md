# 008 — 当前代码的 Abseil 容器对齐

> 实现第八步。只设计当前代码里要改的 Abseil 容器替换项，不冻结 future front/runtime 的实现选择。**不要求 shadow CoW B+ tree 本体改写成 Abseil 结构，但这不等于 `/tree` 下的代码全部不动。** 本步排除的是 tree 本体页格式 / shadow slot / consolidation 等核心结构；tree-adjacent 的运行时容器仍可纳入范围。

## 文件结构

```
format/
└── types.hh                           — 为 `paddr` 补 Abseil hash 适配（保留 std::hash）

core/
└── tree_manifest.hh                   — `slot_map` 改为 `absl::flat_hash_map`

core/
├── clock_cache.hh                     — `index_` 改为 `absl::flat_hash_map`
└── slru_cache.hh                      — `index_` 改为 `absl::flat_hash_map`

tree/
└── scheduler.hh                       — `loading_pages_` 改为 `absl::flat_hash_set`

value/
├── allocator.hh                       — bounded class metadata 改为 `absl::InlinedVector`
└── scheduler.hh                       — round table / class size metadata 改为 Abseil 容器
```

## 设计目标

1. 收敛当前已存在代码里的 Abseil 容器偏差，覆盖 value 和 tree-adjacent runtime bookkeeping。
2. 保持 shadow CoW B+ tree 本体风险为 0；本 step 不触碰页格式、shadow slot、split / consolidation、builder / reader。
3. 明确本步哪些结构该换成 Abseil，哪些结构不该为了“统一风格”机械替换。
4. 把“当前建议”和“硬约束”区分开，避免后续实现把建议误读成冻结结论。

## 优先级规则

本仓库里，决策顺序不是“纯性能优先”，而是：

1. **先对齐 spec / 已冻结设计语义**
2. **在 spec 允许的空间里追求最佳运行效率**

如果判断“性能最佳方案看起来应该偏离当前 spec”，正确动作不是直接实现更快版本，而是：

1. 在 plan 中显式标出这是 `open question`
2. 说明当前 spec 方案和候选方案的收益/代价
3. 在实现前先讨论，必要时先改 spec 再写代码

因此，本 step 里每个替换项都需要标清楚：它到底是 `spec 硬约束`，还是 `当前实现建议`，避免后续实现把“当前建议”误读成“不可讨论的冻结结论”。

## 约束等级

| 等级 | 含义 | 实现动作 |
|---|---|---|
| `L1 — spec 硬约束` | design_doc 已明确冻结语义或结构词汇，偏离会改变模块契约 | 直接按 spec 实现；若想改，先讨论并改 spec |
| `L2 — spec 倾向 + 当前建议` | design_doc 明确给出容器词汇或形状，但更多是实现/性能导向，不直接承载外部契约 | 默认按文档实现；若 profiling 强烈反证，需要先提讨论 |
| `L3 — 实现自由区` | spec 未冻结，当前只是为了不扩散 `std::*` 给出的建议 | 可以基于测量调整，但要在 plan 中写明理由 |

## 设计决策

| # | 决策点 | 结果 | 说明 |
|---|---|---|---|
| D1 | 范围 | **排除 tree 本体，不排除 `/tree` 目录本身** | `page_builder` / `page_reader` / shadow slot / consolidation / split 不在本步；`tree_manifest` / lookup inflight bookkeeping 可纳入 |
| D2 | 类型名 | **直接写 `absl::...`** | 不引入本地 alias；spec 已经用 `flat_hash_map` / `btree_map` / `small_vector` 指具体设计对象 |
| D3 | hash 适配位置 | **在 key 类型上一次性补齐** | `paddr` 增加 `AbslHashValue`，避免每个 `flat_hash_map<paddr, ...>` 重复写 hasher |
| D4 | 范围之外的 future 容器 | **本步不定义** | future front/runtime 的容器选择不在本文件冻结 |
| D5 | `InlinedVector` 适用面 | **只用于有明确小上界的 bounded metadata** | 无界工作集、page image 容器、队列语义结构保留 `std::vector` |
| D6 | 构建依赖 | **本 step 只引入 container 子集** | 不在本 step 引入 `absl::ComputeCrc32c`；CRC 迁移单独留给 INC-032 |
| D7 | issue 对应关系 | **本 step 覆盖 INC-009 + INC-010 的当前代码子集** | 解决 `slot_map` / `loading_pages_` / shared cache `index_`，以及当前 value 里的容器偏差 |

## 本 step 各项的约束等级

| 项目 | 等级 | 说明 |
|---|---|---|
| `core::tree_manifest::slot_map -> absl::flat_hash_map<paddr, uint32_t>` | `L2` | spec 把 manifest 的 slot map 写成 `flat_hash_map`，但这里仍属于实现/性能结构，不是 page format 契约 |
| `tree::lookup_scheduler::loading_pages_ -> absl::flat_hash_set<paddr>` | `L3` | 当前实现是 set + waiter 链；本步只换容器，不重做 single-flight 结构 |
| `core::clock_cache::index_ -> absl::flat_hash_map<paddr, uint32_t>` | `L3` | 这是 cache 内部索引实现，不是外部契约；当前推荐 flat hash 以减少节点分配和 pointer chasing |
| `core::slru_cache::index_ -> absl::flat_hash_map<paddr, uint32_t>` | `L3` | 同上；容器替换不改变 SLRU 的链表策略和淘汰语义 |
| `value_allocator::classes_ -> absl::InlinedVector<per_class, 16>` | `L2` | 上界 16 是 format 冻结的，容器本身主要是实现/性能选择 |
| `value::scheduler::class_sizes_storage_ -> absl::InlinedVector<uint32_t, 16>` | `L2` | 同上，16 上界明确，但这里只是本地稳定副本 |
| `value::scheduler::inflight_rounds_ -> absl::flat_hash_map<uint64_t, unique_ptr<round>>` | `L3` | spec 没直接冻结 round table 的容器；这是当前最合理建议，不是不可讨论的定论 |

## 当前代码立即替换范围

### `format/types.hh`

补 `paddr` 的 Abseil hash 适配，保留现有 `std::hash<paddr>`：

```cpp
template <typename H>
H AbslHashValue(H h, const paddr& p) {
    return H::combine(std::move(h), p.device_id, p.lba);
}
```

理由：

- future value reclaim 里的 `hole_pages` / `dirty_pages` / `deferred_freed` 都以 `paddr` 为 key。
- `tree_manifest::slot_map` / `loading_pages_` 也都以 `paddr` 为 key。
- 把 hash 适配放在类型定义处，避免未来每个 `flat_hash_map<paddr, ...>` 都带一份自定义 hasher。
- 保留 `std::hash<paddr>`，不破坏本 step 之外的现有 `unordered_*` 使用点。

### `core/tree_manifest.hh`

当前：

```cpp
std::unordered_map<paddr, uint32_t> slot_map;
```

改为：

```cpp
absl::flat_hash_map<paddr, uint32_t> slot_map;
```

理由：

- spec 的 `tree_manifest` 直接把 `slot_map` 写成 `flat_hash_map`。
- 这里保存的是 immutable snapshot 的 range_base → slot_index 点查表，适合 open-addressing hash 容器。
- 这是 tree-adjacent 的运行时元数据，不是 tree page format 或 shadow CoW 算法本体。
- **约束等级：L2**。如果后续 tree manifest 内部表示整体改成 `slot_seq` / exact `slot_paddr` / 更紧凑布局，应先讨论再改 plan。

### `tree/scheduler.hh`

当前：

```cpp
std::unordered_set<paddr> loading_pages_;
```

改为：

```cpp
absl::flat_hash_set<paddr> loading_pages_;
```

理由：

- 它是 lookup miss 路径上的 inflight 去重集合，属于 tree lookup runtime bookkeeping，不是 B+ tree 本体。
- 本步只做容器实现替换，保持当前 inflight bookkeeping 结构不变。
- 这样可以把“树本体不改”与“`/tree` 下的运行时容器仍可对齐 Abseil”分开。
- **约束等级：L3**。

### `core/clock_cache.hh`

当前：

```cpp
std::unordered_map<paddr, uint32_t> index_;
```

改为：

```cpp
absl::flat_hash_map<paddr, uint32_t> index_;
```

理由：

- `index_` 是纯 `paddr -> slot index` 点查表，适合 open-addressing hash 容器。
- clock 的策略核心在 `slots_ + hand_`，不是 `index_` 本身；替换 hash 容器不改变算法语义。
- 这属于 cache 内部实现优化，和 tree/value 两边的外部接口都无关。
- **约束等级：L3**。若后续 cache_concept 按 INC-036 重做成 `frame_id -> page_frame*`，这里可能会一起改写。

### `core/slru_cache.hh`

当前：

```cpp
std::unordered_map<paddr, uint32_t> index_;
```

改为：

```cpp
absl::flat_hash_map<paddr, uint32_t> index_;
```

理由：

- `index_` 同样只是 `paddr -> node index` 的点查表。
- SLRU 的真正策略在 `nodes_`、双段链表和 `free_head_`；容器替换不改变 policy。
- 与 `clock_cache` 一起对齐后，page_cache 两个实现的索引层保持一致。
- **约束等级：L3**。若未来 cache 内部 key 从 `paddr` 升级成 `frame_id` 或带 pin/state 的对象索引，应单独讨论重写。

### `value/allocator.hh`

当前 `value_allocator::classes_`：

```cpp
std::vector<per_class> classes_;
```

改为：

```cpp
absl::InlinedVector<per_class, 16> classes_;
```

理由：

- on-disk format 把 `value_size_classes` 固定为最多 16 个，[on_disk_formats.md] 里的 superblock 定义已冻结这个上界。
- `classes_` 是初始化后稳定存在的 bounded metadata，不是无界工作集，适合 inline 存储。
- `per_class::whole_pool` 保持 `std::vector<paddr>` 不变；它是回收池，元素数量不受 16 的上界约束。
- **约束等级：L2**。这里的硬约束是“类数最多 16”，不是“必须用 `InlinedVector`”；若以后发现 `std::array + count` 或其他布局明显更优，应先讨论再改 plan。

### `value/scheduler.hh`

当前立即替换两处：

1. `inflight_rounds_`

```cpp
std::map<uint64_t, std::unique_ptr<round>> inflight_rounds_;
```

改为：

```cpp
absl::flat_hash_map<uint64_t, std::unique_ptr<round>> inflight_rounds_;
```

理由：

- round_id 是单调递增 id，只有按 id lookup / erase，没有 ordered iteration 语义。
- `std::map` 的 RB-tree 节点分配和 O(log N) 查找没有收益，只是多一层指针 chasing。
- 这和 audit/value.md 对 `inflight_rounds_` 的判断一致，属于 INC-010 的自然实施面。
- **约束等级：L3**。如果后续 profiling 表明 round table 的最佳形态不是 hash map，而是 arena/slot table、稳定序容器或其他结构，这里可以重新讨论；不要把它当成“spec 已冻结”的结论。

2. `class_sizes_storage_`

```cpp
std::vector<uint32_t> class_sizes_storage_;
```

改为：

```cpp
absl::InlinedVector<uint32_t, 16> class_sizes_storage_;
```

理由：

- size class 个数同样受 superblock 的 16 上界约束。
- 它只是为了持有一份稳定的 class size 拷贝，适合 inline 保存。
- `class_sizes_view_` 仍然保持 `std::span<const uint32_t>`，外部 helper 接口不变。
- **约束等级：L2**。如果后续 class size 来源改成固定数组 / superblock 直视图，这里也可能不再需要 `InlinedVector` 持有副本。

### 当前明确不替换的 `value/*` 结构

以下结构保留 `std::vector`：

- `round.pages`
- `round.writes`
- `round.followers`
- `std::vector<_value_persist::req*> all_items`
- `writable_pages_`
- `per_class::whole_pool`

理由：

- 它们是批量工作集或回收池，规模由 batch 形态、并发 follower 数、回收状态决定，没有 spec 给出的稳定小上界。
- 把这类结构强行改成 `InlinedVector` 只是“看起来统一”，不是 spec 对齐。
- `writable_pages_` 和 `whole_pool` 还带队列 / 池语义，本 step 不重写它们的行为模型。

## 明确排除项

以下内容**不在 step 8 范围内**：

1. `tree/` 整体：
   - tree page builder / reader / page format
   - shadow range / slot / consolidation / split
   - fold / flush 算法本体

   说明：本步**允许**改 `tree_manifest::slot_map` 和 `tree::scheduler::loading_pages_` 这类运行时容器；排除的是 shadow CoW B+ tree 本体结构，不是整个 `/tree` 目录。

2. CRC 路径：
   - `format/crc.hh`
   - `tree_page_compute_crc`
   - `encode_value_object` / `decode_value_object`

   原因：这属于 INC-032，与 tree/value 共用同一 format 层；本 step 不混入。

3. compatibility alias：
   - 不新增 `small_vector` typedef
   - 不新增 `flat_hash_map` wrapper

   代码应直接写 `absl::...`，让 spec 和实现一眼可对照。

## 实施顺序

1. `format/types.hh` 补 `AbslHashValue(paddr)`。
2. `core/tree_manifest.hh` 把 `slot_map` 改为 `absl::flat_hash_map`。
3. `tree/scheduler.hh` 把 `loading_pages_` 改为 `absl::flat_hash_set`。
4. `core/clock_cache.hh` / `core/slru_cache.hh` 把 `index_` 改为 `absl::flat_hash_map`。
5. `value/allocator.hh` 把 `classes_` 改为 `absl::InlinedVector<per_class, 16>`。
6. `value/scheduler.hh` 把 `inflight_rounds_` 改为 `absl::flat_hash_map`，把 `class_sizes_storage_` 改为 `absl::InlinedVector<uint32_t, 16>`。
7. 编译通过后，验证 tree lookup/page cache/value 的外部行为不变。

## 验证

实现本 step 时应至少验证：

- `inconel_test_tree_lookup`
- `inconel_test_tree_lookup_multicore`
- `inconel_test_page_cache`
- `inconel_test_value`
- `inconel_test_tree_value`
- `inconel_test_runtime`

预期：

- 只有容器实现替换，没有语义变化。
- tree lookup 的 cache miss / inflight 去重行为不变。
- clock / slru 的 get / put / eviction 行为不变。
- value persist / finalize / read 的外部可观察行为不变。
- shadow CoW B+ tree 的页格式、slot 选择、consolidation 语义完全不受影响。

## 已知边界

1. 本 step 只解决 INC-009 / INC-010 在当前代码里的容器替换部分，不处理更深的 tree/value 结构重做。
2. 本 step 解决 shared page cache 的 `index_` 容器替换，但不重写 cache policy / ownership / frame 模型。
3. future front/runtime 的容器选择不在本文件定义；真正做到那一步时，应该单独讨论、单独写 plan。
4. `absl::ComputeCrc32c` 迁移单独留给 INC-032；不要在本 step 混入 format CRC 改造。

## 需要实现前再确认的点

1. `inflight_rounds_` 只是当前推荐用 `absl::flat_hash_map`，不是“性能已被证明最优”的结论。真正动手前，如果 round 生命周期或访问模式发生变化，应先复核。
2. `classes_` / `class_sizes_storage_` 用 `InlinedVector` 的核心依据是“上界 16 + 常驻小对象”，不是已经做过 benchmark。若将来出现明确反例，应先讨论是否把 plan 升级成 `array/count` 或其他布局。
