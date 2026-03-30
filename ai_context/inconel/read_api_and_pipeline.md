# Inconel 详细设计：读 API 与 Pipeline

> 依据：`ai_context/inconel/design_overview.md`（唯一概要规范）
>
> 本文细化 GET / MultiGet / Scan 的 read_handle 生命周期、batch cache、hot_blob 返回路径、tree lookup sender 编排、tree tombstone 读语义以及长读资源上界策略。

## 1. 读路径总览

所有读操作共享同一个核心模型：

```text
1. 获取 read_handle = { cat, read_lsn }
2. 在 cat->prs 定义的拓扑下查找
3. memtable 优先于 tree
4. tombstone = not found
5. 操作结束后 read_handle 析构 → 释放 cat 引用
```

| 操作 | 查找范围 | 并发度 | 结果形式 |
|------|---------|--------|---------|
| GET | 单 key → 单 front_sched + tree | 无 | 单条 value 或 not found |
| MultiGet | 多 key → 按 owner 分组 fan-out + tree | per-front 并发 | value list |
| Scan | range → 所有 front_scheds + tree | 全 front 并发 | sorted stream |

## 2. `read_handle` 生命周期

### 2.1 获取

```text
on(coord_sched):
    cat = atomic_load(current_publish_catalog)    // shared_ptr atomic load
    read_lsn = cat->durable_lsn.load(acquire)
    return read_handle { cat, read_lsn }
```

获取是轻量操作：一次 shared_ptr atomic load + 一次 atomic uint64 load。不涉及内存分配。

### 2.2 持有期间

read_handle 通过 `shared_ptr<publish_catalog>` 间接 pin 住：
- `published_read_set`（memtable gens + tree guard）
- 每个 `memtable_gen`（通过 intrusive_ptr）
- `checkpoint_guard`（通过 shared_ptr）
- `tree_manifest`（通过 guard）

持有期间，所有被 pin 住的对象不会被释放：
- memtable gens 的数据不会被清理
- tree guard 保护的旧 slots / old values 不会被 TRIM
- hot_blob 引用链不会断裂

### 2.3 释放

read_handle 析构 → `shared_ptr<publish_catalog>` ref--

如果这是最后一个引用：
```text
cat 析构 → prs 析构 → {
    fronts 析构 → memtable_gen intrusive_ptr ref--
    tree_guard 析构 → checkpoint_guard 析构 → retire list 投递
}
```

**关键**：read_handle 的生命周期直接控制资源回收的延迟。长时间持有 read_handle 会阻止 memtable 释放和 tree reclaim。

### 2.4 PUMP Context 传递

read_handle 在 pipeline 中通过 context 传递：

```cpp
// 获取后 push 到 context
just() >> on(coord_sched, acquire_read_handle())
       >> push_result_to_context()  // read_handle 进入 context stack
       >> /* ... 读操作 ... */
       >> pop_context()             // read_handle 析构，释放 pin
```

整次逻辑读调用共享同一个 read_handle（概要 §8.2）。不允许中途重新获取。

## 3. Batch Cache

### 3.1 定义与来源

batch cache 是 canonicalization 的自然产物——`write_path_and_pipeline.md` §3.2 中 canonicalize 算法产出的 `last_op: flat_hash_map<key, index>` 就是 batch 内每个 key 的 canonical final image。它不是额外构造的数据结构，而是写路径折叠同 key 操作时已经存在的中间 map。

```cpp
// 概念上等价于：
struct batch_cache {
    // canonicalization 产出的 last_op map
    // key → canonical final op (PUT(value) 或 DELETE)
    flat_hash_map<string_view, batch_cache_entry> entries;
};

struct batch_cache_entry {
    enum class kind : uint8_t { value, tombstone } k;
    // kind == value 时：
    const char* value_data;
    uint32_t value_len;
};
```

### 3.2 v1 状态

v1 只有纯 PUT/DELETE，batch 内不存在 read-your-own-writes 场景。canonicalization 的 `last_op` map 在折叠和路由完成后即可释放，不需要放进 `batch_ctx` 或传给读路径。

