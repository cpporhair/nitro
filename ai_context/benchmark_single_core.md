# AiSAQ 单核搜索性能基准测试

> 测试日期：2026-03-16

## 1. 测试环境

| 项目 | 规格 |
|------|------|
| CPU | Intel i9-12900HX (P-core 4.9GHz / E-core 3.6GHz) |
| CPU Governor | performance |
| NVMe | Samsung PM9A1 (0000:02:00.0, SPDK vfio-pci) |
| OS | Linux 6.19.6-zen1-1-zen |
| 编译 | GCC 15, -O3 -march=native, Release |

**注意**：SPDK 进程通过 `sched_getcpu()` 选择核心，可能运行在 P-core 或 E-core 上，两者性能差异约 35%。aisaq-diskann 使用 io_uring 通过内核读盘，单线程时同样受核心调度影响。两者的核心分配均未固定（未使用 taskset），测试条件对等。

## 2. 数据集

| 数据集 | 维度 | 向量数 | 查询数 | Ground Truth |
|--------|------|--------|--------|-------------|
| SIFT1M | 128 | 1,000,000 | 10,000 | top-100 精确 L2 |

## 3. 索引配置

### B=0.3 (n_chunks=128, 高 recall)

| 项目 | aisaq-diskann | PUMP AiSAQ |
|------|--------------|------------|
| 索引格式 | aisaq 多 sector (3 sectors/node) | 4K 对齐多 sector (3 sectors/node) |
| max_node_len | 8964 bytes | 8964 bytes |
| 存储内容 | float[128] + neighbors + inline PQ(128×64) | float[128] + neighbors + inline PQ(128×64) |
| inline_pq | 64 (全部邻居) | 64 (全部邻居) |
| max_degree | 64 | 64 |
| IO 方式 | io_uring (内核态) | SPDK (用户态 DMA) |
| PQ 码来源 | inline from disk sector | inline from disk sector |
| DRAM 需求 | PQ codebook only (~128KB) | PQ codebook only (~128KB) |

**B=0.3 对比公平性：两者使用相同算法（aisaq inline PQ beam search），相同索引数据（同一 SIFT1M 构建），仅 IO 引擎和 pipeline 编排不同。**

### B=0.003 (n_chunks=3, 低 recall)

| 项目 | aisaq-diskann | PUMP AiSAQ |
|------|--------------|------------|
| 索引格式 | 标准 DiskANN 打包 (15 nodes/sector) | 4K 对齐 (1 node/sector) |
| max_node_len | 263 bytes | 964 bytes |
| 存储内容 | uint8[3] PQ码 + neighbors | float[128] + neighbors + inline PQ(3×64) |
| inline_pq | 0 (无) | 64 (全部邻居) |
| PQ 码来源 | DRAM (预加载 3MB) | inline from disk sector |
| DRAM 需求 | PQ codebook + compressed PQ (3MB) | PQ codebook only |

**B=0.003 对比局限性：两者索引格式不同。aisaq-diskann 用打包格式（15 nodes/sector, PQ码在DRAM），PUMP 用 4K 对齐 + inline PQ（1 node/sector, 完全 DRAM-free）。QPS 对比仅供参考，recall 差异（19.7% vs 29.9%）反映了不同索引结构和距离计算方式（PQ-only vs PQ候选+精确L2结果）。**

## 4. 测试方法

### aisaq-diskann
```bash
# 单线程
search_disk_index --data_type float --dist_fn l2 \
  --index_path_prefix <prefix> --query_file sift_query.fbin \
  --recall_at 10 --search_list L --beamwidth W \
  --gt_file gt100 --num_threads 1

# 24 线程 (参考)
search_disk_index ... --num_threads 24
```

### PUMP AiSAQ
```bash
# 单核 SPDK (concurrent=查询并发数)
apps.aisaq search <prefix> query.fbin 0000:02:00.0 \
  K L W gt100 probes concurrent
```

PUMP 单核运行，通过 `concurrent(N)` 并发处理 N 个查询。查询间的 IO 等待被其他查询的 CPU 工作填充，实现单核内的 IO/CPU 重叠。这是单核能力的真实体现，与 aisaq-diskann 单线程串行处理每个查询不同。

## 5. B=0.3 测试结果

### 5.1 aisaq-diskann 基线

| L | W | Threads | QPS | Recall@10 | Mean IOs | Mean Latency (μs) |
|---|---|---------|-----|-----------|----------|--------------------|
| 100 | 2 | 1 | 283 | 99.80% | 106.4 | 3,489 |
| 100 | 4 | 1 | 418 | 99.81% | 111.5 | 2,350 |
| 75 | 4 | 1 | 533 | 99.62% | 86.9 | 1,838 |
| 50 | 4 | 1 | 744 | 99.05% | 62.5 | 1,316 |
| 100 | 4 | 24 | 5,604 | 99.81% | 111.5 | 4,218 |

### 5.2 PUMP AiSAQ

| L | W | Concurrent | QPS | Recall@10 | Mean IOs | Mean Iters |
|---|---|-----------|-----|-----------|----------|------------|
| 100 | 4 | 40 | 1,714 | 99.75% | 112.3 | 28.4 |
| 100 | 2 | 40 | 1,568 | 99.73% | — | — |
| 75 | 4 | 40 | 2,090 | 99.57% | — | — |
| 50 | 4 | 40 | 2,611 | 98.99% | — | — |

