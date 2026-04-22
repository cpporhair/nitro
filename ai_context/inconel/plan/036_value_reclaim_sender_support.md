# 036 — Value Reclaim Sender Support

> 本文记录 step 036 当前已经落地的 value reclaim 支撑能力。
> 这不是“待实现方案”，而是实现后的设计同步。

## 本 step 解决了什么

036 现在把 value 侧的回收接收面收敛成了一个**批量 sender**：

```cpp
value::reclaim_values(std::span<const value_ref> dead_values);
```

语义是：

1. 上层只需要把“已经 definitively dead 的 `value_ref` 集合”交给 value owner。
2. value owner 自己按 `{class_idx, page_base}` 聚合回收，不再要求上层按页提前拆成 `freed_mask` 或 whole-page recycle。
3. 当一次 batch 让整页变成全空时，value owner 会先把页放入 `trim_pending`，发起批量 TRIM，**TRIM 完成后**才把页归还 whole-page pool。

也就是说，036 不再把 `freed_slots` / `recycle_whole` 作为公开 sender surface；这些页级动作已经退回到 value owner 内部 helper。

## 这次实现包含的能力

### 1. 对外只有一个 reclaim sender

`apps/inconel/value/sender.hh`

公开接口现在是：

```cpp
value::reclaim_values(std::span<const value_ref> dead_values);
```

它的职责只有两件事：

1. 把一批 dead `value_ref` 投递给 value owner。
2. 在 owner 产生 whole-free page 时，自动驱动 trim pipeline，直到这些页被安全放回 whole-page pool。

上层不需要再显式区分：

- sub-LBA page 的 slot reclaim
- LBA-equal / multi-LBA value 的 whole-page reclaim
- “这批回收是否刚好把整页凑满，需要 TRIM”

这些分流全部在 value owner 内完成。

### 2. queue 边界改成 batch reclaim，而不是页级 reclaim

`apps/inconel/value/scheduler.hh`

旧实现里有两条页级队列：

- `_value_freed`
- `_value_recycle`

现在已经替换成：

- `_value_reclaim`
- `_value_trim_prepare`
- `_value_trim_complete`

含义是：

1. `_value_reclaim`
   - 接收一整个 `std::vector<value_ref>`
   - 在 owner 内部完成聚合和状态更新
   - 不再按“每页一次 sender”交叉调度
2. `_value_trim_prepare`
   - 从 owner 的 `trim_pending_pages_` 中提取本轮待 TRIM 的整页列表
3. `_value_trim_complete`
   - 在 NVMe trim 完成后，把这些页真正归还 allocator whole pool

这样 queue 成本发生在“一个 reclaim batch”上，而不是“每个 dead page / dead slot”上。

### 3. owner 内部自己做 page-level 聚合

`handle_reclaim(...)` 现在按 `value_ref` 自动分两类：

1. **sub-LBA class**
   - 用 `class_for_len(vr.len)` 推出 `class_idx`
   - 用 `vr.byte_offset / class_size` 推出 slot index
   - 聚合成 `partial_by_class[ci][page_base] |= (1ULL << slot_idx)`
2. **LBA-equal / multi-LBA class**
   - 这些 class 每页只有 1 个 slot
   - 直接聚合成 `whole_by_class[ci].insert(page_base)`

然后 owner 再分别走：

- `apply_partial_reclaim(ci, page_base, freed_mask)`
- `apply_whole_reclaim(ci, page_base)`

这两个 helper 是**owner-local synchronous helper**，不再是 queue item。

### 4. whole-free page 不会直接重用，必须先过 TRIM

本次新增了 owner 状态：

```cpp
absl::flat_hash_map<paddr, trim_pending_descriptor> trim_pending_pages_;
absl::flat_hash_map<uint64_t, std::unique_ptr<trim_batch_state>> inflight_trim_batches_;
```

其中 `trim_pending_descriptor` 记录：

- `class_idx`
- `span_lbas`
- `state { pending, inflight }`

状态机是：

```text
partial reclaim / whole reclaim
    -> page becomes all-free
    -> trim_pending_pages_[page_base] = pending

prepare_trim_batch()
    -> collect all pending pages
    -> state = inflight
    -> return trim_desc[]

sender drives nvme trim

complete_trim_batch(ok=true)
    -> erase from trim_pending_pages_
    -> alloc_.recycle_whole_page(class_idx, page_base)

complete_trim_batch(ok=false)
    -> state = pending
    -> page stays withheld, future drain can retry
```

这里最关键的变化是：

**“逻辑上全空” 和 “物理上可安全重分配” 现在被拆成两个阶段。**

旧实现里页一旦全空就直接回 whole pool；现在必须先等 TRIM 完成。

### 5. resident/cache/hole 三类状态都能被 batch reclaim 消费

`apply_partial_reclaim(...)` 现在会覆盖这些分支：