未来若支持 MERGE/INCREMENT（需要先读当前值再计算新值），只需把 `last_op` 保留在 `batch_ctx` 中，读路径通过 context 访问它即可。不需要引入新的数据结构。

### 3.3 生命周期

```text
创建：canonicalization 时自然产出（last_op map）
v1：  折叠 + 路由完成后释放
未来：保留在 batch_ctx 中，直到 batch pipeline 完成后析构
```

batch cache 不持久化，不参与恢复，不跨 batch 共享。

### 3.4 canonical 一致性

概要 §8.1：在 v1 canonical batch 模型下，batch cache 对同一逻辑 key 暴露的也必须是 canonical final image，不是原始中间步骤。

因为 canonicalization 在 `coord_sched.handle_assign_lsn` 中已完成，batch cache 直接索引 canonical entries 即可。

## 4. Point GET

### 4.1 完整流程

```text
point_get(key)

1. 获取 read_handle = { cat, read_lsn }

2. 如果在 batch 写路径的上下文中（即 batch_ctx 中携带了 batch_cache）：先查 batch cache
   if batch_cache != nullptr && batch_cache.contains(key):
       if entry.kind == value: return value
       if entry.kind == tombstone: return not_found
   // 注：batch_cache 只存在于写路径内部的 read-your-own-writes 场景。
   // 独立的 GET/MultiGet/Scan API 调用不携带 batch_cache。

3. 确定目标 front_sched
   owner = key_hash % front_sched_count

4. 查 memtable（on front_sched[owner]）
   result = lookup_memtable(key, read_lsn, cat->prs->fronts[owner])

5. 处理 memtable 结果：
   match result:
       value_handle(vh):
           // memtable hit → 直接从 hot_blob 返回
           return value_from_hot_blob(vh.hot)
       tombstone:
           // memtable hit tombstone → not found
           return not_found
       miss:
           // memtable miss → 继续查 tree
           goto step 6

6. 查 tree（使用 cat->prs->tree_guard.manifest）
   tree_result = tree_lookup(key, cat->prs->tree_guard->manifest)

7. 处理 tree 结果：
   match tree_result:
       leaf_value_record(data_ver, vr):
           // tree hit value → 从 NVMe 读 value body
           value_data = nvme_read_value(vr)
           return value_data
       leaf_tombstone(data_ver):
           // tree hit tombstone → not found
           return not_found
       absent:
           // tree miss → key 不存在
           return not_found

8. 释放 read_handle
```

### 4.2 PUMP Pipeline

```text
point_get(key)
  = on(coord_sched, acquire_read_handle())
  >> push_result_to_context()
  >> get_context<read_handle>()
  >> then([key](read_handle& rh) {
         uint32_t owner = key_hash(key) % front_sched_count;
         auto& frs = rh.cat->prs->fronts[owner];  // PRS snapshot 的 front_read_set
         return front_sched[owner]->lookup_memtable(key, rh.read_lsn, frs);
     })
  >> flat()
  >> visit()                    // variant<value_handle, tombstone, miss>
  >> then([key](auto&& result) {
         using T = std::decay_t<decltype(result)>;
         if constexpr (std::is_same_v<T, value_handle>) {
             // memtable hit → hot_blob 返回
             return just(value_from_hot_blob(result.hot));
         } else if constexpr (std::is_same_v<T, tombstone_tag>) {
             return just(not_found_result{});
         } else {
             // miss → tree lookup（就地执行，不经过 tree_sched）
             return get_context<read_handle>()
                 >> then([key](read_handle& rh) {
                        return tree_lookup(key, rh.cat->prs->tree_guard->manifest);
                    })
                 >> flat()
                 >> then([](auto&& tree_result) {
                        return process_tree_result(tree_result);
                    });
         }
     })
  >> flat()
  >> pop_context()              // 释放 read_handle
```