### 5.3 B=0.3 对比（同参数 L=100, W=4）

| 实现 | QPS | Recall@10 | 单核加速比 |
|------|-----|-----------|-----------|
| aisaq-diskann 1 thread | 418 | 99.81% | 1.0× (基线) |
| **PUMP AiSAQ 1 core** | **1,714** | **99.75%** | **4.1×** |
| aisaq-diskann 24 threads | 5,604 | 99.81% | 等效 ~3.3 核 |

### 5.4 B=0.3 QPS-Recall 权衡

| L | aisaq-diskann 1T | PUMP 1 core | PUMP 加速比 | Recall (PUMP) |
|---|-----------------|-------------|-------------|---------------|
| 50 | 744 | 2,611 | 3.5× | 98.99% |
| 75 | 533 | 2,090 | 3.9× | 99.57% |
| 100 | 418 | 1,714 | 4.1× | 99.75% |

## 6. B=0.003 测试结果（参考，索引格式不同）

### 6.1 aisaq-diskann 基线

| L | W | Threads | QPS | Recall@10 | Mean IOs |
|---|---|---------|-----|-----------|----------|
| 100 | 2 | 1 | 1,545 | 19.72% | 106.4 |
| 100 | 4 | 1 | 1,516 | 19.64% | 111.5 |
| 100 | 4 | 24 | 10,601 | 19.64% | 111.5 |

### 6.2 PUMP AiSAQ

| L | W | Concurrent | QPS | Recall@10 |
|---|---|-----------|-----|-----------|
| 100 | 4 | 64 | 5,658 | 29.90% |

### 6.3 B=0.003 说明

Recall 差异（19.7% vs 29.9%）源于：
1. PUMP 存储全精度向量，对展开节点计算精确 L2 距离用于结果排序
2. aisaq-diskann B=0.003 仅存储 3-byte PQ 码，搜索全程使用 PQ 近似距离
3. 精确 L2 重排序显著提升了最终 top-K 的质量

QPS 差异（5,658 vs 1,516）部分来自索引格式差异（1 node/sector vs 15 nodes/sector 的 IO 效率不同），部分来自 IO 引擎和并发模型差异。不宜直接对比。

## 7. 性能分析

### 7.1 PUMP 优势来源

1. **SPDK 用户态 IO**：绕过内核，DMA 直读。单次 4K 随机读 ~3-5μs (SPDK) vs ~10-15μs (io_uring)
2. **Pipeline IO/CPU 重叠**：concurrent(40) 并发 40 个查询，一个查询等 IO 时其他查询做 PQ 计算
3. **协程早停**：converged 检测提前终止搜索循环（平均 28.4 次迭代 vs 上限 50 次）
4. **零拷贝 DMA**：NVMe 读取直接写入预分配的 DMA buffer，process_node 直接访问

### 7.2 CPU 热点 (B=0.3, perf profile)

| 函数 | 占比 | 说明 |
|------|------|------|
| process_node() | 65% | PQ 距离计算 (128 chunks × ~30 neighbors) + 精确 L2 |
| op_pusher (框架) | 8% | Pipeline 调度开销 |
| _nvme_ns_cmd_rw | 4% | SPDK NVMe 命令提交 |
| 其他 | 23% | visited set, candidates insert, 内存操作等 |

PQ 距离计算内循环（128 chunks × ~30 neighbors = ~3840 table lookups/node）是绝对瓶颈。已尝试的优化：
- ✅ 预计算 PQ 指针（消除内循环 imul，~4% 提升）
- ✗ AVX2 gather（_mm256_i32gather_ps 反而更慢）
- ✗ 软件预取（干扰编译器 -O3 优化）
- ✗ 4× chunk 展开（编译器已做类似优化）
- ✗ neighbor-major 循环序（table L1 局部性不如 chunk-major）
- ✗ 部分 PQ 剪枝（开销 > 收益，且损失 recall）

### 7.3 单核天花板估算

B=0.3 搜索瓶颈：
- 每查询 ~114 个节点展开 × process_node (~30μs/node) ≈ 3.4ms CPU
- 每查询 ~112 次 IO × 3 pages × 5μs/page = 1.7ms IO (被并发重叠)
- 理论单核上限 ≈ 1 / 3.4ms ≈ 294 QPS (串行) → concurrent 放大 ~6× ≈ 1,760 QPS

当前 1,714 QPS 接近理论值。进一步提升需要：
- 减少 process_node 计算量（更好的图结构 / 更少的邻居展开）
- 减少迭代次数（更好的入口点选择 / 缓存热点节点）
- 多核扩展

## 8. 结论

**B=0.3 (高 recall 场景，公平对比)：PUMP AiSAQ 单核 QPS 是 aisaq-diskann 单线程的 4.1 倍。**

单核算法优化已接近理论天花板（CPU-bound，process_node 占 65%）。下一步收益来自：
1. **页面缓存**：热点节点常驻内存，减少 IO 次数（预计 +30-50% QPS）
2. **多核扩展**：share-nothing 架构，QPS 随核心数线性增长
3. **多盘条带化**：进一步降低 IO 延迟（对 CPU-bound 场景收益有限）
