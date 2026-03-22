# Sider 代码审查 — 优化与问题清单

> 基于 commit dfce824 (Phase 1.11 inline) 的全量代码审查。按优先级排序。

## P0：正确性问题

### 1. access_clock_ uint32_t 溢出导致 LRU 失效

**文件**: `store/store.hh:63`

`access_clock_` 是 `uint32_t`，每次 GET/SET 递增。5M QPS 下 **14 分钟溢出**，溢出后旧 entry 的 hotness 值比新 entry 大，LRU 采样会把热数据误判为冷数据淘汰。

**方案**:
- A: 改为 `uint64_t`（584 年才溢出）
- B: 保持 `uint32_t`，比较时用 `(int32_t)(a - b) > 0`（wrap-aware）

### 2. resp_slot::set_inline() 无边界检查

**文件**: `resp/batch.hh:22`

`memcpy(small_buf, src, src_len)` 没有检查 `src_len <= 16`。如果调用方传错长度，栈溢出。

**方案**: 加 `assert(src_len <= sizeof(small_buf))`。

## P1：热路径性能

### 3. wire_size() + write_to() 双重 snprintf

**文件**: `resp/batch.hh:28-71`

每个 resp_slot 的 BULK 响应先 `snprintf` 算长度（wire_size），再 `snprintf` 写 header（write_to）。P32 batch 调两次 × 32 = 64 次 snprintf。

**方案**: 用算术替代 snprintf：
```cpp
static inline uint32_t digit_count(uint32_t n) {
    if (n < 10) return 1; if (n < 100) return 2;
    if (n < 1000) return 3; if (n < 10000) return 4;
    // ...
}
// wire_size: 1('$') + digits + 2(\r\n) + len + 2(\r\n)
// write_to: 手写 itoa 替代 snprintf
```

### 4. hash_table::insert() 双重查找

**文件**: `store/hash_table.hh:74-85`

`insert()` 先调 `lookup()`（hash + probe），再调 `robin_insert()`（再 hash + probe），最后再 `lookup()` 返回指针。一次 insert 做了**三次 hash + probe**。

**方案**: 合并为 `find_or_insert()` 一次完成。

### 5. batch_receiver 每次 recv 堆分配

**文件**: `server/session.hh`（batch_recv_req）

每次 `recv_batch` 创建 `new batch_recv_req{...}`，处理完 `delete`。高 QPS 下频繁 malloc/free。

**方案**: 用 thread_local 对象池（类似 `sider_page_pool`）。

### 6. slab remove_from_partials O(n) 线性扫描

**文件**: `store/slab.hh:141-150`

`free_slot()` 和 `evict_page()` 都调 `remove_from_partials()`，线性扫描 vector 找 page_id。50 万 key 的 SC_64 有 ~7800 个 partial page，每次 free 都要扫 7800 元素。

**方案**:
- A: page_entry 里加 `partial_index` 字段，O(1) swap-remove
- B: 接受 stale entry，分配时 lazy 跳过已淘汰的 page

### 7. discard_page_entries 拷贝 key 到临时 vector

**文件**: `store/store.hh:596-609`

淘汰一个 page 时，遍历整个 hash table 找匹配的 entry，把 key 拷贝出来再逐个 erase。O(capacity) 遍历 + O(n) 堆分配。

**方案**: page_entry 里维护 entry 反向索引（key hash list 或 slot→entry 映射），避免全表扫描。

## P2：可改进但不紧急

### 8. hash table grow() 阻塞数据路径

**文件**: `store/hash_table.hh:153-187`

rehash 是 O(n) 同步操作。load factor 0.75 触发 grow，百万级 entry 时 rehash 造成毫秒级卡顿。

**方案**: 增量 rehash（每次 insert/lookup 迁移 k 个 bucket），类似 Redis 的渐进式 rehash。但实现复杂度高，优先级低。

### 9. set() 同步淘汰循环

**文件**: `store/store.hh:161-168`

内存达到 max_bytes 时，set() 内部 while 循环同步 discard page。极端情况下单次 set 触发多次 page discard，延迟不可控。

**方案**: set() 只做一次 discard，剩余交给 scheduler advance() 异步淘汰。但需要处理"内存真的满了怎么办"的边界。

### 10. nvme_allocator::allocate_contiguous() O(n) 线性扫描

**文件**: `nvme/allocator.hh:111-137`

大 value 淘汰需要连续 LBA，当前逐 bit 扫描整个 bitmap。1TB 盘有 2.44 亿页，最坏情况扫描量巨大。

**方案**: 维护 free run list（空闲段链表），O(1) 分配。但大 value 淘汰频率低，优先级不高。

### 11. promote 后二次 get() 查询

**文件**: `server/handler.hh:171-185`

冷读 promote 后，再调一次 `store.get()` 确认是否成功。这次 get 走完整的 hash lookup + page_table 路径。

**方案**: `promote()` 直接返回结果指针（成功时返回 slab ptr，失败时返回 nullptr）。

## P1.5：DMA 分配 fallback 到非 DMA 内存

### 15. dma_alloc_page/dma_alloc_large fallback 到 aligned_alloc

**文件**: `nvme/init.hh:39-57`

DMA pool 耗尽时 `spdk_mempool_get` 返回 NULL，fallback 到 `aligned_alloc` 分配普通堆内存。这些页淘汰到 NVMe 时 `vtophys` 失败，触发 SPDK 错误风暴。同样 `spdk_dma_zmalloc` 失败时也 fallback 到 `aligned_alloc`。

**根因**：DMA pool 配了 8192 页（32MB），但 memory_limit 512MB 需要 ~128K 页。Pool 用完后大量非 DMA 页进入系统。

**方案**：
1. DMA pool 大小从 memory_limit 自动计算（≥ memory_limit / PAGE_SIZE + 冷读缓冲余量）
2. Fallback 链改为 `mempool_get → spdk_dma_zmalloc → 报错丢弃 key`（永不分配非 DMA 内存）
3. `dma_free_page` 按地址范围判断归还 pool 还是 `spdk_dma_free`

## P3：代码质量

### 12. classify_command 硬编码字符串比较

**文件**: `server/handler.hh:79-119`

逐个 `if (cmd_is(cmd, "GET"))` 比较。命令多了之后不好维护。

**方案**: 用 perfect hash 或 switch on first char + length。但当前命令少（6 个），不急。

### 13. config 默认值不一致

**文件**: `config.hh`

JSON 模式 evict_begin 默认 90%，CLI 模式默认 60%。

**方案**: 统一默认值。

### 14. alloc_page()/alloc_large() 不检查返回值

**文件**: `store/types.hh:63-88`

`aligned_alloc` 和 SPDK DMA 分配可能返回 nullptr，调用方都没检查。

**方案**: 分配失败时 abort（cache 系统内存不足应视为致命错误）。

## 不是问题（审查中排除的项）

| 项 | 原因 |
|---|---|
| 单线程安全假设 | PUMP 框架保证每个 scheduler 单线程运行，无需锁 |
| page_table LIFO 复用 | 复用热 page_id 对 cache 有利（刚淘汰的页面 page_entry 还在 cache） |
| main.cc 退出时资源泄漏 | 进程退出后 OS 回收，不需要显式 cleanup |
| compute_sender_type 方法名 | `get_value_type_identity()` 是框架实际使用的名字（已验证） |
| store_req local::queue 4096 容量 | 单线程 scheduler，batch 最多 128 命令，4096 足够 |