**tree_lookup 就地执行**：tree_lookup 不经过 tree_sched，而是在当前 scheduler 上就地执行（见 `runtime_state_machine.md` §4.7）。它只依赖 immutable manifest 和 readonly_frame_cache，不需要 tree_sched 的可变状态。CoW tree 的核心就是读写隔离——读路径不应被 flush/reclaim 阻塞。

### 4.3 Memtable Lookup 实现

dispatch 到 `front_sched[owner]` 执行（保证 btree_map 线程安全），但搜索的是调用方传入的 PRS snapshot 中的 active/imms（概要 §8.1 step 3），不是 front_sched 当前状态：

```text
lookup_memtable(key, read_lsn, frs):
    // frs = cat->prs->fronts[owner]，由 PRS 的 intrusive_ptr 保活

1. 查 frs.active gen：
   在 frs.active.table 中查找 key
   收集所有 data_ver <= read_lsn 的 entries
   winner_active = 其中 data_ver 最大的

2. 如果 winner_active 存在 → 返回（最新在 active，不需要查 imms）

3. 查 frs.imms（从新到旧）：
   for gen in frs.imms:
       在 gen.table 中查找 key
       收集所有 data_ver <= read_lsn 的 entries
       winner_gen = 其中 data_ver 最大的
       if winner_gen 存在 → 返回

4. 全部 miss → 返回 miss
```

**优化**：因为 `same key → same front_sched`，同一 gen 中同一 key 的 entries 来自不同 batch（不同 batch_lsn）。按 data_ver 降序扫描，找到第一条 `data_ver <= read_lsn` 的即为 winner。

### 4.4 hot_blob 返回路径

概要 §8.1 规则 4：memtable hit value 时，必须直接从 `value_handle.hot_blob` 返回，绝不能退化成 SSD 读。

```text
value_from_hot_blob(hot_blob* hb):
    // hb->data 包含完整 value bytes
    // hb->len 包含 value 长度
    // 返回给调用方的可以是：
    //   - 直接引用（如果调用方保证 read_handle 存活期间使用）
    //   - 拷贝（如果需要跨异步边界）
    // 不需要从 SSD 读取
```

**保证链**：
```text
read_handle pin cat → cat.prs pin memtable_gen → gen 持有 memtable_entry
→ entry 持有 value_handle → value_handle 持有 hot_blob (intrusive_ptr)
→ hot_blob.ref_count > 0 → 不被释放
```

### 4.5 Tree Path Value Read

memtable miss 后查 tree，如果命中 value record：

```text
read_value(value_ref vr):
    1. 通过 readonly_frame_cache 获取 value page frame（见 §9.3）
       cache hit → 直接使用 DMA frame（零 NVMe read）
       cache miss → NVMe read 整个 LBA → 回填 cache
    2. 在 frame->dma_buf + vr.byte_offset 处定位 value_object_header
    3. 校验 magic == 0x56414C55
    4. 校验 body_len == vr.len
    5. 校验 body_crc
    6. 返回 value_view { frame, vr.byte_offset, vr.len }（零拷贝）
```

sub-LBA value 和同页其他 value 共享一个 frame。读一个 LBA 可能同时使多个 value 变成 cache resident。

## 5. MultiGet

### 5.1 完整流程

```text
multi_get(keys[])

1. 获取 read_handle（一次，整次调用共享）

2. 按 owner 分组
   groups = group_by(keys, key -> key_hash % front_sched_count)

3. Fan-out 到各 front_scheds（并发）
   for each (owner, group_keys) in groups:
       memtable_results[owner] = front_sched[owner]->batch_lookup(group_keys, read_lsn)

4. 收集 memtable miss 的 keys → tree_miss_keys

5. Tree batch lookup（对 tree_miss_keys）
   tree_results = tree_batch_lookup(tree_miss_keys, manifest)

6. 合并结果
   for each key in keys:
       if memtable_results has value: use memtable result（hot_blob）
       else if tree_results has value: use tree result（NVMe read value）
       else if memtable_results has tombstone: not found
       else if tree_results has tombstone: not found
       else: not found

7. 过滤 tombstone → 只返回 live values

8. 释放 read_handle
```

