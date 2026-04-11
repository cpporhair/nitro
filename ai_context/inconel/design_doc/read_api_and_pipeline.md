# Inconel 详细设计：读 API 与 Pipeline

> 依据：`ai_context/inconel/design_overview.md`（唯一概要规范）
>
> 本文细化 GET / MultiGet / Scan 的 read_handle 生命周期、batch cache、memtable value zero-copy 返回路径、tree lookup sender 编排、tree tombstone 读语义以及长读资源上界策略。

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
- 每个 `memtable_gen`（通过 `std::shared_ptr<memtable_gen>`）
- `checkpoint_guard`（通过 shared_ptr）
- `tree_manifest`（通过 guard）

持有期间，所有被 pin 住的对象不会被释放：
- memtable gens 的数据不会被清理
- tree guard 保护的旧 slots / old values 不会被 TRIM
- memtable 的 `kv_arena` 引用链不会断裂（所有 key/value bytes 随 gen 一起保活）

### 2.3 释放

read_handle 析构 → `shared_ptr<publish_catalog>` ref--

如果这是最后一个引用：
```text
cat 析构 → prs 析构 → {
    fronts 析构 → shared_ptr<memtable_gen> use_count--
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
       value_view(vv):
           // memtable hit → vv 指向 owning gen 的 kv_arena 切片，zero-copy
           // read_handle 仍在手，shared_ptr<memtable_gen> 保活整个 arena
           return vv   // 上层把 vv.data/vv.len 写到响应 buffer
       tombstone:
           // memtable hit tombstone → not found
           return not_found
       miss:
           // memtable miss → 继续查 tree
           goto step 6

6. 路由到 owner front 的 home `tree_lookup_sched`
   tree_owner = route_tree_lookup(owner)

7. 查 tree（on tree_lookup_sched[tree_owner]，使用 cat->prs->tree_guard.manifest）
   tree_result = tree_lookup(key, cat->prs->tree_guard->manifest)

8. 处理 tree 结果：
   match tree_result:
       leaf_value_record(data_ver, vr):
           // tree hit value → 路由到 value_alloc_sched 读 value body
           value_data = value_alloc_sched->read_value(vr)
           return value_data
       leaf_tombstone(data_ver):
           // tree hit tombstone → not found
           return not_found
       absent:
           // tree miss → key 不存在
           return not_found

9. 释放 read_handle
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
  >> visit()                    // variant<value_view, tombstone, miss>
  >> then([key](auto&& result) {
         using T = std::decay_t<decltype(result)>;
         if constexpr (std::is_same_v<T, value_view>) {
             // memtable hit → zero-copy view，read_handle 仍持有
             return just(std::move(result));
         } else if constexpr (std::is_same_v<T, tombstone_tag>) {
             return just(not_found_result{});
         } else {
             // miss → tree_lookup_sched 查 tree → value_alloc_sched 读 value
             return get_context<read_handle>()
                 >> then([key](read_handle& rh) {
                        uint32_t owner = key_hash(key) % front_sched_count;
                        uint32_t tree_owner = route_tree_lookup(owner);
                        return tree_lookup_sched[tree_owner]->tree_lookup(
                            key, rh.cat->prs->tree_guard->manifest);
                    })
                 >> flat()
                 >> visit()         // variant<leaf_value, leaf_tombstone, absent>
                 >> then([](auto&& tree_result) {
                        using T = std::decay_t<decltype(tree_result)>;
                        if constexpr (std::is_same_v<T, leaf_value_record>)
                            return value_alloc_sched->read_value(tree_result.vr);
                        else
                            return just(not_found_result{});
                    })
                 >> flat();
         }
     })
  >> flat()
  >> pop_context()              // 释放 read_handle
```

**tree traversal 与 value read 分离**：memtable miss 后，读路径分两步：(1) 把 `(key, manifest)` 路由到 owner front 的 home `tree_lookup_sched`（见 `runtime_state_machine.md` §4.7）执行 tree traversal；(2) tree hit value 后，路由到 `value_alloc_sched` 读 value body（见 `runtime_state_machine.md` §6.5）。Point GET 走 `read_value(vr)`；MultiGet / Scan 先在调用方按 value page 分组，再走 `read_page_values(group)`。`value_alloc_sched` 集中持有 value_page cache（含 dirty open frames）。这样 tree-node cache 与 tree traversal 共置在 `tree_lookup_sched`，value cache 与 value allocator 共置在 `value_alloc_sched`，两者都不依赖 tree_sched 的可变状态。

### 4.3 Memtable Lookup 实现

