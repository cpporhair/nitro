# 037 — INC-051 Value Space Manager 设计草案

> 本文记录 `INC-051` 当前讨论结论。状态是设计草案，不是实现说明。
> 目标是从页面管理角度重新设计一套独立机制，而不是在现有
> `whole_pool + hole_pages + trim_pending_pages_` 上继续补丁式演进。

## 目标

`value_space_manager` 是一个独立的、可单元测试的 metadata 组件：

```text
value_alloc_sched
  -> value_space_manager   // 纯逻辑空间 truth + reservation
  -> DMA frame/cache layer // 页像与 DMA 内存
  -> nvme_sched            // read/write/trim I/O
```

它负责：

1. 管理 value area 内 LBA / sub-LBA byte range 的逻辑占用状态。
2. 为 batch value 写入产生逻辑 allocation plan。
3. 接收 value reclaim，释放 byte range / page / extent。
4. 为后台 TRIM 选择当前仍 free 的 LBA run。
5. 从 recovery 的 live extents 重建 runtime free-state。
6. 维护“已 cached 的 partial page”地址级索引，让 planner 优先消费这类
   零 prefill-read 候选。

它不负责：

1. 不拥有 DMA frame，不分配 / 释放 DMA 内存；cached partial index
   只保存 page address、free summary 和 residency token。
2. 不提交 NVMe read / write / trim。
3. 不持久化自身 metadata；runtime 状态由 recovery 重建。
4. 不直接依赖 PUMP sender，可由 scheduler 同步调用。
5. 不管理 tree shadow range；tree fixed-range allocator 可另按 `INC-053` 独立收敛。

## 基本模型

### 地址空间

管理对象是单设备 value-allocatable LBA 区间：

```text
[data_area_base_lba, data_area_end_lba)
```

由于 Inconel 的 Data Area 由 tree 从低地址向高地址、value 从高地址向低地址共享，`value_space_manager` 的分配必须接受调用方传入的当前 `alloc_floor_lba`：

```text
alloc_floor_lba = current tree allocator head
```

任何 allocation plan 都不能返回低于 `alloc_floor_lba` 的 LBA。

本节的 `published_alloc_floor_lba` / `sync_alloc_floor` 是对
`design_overview.md §10.4 #4` 旧 relaxed 设计的修正。旧设计认为两侧
relaxed load 对方 head 即可本地 bump，且读到旧值只会保守少分配；这个论证
方向错误，stale load 可能让 tree/value 分别以为对方占用更少，从而分配到
同一段 LBA。release/acquire 是必要的发布语义，但不是 check-and-claim 的
原子性来源；真正的安全边界是 floor reservation 被 value owner 处理并确认。

跨 core 正确性不通过 relaxed head 采样解决，而通过一个 floor reservation
边界：

```cpp
std::atomic<uint64_t> published_alloc_floor_lba;
```

固定 contract：

```cpp
alloc_floor_sync_result
sync_alloc_floor(uint64_t new_alloc_floor_lba,
                 std::span<const free_extent> tree_returned_extents);
```

语义：

1. tree 想提升 floor 时，必须先发布 reservation request：对
   `published_alloc_floor_lba` 做 `store-release(new_floor)`，并把
   `sync_alloc_floor(new_floor, ...)` 投递到 value owner。
2. tree 不能在 value owner 确认该 floor reservation 前使用新吞掉的低位
   LBA。`store-release` 只是发布请求，不等于 reservation 已完成。
3. value owner 在处理 `sync_alloc_floor(...)` 时，用 owner-local metadata
   判断 `[old_floor, new_floor)` 是否仍可让渡给 tree：不能包含 live value、
   partial page、dirty/open frame、trim_inflight 或已被更早 value allocation
   claim 的范围。若不能确认，返回 rejected，tree 必须 retry/backpressure，
   不能使用该区间。
4. value core 在任何消费 whole-free extents 的入口前，都必须对
   `published_alloc_floor_lba` 做 `load-acquire()`，并用
   `max(owner_local_floor, loaded_floor)` 裁掉 below-floor free spans。这个
   acquire 是防御性 fence；真正让 tree 可以使用新区间的是第 2 条的 owner
   ack。
5. 在 floor request pending 的短窗口里，`global_free_extents` 允许暂时保留
   below-proposed-floor stale extents；tree 尚未使用该区间，因此这些 stale
   extents 只会导致 reservation rejected 或在 ack 前被 owner 清理，不会和
   tree 写入并发。
6. `sync_alloc_floor(...)` 被 value owner 接受后，owner 负责把 below-floor
   stale extents 从 `global_free_extents` 中清走，并把
   `tree_returned_extents` 合并回 map。
7. 如果 tree head 下移并把 whole-free extents 重新让渡给 value，caller 必须
   通过 `tree_returned_extents` 显式把这些 span 交回 manager；manager 不能
   仅凭更低的 floor 自动推断哪些 LBA 已重新可用。
8. `tree_returned_extents` 必须全部满足 `base_lba >= new_alloc_floor_lba`，
   并在插入 `global_free_extents` 前做 coalesce。
9. `published_alloc_floor_lba` 是跨 core reservation request fence；
   `sync_alloc_floor(...)` 是 owner-local confirmation / reconcile。整个
   方案不引入跨 core 锁，但禁止无 ack 的双边独立 bump。

### 状态表示

系统初始不是全局大 bitmap，而是一条连续 free span：

```text
global_free_extents = { [data_area_base_lba, data_area_end_lba) }
group_directory = empty
```

逻辑状态定义如下：

```text
free whole LBA / extent:
  存在于 free_extents，按 base_lba 排序并 coalesce

partial sub-LBA page:
  存在于 partial_pages，记录 page-local allocator metadata

full allocated page:
  implicit，既不在 free_extents，也不在 partial_pages

trim_inflight:
  后台 TRIM 临时 withheld 的 free span / bits
```

因此 `lba_state[]` 只是概念模型，不是启动时分配的全局数组。

注意：在 floor reservation request pending 的短窗口里，`global_free_extents`
可以是“当前可分配 whole-free spans”的超集，也就是临时包含一些
below-proposed-floor stale extents。真正的跨 core 安全边界是 value owner
接受该 floor reservation 后形成的 owner-local acknowledged floor；单独的
`published_alloc_floor_lba` 只是 request fence。

### Group 分片

为控制元数据局部性，整个 value area 按固定大小 group 切分，例如 256 MiB 或 1 GiB：

```cpp
struct group_state {
    uint64_t base_lba;
    uint32_t lba_count;

    sparse_partial_repr partial_pages; // exact sparse metadata + selection index

    uint32_t partial_page_count;
};
```

group 是 partial-page metadata 和局部统计的 locality boundary，不持有 whole-free space truth。group 边界由公式固定计算，但 group metadata 按需物化：

```text
group_id = (lba - data_area_base_lba) / group_size_lbas
```

格式字段：

```cpp
uint32_t value_space_group_size_lbas;
```

约束：

1. `value_space_group_size_lbas * lba_size` 必须是 2 的幂字节数。
2. 合法范围固定为 `[64 MiB, 1 GiB]`。
3. 生产默认值固定为 `256 MiB`。
4. `group_size_lbas` 必须是整数个 LBA，且天然覆盖整数个 `value_space_quantum_bytes`。
5. 该字段由 format/init 写入 superblock，start 只读，不允许 runtime 覆盖。

触发物化的场景：

1. 释放 sub-LBA allocation 后页面未全空，需要记录 `partial_pages`。
2. recovery 在该 group 内发现 live sub-LBA allocations，需要重建 partial metadata。
3. 局部统计（如 partial page 数、cached partial 热度）首次需要落到该 group。

新系统的初始状态仍然只是一条全量 free span，不预先物化所有 group 的 bitmap / map：

```text
global_free_extents = { [data_area_base_lba, data_area_end_lba) }
group_directory = empty
```

group 目录可以是稀疏 map，也可以是 pointer vector；即使使用 vector，也只允许预分配轻量指针 / header，不允许启动时为每个 group 分配 bitmap 或 partial map。

完整实现必须保持：

