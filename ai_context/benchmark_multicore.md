# AiSAQ 多核多盘搜索性能基准测试

> 测试日期：2026-03-17

## 1. 测试环境

| 项目 | 规格 |
|------|------|
| CPU | Intel i9-12900HX (8P × 4.9GHz / 8E × 3.6GHz) |
| CPU Governor | performance |
| NVMe | 990 PRO 1TB (02:00.0) + 980 PRO 1TB × 2 (04:00.0, 06:00.0) |
| NVMe 驱动 | SPDK vfio-pci（用户态 DMA） |
| OS | Linux 6.19.6-zen1-1-zen |
| 编译 | GCC 15, -O3 -march=native, Release |

**SSD format 注意**：所有 NVMe 设备在测试前执行 `nvme format --ses=1`（用户数据擦除），确保 FTL 处于初始状态（无 GC 开销）。实测未 format 的 990 PRO 性能下降约 40%。

## 2. 数据集与索引

| 项目 | 值 |
|------|-----|
| 数据集 | SIFT1M (128 维, 100 万向量) |
| 查询 | 10,000 条 (10K) / 100,000 条 (100K) |
| Ground Truth | top-100 精确 L2 |
| 索引 | B=0.3, n_chunks=128, max_degree=64, inline PQ |
| sectors/node | 3 (max_node_len=8964 bytes) |
| 搜索参数 | K=10, L=100, W=4, num_probes=1 |

## 3. 架构配置

| 配置 | 核心分配 | NVMe | 并发 |
|------|---------|------|------|
| 1 核 | cpu0 (P-core): task + nvme + cache | 990 PRO × 1 | 40 |
| 8P 单盘 | cpu16 (E): dispatcher + cache; cpu0,2,4,6,8,10,12,14 (P): task + nvme | 990 PRO × 1 | 512 |
| 8P 三盘 | cpu16 (E): dispatcher + cache; cpu0,2,4,6,8,10,12,14 (P): task + nvme×3 | 990 PRO + 980 PRO × 2 | 512 |

多盘条带化策略：节点级（device = node_id % num_devices），每设备每核心独立 qpair。

## 4. 测试结果

### 4.1 单核基线 (1 P-core, 单盘, concurrent=40)

| 缓存 | QPS | Recall@10 | IOs/query |
|------|-----|-----------|-----------|
| 无 | 1,916 | 99.75% | 112.3 |
| 10K (1%) | 1,944 | 99.74% | 107.2 |
| 全量 (100%) | 2,786 | 99.90% | 0.0 |

### 4.2 多核单盘 (8 P-core, 单盘, concurrent=512)

| 缓存 | QPS | Recall@10 | IOs/query | vs 单核 |
|------|-----|-----------|-----------|---------|
| 无 | 3,233 | 99.75% | 112.3 | 1.7× |
| 10K (1%) | 3,283 | 99.74% | 107.2 | 1.7× |
| 全量 (100%) | **20,060** | — | 0.0 | **7.2×** |

### 4.3 多核三盘 (8 P-core, 3 盘条带化, concurrent=512)

| 缓存 | QPS | Recall@10 | IOs/query | vs 单核 | vs 单盘 8P |
|------|-----|-----------|-----------|---------|-----------|
| 无 | 7,752 | 99.74% | 112.3 | 4.0× | 2.4× |
| 10K (1%) | 8,052 | 99.74% | 107.2 | 4.1× | 2.5× |
| 全量 (100%) | **20,145** | — | 0.0 | **7.2×** | 1.0× |

### 4.4 与原版 aisaq-diskann 对比

**有 IO 场景（DRAM-free，对比 aisaq 模式）**：

| 实现 | 条件 | QPS | 加速比 |
|------|------|-----|--------|
| aisaq-diskann 1 thread | PM9A1, io_uring | 418 | 1.0× |
| aisaq-diskann 24 threads | PM9A1, io_uring | 1,548 | 3.7× |
| PUMP 1 core, 无缓存 | 990 PRO × 1, SPDK | 1,916 | **4.6×** |
| PUMP 8P, 单盘, cache=10K | 990 PRO × 1 | 3,283 | **7.9×** |
| PUMP 8P, 3 盘, cache=10K | 990 PRO + 980 PRO × 2 | **8,052** | **19.3×** |