dispatch 到 `front_sched[owner]` 执行（保证 btree_map 线程安全），但搜索的是调用方传入的 PRS snapshot 中的 active/imms（概要 §8.1 step 3），不是 front_sched 当前状态：

```text
lookup_memtable(key, read_lsn, frs):
    // frs = cat->prs->fronts[owner]，由 PRS 的 shared_ptr<memtable_gen> 保活

1. 查 frs.active gen：
   在 frs.active.table 中查找 key
   收集所有 data_ver <= read_lsn 的 entries
   winner_active = 其中 data_ver 最大的

2. 查 frs.imms（从新到旧）：
   for gen in frs.imms:
       在 gen.table 中查找 key
       收集所有 data_ver <= read_lsn 的 entries
       winner_gen = 其中 data_ver 最大的

3. winner = winner_active 与所有 winner_gen 中 data_ver 最大的那条

4. 如果 winner 不存在 → 返回 miss

5. 如果 winner.kind == value → 返回 `value_view{winner.vh.hot->data, winner.vh.hot->len}`（zero-copy，生命周期绑定 read_handle）

6. 否则 winner.kind == tombstone → 返回 tombstone
```

这里不能因为 active 命中就提前返回。gen 的拓扑顺序（active / imms）不等价于同一 key 的 `data_ver` 新旧顺序；正确性标准始终是“在 `active + imms` 中取 `data_ver <= read_lsn` 的最大版本”。

### 4.4 memtable value 的 zero-copy 返回契约

概要 §8.1 规则 4：memtable hit value 时，必须直接从 `value_handle.hot` 返回，绝不能退化成 SSD 读。

`lookup_memtable` 命中 value 时直接返回 `winner.vh.hot`——它本身就是 `value_view { const char* data; uint32_t len; }`，指向 owning `memtable_gen.kv_arena` 中的某段切片。**不做任何 copy**——调用方在 `read_handle` 作用域内消费 view 即可（例如写到 RESP / RPC 响应 buffer）。

**保证链**（唯一的跨线程 atomic gate 在 `shared_ptr<memtable_gen>` 的 control block）：
```text
read_handle          shared_ptr<publish_catalog>
  → publish_catalog  shared_ptr<published_read_set>
  → prs              shared_ptr<front_read_set[]>
  → front_read_set   shared_ptr<memtable_gen>   ← 整条链上唯一的 atomic gate
  → memtable_gen     by-value { kv_arena, table, ... }
    → kv_arena         vector<unique_ptr<char[]>> (所有 gen-local bytes)
    → table            btree_map<string_view → InlinedVec<memtable_entry>>
      → memtable_entry    POD
        → value_handle    POD
          → value_view    指向 kv_arena 的 slice（const char*, uint32_t）
```

只要 `read_handle` 未释放，`shared_ptr<memtable_gen>` 的 `use_count ≥ 1`，整条链上的所有对象都活着，`value_view` 里的指针稳定可读。value bytes 自身**没有** refcount——它们是 `kv_arena` 的内部切片，随 gen 析构一次 sweep 释放，跨线程访问完全由 shared_ptr control block 这个唯一的 atomic gate 保证。

**调用方义务**：
1. 在 `read_handle` 作用域内消费 view；不能把 `value_view` 保存在比 read_handle 更长寿的对象里。
2. 如果需要把 bytes 送到 `read_handle` 作用域之外（例如 enqueue 到一个跨 request 的 buffer），调用方自行做一次 copy。lookup path 不为这种情况提前 copy。

### 4.5 Tree Path Value Read

memtable miss 后查 tree，如果命中 value record：

- Point GET：直接路由到 `value_alloc_sched` 执行 `read_value(vr)`
- MultiGet / Scan：先把 tree-sourced `value_ref` 按 value page 分组，再调用 `read_page_values(group)`

Point GET 的单值路径如下（见 `runtime_state_machine.md` §6.5）：

```text
value_alloc_sched.read_value(value_ref vr):
    1. 先查 dirty open frames（当前正在填充的页）→ 命中则直接从 DMA buffer 读
    2. 查本地 readonly_frame_cache（value_page domain）→ cache hit 零 NVMe
    3. cache miss → 通过本核 nvme_sched 读整个 LBA → 回填 cache
    4. 在 frame->dma_buf + vr.byte_offset 处定位 value_object_header
    5. 校验 magic + body_len + body_crc（数据在 L1/L2 中是热的）
    6. copy body bytes → 返回 owning bytes 给调用方
```

sub-LBA value 和同页其他 value 共享一个 frame。读一个 LBA 同时使同页所有 value 变成 cache resident。