1. 不存在全盘级别巨大 bitmap。
2. `global_free_extents` 是 whole-free space 的唯一 owner-local 表示，free extent coalesce 与 group 无关；跨 core 安全边界由 `published_alloc_floor_lba` request fence + value owner acknowledged floor 提供。
3. 一条连续 free span 可以跨任意多个 group，不能因为 group 边界被逻辑切断。
4. 单元测试可以构造很小的 group 来验证 partial metadata 与 global span 的交互不变量。

## Free Space 表示

### Extent Mode

whole-free space 的唯一表示是：

```cpp
absl::btree_map<uint64_t, uint32_t> global_free_extents;
// key   = base_lba
// value = len_lbas
```

分配时从 extent 切出一段，释放时插入新 span 并尝试与左右邻居合并。
固定使用 `absl::btree_map<uint64_t, uint32_t>`，与 `INC-053` 的 tree
extent allocator 方向一致；禁止用逐页 `std::vector<paddr>`、无序 hash map
或按 group 切碎的 per-group free map 承载 whole-free truth。

固定 allocation policy：

1. value 写入优先从高地址端分配，保持与现有“value 从高向低”方向一致。
2. 对 multi-LBA value，请求连续 `span_lbas`。
3. 对 sub-LBA 新页，请求 1 LBA。
4. 分配时必须尊重已确认的 `alloc_floor_lba`。
5. 任何消费 `global_free_extents` 的入口都必须先读取 `published_alloc_floor_lba`
   并与 owner-local acknowledged floor 取 max，然后跳过 / 裁掉 below-floor
   stale extents；但 tree 只有在 value owner ack 后才能使用新 floor。

## Partial Page 表示

sub-LBA page 不能只靠 1-LBA free bitmap 表示。一个 LBA 内哪些 byte range / quantum range 可用必须有 page-local allocator metadata。

`class_idx` 仍然存在，但它只表示 allocation size class，不表示 page layout，也不表示 allocator owner：

```text
whole-free page:
  无 class，可被任意 allocation class 重新使用

partial page:
  可以混合多个 allocation class
  用 page-local allocator metadata 表达哪些 quantum ranges 可用

full page:
  不显式保存 metadata；live objects 的 class 可由 value_ref.len + format class table 推出
```

例如 4 KiB LBA、512 B allocation quantum：

```text
1 KiB value  -> 2 contiguous quantums
512 B value  -> 1 quantum
2 KiB value  -> 4 contiguous quantums

同一个 page 可以同时容纳：
  1 KiB + 1 KiB + 512 B + 512 B + 1 KiB = 4 KiB
```

这避免生产混合 value size batch 因为“每 class 一张 tail page”而放大 write page 数。YCSB 这类同 size workload 会自然退化为单 class 连续填页的 fast path，而不是单独的 workload-specific 设计。

### Boundary Impact

mixed-size page-local allocation 必须封装在 `value_space_manager` / `value_alloc_sched` 边界内：

1. `value_ref { base, byte_offset, len, flags }` 已经表达 page-local byte offset，tree / WAL / read API 不需要新的地址语义。
2. `value_space_manager` 拥有 page-local allocation metadata，并返回 `byte_claim { page_base, byte_offset, alloc_bytes }`。
3. `value_alloc_sched` 把 byte claim 翻译成 DMA page image 写入。
4. 上层只看 durable `value_ref`，不需要知道该 page 是否 mixed-size。

batch path 的映射 contract 固定为：

```text
allocate_batch(reqs, ...)
  可以内部重排 reqs 做 planner
  但返回的 claims 必须恢复为与 reqs 相同的顺序

claims.size() == reqs.size()
claims[i] <-> reqs[i] <-> reqs[i].entry_index
```

也就是说，`byte_claim` 本身不携带 `entry_index`；caller 通过返回向量的位置拿回原始 batch entry 的 claim。实现可以内部按 size descending 排序，但对外必须恢复输入顺序。

因此 mixed-size support 不是 future rewrite；它是当前完整设计的基础形态。YCSB 单 size workload 只是该机制的 fast path。

### Allocation Quantum

page-local allocator 以 allocation quantum 为最小单位：

```text
value_space_quantum_bytes = 64B
```

这是 superblock 中的 disk-format 字段，不是 start-time runtime 配置，也不是
从当前 class table 运行时反推出来的 `gcd(...)`。format/profile 只能提供
“格式化新盘时写入 superblock 的默认模板”；start / recovery 必须从
superblock 读取该字段并校验，不允许由启动参数覆盖。

格式字段：

```cpp
uint32_t value_space_quantum_bytes;
```

format / start validation 必须保证：

1. `value_space_quantum_bytes` 固定为 `64B`
2. `lba_size % value_space_quantum_bytes == 0`
3. 每个 sub-LBA class size 都必须满足：

```text
class_size = 64B * 2^n
class_size < lba_size
```

4. 每个 LBA-equal / multi-LBA class size 都必须满足：

```text
class_size = lba_size * 2^m
class_size >= lba_size
```

5. `byte_offset` 始终按 quantum 对齐
6. `value_size_classes` 必须严格升序且去重
7. 对每个合法 `value_ref.len`，都必须存在唯一的 canonical class 映射

这样固定后，4 KiB LBA 的 page-local bitmap 语义也随之固定：

```text
quantums_per_lba = 4096 / 64 = 64
```

也就是说，对最常见的 4 KiB LBA，一张 partial page 的最小 free-space bitmap 恒为 64 bits；reclaim / recovery / allocator 的页内量化边界不再随 class table 变化。

```text
canonical_class_idx_for_len(len)
  = smallest class whose class_size >= len

canonical_class_size_for_len(len)
  = class_size(canonical_class_idx_for_len(len))
```

8. `persist` / `reclaim` / `recovery` 必须共享同一套 `canonical_class_idx_for_len(len)` 规则
9. 因此 `value_ref` 不需要额外持久化 `class_idx`；`len` 足以唯一恢复 `alloc_bytes` / `alloc_quantums`

`value_space_quantum_bytes` 是 superblock schema 的一部分。把它只放在
`format_profile` 会让 recovery 在 profile 变更后用错误 quantum 重建
partial metadata；因此该字段必须随 disk format 持久化。任何不满足 `64B`
+ `2^n` 约束的 superblock / format profile 都必须在 format / start 阶段
fail-fast 拒绝。

### Page-Local Allocator Algorithm

page-local allocator 固定采用：

```text
quantum bitmap + run summary + best-fit contiguous run
```

拒绝使用 buddy allocator。原因：

1. 单页只覆盖一个 LBA，quantum 数上界很小（例如 4 KiB / 64 B = 64 quantums）。
2. mixed-size page 内的 live range 天然可由 `byte_offset + len` 重建；bitmap 与 recovery / reclaim 直接对齐。
3. buddy 会引入不必要的层级状态和额外内部碎片规则，而单页 bitmap 扫描成本是常数级。

算法：

```text
claim(alloc_quantums):
  find all free runs with len >= alloc_quantums
  choose the smallest run (best-fit)
  if ties, choose lower byte_offset
  mark the chosen quantum range occupied

release(offset_quantum, len_quantums):
  mark the quantum range free
```

每次 claim / release 后重算：

```text
free_quantum_count
largest_free_run_quantums
```

因为每页 quantum 数很小，允许 O(quantums_per_lba) 线性重算，不引入额外的 page-local tree / heap。

```cpp
struct partial_page_meta {
    uint16_t free_quantum_count;
    uint16_t largest_free_run_quantums;
    // 1 bit per quantum; 1 means free.
    small_bitset free_quantum_bits;
};
```

`partial_page_meta` 是 `value_space_manager` 的 runtime metadata，不是 value page 的 on-disk header。释放 value 时只更新 manager metadata，不读写旧 value page；旧 value bytes 留作 garbage，直到对应 byte range 被重新分配并随新的 page image 写回，或整页 all-free 后被 TRIM。

规则：

1. 一个 LBA 可以同时包含多个 allocation class 的 live values。
2. `free_quantum_count == 0` 时页面是 full，删除 partial record。
3. `free_quantum_count == quantums_per_lba` 时页面是 whole-free，删除 partial record，并插入 1-LBA free span。
4. `largest_free_run_quantums` 决定当前 page 能否容纳某个 allocation class。
5. `free_quantum_count == popcount(free_quantum_bits)` 是 runtime invariant；debug / 单测必须校验，热路径按 claim/release quantum 数增减维护。
6. `largest_free_run_quantums` 允许每次 claim / release 后线性重算；不允许引入独立 buddy tree。