### 5.2 PUMP Pipeline

```text
multi_get(keys)
  = on(coord_sched, acquire_read_handle())
  >> push_result_to_context()
  >> get_context<read_handle>()
  >> then([keys](read_handle& rh) {
         return group_keys_by_owner(keys);
     })
  >> push_result_to_context()                // groups 进入 context
  // ── Phase 1: memtable fan-out ──
  >> get_context<key_groups, read_handle>()
  >> for_each(groups)
     >> concurrent(front_sched_count)
     >> flat_map([&rh](auto&& group) {
            auto& frs = rh.cat->prs->fronts[group.owner];
            return front_sched[group.owner]->batch_lookup(group.keys, read_lsn, frs);
        })
     >> reduce(merged_memtable_results, merge)
  // ── Phase 2: tree fill misses ──
  >> then([](auto&& memtable_results) {
         auto miss_keys = extract_miss_keys(memtable_results);
         if (miss_keys.empty())
             return just(std::move(memtable_results));
         return tree_batch_lookup(miss_keys, manifest)
             >> then([mr = std::move(memtable_results)](auto&& tree_results) mutable {
                    return merge_results(std::move(mr), std::move(tree_results));
                });
     })
  >> flat()
  // ── Phase 3: 合并与过滤 ──
  >> then([](auto&& merged) {
         return filter_tombstones_and_format(merged);
     })
  >> pop_context()      // groups
  >> pop_context()      // read_handle
```

### 5.3 Memtable Batch Lookup

在单个 front_sched 上批量查找多个 key：

```text
batch_lookup(keys[], read_lsn, front_read_set):
    results = []
    for key in keys:
        result = lookup_memtable(key, read_lsn, front_read_set)
        results.push({ key, result })
    return results
```

这是顺序的（在同一 front_sched 上），但 across front_scheds 是并发的。

### 5.4 Tree Batch Lookup

```text
tree_batch_lookup(keys[], manifest):
    // 按 key 排序（利用 B+ tree 的顺序访问局部性）
    sorted_keys = sort(keys)

    results = []
    for key in sorted_keys:
        result = tree_lookup(key, manifest)
        results.push({ key, result })
    return results
```

优化方向：
- 排序后顺序遍历，叶子节点有缓存局部性
- 可以按 leaf range 分组，批量读取相邻 leaf pages

### 5.5 全局合并规则

概要 §8.2 规则 3-5：

```text
对每个 key：
  1. 先在 owner front_sched 的 memtable 中找 winner
  2. 如果 memtable 命中（value 或 tombstone）→ memtable winner 为准
  3. 如果 memtable miss → 查 tree
  4. winner 为 tombstone → 该 key 不出现在输出中
  5. 不同 front_scheds 不会为同一 key 竞争（same key → same front）
```

## 6. Range Scan

### 6.1 完整流程

```text
range_scan(begin, end)

1. 获取 read_handle（一次，整次调用共享）

2. 并发执行：
   a. 各 front_sched 的 memtable scan
   b. tree range scan

3. Merge：memtable results overlay on tree results

4. 过滤 tombstone → stream 输出
```

### 6.2 PUMP Pipeline

```text
range_scan(begin, end)
  = on(coord_sched, acquire_read_handle())
  >> push_result_to_context()
  // ── 并发收集 ──
  >> when_all(
         // memtable scans（所有 front_scheds 并发，使用 PRS snapshot 的 front_read_set）
         get_context<read_handle>()
           >> then([begin, end](read_handle& rh) {
                  return indexed_front_sched_list(rh);  // [(owner, fs*, frs), ...]
              })
           >> for_each()
           >> concurrent(N)
           >> flat_map([begin, end, read_lsn](auto&& item) {
                  return item.fs->scan_memtable(begin, end, read_lsn, item.frs);
              })
           >> reduce(merged_memtable_scan, merge_sorted),
         // tree scan（就地执行，不经过 tree_sched）
         get_context<read_handle>()
           >> then([begin, end](read_handle& rh) {
                  return tree_scan(begin, end, rh.cat->prs->tree_guard->manifest);
              })
           >> flat()
     )
  // ── K-way merge ──
  >> then([](auto&& when_all_res) {
         auto& memtable_scan = std::get<2>(std::get<0>(when_all_res));
         auto& tree_scan = std::get<2>(std::get<1>(when_all_res));
         return merge_memtable_over_tree(memtable_scan, tree_scan);
     })
  // ── 过滤 tombstone + stream out ──
  >> then([](auto&& merged) {
         return filter_tombstones(merged);
     })
  >> pop_context()
```