**返回 copy 而非 zero-copy view**：`value_alloc_sched` 返回 owning bytes，DMA frame 生命周期完全封闭在 `value_alloc_sched` 内部。copy 发生在 CRC 校验后数据 cache line 热的时刻，成本最低；上层（如网络发送）最终也需要一次 copy，在这里做等价于在那里做。

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

5. 按 `route_tree_lookup(owner_front(key))` 对 tree_miss_keys 分组

6. Fan-out 到各 `tree_lookup_scheds` 做 tree batch lookup
   tree_results = parallel tree_batch_lookup(group.keys, manifest, group.tree_owner)

7. 从 tree results 中提取 value refs，按 value page 分组
   value_groups = group_value_refs_by_page(tree_results)

8. Fan-out 到 `value_alloc_sched` 做批量 value read
   for each group in value_groups:
       page_values[group.page_fid] = value_alloc_sched->read_page_values(group)

9. 合并结果
   for each key in keys:
       if memtable_results has value: use memtable result（value_view → kv_arena 切片）
       else if tree_results has value: use grouped page values
       else if memtable_results has tombstone: not found
       else if tree_results has tombstone: not found
       else: not found

10. 过滤 tombstone → 只返回 live values

11. 释放 read_handle
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
         auto groups = group_keys_by_tree_lookup_owner(miss_keys);
         return get_context<read_handle>()
             >> then([groups = std::move(groups), mr = std::move(memtable_results)]
                     (read_handle& rh) mutable {
                    auto manifest = rh.cat->prs->tree_guard->manifest;
                    return for_each(groups)
                        >> concurrent(tree_lookup_sched_count)
                        >> flat_map([manifest](auto&& group) {
                               return tree_lookup_sched[group.tree_owner]->tree_batch_lookup(
                                   group.keys, manifest);
                           })
                        >> reduce(merged_tree_results, merge)
                        // ── Phase 2b: value read for tree hit values ──
                        >> then([](auto&& tree_results) {
                               auto value_groups = group_value_refs_by_page(tree_results);
                               if (value_groups.empty())
                                   return just(std::move(tree_results));
                               return for_each(value_groups)
                                   >> concurrent()
                                   >> flat_map([](auto&& group) {
                                          return value_alloc_sched->read_page_values(group);
                                      })
                                   >> reduce()
                                   >> then([tr = std::move(tree_results)](auto&& values) mutable {
                                          return fill_tree_results_with_values(std::move(tr), std::move(values));
                                      });
                           })
                        >> flat()
                        >> then([mr = std::move(mr)](auto&& tree_results) mutable {
                               return merge_results(std::move(mr), std::move(tree_results));
                           });
                })
             >> flat();
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
tree_batch_lookup(keys[], manifest, tree_owner):
    // 按 key 排序（利用 B+ tree 的顺序访问局部性）
    sorted_keys = sort(keys)

    results = []
    for key in sorted_keys:
        result = tree_lookup(key, manifest)         // 在同一个 tree_lookup_sched 上顺序执行
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
   b. on one `tree_lookup_sched` 执行 tree range scan

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
         // tree scan（在选定的 tree_lookup_sched 上执行，不经过 tree_sched）
         get_context<read_handle>()
           >> then([begin, end](read_handle& rh) {
                  uint32_t tree_owner = route_tree_scan(begin, end);
                  return tree_lookup_sched[tree_owner]->tree_scan(
                      begin, end, rh.cat->prs->tree_guard->manifest);
              })
           >> flat()
     )
  // ── K-way merge ──
  >> then([](auto&& when_all_res) {
         auto& memtable_scan = std::get<2>(std::get<0>(when_all_res));
         auto& tree_scan = std::get<2>(std::get<1>(when_all_res));
         return merge_memtable_over_tree(memtable_scan, tree_scan);
     })
  // ── 过滤 tombstone ──
  >> then([](auto&& merged) {
         return filter_tombstones(merged);
     })
  // ── value read for tree-sourced value records ──
  // memtable-sourced records 已有 value_view（直接输出 zero-copy view）
  // tree-sourced value records 先按 value page 分组，再通过 value_alloc_sched 批量读 value body
  >> then([](auto&& filtered) {
         auto tree_value_groups = group_value_refs_by_page(filtered);
         if (tree_value_groups.empty())
             return just(std::move(filtered));
         return for_each(tree_value_groups)
             >> concurrent()
             >> flat_map([](auto&& group) {
                    return value_alloc_sched->read_page_values(group);
                })
             >> reduce()
             >> then([f = std::move(filtered)](auto&& values) mutable {
                    return fill_with_values(std::move(f), std::move(values));
                });
     })
  >> flat()
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
| `value_cache` | value page frame residency + `read_value()` / `read_page_values()` copy-out 服务 |
| `hot_blob` | **已废除**。value bytes 住在 `memtable_gen.kv_arena` 里，随 gen 一起生灭 |