### Sparse-Only Partial Metadata

当前纯内存方案固定为 sparse-only exact metadata：

```text
partial_pages 逻辑 truth:
  page_lba -> partial_page_meta
```

它的内存占用随实际 partial page 数线性增长；这比 dense 在稀疏 group
上提前预付整组数组成本更可控。纯内存模式不把 dense partial 作为兜底：
当 partial page 数量过大时，正确动作是改变 allocation policy / backpressure，
而不是继续用另一种内存布局硬撑。

group 内 sparse truth：

```text
sparse_partial:
  absl::flat_hash_map<uint32_t, uint32_t> by_page_delta
    // key   = page_delta
    // value = node_id
```

其中 `page_delta = page_lba - group.base_lba`，避免每条记录保存完整
`paddr`。production 固定为 `absl::flat_hash_map<uint32_t, uint32_t>` +
紧凑 node arena；node_id 是 `uint32_t` handle。禁止每个 partial page 单独
heap allocation，也禁止用 full `paddr` key 的全局 `std::unordered_map`
作为 source truth。

group boundary 保留的原因不是 dense，而是：

1. 将 sparse metadata 的扩容 / rebuild / 校验局部化。
2. 为后续 non-resident partial candidate selection 提供 group-level summary。
3. 为未来如果引入 on-disk group spill / cold metadata cache 保留天然单位。
4. 让单元测试可以用很小 group 覆盖 free span 与 partial metadata 的交互。

当前不实现 dense partial：

```text
no dense partial in pure-memory INC-051
```

理由：

1. Sparse / dense 都无法让 10 亿级 partial page 的纯内存 metadata 变得可接受；dense 只改变常数。
2. Dense 在高密度 group 上有更低 per-page 成本，但误触发会让稀疏 group 预付整组数组成本，内存行为更不可控。
3. 当 partial metadata 接近预算上限时，allocator 必须减少制造新 partial、优先消费已有 partial，必要时 backpressure。
4. 如果未来支持 on-disk group metadata / spill，再重新评估 dense image 是否适合作为 resident/spilled group 的编码格式；这属于后续设计，不进入当前纯内存方案。

### Partial Metadata Budget

`partial_pages` 是受控资源，不能无上限增长。当前纯内存默认预算：

```text
partial_metadata_soft_limit_pages = 10,000,000
partial_metadata_hard_limit_pages = 100,000,000
```

语义：

1. `partial_page_count < soft_limit`：不提升 allocation mode。
2. `soft_limit <= partial_page_count < hard_limit`：把 allocation mode 至少
   提升到 `reuse_pressure`。
3. `partial_page_count >= hard_limit`：把 allocation mode 至少提升到
   `hard_pressure`，并启用 hard gate：本 round 的 projected
   `partial_page_count` 不允许增加。
4. 该预算是 runtime policy，不写入 on-disk format；crash 后仍由 live `value_ref` 重建。

`partial_metadata_*` 不引入第四套分配状态，也不改变三档 cost tuple。
它只参与 mode selection：

```text
space_mode   = mode selected by whole_free_lba ratio
partial_mode = mode selected by partial_page_count
effective_mode = max(space_mode, partial_mode)
```

hard gate 的 projected partial count 规则固定为：

```text
claim existing partial -> page becomes full:
  partial_page_count - 1

claim existing partial -> page remains partial:
  partial_page_count unchanged

fresh whole page commit -> page full:
  partial_page_count unchanged

fresh whole page commit -> page remains partial:
  partial_page_count + 1
```

因此 `partial_page_count >= hard_limit` 时，planner 可以消费已有 partial
或写满 fresh page，但不能提交会新增 partial node 的 plan；如果无法满足当前
batch，返回 space exhausted / backpressure，由上层等待更多 batch 合并、
reclaim 或 compaction。

### Continuous Slot Lookup Index

`partial_pages` 不能只是一张 `page_delta -> partial_page_meta` 表。该表只支持
按地址更新：

```text
reclaim / abort / recovery:
  page_lba + byte range -> direct metadata update
```

分配时还需要快速回答：

```text
find a partial page whose largest_free_run_quantums >= need_quantums
```

因此 sparse group 必须维护一个派生 selection index。truth 仍然只有一份；
index 不复制 `partial_page_meta`。

#### Index Layout

当前纯内存方案固定为三层结构：

```cpp
struct partial_page_node {
    uint32_t page_delta;
    uint16_t free_quantum_count;
    uint16_t largest_free_run_quantums;
    uint64_t free_quantum_bits; // for 4 KiB LBA / 64 B quantum

    uint32_t bucket_prev; // node_id, invalid = none
    uint32_t bucket_next; // node_id, invalid = none
};

struct sparse_partial_repr {
    // Address lookup; value is node_id into a compact arena.
    absl::flat_hash_map<uint32_t, uint32_t> by_page_delta;
    node_arena<partial_page_node> nodes;

    // Selection index. Bucket k contains pages whose current
    // largest_free_run_quantums == k.
    intrusive_node_list pages_by_largest_run[quantums_per_lba];
    uint64_t nonempty_run_bucket_mask;
};

struct group_state {
    uint64_t base_lba;
    uint32_t lba_count;
    sparse_partial_repr partial_pages;
    uint32_t partial_page_count;

    // A group may be present in multiple global run buckets at the same
    // time, so global membership needs one link per run bucket, not one
    // generic prev/next pair.
    intrusive_group_link global_run_links[quantums_per_lba];
};

struct value_space_manager {
    intrusive_group_list groups_by_largest_run[quantums_per_lba];
    uint64_t nonempty_group_run_bucket_mask;
};
```

`quantums_per_lba` 当前典型为 64，因此 `nonempty_run_bucket_mask`
可以用一个 `uint64_t` 表示。partial record 不保存 all-free page，因此
有效 bucket 是 `[1, quantums_per_lba)`；`largest_run == quantums_per_lba`
时必须删除 partial record 并插入 whole-free span。所有 mask 操作必须走
helper，禁止直接写 `1ULL << quantums_per_lba` 这类在 64-bit 上 UB 的表达式。

bucket key 必须是 `largest_free_run_quantums`，不是 `free_quantum_count`：

```text
page A free bits = 111000111000
free_count = 6
largest_run = 3

page B free bits = 111111000000
free_count = 6
largest_run = 6
```

请求 6 个连续 quantum 时只有 B 可用。按 free_count 分桶会把 A/B 混在一起，
导致反复验证和跳过碎片页。

#### Claim Algorithm

`need_quantums` 必须满足：

```text
1 <= need_quantums < quantums_per_lba
```

LBA-equal / multi-LBA allocation 不走 partial index，直接走
`global_free_extents`。

查找流程：

```text
1. cand = nonempty_group_run_bucket_mask & run_buckets_ge(need_quantums)
   if cand == 0:
       no partial page can satisfy need_quantums

2. k = first_set_bucket(cand)
   group = groups_by_largest_run[k].front()

3. page = group.partial_pages.pages_by_largest_run[k].front()
   invariant: page.largest_free_run_quantums == k >= need_quantums

4. pos = choose_claim_offset(page.free_quantum_bits, need_quantums, k)
   invariant: pos + need_quantums <= quantums_per_lba

5. page.free_quantum_bits &= ~quantum_range_mask(pos, need_quantums)
   page.free_quantum_count -= need_quantums
   new_k = recompute_largest_run(page.free_quantum_bits)

6. Move the page from bucket k to bucket new_k, or remove it from
   partial_pages if new_k == 0. If any group bucket becomes empty/non-empty,
   update groups_by_largest_run and nonempty_group_run_bucket_mask.
```

`choose_claim_offset` 有两个合法策略：

1. `page-local best-fit`（默认）: choose the shortest actual free run whose
   length is `>= need_quantums`; if ties, take the lowest offset. This matches
   the page-local allocator contract and avoids consuming a larger run when the
   same page still has a smaller fitting hole.
2. `largest-run-leftmost`: choose the leftmost start of an actual largest run
   of length `k`. This is a legal low-CPU placement policy, but not the default,
   because it can cut a `k`-sized run even when a smaller fitting run exists in
   the same page.