### 6.3 Memtable Scan

dispatch 到 front_sched 执行，但搜索 PRS snapshot 的 active/imms（与 lookup_memtable 同理）：

```text
scan_memtable(begin, end, read_lsn, frs):
    // frs = cat->prs->fronts[owner]

1. 在 frs.active 和 frs.imms 中收集 [begin, end) 范围的 entries
   每个 gen 的结果是一个 sorted list

2. 对所有 gen 的结果做 merge：
   - 同一逻辑 key：取 data_ver <= read_lsn 的最大版本
   - 按 key 排序输出

3. 返回 sorted [(key, memtable_entry)] list
```

**注意**：因为 range scan 不像 point lookup 只涉及一个 front_sched，它需要扫描所有 front_scheds。不同 front_scheds 返回的结果不会有 key 冲突（same key → same front），所以全局 merge 只是按 key 交错排列。

### 6.4 Tree Range Scan

```text
tree_scan(begin, end, manifest):

1. 从 root 开始，用 begin 做 B+ tree 下降到第一个可能包含 begin 的 leaf

2. 顺序扫描 leaf records：
   for each leaf in leaf_chain starting from begin_leaf:
       for each record in leaf:
           if record.key >= end: break
           if record.key >= begin:
               emit record

3. 跨 leaf 时：
   - 当前 leaf 扫描完 → 找 parent internal 中的下一个 child
   - 下一个 child range → manifest.resolve → read next leaf

4. 返回 sorted [(key, leaf_record)] list
```

**B+ tree 顺序遍历**：leaf nodes 在逻辑上是有序链的一部分（通过 parent internal 的 child pointers 链接）。扫描时从 lower_bound 开始，逐个 leaf 向右遍历。

### 6.5 Memtable-Over-Tree Merge

```text
merge_memtable_over_tree(memtable_results, tree_results):

// 两者都是按 key 排序的
i = 0, j = 0
merged = []

while i < memtable_results.size() && j < tree_results.size():
    mk = memtable_results[i].key
    tk = tree_results[j].key

    if mk < tk:
        // memtable 独有 → 输出（如果不是 tombstone）
        merged.push(memtable_results[i])
        i++
    else if mk > tk:
        // tree 独有 → 输出
        merged.push(tree_results[j])
        j++
    else:
        // 同 key → memtable 优先（概要 §8.2 规则 3）
        merged.push(memtable_results[i])
        // tree 结果被遮蔽
        i++; j++

// 处理剩余
while i < ...: merged.push(memtable_results[i++])
while j < ...: merged.push(tree_results[j++])

// 过滤 tombstone（概要 §8.2 规则 4）
return [ r for r in merged if r.kind != tombstone ]
```

## 7. Tree Tombstone 读语义

### 7.1 Tombstone 不穿透

概要 §8.1 规则 3：只要 memtable 命中（无论 value 还是 tombstone），不再回退到 tree。

```text
场景：
  memtable 有 (key, data_ver=10, tombstone)
  tree 有 (key, data_ver=5, value, vr)

  reader.read_lsn >= 10:
    → memtable hit tombstone → not found
    → 不查 tree

  reader.read_lsn = 7（但 tombstone data_ver=10 > 7）:
    → memtable 中 data_ver=10 > read_lsn → 对该 reader 不可见
    → 继续往旧 gen 查找，或 memtable miss
    → 如果旧 gen 也没有 → 查 tree
    → tree hit value (data_ver=5 <= 7) → 返回 value
```