1. `dirty_pages_`
   - 不直接改 resident frame
   - 只把 reclaim 累积到 `deferred_freed_[page_base]`
2. `open_frames_[ci]`
   - 直接 OR 到 resident `free_mask`
   - 若变成全空，摘掉 open frame，进入 `trim_pending`
3. `allocatable_frames_[ci]`
   - 直接更新 `free_mask`
   - 若变成全空，移出 resident list，进入 `trim_pending`
4. `readonly_cache_`
   - `take(frame_id)` 摘出 cache frame
   - 若只是部分空，转成 `clean_allocatable`
   - 若全空，丢弃 frame，进入 `trim_pending`
5. `hole_pages_[ci]`
   - metadata-only 页做 `free_mask |= freed_mask`
   - 若变成全空，移出 hole metadata，进入 `trim_pending`
6. 完全未追踪页
   - 部分空：插入 `hole_pages_`
   - 全空：直接进入 `trim_pending`

`apply_whole_reclaim(...)` 则处理 single-slot class：

1. dirty 页：记到 `deferred_freed_`
2. clean resident / cache / hole metadata：全部清理掉 identity
3. 最后统一进入 `trim_pending`

### 6. dirty page 上的 deferred reclaim 也接入了 TRIM 流

这一步不是只处理“clean page 上的回收”。

如果 page dirty 时收到 reclaim：

```cpp
deferred_freed_[page_base] |= ...
```

等 round 在 `commit_pages()` / `rollback_pages()` 合并 deferred reclaim 后：

1. 如果页仍是 partial free
   - 回到 `clean_allocatable` / `hole_pages_`
2. 如果页已经 all-free
   - **不再直接 `recycle_whole_page()`**
   - 改成 `mark_trim_pending(ci, page_base)`

这保证了两条路径都满足同一个约束：

- clean page 回收成全空
- dirty page 在 finalize 时因为 deferred reclaim 变成全空

都必须经过 `trim_pending -> trim_complete -> whole_pool`。

### 7. TRIM drain 从 persist path 解耦

最初实现里把 trim drain 挂在了 `persist_values()` finalize 尾部，但这会把前台写 tail latency 直接绑到 TRIM 延迟上。这个耦合已经去掉。

现在对外是两个独立 sender：

```text
reclaim_values(dead_value_refs[])
drain_trim_pending()
```

语义边界是：

1. `reclaim_values(...)`
   - 只负责 ingest reclaim metadata
   - 可以把页推进到 `trim_pending`
   - **不会**主动发 NVMe TRIM
2. `drain_trim_pending()`
   - 负责 `prepare_trim_batch -> NVMe trim[] -> complete_trim_batch`
   - 由上层按 reclaim cadence / maintenance cadence 决定何时调用

这样 dirty page 在 finalize 时因为 `deferred_freed_` 合并而变成 whole-free，也只是进入 `trim_pending`；不会把本次 persist round 的尾延迟拉长。

## 文件改动

```text
apps/inconel/value/scheduler.hh
  - 删除公开页级 reclaim queue
  - 新增 batch reclaim queue + trim prepare/complete queue
  - 新增 trim_pending_pages_ / inflight_trim_batches_
  - 新增 owner-local apply_partial_reclaim / apply_whole_reclaim
  - commit/rollback 全空页改为 trim_pending，而不是直接 recycle

apps/inconel/value/sender.hh
  - 对外公开 value::reclaim_values(...)
  - 对外公开 value::drain_trim_pending()
  - persist_values 不再自动 drain trim_pending

apps/inconel/mock_nvme/device.hh
  - 新增 trim_count_ 计数，供 regression test 断言

apps/inconel/test/test_value.cc
  - 原 reclaim regression 全部切到 batch reclaim API
  - 新增“整页 sub-LBA batch reclaim -> exactly one trim -> whole-page reuse”用例
```

## 验证

本 step 当前覆盖的回归点：

1. cached full page 上 reclaim 单个 sub-LBA slot
   - cache entry 被摘出
   - resident partial page 可直接复用
   - 不发生 trim
2. cached whole page reclaim
   - 会 trim
   - trim 完成后 whole-page reuse
3. non-resident hole page reclaim
   - 只建立 `hole_pages_`
   - 下一次 persist 走 prefill read continuation
4. 一个 batch 把整张 sub-LBA page 全部回收
   - owner 内部聚合成一次 page-level whole-free
   - 只发一次 trim
   - 页完成 trim 后可 whole-page reuse

## 还没做的相邻项

036 把 steady-state reclaim sender 面补齐了，但相邻项仍然没做：

1. `install_recovered_state`
   - 这决定 boot/recovery 后如何重建 `hole_pages_` / whole pool / generic free spans
2. `read_page_values`
   - 这决定 MultiGet / Scan 的 value page 批量读取面

这两个仍然是 value 组里最接近、也最自然的后续步骤。
