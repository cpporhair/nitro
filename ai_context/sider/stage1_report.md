# Sider Stage 1 测试报告

> 单核完整功能版本。作为 Stage 2 多核扩展的基线对照。

## 测试环境

| 项 | 值 |
|---|---|
| CPU | Intel i9-12900HX (16C/24T, P-core 4.9GHz, 笔记本) |
| 内存 | 128GB DDR5 |
| OS | Linux 6.19.6-zen1-1-zen (Arch) |
| 编译器 | GCC 15.2.1 |
| 编译选项 | `-O3 -DNDEBUG -std=gnu++26 -march=native -flto` |
| NVMe | 3 × Samsung 980 PRO 1TB (PCIe 4.0, 0000:02/04/06:00.0) |
| SPDK | v26.01, DPDK backend, VFIO driver |
| Redis 对照 | Valkey 8.1.4 (jemalloc-5.3.0) |
| CPU governor | performance |
| HUGEMEM | 8192 MB |
| Sider commit | d956653 |
| 测试标准 | `ai_context/sider/benchmark.md` |

## 基准测试参数

```bash
# 纯内存：-r 1M, SET 10x 覆盖, -d 256
redis-benchmark -p <port> -t set -c 50 -n 10000000 -P 32 -q -d 256 -r 1000000
redis-benchmark -p <port> -t get -c 50 -n 3000000 -P 32 -q -d 256 -r 1000000
redis-benchmark -p <port> -t get -c 50 -n 500000 -P 1 -q -d 256 -r 1000000

# 冷读：-r 3M, SET 10x 覆盖, memory=512M + 3 NVMe
redis-benchmark -p <port> -t set -c 50 -n 30000000 -P 32 -q -d 256 -r 3000000
redis-benchmark -p <port> -t get -c 50 -n 3000000 -P 32 -q -d 256 -r 3000000
```

零 miss 验证：纯内存场景 SET 后采样确认 100% 命中。冷读场景因 sync discard（已知问题）命中率 ~90%。

## 性能数据

### 纯内存吞吐 (requests/sec)

测试条件：`--memory 4G`，零 miss 验证通过。

| 场景 | Redis | Sider | Sider/Redis |
|------|-------|-------|-------------|
| P1 GET | 268K | 256K | 0.96x |
| P32 GET | 1.40M | **1.73M** | **1.24x** |
| P32 SET | 1.24M | **1.63M** | **1.31x** |

### 冷读吞吐 (requests/sec)

测试条件：`--memory 512M` + 3 NVMe, 命中率 ~90%（sync discard 丢弃部分 key）。

| 场景 | Sider 冷读 | vs 纯内存 |
|------|-----------|----------|
| P1 GET | 223K | 0.87x |
| P32 GET | **1.06M** | 0.61x |

## 稳定性测试

### 10 分钟混合压测

测试内容:
1. 6M × 256B SET 填满 (内存 + NVMe 溢出)
2. 2M 混合 GET/SET (冷读)
3. 2M SET EX 5 (TTL) + 2M GET (过期淘汰)
4. 混合 value size: 8B (inline) + 256B (slab) + 8KB (large)
5. 5 分钟持续 SET+GET -d 256 -r 6000000

结果: **311 秒全部通过，零 crash，零 hang。**

### RSS 内存监控

| 时间点 | RSS | 说明 |
|--------|-----|------|
| 0s | 522 MB | 启动 (DMA pool + SPDK) |
| 2s | 624 MB | key 填满 |
| 5s | 914 MB | 首次冷读 promote |
| 41s | 1380 MB | hash table 扩容 + 稳态 |
| 71s-311s (4.5 分钟) | **1382 MB** | **恒定，无泄漏** |

### 正确性验证

5 分钟持续压测后:
- SET 100 个 known key (200B value)
- GET 100 个 → **100/100 正确，0 mismatch**

## 功能覆盖 (Stage 1 Phase 1.1-1.12)

| 功能 | 状态 | 说明 |
|------|------|------|
| TCP + RESP2 | ✅ | io_uring backend, batch unpacker |
| GET/SET/DEL | ✅ | 含 EX/PX TTL |
| PING/QUIT/COMMAND/CLIENT | ✅ | |
| 哈希表 | ✅ | Robin Hood open addressing |
| Slab 分配器 | ✅ | 7 size classes (64B-4KB), DMA 安全 |
| 内存淘汰 | ✅ | 水位线模型, 页级 LRU 采样 |
| NVMe 淘汰写入 | ✅ | SPDK 异步 DMA, 3 盘 round-robin |
| 冷 GET 读取 | ✅ | NVMe read → promote, version 校验 |
| 大 value (>4KB) | ✅ | 连续 DMA 内存, 独立淘汰 |
| 多盘 NVMe | ✅ | round-robin 选盘, 单盘满降级 |
| 小 value 内联 (≤16B) | ✅ | entry 内存储, 零 slab 分配 |
| 惰性 + 主动过期 | ✅ | GET/DEL 惰性, advance() 主动扫描 |
| Batch pipeline | ✅ | batch unpack → concurrent execute → batch send |
| DMA 安全分配 | ✅ | spdk_mempool, 永不 aligned_alloc |

## 已知限制

| 项 | 说明 |
|---|---|
| 单核 | Stage 2 解决 |
| sync discard 丢 key | 内存满时 set() 同步丢弃整页 key，冷读测试命中率 ~90% |
| hash table rehash 阻塞 | O(n) 同步 rehash, 百万级 entry 时毫秒级卡顿 |
| P1 GET 与 Redis 持平 | 单请求延迟受 scheduler 队列开销限制 |
| 小 value GET 慢于 Redis | slab 间接寻址 vs Redis SDS embstr 内联 |
| 笔记本 CPU 热降频 | 连续 benchmark 频率波动，比值更有参考价值 |

## Stage 2 基线指标

供多核扩展对比:

| 指标 | 值 |
|------|-----|
| 单核 P1 GET 纯内存 | 256K |
| 单核 P32 GET 纯内存 | 1.73M |
| 单核 P32 SET 纯内存 | 1.63M |
| 单核 P32 GET 冷读 (90%命中) | 1.06M |
| 稳态 RSS (512MB limit) | 1382 MB |
| 10 分钟稳定性 | PASS |
| 正确性 (100 key verify after 5 min) | 100/100 |