aisaq 模式：inline PQ + DRAM-free 搜索，与 PUMP 算法等价。

**全缓存场景（对比 DiskANN 标准模式）**：

| 实现 | 条件 | QPS | 加速比 |
|------|------|-----|--------|
| DiskANN 标准模式 24 threads | PM9A1, io_uring, PQ 全在内存 | 4,358 | 1.0× |
| PUMP 1 core, 全缓存 | 990 PRO, SPDK | 2,786 | 0.6× |
| PUMP 8P, 全缓存 | 990 PRO, SPDK | **20,060** | **4.6×** |

DiskANN 标准模式将 PQ 码全量加载到内存（非 DRAM-free），与 PUMP 全缓存场景对等。

## 5. 分析

### 5.1 扩展效率

| 场景 | 理论线性 | 实测 | 效率 | 瓶颈 |
|------|---------|------|------|------|
| 8P 全缓存 vs 1 核 | 8.0× | 7.2× | 90% | CPU（preemptive 分发开销） |
| 8P 单盘有 IO vs 1 核 | 8.0× | 1.7× | 21% | 单盘 IOPS（990 PRO ~670K） |
| 8P 三盘有 IO vs 单盘 | 3.0× | 2.4× | 80% | 多盘接近线性，但 CPU 也开始成为瓶颈 |

### 5.2 NVMe IOPS 分析

| 配置 | QPS | pages/query | 总 pages/s | 每盘 IOPS |
|------|-----|-------------|-----------|-----------|
| 8P 单盘无缓存 | 3,233 | 337 | 1,089K | 1,089K (超载) |
| 8P 3 盘无缓存 | 7,752 | 337 | 2,612K | 871K/盘 |

单盘 1,089K pages/s 已超过 990 PRO 理论 ~670K 4K IOPS，说明 concurrent pipeline 的 IO/CPU 重叠有效利用了 NVMe 队列深度。3 盘将负载均匀分散，每盘 871K pages/s。

### 5.3 缓存效果

| 缓存 | 单核 QPS | 加速 | 说明 |
|------|---------|------|------|
| 无 | 1,916 | baseline | IO bound |
| 10K (1%) | 1,944 | +1.5% | 5% cache hit，减少少量 IO |
| 全量 | 2,786 | +45% | 纯 CPU，消除所有 IO |

B=0.3 为 CPU bound（process_node 65%），缓存消除 IO 后收益 +45%。多核场景下缓存收益更显著（8P 单盘：3,283 → 20,060 = 6.1×），因为缓存同时消除了 IOPS 瓶颈。

### 5.4 框架优化效果

| 优化 | 全缓存 8P QPS | 提升 |
|------|-------------|------|
| preemptive bounce (ff04305) | 8,052 | baseline |
| 去除 bounce | 15,326 | +90% |
| per-core task bounce (base_task) | 19,200 | +138% |
| + local queue fast path | **20,060** | **+149%** |

per_core::queue 的 local fast path（同核心零原子操作）在大并发场景下贡献了额外 ~5% 的提升。

## 6. 结论

**有 IO（DRAM-free）：PUMP 8P 3 盘 8,052 QPS，是 aisaq 模式 24 线程（1,548）的 5.2 倍，线程少 3 倍。**

**全缓存：PUMP 8P 20,060 QPS，是 DiskANN 标准模式 24 线程（4,358）的 4.6 倍，线程少 3 倍。**

主要优势来源：
1. SPDK 用户态 DMA（vs io_uring 内核态）
2. Pipeline IO/CPU 重叠（concurrent 并发查询）
3. Share-nothing per-core 架构（无锁，线性扩展）
4. 多盘节点级条带化（IOPS 线性扩展）
5. per_core::queue local fast path（同核心零原子操作 bounce）

下一步提升方向：
- 更多 NVMe 设备（当前 3 盘，4+ 盘可进一步突破 IOPS 瓶颈）
- 每核心自主调度（消除 preemptive MPMC 分发开销）
- 更大数据集验证（SIFT1B, 10 亿向量）