`page-local best-fit` must operate on actual run starts, not just
`run_start_mask(bits, r)`, because `run_start_mask(bits, r)` also matches starts
whose run length is greater than `r`. One valid helper shape is:

```text
actual_run_start_mask(bits):
  bits & ~(bits << 1) & valid_quantum_mask

exact_run_start_mask(bits, r):
  starts = actual_run_start_mask(bits)
  ge_r = starts & run_start_mask(bits, r)
  if r == quantums_per_lba:
      return ge_r
  return ge_r & ~run_start_mask(bits, r + 1)

choose_claim_offset(bits, need_quantums, k):
  for r in [need_quantums, k]:
      exact = exact_run_start_mask(bits, r)
      if exact != 0:
          return ctz(exact)
```

The extra bit work is a bounded page-local constant (`quantums_per_lba <= 64`)
and is negligible next to hash lookup, bucket movement, and intrusive-list
maintenance. The group/page bucket selection still provides page-level best-fit
by choosing the smallest non-empty `largest_run >= need_quantums`; the offset
policy then applies best-fit inside that selected page without scanning other
pages.

#### Release Algorithm

Release / abort / recovery update by address, then repair the selection index:

```text
1. group_id = (page_lba - data_area_base_lba) / group_size_lbas
   page_delta = page_lba - group.base_lba

2. node = group.by_page_delta.find(page_delta)
   if not found:
       create node with free_quantum_bits = 0
       old_k = 0
   else:
       old_k = node.largest_free_run_quantums

3. node.free_quantum_bits |= quantum_range_mask(offset_quantum, len_quantums)
   node.free_quantum_count += len_quantums
   new_k = recompute_largest_run(node.free_quantum_bits)

4. if node.free_quantum_count == quantums_per_lba:
       remove node from partial_pages
       insert [page_lba, page_lba + 1) into global_free_extents
   else:
       move node from old_k bucket to new_k bucket if old_k != new_k
```

`page_delta` is an LBA delta, not a byte offset; it must not be shifted by
`log2(lba_size)`.

#### Bit Helpers

All bit helpers are fixed-width and must avoid undefined shifts:

```text
quantum_range_mask(pos, len):
  if len == 64: require pos == 0; return UINT64_MAX
  return ((1ULL << len) - 1) << pos

run_buckets_ge(need):
  // valid for 1 <= need < quantums_per_lba <= 64
  return ~((1ULL << need) - 1) & valid_partial_run_bucket_mask

run_start_mask(bits, len):
  m = bits
  span = 1
  while span < len:
      step = min(span, len - span)
      m &= (m >> step)
      span += step
  return m
```

For `quantums_per_lba == 64`, `run_start_mask(bits, len)` takes at most
six shift/and steps for any legal `len`. `recompute_largest_run(bits)` must
use a fixed bounded procedure, such as a six-probe binary search over run
lengths using `run_start_mask(bits, probe)`. A data-dependent linear scan over
all 64 quantums is not allowed in the production hot path.

#### Implementation Constraints

| 项 | 必须 | 不允许 |
|---|---|---|
| Whole-free extent map | `absl::btree_map<uint64_t, uint32_t>` keyed by `base_lba` | per-page vector, unordered map, per-group split truth |
| Group directory | sparse `absl::flat_hash_map<uint32_t, group_state>` or pointer vector with only lightweight headers preallocated | preallocating per-group partial bitmap / dense metadata |
| Bucket container | intrusive doubly-linked list with O(1) insert/remove by handle | `std::vector<page*>` with O(n) erase |
| Partial node storage | arena + `uint32_t node_id` handle | one heap allocation per partial page |
| Sparse address index | `absl::flat_hash_map<uint32_t, uint32_t>` (`page_delta -> node_id`) | `std::unordered_map<paddr, ...>` with full paddr key and node allocation |
| Largest-run recompute | fixed bounded bit helper | data-dependent full bitmap scan in the hot path |
| Reclaim batch | group / page ordered processing when caller can batch | intentionally scattered release order |

#### Cost / Memory Budget

The following numbers are design budgets, not benchmark results. They must be
validated once the implementation exists.

For 4 KiB LBA / 64 B quantum, one partial node should stay near 24-32 B
excluding hash-table control bytes:

```text
page_delta          4 B
free_count/run      4 B
free_quantum_bits   8 B
bucket prev/next    8 B
padding/flags       implementation-dependent
```

For a 1 TiB value area with 256 MiB groups, group-level bucket heads and
global membership links are only a few MiB; the dominant term is still the
number of partial pages:

```text
1 M partial pages     ~ 32-40 MiB RSS target
10 M partial pages    ~ 300-400 MiB RSS target
100 M partial pages   ~ 3-4 GiB RSS target
```

这样 `by_page_delta` 负责按地址更新，`pages_by_largest_run` /
`groups_by_largest_run` 负责按连续空间需求选择。两者职责分离，metadata
只保留一份。

## Batch Allocation 策略

value 写入是 batch 处理，因此分配策略必须是 batch-first，而不是每个 value 单独贪心找 byte range。主目标不是“盲目整页优先”，而是成本模型：

```text
目标 1: 最小化本 batch 触发的 NVMe I/O page 数
目标 2: 在目标 1 相同的前提下，最小化新增 whole page 数
目标 3: 在前两者相同的前提下，最小化 leftover quantums / fragmentation
```

这等价于带成本的 mixed-size bin packing。manager 把 batch 中不同 allocation class 的 values 转成 quantum length，把可用 page candidate 看成 bins，按字典序 cost 选择：

```text
cost = (read_pages, write_pages, new_pages, leftover_quantums)

cached partial page:
  (0, 1, 0, leftover)

new whole page:
  (0, 1, 1, leftover)

non-resident partial page:
  (1, 1, 0, leftover)
```

normal mode 下，non-resident partial page 因为需要 prefill read，不参与主分配；
cached partial page 仍然参与，因为它预期不增加 read I/O。实际 frame 是否仍
resident 由 scheduler/cache layer 在执行 plan 时 pin/take 验证；验证失败时
abort 本次 cached claim 并重新选择候选。

### Allocation Mode Selection

allocation mode 仍然只有三档：

```text
normal
reuse_pressure
hard_pressure
```

mode selection 有两个输入维度：

1. `space_mode`：由 whole-free LBA 水位决定。这里只统计 whole-free LBA，
   不把 partial page 内 free quantums 计入 free ratio；pressure 的意义是
   whole pages 不够时，才用额外 read I/O 换空间复用。
2. `partial_mode`：由 `partial_page_count` 与 partial metadata budget
   决定；它只提升 allocation mode，不引入新策略。

```text
free_ratio = whole_free_lba_count / value_usable_lba_count
```

`space_mode` 固定三档：

```text
normal:
  free_ratio >= normal_low_watermark

reuse_pressure:
  hard_low_watermark <= free_ratio < normal_low_watermark

hard_pressure:
  free_ratio < hard_low_watermark
  or new whole page allocation failed for this batch
```

默认水位：

```text
normal_low_watermark  = 15%
normal_high_watermark = 20%
hard_low_watermark    = 3%
hard_high_watermark   = 5%
```

额外规则：

```text
if new whole page allocation fails:
  force hard_pressure for the current round
```

`partial_mode` 固定映射：

```text
normal:
  partial_page_count < partial_metadata_soft_limit_pages

reuse_pressure:
  partial_metadata_soft_limit_pages
    <= partial_page_count
    < partial_metadata_hard_limit_pages

hard_pressure:
  partial_page_count >= partial_metadata_hard_limit_pages
```

最终模式：

```text
effective_mode = max(space_mode, partial_mode)
```

例如 whole-free ratio 仍处于 normal，但 partial metadata 已经超过 soft
limit，则本 batch 按 `reuse_pressure` 策略规划；超过 hard limit 则按
`hard_pressure` 策略规划，并启用 projected partial count hard gate。

水位必须使用 hysteresis，避免模式抖动：

```text
normal -> reuse_pressure:
  free_ratio < normal_low_watermark

reuse_pressure -> normal:
  free_ratio > normal_high_watermark

reuse_pressure -> hard_pressure:
  free_ratio < hard_low_watermark

hard_pressure -> reuse_pressure:
  free_ratio > hard_high_watermark
```