### 7.2 Tree Tombstone 遮蔽 Tree Value

如果 tree 中同一 key 有 tombstone（flush 后的状态），reader 通过 tree_guard.manifest 读到的 leaf record 是 tombstone → not found。

tree 中同一 key 在同一 snapshot 下只有一条 winner record（flush 确保了这一点）。

### 7.3 Tombstone 对 Range Scan 的影响

range scan 输出时过滤 tombstone（merge 后的 filter 步骤）。tombstone 在 merge 阶段参与遮蔽旧 tree value，但最终不输出给上层。

## 8. 长读资源上界策略

### 8.1 问题

长时间持有 read_handle 会：
1. 阻止 old memtable_gen 释放 → 内存累积
2. 阻止 old checkpoint_guard 释放 → tree reclaim 延迟 → 空间不回收
3. 阻止 old value_ref 回收 → Value Area 空间压力

### 8.2 策略

#### 策略 1：读超时

```text
read_handle 设置 TTL（如 30 秒）。
超时后：
  - 不强制释放（会破坏正在进行的读）
  - 标记为 stale，下次 tree/NVMe 操作前检查
  - 如果 stale → 中止读操作，返回 timeout 错误给客户端
```

#### 策略 2：Generation 距离限制

```text
当新 CAT 安装时，检查仍存活的旧 CAT 列表。
如果某个旧 CAT 的 epoch 与 current epoch 差距 > MAX_GENERATION_LAG：
  - 不强制释放
  - 只是发出告警 / 更新 metrics
  - 运行时可以选择拒绝新读直到旧读完成（backpressure）
```

#### 策略 3：内存水位反压

```text
当 sealed memtable 内存总量超过阈值：
  - 暂停接受新写（或减速）
  - 等待旧 reader 释放后 memtable 回收
  - 或加速 flush 减少 sealed gen 数量
```

### 8.3 v1 建议

v1 先实现策略 3（内存反压）和策略 2（作为观测/告警手段）。

关于策略 1（读超时）：v1 **不在 KV 层主动中断读操作**。read_handle 在拓扑和正确性层面始终有效（shared_ptr pin 保证，见 §10.3）。"长读"的资源代价由内存反压和上层协议约束来管控，而非 KV 层内部的超时机制。

长读问题在实际部署中主要来自：
- range scan 扫描大量数据
- 上层事务 hold 住 read_handle 做多次读

控制手段是上层协议约束（限制单次 scan 的 key 数、限制 read_handle 持有时间）。

## 9. Page/Frame Cache 模型

> 统一模型定义见 `runtime_memory_and_cache.md`。本节只说明读路径如何使用该模型。

### 9.1 术语映射

| 旧术语 | 统一模型 |
|--------|---------|
| `node_cache` | `readonly_frame_cache` 在 `tree_node` domain 的逻辑视图 |
| `value_cache` | value frame residency + `value_view` 零拷贝引用 |
| `hot_blob` | 不变。correctness-carried hot data，不属于 page cache |

### 9.2 Tree Node Frame Cache

tree lookup 使用 `readonly_frame_cache` 获取 tree page frame：

```cpp
// 概念接口（返回 frame_pin，RAII 自动 pin/unpin）
frame_pin get_or_read_tree_node(paddr slot_paddr, uint16_t span_lbas) {
    frame_id fid = { slot_paddr, span_lbas, domain::tree_node };
    if (auto* f = readonly_frame_cache.get(fid))
        return frame_pin(f);       // pin_count++ by constructor
    // cache miss → 从 DMA pool 分配 frame → NVMe read → 回填 cache
    auto* f = alloc_frame(fid);
    nvme_read(slot_paddr, f->dma_buf, span_lbas * lba_size);
    f->st = frame_state::clean_readonly;
    readonly_frame_cache.put(fid, f);
    return frame_pin(f);
}
```

**pin 语义**：tree 遍历是多步异步操作（root → internal → leaf）。每步获取 `frame_pin`（RAII），pin 析构时自动 `pin_count--`。pin_count > 0 的 frame 不可驱逐。frame 由 cache 持有，消费者不持有 ownership（见 `runtime_memory_and_cache.md` §5.4）。

