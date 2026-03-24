# Sider 背压设计

## 问题

内存满时 SET 怎么办？需要在"不丢数据"、"不报错"、"不变慢"三者中取舍。

## 排除的方案

### 1. 服务端排队（pending queue）

SET 返回前排队等待后台淘汰释放内存。

**排除原因**：Sider 单机可能承接整个机柜的连接数（万级并发）。每个排队的 SET 占 pipeline 上下文——用内存去缓冲一个内存不足的问题，方向反了。队列有界则满了还是要拒绝，无界则 OOM 风险转移到队列本身。

### 2. Write-through NVMe

内存满时新值直接写 NVMe，不经过内存。

**排除原因**：时间局部性反转。刚写的数据最可能被马上读取，结果它在 NVMe 上。持续高压下所有新数据都在 NVMe → 所有 GET 走 NVMe → 读写吞吐一起崩。作为偶发降级路径可以接受，但本质上还是把命令淤积到 NVMe 队列里。

### 3. Copy-evict

内存满时同步 memcpy 一个 victim 到临时 buffer，释放 victim 内存页，新值写入空闲内存，victim 异步写 NVMe。

**排除原因**：临时 buffer pool 就是隐藏的额外内存。N 个 4KB temp buf = N×4KB 内存。把这 N×4KB 直接加到 `--memory` 上效果完全一样。没有创造新的能力，只是把"多配点内存"包装成了一个算法。

### 4. 独立直写 Scheduler

内存满时将 SET 调度到独立的 direct_write_scheduler，由它执行 NVMe 淘汰+写入。

**讨论过的优点**：store_scheduler 不阻塞，继续服务 GET；全局单个可以合并页面写入。
**排除原因**：合并页面在 hash 分片架构下不好做（各核心的 page 属于不同 shard，读回来拆不干净）。本质上还是把命令淤积到队列里，高并发下同样面临排队资源问题。

## 选定方案：-BACKPRESSURE 协议扩展

### 设计

内存满时返回 RESP 错误：

```
-BACKPRESSURE retry 10\r\n
```

客户端收到后自行 sleep + 重试。

### 为什么这是对的

1. **零服务端资源消耗**：不排队、不缓冲、不占内存。拒绝一个 SET 的代价 = 发一个几十字节的响应。

2. **等待成本分布式化**：N 个客户端各自在自己进程里 sleep，等待内存被后台淘汰释放。不集中在服务端。

3. **天然流控**：RESP 是 request-response 协议，客户端在等响应期间不会发下一条命令。被 BACKPRESSURE 的客户端自动降速，写入压力自然减小。

4. **语义明确**：
   - `-ERR OOM` → 听起来像服务挂了（永久性？），redis-benchmark 直接 exit(1)
   - `-BACKPRESSURE retry N` → 明确表示"暂时的，等一下再试"

5. **向后兼容**：RESP 协议规定 `-` 开头是错误。不认识 BACKPRESSURE 前缀的客户端会当作通用错误处理，不会崩溃。

### retry hint

当前固定 10ms。后续可根据内存水位动态调整（95% → 5ms，99% → 50ms）。

### 与 Redis 的关系

Redis 内存满时有两种策略：
- `noeviction`：返回 `-OOM`，和 Sider 之前的做法一样
- `allkeys-lru`：同步淘汰旧 key（数据丢失），SET 成功

Sider 选择 `noeviction` 语义（不丢数据）+ 更好的错误信号（BACKPRESSURE 而非 OOM）。配合 NVMe 后台淘汰（数据保留到 NVMe，不是删除），背压事件应该非常稀少。

### 测试

使用 memtier_benchmark + `--ignore-errors` 测试背压场景。详见 `benchmark.md` 场景 D。