分配模式语义：

```text
normal:
  candidates = cached partial + new whole pages
  non-resident partial 禁用
  cost = (read_pages, write_pages, new_pages, leftover_quantums)

reuse_pressure:
  candidates = cached partial + new whole pages + selected non-resident partial
  non-resident partial 受 read budget 限制
  cost = (write_pages, new_pages, read_pages, leftover_quantums)

hard_pressure:
  candidates = cached partial + non-resident partial + new whole pages
  优先减少 new_pages，接受更多 prefill reads
  cost = (new_pages, write_pages, read_pages, leftover_quantums)
```

固定约束：

1. `reuse_pressure` 不允许为了减少 `new_pages` 增加 `write_pages`；它只在 write page 数不变时，用有限 prefill reads 换更少新增页面。
2. `hard_pressure` 允许在 hard cap 内增加 read/write pages 来减少 `new_pages`。
3. 三档 cost tuple 是设计契约，不允许由运行时配置改成任意加权分数。
4. partial metadata pressure 只能提升 `effective_mode`，不能改变三档策略；
   `partial_page_count >= hard_limit` 时额外检查 projected
   `partial_page_count` 不增加。

`reuse_pressure` 必须有明确 read budget，防止一次 batch 为了少分少量新页读入大量冷 partial：

```text
reuse_pressure_max_prefill_reads_per_batch = 16 pages
reuse_pressure_max_prefill_reads_per_class = 4 pages
```

`hard_pressure` 仍需保留 hard cap，避免单 batch 无限读旧页；超过 hard cap 后如果仍无法满足 allocation，返回 space exhausted / backpressure，由上层决定等待 reclaim / compaction / fail-fast。

默认 hard cap：

```text
hard_pressure_max_prefill_reads_per_batch = 128 pages
hard_pressure_max_prefill_reads_per_class = 32 pages
```

调用方先把每条 value 映射到 allocation class 和 quantum length：

```text
entry -> { class_idx, alloc_bytes, alloc_quantums }
```

固定分配流程：

1. 按 allocation size descending 排序 batch entries（best-fit decreasing）。
2. 从 `cached_partial_index` 收集 cached candidates。
3. 对 cached candidates 做 best-fit：只考虑
   `largest_free_run_quantums >= alloc_quantums` 的 page，优先使用
   placement 后 leftover 最小的 page。
4. 对剩余需求用 new whole pages 混合 packing；一个新页可以容纳多个 allocation class。
5. 只有在空间压力模式下，才把 non-resident partial pages 纳入 candidate 集合。
6. 生成 allocation plan 后立即 reserve/claim page-local quantum ranges，避免同 batch 内重复分配。

对外 contract：

```text
allocate_batch(...)
  可以内部对 reqs 做 BFD / best-fit 规划
  但返回 claims 必须恢复为与 reqs 相同的顺序
```

candidate 来源语义：

1. `cached_partial_index`
   - manager 内部维护的 page address 索引，表示该 partial page 当前预期在
     value page cache / active tail / clean allocatable frame 中 resident。
   - 索引不是 frame owner；只有 metadata claim 成功并且 scheduler 后续
     pin/take frame 成功后，才可真正写入。
2. new whole pages from `global_free_extents`
   - 在 cached candidates 之后满足剩余需求，直接构造新页像，零读。
3. non-resident partial pages
   - 只在 `reuse_pressure` / `hard_pressure` 或显式 maintenance 策略下使用。
   - 需要 DMA frame + NVMe prefill read，最低优先级。

大 batch 不应为了填碎片扫描大量 partial page。固定策略是按成本模型限制 candidate 数量：

```text
cached candidates:
  参与正常分配，但每 batch 有 max_candidate_pages 上限

new whole pages:
  在 cached candidates 之后满足剩余需求

non-resident partial:
  normal mode 禁用
  reuse_pressure 按 read budget + adjusted cost 参与
  hard_pressure 按 hard cap + adjusted cost 参与
```

默认扫描上限：

```text
max_candidate_pages = 64
```

这保证热路径在碎片压力不大时优先减少 I/O，同时在同等 I/O 成本下尽量复用 cached free ranges，减少新增页面。

### 分配示例

假设：

```text
LBA = 4096 B
allocation quantum = 512 B
batch:
  3 个 1 KiB value  -> 每个 2 quantums
  2 个 512 B value  -> 每个 1 quantum
cached partial page R 有 2 个连续 free quantums
```

方案比较：

```text
方案 A: 直接新分配 1 页
  page N1: 3 * 1 KiB + 2 * 512 B = 4096 B
  cost = (0 reads, 1 write, 1 new_page)

方案 B: 先用 cached partial page R，再新分配 1 页
  page R: 1 * 1 KiB
  page N1: 2 * 1 KiB + 2 * 512 B = 3072 B
  cost = (0 reads, 2 writes, 1 new_page)
```

方案 A write pages 更少，因此选择 A。mixed-size page-local allocator 允许一个新页同时容纳多个 class，避免“每个 class 各开 tail page”。

如果只有 non-resident partial pages：

```text
page P1: 4 free quantums, not resident
page P2: 4 free quantums, not resident

claim P1 + P2:
  cost = (2 reads, 2 writes, 0 new_pages)

new pages:
  cost = (0 reads, 1 write, 1 new_page)
```

normal / reuse_pressure 都选择 new pages：normal 因为 read pages 更少，reuse_pressure 因为 non-resident partial 会增加 write pages；`hard_pressure` 可以在 hard cap 内优先减少 new pages，选择 non-resident partial。

## DMA Frame / Cache 边界

所有 page cache 均按 DMA frame 设计。当前代码即使还没有完全切到
DMA pool，本机制也必须以 DMA frame 为唯一页像抽象；但
`value_space_manager` 不拥有这些 frame，不保存 `value_page_frame*`，
也不 pin / unpin / evict frame。

```text
logical truth:
  value_space_manager

resident page image:
  DMA frame/cache layer

cached partial page index:
  value_space_manager 内部的地址级派生索引
```

`cached_partial_index` 是 manager 的派生索引，用来回答：

```text
哪些 partial page 当前预期已有 resident frame，可以优先尝试零读复用？
```

它只保存：

```cpp
struct cached_partial_entry {
    paddr page_base;
    uint16_t free_quantum_count;
    uint16_t largest_free_run_quantums;
    uint64_t heat_seq;     // larger means newer / hotter
    uint64_t cache_epoch;  // residency token supplied by cache layer
};
```

`free_quantum_count` / `largest_free_run_quantums` 是 `partial_page_node`
truth 的摘要副本，用于 O(1) candidate filtering。索引条目不是新的
allocator truth；claim / release / abort / recovery 的真相仍是
`partial_pages` 的 node 和 selection buckets。

推荐实现：

```cpp
absl::btree_map<paddr, cached_partial_entry> cached_partial_index_by_page;
absl::btree_multimap<cached_partial_score, paddr> cached_partial_by_score;
```

`cached_partial_index_by_page` 负责按地址删除 / 更新 stale entry；
`cached_partial_by_score` 负责按 active-tail / largest-run / leftover /
heat 顺序挑候选。也可以用等价 intrusive indexed structure，但必须同时支持
按 page O(log n) 删除和按 score 有界扫描；禁止只用无序 map 后每个 batch
全表扫描 cached candidates。

### Cached Partial Admission

以下事件可以让 page 进入 `cached_partial_index`：

1. `active_tail` / fresh whole page 写完后仍有 free quantums。
2. `clean_allocatable` frame 写完成后仍有 free quantum ranges。
3. `read_value` / `read_page_values` 读出整页，且 manager metadata 显示该
   page 仍是 partial。
4. non-resident partial 被 prefill read，写完后仍有 free ranges。
5. reclaim 命中 resident page，更新后该 page 仍 partial。

admission 规则不随 pressure mode 改变：只要 page 逻辑上 partial，且
cache layer 告诉 manager 当前存在 resident frame，就可以进入索引。
pressure mode 只影响 selection，即当前 batch 如何消费 cached / non-resident
candidates。

cache layer 必须在以下事件通知 manager 删除或更新索引：