### 9.2 Tree Node Frame Cache

`tree_lookup_sched` 使用自己持有的 `readonly_frame_cache` 获取 tree page frame：

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

这里仍然用裸 `frame_id` 命中 tree page cache。v1 的正确性前提不是“地址永不复用”，而是 tree old range 在进入 `tree_allocator.free_ranges` 前已经完成 `tree_node` invalidate barrier（见 `runtime_memory_and_cache.md` §10.2）。`front_sched` 本身不拥有 tree-node cache；它只把 memtable miss 路由到 home `tree_lookup_sched`。

**pin 语义**：tree 遍历是多步异步操作（root → internal → leaf）。每步获取 `frame_pin`（RAII），pin 析构时自动 `pin_count--`。pin_count > 0 的 frame 不可驱逐。frame 由 cache 持有，消费者不持有 ownership（见 `runtime_memory_and_cache.md` §5.4）。

**驱逐安全**：pin_count == 0 的 frame 可按 LRU 淘汰。manifest 由 checkpoint_guard pin 住，保证结构信息不丢失；frame 只是 page image 的可重建载体。

### 9.3 Value Read（`value_alloc_sched` 集中服务）

tree-path 命中 value record 后，读路径路由到 `value_alloc_sched` 读 value body（见 `runtime_state_machine.md` §6.5）。`value_alloc_sched` 是 value_page cache 的唯一 owner，同时持有 dirty open frames 和 `readonly_frame_cache`。

```text
Point GET:
    read_value(vr)                  // 单值包装

MultiGet / Scan:
    group_value_refs_by_page(...)   // 调用方先按 frame_id 分组
    read_page_values(group)         // 同页多个 refs 一次处理
```

**设计要点**：

1. **集中 cache，无 coherence 开销**：value_page cache 只在 `value_alloc_sched` 上，不存在跨 shard stale hit，hole-fill writeback 后直接更新本地 cache，无需 invalidate barrier。
2. **请求内按页分组**：MultiGet / Scan 不应把同页多个 `value_ref` 变成多次独立 `read_value()` 调用。
3. **dirty frame 可读**：sub-LBA page 停留在 `open_frames` 期间，tree-path read 可直接从 DMA buffer 读取已 durable 的 slot，零 NVMe。
4. **copy 返回**：返回 owning bytes 而非 `value_view` + `frame_pin`。DMA frame生命周期封闭在 `value_alloc_sched` 内部，`pin_count` 保持 `uint32_t`。copy 发生在 CRC 校验后数据 cache line 热的时刻；上层（如网络发送）最终也需要 copy，在这里做等价于在那里做。
5. **sub-LBA 共享**：同一 LBA 的多个 value objects 共享一个 frame。按页分组后，同页多个 value 只做一次 page lookup / page miss 判定。
6. **部署建议**：最佳实践是把 `value_alloc_sched` 放在独占核心上，读写共享同一份大 cache；但这属于部署优化，不是语义前提。

### 9.4 memtable value bytes 不经任何 frame cache

memtable hit 的语义不受 frame cache 影响：

1. memtable hit value → 返回 `value_view`，zero-copy 指向 owning gen 的 `kv_arena` 切片，不经过任何 frame cache
2. value bytes 住在 `memtable_gen.kv_arena` 里，没有独立的 `hot_blob` 对象；`value_alloc_sched` **不**维护 `value_ref -> value bytes` 的 materialized 索引
3. page cache 驱逐不影响 memtable 的 kv bytes（它们不在任何 page cache 中）
4. memtable hit 绝不退化成 SSD 读
5. view 的生命周期由 `read_handle → cat → prs → shared_ptr<memtable_gen>` 的 pin 链保证，`shared_ptr<memtable_gen>` 的 control block 是唯一的跨线程 atomic gate

### 9.5 读路径 Frame 使用总结

```text
point_get(key):
  memtable hit value → value_view → kv_arena 切片（零拷贝，不经 frame cache）
  memtable hit tombstone → not found
  memtable miss → route to home tree_lookup_sched:
    tree traversal: get_or_read_tree_node() × depth 次（在 tree_lookup_sched 上执行）
    leaf hit value → route to value_alloc_sched:
      read_value(vr) → owning bytes（cache hit 含 dirty frame / miss 时 NVMe read）
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

memtable hit value 时直接返回 `value_view` 指向 owning gen 的 `kv_arena` 切片，不走 SSD。这不是优化，是正确性保证（概要 §5.1 规则 5：memtable live 时，value bytes 的唯一来源就是 gen 的 `kv_arena`）。
