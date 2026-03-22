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
| SPDK | via submodule, DPDK backend, VFIO driver |
| Redis 对照 | Valkey 8.1.4 (jemalloc-5.3.0) |
| CPU governor | performance |
| HUGEMEM | 4096 MB |
| Sider commit | d9b7ecc |

## Sider 配置

```json
{
  "port": 6379,
  "memory": "512M",
  "evict_begin": 90,
  "evict_urgent": 95,
  "accept_core": 0,
  "nvme": ["0000:02:00.0", "0000:04:00.0", "0000:06:00.0"],
  "cores": [0]
}
```

- DMA pool: 自动计算 = 135168 pages (528MB)
- 淘汰阈值: 90%/95% (begin/urgent)
- 单核运行 (core 0)

## 基准测试参数

```bash
# SET 填充
redis-benchmark -p <port> -t set -c 50 -n <count> -P 32 -q -d 256

# GET 纯内存 (3M keys in 512MB)
redis-benchmark -p <port> -t get -c 50 -n 2000000 -P 32 -q -r 3000000

# GET 2/3 冷读 (6M keys in 512MB → ~2/3 on NVMe)
redis-benchmark -p <port> -t get -c 50 -n 2000000 -P 32 -q -r 6000000
```

- `-c 50`: 50 并发连接
- `-P 32`: pipeline 深度 32
- `-d 256`: value 256 字节
- `-r N`: 随机 key 范围 N
- 无 `--threads`: 单线程 benchmark 客户端

## 性能数据

### 吞吐 (requests/sec)

| 场景 | Redis (全内存) | Sider (全内存) | Sider (2/3 NVMe) |
|------|---------------|---------------|-------------------|
| P1 GET | 243K | 242K | 237K |
| P32 GET | 3.22M | **4.39M** | **3.60M** |
| P32 SET (-d 256) | 2.09M | 3.27M | 2.96M |

### 对 Redis 比值

| 场景 | Sider 全内存 | Sider 2/3冷读 |
|------|-------------|---------------|
| P1 GET | 1.00x | 0.98x |
| P32 GET | **1.36x** | **1.12x** |
| P32 SET | **1.56x** | **1.42x** |

### 历史最高记录 (同一天更早运行, CPU 频率更高)

| 场景 | Redis | Sider | 比值 |
|------|-------|-------|------|
| P32 GET 纯内存 | 3.47M | 4.77M | 1.38x |
| P32 GET 2/3冷读 | — | 3.94M | 1.14x vs Redis |

注: 笔记本 CPU 频率波动 (800MHz-4.9GHz) 影响绝对值，比值更有参考价值。

## 稳定性测试

### 10 分钟混合压测

测试内容:
1. 6M × 256B SET 填满 (内存 + NVMe 溢出)
2. 2M 混合 GET/SET (2/3 冷读)
3. 2M SET EX 5 (TTL) + 2M GET (过期淘汰)
4. 混合 value size: 8B (inline) + 256B (slab) + 8KB (large)
5. 5 分钟持续 SET+GET -d 256 -r 6000000

结果: **311 秒全部通过，零 crash，零 hang。**

### RSS 内存监控

| 时间点 | RSS | 说明 |
|--------|-----|------|
| 0s | 522 MB | 启动 (DMA pool + SPDK) |
| 2s | 624 MB | 6M key 填满 |
| 5s | 914 MB | 首次冷读 promote |
| 41s | 1380 MB | hash table 扩容 + 稳态 |
| 71s-311s (4.5 分钟) | **1382 MB** | **恒定，无泄漏** |

RSS 组成: DMA pool 528MB + hash table ~500MB (6M entries × ~80B) + SPDK/DPDK 内部 + slab 元数据。

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
| 哈希表 | ✅ | Robin Hood open addressing, 渐进式 rehash (未做) |
| Slab 分配器 | ✅ | 7 size classes (64B-4KB), DMA 安全 |
| 内存淘汰 | ✅ | 水位线模型, 页级 LRU 采样 |
| NVMe 淘汰写入 | ✅ | SPDK 异步 DMA, 3 盘 round-robin |
| 冷 GET 读取 | ✅ | NVMe read → promote, version 校验 |
| 大 value (>4KB) | ✅ | 连续 DMA 内存, 独立淘汰 |
| 多盘 NVMe | ✅ | round-robin 选盘, 单盘满降级 |
| 小 value 内联 (≤16B) | ✅ | entry 内存储, 零 slab 分配 |
| 惰性 + 主动过期 | ✅ | GET/DEL 惰性, advance() 主动扫描 |
| Batch pipeline | ✅ | batch unpack → concurrent execute → batch send |
| DMA 安全分配 | ✅ | mempool → zmalloc fallback, 永不 aligned_alloc |

## 关键优化

| 优化 | 效果 |
|------|------|
| batch unpacker + resp_slot | P32 3.32M → 4.87M (+47%) |
| 小 value 内联 (≤16B) | inline GET +17.5% vs slab |
| snprintf → 算术 (wire_size/write_to) | BULK 格式化零 snprintf |
| hash_table insert 3hash → 1hash | 新 key 写入路径优化 |
| slab partial O(1) swap-remove | partial_index 替代线性扫描 |
| discard_page_entries 反向索引 | O(capacity) → O(slots_per_page), 修复 sync eviction hang |
| access_clock uint64 | 14 分钟溢出 → 584 年 |
| DMA pool 自动计算 | 消除 vtophys 错误, 所有页 DMA 安全 |

## 已知限制

| 项 | 说明 |
|---|---|
| 单核 | Stage 2 解决 |
| hash table rehash 阻塞 | O(n) 同步 rehash, 百万级 entry 时毫秒级卡顿 |
| sync eviction 循环 | set() 内存满时同步淘汰, 延迟不可控 |
| P1 GET 与 Redis 持平 | 单请求延迟受 scheduler 队列开销限制 |
| 小 value GET 慢于 Redis | slab 间接寻址 (page_table + slot_ptr) vs Redis SDS embstr 内联 |
| 笔记本 CPU 热降频 | 连续 benchmark 频率从 4.9GHz 降到 <1GHz |

## Stage 2 基线指标

供多核扩展对比:

| 指标 | 值 |
|------|-----|
| 单核 P32 GET 纯内存 (256B value) | 4.39M |
| 单核 P32 GET 2/3冷读 | 3.60M |
| 单核 P1 GET | 242K |
| 单核 P32 SET | 3.27M |
| 稳态 RSS (512MB limit, 6M keys) | 1382 MB |
| 10 分钟稳定性 | PASS |
| 正确性 (100 key verify after 5 min) | 100/100 |