1. clean frame eviction / drain。
2. cached partial 被成功 claim 并转 dirty/open。
3. writeback completion 后 page 变 full 或 all-free。
4. reclaim / abort 改变该 page 的 free summary。
5. whole-page reclaim / trim withheld 清掉该 page identity。

### Cached Partial Budget

cached partial 预算是 runtime 配置项，不是 format 参数，不写 superblock。
启动命令未来可以来自配置文件；不允许影响 recovery 语义。

默认值：

```text
value_cached_partial_budget_bytes = 256 MiB
```

预算含义：

1. 只限制作为 cached partial candidate 保留的 DMA frame 总量。
2. 这是 write-reuse cache 的专用预算；readonly value read cache 不计入，也不受此预算限制。
3. `active_tail_pages` 不计入该预算；其数量由 scheduler 的 active tail cap 固定约束，默认 `active_tail_pages_cap = 8`。
4. `cached_partial_index` 当前只受 global budget 约束；**不做 per-size soft quota**。
5. 单一 size workload（例如 YCSB）会自然把这块预算“挤”成更适合该 workload 的页面形态；这是正常结果，不需要也不允许写 benchmark-specific 特判。
6. 如需偏向当前主导请求形态，只允许增加通用 runtime bias（例如按最近窗口的主导 `alloc_quantums` 轻微偏向 admission / retention），不允许写死 YCSB 特判。
7. 超预算时从 `cached_partial_index` 移除候选；移除不能丢失
   `partial_pages` 中的 page-local allocator truth。
8. frame 是否真正 evict / 释放 DMA 内存由 cache layer 决定，不由 `value_space_manager` 决定。

### Cached Partial Selection

selection 与三档分配策略强相关：

```text
normal:
  consume cached partial candidates + new whole pages
  do not read non-resident partial pages

reuse_pressure:
  consume cached partial candidates first
  then add non-resident partial pages within read budget

hard_pressure:
  aggressively consume cached + non-resident partial pages within hard cap
```

cached candidates 内部使用 best-fit ordering：

```text
1. active_tail / recently written pages first
2. pages that can satisfy the current alloc_quantums
3. smallest leftover_quantums after placement
4. recency / cache heat as tie-breaker
```

`free_quantum_count` 和 `largest_free_run_quantums` 不是 on-disk value
page 字段；它们用于 O(1) candidate filtering / bucket selection，invariant
是它们与 `partial_page_node.free_quantum_bits` 一致。

claim cached page 的流程：

```text
1. value_space_manager 从 cached_partial_index 选 page
2. value_space_manager.try_claim_range(page_lba, alloc_quantums)
3. 返回 byte_claim { src=cached_partial, page_lba, byte_offset, cache_epoch }
4. scheduler/cache layer 用 page_lba + cache_epoch pin/take frame
5. pin/take 成功：frame 转 dirty/open，按 byte_offset / alloc_bytes 写入
6. pin/take 失败：abort 本 round 对该 page 的 claim，删除 stale index，
   重新规划或退回 new/non-resident candidate
```

关键约束：

1. DMA frame 不是空间 truth；它只是页像和热路径 hint。
2. frame eviction 不能丢 page-local allocator truth；只从
   `cached_partial_index` 移除候选。
3. 不能为了复用冷 partial ranges 大量物化 DMA frame。
4. 新 whole page 分配只需要一个 DMA frame 来构造新页像，不需要读旧内容。
5. `cached_partial_index` 允许 stale 条目存在一小段时间，但 stale 条目
   不能绕过 pin/take 验证；验证失败必须释放本次 claim delta。

## Reservation / Rollback

space manager 需要的是 **per-round reservation handle**，不是“恢复整对象旧状态”的 ACID transaction。原因是 value owner 可能同时持有多个 inflight round；后分配的 round 可以先完成，先分配的 round 之后才 abort。如果 abort 直接 restore 某个 page 的 old metadata，会把后续 round 已提交的 claim 一起抹掉。

例子：

```text
page P 初始 free_quantum_bits = 11111111

round A claim [0,2) -> 当前 bits = 00111111
round B claim [2,4) -> 当前 bits = 00001111
round B commit
round A abort
```

正确结果应为：

```text
page P free_quantum_bits = 11001111
```

也就是只释放 round A 自己 claim 的 `[0,2)`；不能把 page 直接恢复成 `11111111`。

因此 rollback 语义应是：

```text
abort(round) = release this round's own reservations into current state
```

而不是：

```text
abort(round) = restore objects touched by this round to their old snapshot
```

也就是说，`abort(round)` 不是另一套 allocator 语义；它只是 **unpublished claims 的释放入口**。和 `release_values(value_ref[])` 的区别只在输入对象不同：

```text
release_values:
  input = published value_ref[]

abort(round):
  input = unpublished byte_claim[] + reserved_extent[]
```

两者共享同一套底层 free-space 更新 primitive：

```text
free claimed byte range back to page-local allocator
if page becomes all-free -> convert to whole-free span
return fresh extent back to global_free_extents
```

对 fresh whole page / extent 需要额外区分：它从 `global_free_extents` 中摘出
后，在 round commit 前不能把 leftover tail partial 发布给其他 round。因此
fresh source 的 round-local 状态分成两类：

1. `reserved_extent`: 已经从 `global_free_extents` 预留的 whole-free LBA range。
   abort 时整段放回 `global_free_extents`，不逐个 replay 其中的
   `byte_claim`。
2. `pending_tail_page`: fresh page 内同 round packing 后剩余的 partial free
   bitmap。commit 时如果 page 仍 partial，把它插入 `partial_pages`；abort
   时直接丢弃，因为对应 `reserved_extent` 会整体回到 whole-free。

推荐结构：

```cpp
struct reserved_extent {
    paddr base;
    uint32_t len_lbas;
};

struct pending_tail_page {
    paddr page_base;
    small_bitset free_quantum_bits;
    uint16_t free_quantum_count;
    uint16_t largest_free_run_quantums;
};

class value_space_round {
public:
    std::vector<byte_claim> claims;
    std::vector<reserved_extent> fresh_extents;
    std::vector<pending_tail_page> unpublished_tails;
};
```

### Public Surface

`value_space_manager` 的 public surface 是 `value_alloc_sched` 内部同步调用
的 metadata API，不是 PUMP sender。公开 API 不接收 PUMP req，不保存 /
返回 `value_page_frame*`，也不提交 NVMe I/O。

```cpp
class value_space_manager {
public:
    // Owner-local reservation/reconcile message. Tree must wait for an
    // accepted result before using the newly covered floor interval.
    alloc_floor_sync_result
    sync_alloc_floor(uint64_t new_alloc_floor_lba,
                     std::span<const free_extent> tree_returned_extents);

    value_space_round begin_round();

    byte_claim try_claim_range(value_space_round& round,
                               paddr page_base,
                               uint32_t alloc_quantums);

    void mark_cached_partial(cached_partial_update update);
    void erase_cached_partial(paddr page_base, uint64_t cache_epoch);

    std::vector<byte_claim> allocate_batch(value_space_round& round,
                                           std::span<const allocation_request> reqs,
                                           allocation_policy policy);

    void commit(value_space_round&& round);
    void abort(value_space_round&& round);

    void release_values(std::span<const value_ref> refs);

    trim_plan prepare_trim(uint32_t max_ranges, uint32_t max_lbas);
    void complete_trim(trim_plan_id id, bool ok);

    void install_recovered_state(
        std::span<const live_value_extent> live_extents,
        uint64_t tree_alloc_head_lba,
        uint64_t data_area_end_lba,
        std::span<const dead_class_hint> hints);
};
```

接口语义：

1. `sync_alloc_floor` 是 tree/value 共享 Data Area 的 floor reservation
   入口；返回 `accepted` 前 tree 不能使用新 floor 区间，返回
   `rejected_collision` 时 tree 必须 retry/backpressure。
2. `allocate_batch` 返回 logical claims，返回向量顺序必须与输入 `reqs`
   一致；内部可以重排做 planner。
3. `try_claim_range` 是对指定 partial page 的 owner-local claim primitive，
   主要供 cached candidate 执行 / stale miss retry / 单元测试使用。
4. `mark_cached_partial` / `erase_cached_partial` 只维护 cached partial
   地址级索引；实际 frame pin、take、dirty/open、evict 仍归
   scheduler/cache layer。