**驱逐安全**：pin_count == 0 的 frame 可按 LRU 淘汰。manifest 由 checkpoint_guard pin 住，保证结构信息不丢失；frame 只是 page image 的可重建载体。

### 9.3 Value Frame 与 `value_view`

tree-path 命中 value record 后，通过 `value_ref` 读取 value body：

```cpp
struct value_view {
    frame_pin pin;                       // RAII 保活 DMA page（见 runtime_memory_and_cache.md §5.4）
    uint16_t byte_offset;                // value_object_header 起始位置
    uint32_t len;                        // value body 长度
    // value body 指针 = pin.frame->dma_buf + byte_offset + sizeof(value_object_header)
};
```

获取流程：

```text
read_value(value_ref vr):
    frame_id fid = { vr.base, span_lbas_for_class(vr), domain::value_page };
    frame = readonly_frame_cache.get_or_read(fid)
    // 校验 magic + body_crc（在 frame->dma_buf + vr.byte_offset 处）
    return value_view { frame, vr.byte_offset, vr.len }
    // 上层直接从 value_view 读 bytes → 零拷贝
```

**sub-LBA 共享**：同一 LBA 中的多个 value objects 共享一个 frame。不同 `value_view` 指向同一 frame 的不同 byte_offset。

**上层需要 owning bytes 时**：显式 materialize（memcpy 到 heap buffer）。但大多数读路径只需要 `value_view` 的生命周期内引用。

### 9.4 hot_blob 不变

memtable hit 的语义不受 frame cache 影响：

1. memtable hit value → 直接从 `hot_blob` 返回，不经过 frame cache
2. `hot_blob` 由 memtable_entry → value_handle → intrusive_ptr 持有
3. cache 驱逐不影响 `hot_blob`（它不在 page cache 中）
4. memtable hit 绝不退化成 SSD 读

### 9.5 读路径 Frame 使用总结

```text
point_get(key):
  memtable hit value → hot_blob（零拷贝，不经 frame cache）
  memtable hit tombstone → not found
  memtable miss → tree lookup:
    tree traversal: get_or_read_tree_node() × depth 次（frame pin/unpin）
    leaf hit value → read_value(vr) → value_view（零拷贝 DMA frame 引用）
    leaf hit tombstone → not found
    leaf miss → not found
```

## 10. 读路径异常处理

### 10.1 NVMe Read 失败

```text
tree page read 失败 → 返回 IO error
value read 失败 → 返回 IO error
不影响其他 reader 或写入路径
```

### 10.2 CRC 校验失败

```text
tree page CRC 不通过 → 返回 data corruption error
value body CRC 不通过 → 返回 data corruption error
这表示 torn write 或 bit rot
```

### 10.3 read_handle 不会失效

如果某次读操作跨越了多次 seal + frontier switch：
- read_handle 持有的 cat 仍然有效（shared_ptr pin 住）
- reader 仍然能正确读取旧拓扑
- read_handle 在 KV 层不会被主动中断或标记为 stale
- 唯一风险是资源回收延迟（§8 的长读问题，由内存反压和上层协议约束管控）

## 11. 可见性判定总结

### 11.1 单条 Entry 可见性

```text
entry 对 reader 可见 ⟺ entry.data_ver <= reader.read_lsn
```

不需要额外的 visible bit（概要 §7.3）。

### 11.2 Winner 规则

对同一逻辑 key 的多条候选（跨 memtable gens + tree）：

```text
1. memtable 中：取 data_ver <= read_lsn 的最大版本
2. 如果 memtable 命中 → memtable winner 为最终结果
3. 如果 memtable miss → tree record 为最终结果
4. 最终 winner 为 tombstone → not found
```

### 11.3 Never 退化

memtable hit value 时走 hot_blob，不走 SSD。这不是优化，是正确性保证（hot_blob 是 memtable entry 的唯一值来源，概要 §5.1 规则 5）。