5. `release_values` 只接收 caller 已证明 dead 的 `value_ref`，不做 tree
   liveness 防御。
6. `prepare_trim` 只返回 trim plan 并 withheld 对应 free ranges；NVMe trim
   submit 和 completion 由 scheduler 推进，再调用 `complete_trim`。
7. `install_recovered_state` 从 tree/WAL 导出的 live refs 重建 free-space
   truth；不读取 Value Area payload，也不恢复 cached residency。

round 语义：

1. claim 后对应 quantum range 立即从当前 allocatable state 中移除，避免后续 round 重复分配。
2. later round 可以在同一 page 的剩余 free ranges 上继续 claim；因此 abort 只能释放本 round 自己的 claim delta。
3. 对已经存在的 partial page，abort 相当于把本 round claim 的 byte range 当作“未发布 allocation 的 release”重新加回当前 page-local allocator state。
4. 对从 `global_free_extents` 新拿到的 fresh page / extent，abort 直接把
   `fresh_extents` 整段还回 `global_free_extents`；这些 extent 内的
   `byte_claim` 不再逐个走 partial release，因为 fresh page 的 tail metadata
   从未对其他 round 发布。
5. fresh page 的 leftover free ranges 只在 commit 时由 `unpublished_tails`
   发布到 `partial_pages`；abort 直接丢弃 `unpublished_tails`，不需要撤销
   一个已经对外可见的 tail partial。
6. commit 后 round 的 claims 成为 live allocations；round handle 随即销毁。

由于 `value_alloc_sched` 是单线程 owner，round 操作不需要跨线程锁；但 API 层应保持纯 metadata，方便单元测试和 out-of-order round completion 验证。

## Reclaim

回收接口以 batch 输入为主：

```cpp
void release_values(std::span<const value_ref> refs);
```

manager 内部按 `page_lba` 聚合：

```text
sub-LBA:
  offset_quantum = value_ref.byte_offset / value_space_quantum_bytes
  len_quantums   = canonical_class_size_for_len(value_ref.len)
                   / value_space_quantum_bytes
  mark [offset_quantum, offset_quantum + len_quantums) free
  all-free -> erase partial, insert 1-LBA free span

LBA-equal / multi-LBA:
  insert free span [base_lba, base_lba + span_lbas)
  coalesce with neighbors
```

这里的 correctness precondition 仍然由调用方保证：传入的 `value_ref` 必须对 tree + outstanding read handles 都 dead。`value_space_manager` 不扫 tree 做 liveness 防御。

## TRIM

逻辑 free 与物理 TRIM 解耦。

释放后空间立即进入 logical free state，可被重用。Value 侧只对 **truly whole-free** 的 range 暴露 TRIM 资格；正常复用优先，TRIM 只是后台硬件层 maintenance。

当前设计只冻结 TRIM 的资格与 ownership，不冻结触发时机 / cadence。何时发 TRIM、多久发一次、是否按维护窗口批量发，留到 tree / reclaim / 硬件层整体完成后再统一拍板。

占位接口保留为：

```cpp
trim_plan prepare_trim(uint32_t max_ranges, uint32_t max_lbas);
void complete_trim(trim_plan_id id, bool ok);
```

约束：

```text
1. 只有 `global_free_extents` 中且 `base_lba >= acknowledged_alloc_floor_lba` 的 whole-free ranges 有资格进入 trim
2. partial page / page-local free ranges 不直接 trim
3. prepare_trim 选中的 range 从 allocatable free truth 中摘出，进入 trim_inflight/withheld
4. complete_trim(ok=true/false) 后 range 回到 global_free_extents
```

TRIM inflight 期间这些 LBA 必须 withheld，不能重新分配。完成或失败后回到 free。这样避免 TRIM 与 write 同 LBA 竞态，同时又不要求当前阶段就拍死 maintenance cadence。

## Recovery

当前阶段不展开 recovery rebuild 的具体实现算法；这里只论证该设计与 Inconel recovery 策略兼容，尤其是 **recovery 不需要读取 Value Area**。

recovery 侧只需要把 occupied truth 交给 manager：

```cpp
void install_recovered_state(std::span<const live_value_extent> live_extents,
                             uint64_t tree_alloc_head_lba,
                             uint64_t data_area_end_lba,
                             std::span<const dead_class_hint> hints);
```

兼容性论证：

1. `live_value_refs` 展开后的 `occupied` 是唯一 correctness source。
2. `dead_class_hints` 只用于 class 归桶 / TRIM hint；丢失 hints 不能导致 free 空间泄漏。
3. `[tree_alloc_head_lba, data_area_end_lba)` 减去 occupied 后得到 whole-free truth。
4. sub-LBA 页内有 live values 时，从 live `byte_offset + len` 以及 format profile 固定的 `canonical_class_idx_for_len(len)` 反推出 occupied quantum ranges，再得到 partial free ranges。
5. 没有 live values 的 whole-free page / extent 必须进入 free space，即使没有任何 dead hint。

因此 recovery 不需要读 Value Area 的原因是：

1. tree/WAL recovery 已经产出 live `value_ref { base, byte_offset, len }` 集合。
2. format profile 保证 `len -> canonical_class_idx_for_len(len)` 是唯一且稳定的，因此 `value_ref` 的 `byte_offset + len` 足够重建 occupied quantum ranges。
3. `global_free_extents` 是对整个 value-allocatable 区间做 `free = range - occupied` 的补集结果，不依赖 Value Area 上是否残留旧 bytes。
4. partial page metadata 是 runtime state，不是 on-disk source truth；它应由 `live_value_refs` 推导，而不是反向扫描旧 value page。
5. 因此扫描 Value Area 只会读取 stale/garbage bytes，不能提供比 `live_value_refs` 更多的 correctness 信息；按设计应视为多余工作。

结论：

```text
recovery input:
  tree / WAL 导出的 live_value_refs (+ optional dead hints)

recovery does not read:
  Value Area payload pages
```

只要 `value_space_manager` 能从 `live_value_refs` 重建 `global_free_extents + group partial metadata`，它就满足设计文档要求。

`cached_partial_index` 不参与 recovery。boot 后它从 empty 开始，由后续
writeback completion、read cache fill、prefill read 和 reclaim 命中 resident
page 等 runtime 事件逐步填充。recovery 不恢复“哪些 partial page 当时 cached”。

## 独立类边界

固定代码边界：

```text
apps/inconel/value/space_manager.hh
apps/inconel/value/space_manager.cc    // 如实现不适合 header-only
```

公开类型不包含 PUMP req、DMA frame、nvme descriptor：

```cpp
struct space_class {
    uint16_t class_idx;
    uint32_t class_size;
    uint32_t span_lbas;
    uint16_t alloc_quantums;
};

struct allocation_request {
    uint32_t entry_index;
    uint16_t class_idx;
    uint32_t alloc_bytes;
    uint32_t alloc_quantums;
};

enum class cached_partial_kind : uint8_t {
    active_tail,
    cached_free_candidate,
};

struct cached_partial_update {
    paddr page_base;
    cached_partial_kind kind;
    uint64_t heat_seq; // larger means newer / hotter
    uint64_t cache_epoch;
};

struct byte_claim {
    paddr page_base;
    uint32_t byte_offset;
    uint32_t alloc_bytes;
    uint16_t class_idx;
    uint64_t cache_epoch; // valid only for source::cached_partial
    enum class source : uint8_t {
        cached_partial,
        new_whole_page,
        nonresident_partial,
    } src;
};

struct trim_range {
    uint64_t lba;
    uint32_t len_lbas;
};

struct free_extent {
    uint64_t base_lba;
    uint32_t len_lbas;
};

struct alloc_floor_sync_result {
    enum class status : uint8_t {
        accepted,
        rejected_collision,
    } st;
    uint64_t acknowledged_floor_lba;
};
```

`value_alloc_sched` 只负责把这些 logical claims 转成 DMA frame 操作：

```text
cached_partial claim -> pin/take existing DMA frame by page_base+cache_epoch
                        -> dirty write, or abort claim on stale miss
new_whole_page       -> allocate DMA frame -> build fresh page image
nonresident_partial  -> allocate DMA frame -> NVMe read prefill -> dirty write
```

## 单元测试面

因为 `value_space_manager` 是纯 metadata，可以不启动 runtime / scheduler / mock NVMe 单测。

必须覆盖：

1. 初始单 span，连续分配切 span。
2. 释放相邻 span 后 coalesce。
3. sub-LBA value release 创建 page-local partial metadata。
4. partial page all-free 后转 whole-free span。
5. mixed-size batch allocation 能把不同 class pack 到同一 page。
6. batch allocation 按 cost model 选择 cached partial / new whole page / non-resident partial。
7. cached partial claim 成功后，scheduler pin/take stale miss 会 abort 本次 claim 并删除 stale index。
8. round abort 只释放本 round claim delta，不回滚后续 round 已提交的 claim。
9. round commit 发布正确 fresh-page tail partial ranges。
10. global free span 与 group-local partial metadata 的交互不变量。
11. trim prepare withheld free range，complete 后回 free，失败后可重试。
12. recovery 从 live extents 重建 whole-free 补集，不依赖 dead hints，也不读取 Value Area。
13. tree 提高 floor 后，在 value owner ack 前 tree 不能使用新区间；ack 后即使 `global_free_extents` 中暂时还有 below-floor stale extents，allocation / trim 也必须被 acknowledged floor 挡住，绝不能分配或 trim 到 tree 已接管的 LBA。
14. cached partial admission 覆盖 active tail / clean allocatable / read cache / prefill frame。
15. cached partial budget 超限只移除 candidate，不丢 manager metadata。
16. cached partial selection 按 active_tail 优先、largest_free_run / leftover best-fit、recency tie-breaker。
17. 三档 pressure mode 对 non-resident partial 的启用和 read budget / hard cap。
18. 两个 inflight round 在同一 partial page 上交错 claim，后 round 先 commit、前 round 后 abort，最终状态仍正确。
19. `allocate_batch` 可以内部重排 reqs，但返回 claims 必须恢复为与 reqs 相同的顺序。
20. `canonical_class_idx_for_len(len)` 对 reclaim / recovery 是唯一且稳定的；同一 `value_ref.len` 不允许映射到多个 class。
21. sparse `by_page_delta` 支持 reclaim / abort 按地址直接更新 partial metadata。
22. group-local `pages_by_largest_run` 能快速找到 `largest_free_run_quantums >= need` 的 page，并在 claim / release 后更新 bucket membership。
23. global `groups_by_largest_run` 能避免跨 group 扫描；group bucket 空 / 非空转换时同步更新全局索引。
24. partial metadata soft / hard limit 只提升 effective mode；hard limit 下 projected partial count 不允许增加。
25. bit helper 覆盖 `need=1`、`need=quantums_per_lba-1`、`len=64` mask、all-free 删除 partial record，不能依赖 UB shift。
26. default `page-local best-fit` 至少覆盖一个碎片化示例：同页同时存在 exact/smaller fitting run 和 larger run 时必须选择前者；`largest-run-leftmost` 作为可选 policy 也要覆盖一个 correctness 示例，确认它只是 placement 差异。
27. `cached_partial_update.kind + heat_seq` 驱动 cached selection；manager 不依赖外部输入数组的偶然顺序。
28. tree head 下移时，只有经 `sync_alloc_floor(..., tree_returned_extents)` 显式交回的 spans 才能重新进入 `global_free_extents`。

## 与现有机制的替换关系

该设计完成后，现有状态不应继续作为 source truth：

```text
per_class::whole_pool         -> value_space_manager global_free_extents
trim_pending_pages_           -> value_space_manager trim_inflight
hole_pages_ as truth          -> value_space_manager partial_pages
cached partial candidate list -> value_space_manager cached_partial_index
open/allocatable frame lists  -> DMA frame/cache layer frame state
```

旧 `hole_pages_` 如果保留，只能作为 cache/frame 执行层的临时前端结构，
不能作为无限增长的 correctness metadata，也不能绕过
`value_space_manager.partial_pages`。

## 固定设计决策

1. group size 是 format-time disk 参数，由 format/init 命令指定并写入 superblock；start 命令不接收该参数，只从 superblock 读取。生产默认值固定为 256 MiB，单元测试必须支持小 group 覆盖边界。
2. `global_free_extents` 是 whole-free space 的唯一 owner-local 表示；group 不持有 whole-free free-space truth，跨 core 安全边界由 `published_alloc_floor_lba` request fence + value owner acknowledged floor 提供。
3. 当前纯内存 INC-051 固定为 sparse-only partial metadata；dense partial 不进入本阶段，未来若引入 on-disk group spill / cold metadata cache 再重新评估。
4. partial page 必须支持 mixed-size page-local allocator；不允许把 partial page 固定成单一 class layout。
5. `value_space_quantum_bytes` 是 superblock disk-format 字段，当前支持值固定为 `64B`；sub-LBA class size 必须是 `64B * 2^n`，LBA-equal / multi-LBA class size 必须是 `lba_size * 2^m`。`format_profile` 只能作为格式化新盘时写入 superblock 的默认模板；start / recovery 必须以 superblock 为准，不允许从 class table 动态求 `gcd(...)`。
6. `trim_inflight` 必须 withheld，不允许被前台分配覆盖。
7. high-level batch planner 放在 `value_space_manager` 内，scheduler/cache
   layer 只通过 `mark_cached_partial` / `erase_cached_partial` 维护 cached
   partial index，并执行 returned plan。
8. `value_cached_partial_budget_bytes` 是 runtime 配置项，默认 256 MiB，不写 superblock，不影响 recovery。
9. recovery rebuild 只以 tree/WAL 导出的 live_value_refs 为 truth，不读取 Value Area payload pages。
10. 默认 runtime 限额固定为：`active_tail_pages_cap = 8`、`max_candidate_pages = 64`、`reuse_pressure_max_prefill_reads_per_batch = 16`、`reuse_pressure_max_prefill_reads_per_class = 4`、`hard_pressure_max_prefill_reads_per_batch = 128`、`hard_pressure_max_prefill_reads_per_class = 32`。
11. partial metadata 默认预算固定为：`partial_metadata_soft_limit_pages = 10,000,000`、`partial_metadata_hard_limit_pages = 100,000,000`；这是 runtime policy，不写 superblock。soft limit 把 effective mode 至少提升到 `reuse_pressure`，hard limit 至少提升到 `hard_pressure` 并禁止 projected partial count 增加。
12. `allocate_batch` 对外必须返回与输入 `reqs` 同顺序的 `claims`；内部 planner 可以重排，但不得把 entry-to-claim 映射泄漏给 caller。
13. format profile 必须定义唯一且稳定的 `canonical_class_idx_for_len(len)`；`value_ref.len` 必须足以唯一恢复 allocation class / alloc_quantums。
14. `cached_partial_update` 必须显式携带 `kind`、`heat_seq` 和
   `cache_epoch`；manager 的 selection 不得依赖通知到达数组的偶然顺序。
15. tree/value 边界切换只能通过 `published_alloc_floor_lba` + `sync_alloc_floor(new_floor, tree_returned_extents)` 完成：前者负责发布 reservation request，后者负责 owner-local confirmation / reconcile；tree 必须等 accepted ack 后才能使用新区间。允许 pending 期间短暂 stale metadata，但不允许 ack 后 stale span 被分配或 trim。
16. sparse partial 必须同时维护按地址更新的 truth table 和按 `largest_free_run_quantums` 选择连续空间的派生索引；索引不得复制完整 `partial_page_meta`。
17. partial bucket 容器必须支持 O(1) insert/remove by handle，固定为 intrusive doubly-linked list 或等价结构；禁止用 `std::vector<page*>` 这类 O(n) erase 结构承载 hot bucket。
18. partial node 必须由 arena + `uint32_t` handle 管理；禁止每个 partial page 独立 heap allocation。
19. sparse address index 必须使用紧凑 `page_delta -> node_id` 形态；禁止 `std::unordered_map<paddr, ...>` 这类 full paddr key + per-node allocation 结构。
20. `largest_free_run_quantums` 重算必须使用固定有界 bit helper；生产 hot path 禁止对 64 quantum 做数据相关线性扫描。
21. `choose_claim_offset` 默认使用 `page-local best-fit`；`largest-run-leftmost` 只作为显式配置 / benchmark 证明后的可选 placement policy，不作为默认策略。
