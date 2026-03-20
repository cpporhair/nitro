# AiSAQ-DiskANN on PUMP — 设计文档

> 基于 [antifreeze53/aisaq-diskann](https://github.com/antifreeze53/aisaq-diskann) 的设计，使用 PUMP 框架重新实现的磁盘级向量近似最近邻搜索引擎。

## 1. 项目目标

**基于 NVMe 的高性能向量搜索引擎**：

- **DRAM-free 搜索**：Inline PQ 码嵌入节点 sector，搜索不需要全量 PQ 码在内存
- **SPDK 直通 IO**：bypass 内核，DMA 直读 sector
- **Pipeline 原生**：beam search 用 PUMP sender pipeline 编排
- **Share-nothing 多核**：per-core scheduler，无锁

**不做**：filtered search、dynamic index、Python 绑定、OPQ

## 2. 算法概述

### 2.1 DiskANN / Vamana

构建稀疏近邻图（R 条边/节点），搜索时从 medoid 出发贪心遍历。

**RobustPrune**：alpha=1.2 控制多样性，避免选出互相太近的邻居，保证图可达性。多 pass occlude_factor 机制 + saturate 填充。

### 2.2 AiSAQ 增强

| 增强项 | 作用 |
|--------|------|
| Inline PQ | 邻居 PQ 码嵌入 sector，减少额外 IO |
| BFS 重排列 | 图上相近节点在磁盘连续，提升局部性 |
| 多入口点 | farthest-point sampling，选 PQ 距离最近的入口 |
| 静态缓存 | BFS 预热 medoid 附近节点常驻内存 |

## 3. 磁盘布局

### 3.1 主索引 `{prefix}_disk.index`

```
Sector 0: 元数据 (uint64_t × 11: npts, ndims, medoid, max_node_len, ...)
Sector spn..(1+npts)*spn-1: 节点数据（4K 对齐，1 node/sector）
  ┌───────────────────────────────────────┐
  │ float[ndims]          向量坐标         │
  │ uint32_t              邻居数量         │
  │ uint32_t[degree]      邻居 ID          │
  │ uint8_t[degree×n_chunks]  Inline PQ 码 │
  └───────────────────────────────────────┘

寻址: sector_id = (node_id + 1) * spn
max_node_len = ndims*4 + 4 + R*4 + R*n_chunks
```

### 3.2 辅助文件

| 文件 | 内容 |
|------|------|
| `_pq_pivots.bin` | float[256×ndims] 质心 |
| `_pq_pivots.bin_centroid.bin` | float[ndims] 全局质心 |
| `_pq_pivots.bin_chunk_offsets.bin` | uint32_t[n_chunks+1] 维度范围 |
| `_pq_compressed.bin` | uint8_t[npts×n_chunks] PQ 码 |
| `_disk.index_medoids.bin` | medoid ID |
| `_disk.index_centroids.bin` | medoid 全精度向量 |
| `_disk.index_entry_points.bin` | 多入口点 ID |

## 4. PUMP 架构

### 4.1 Scheduler

| Scheduler | 职责 | 实例数 |
|-----------|------|--------|
| task scheduler | 搜索 pipeline 执行 | per-core, preemptive 分发 |
| NVMe scheduler | sector 读写 | per-core × N 盘 |
| cache_scheduler | LRU 缓存管理 | 单实例（E-core） |

缓存两层：Layer 1 atomic bitmap（任意核心 relaxed load）+ Layer 2 LRU 数据（cache_scheduler 串行管理）。

### 4.2 搜索 Pipeline

```
query → precompute dist_table → init candidates（多入口 PQ 选最近）
→ beam_search_loop（协程早停，~28 次迭代）:
    prepare_beam → cache hit 直接 process / cache miss NVMe 读
    → concurrent(W) IO → process_pending_nodes（PQ 距离 + 精确 L2）
→ extract top-K → merge probes
```

### 4.3 多核架构

- E-core (cpu16): dispatcher + cache_scheduler
- 8 P-cores: task + nvme（纯计算）
- preemptive_scheduler 跨核分发，per-core NVMe qpair
- 多盘条带化：`device = node_id % N`，写入时条带化

### 4.4 索引构建

```
Stage 1  PQ 训练（GPU/CPU）→ codebook + PQ codes
Stage 2  Vamana 图构建（8 线程）→ graph
Stage 3  后处理 → 入口点 + BFS 重排列
Stage 4  磁盘写入 → _disk.index + 辅助文件
```

**Vamana 关键设计**：
- `build_graph`: 预分配 neighbors[npts×max_slots] + atomic degrees + per-node spinlock
- greedy_search 收集 expanded_pool（所有展开节点）传给 RobustPrune
- **inter_insert 带 re-prune**：反向边满时执行 RobustPrune 保留最优边（非丢弃）
  - 无 re-prune → Recall 72%；有 re-prune → 98.87%
- cleanup pass 最终修剪至 R（inter_insert 维持 slack_R ≈ 1.3R）

## 5. 性能

> SIFT1M (128维, 100万向量), L=100, W=4, i9-12900HX

### 5.1 搜索

**单核 vs 原版**：

| 配置 | aisaq-diskann | PUMP | 加速比 |
|------|--------------|------|--------|
| L=100, 无缓存 | 418 QPS | 1,714 QPS | **4.1x** |

**多核扩展（全缓存）**：8 P-core → 20,367 QPS（线性扩展 96%效率）

**自建索引（R=27）**：

| 缓存 | QPS | Recall@10 |
|------|-----|-----------|
| 100% | 32,000 | 98.87% |
| 10% | 10,800 | 98.76% |

**与原版总对比**：

| 实现 | 条件 | QPS |
|------|------|-----|
| 原版 aisaq-diskann | 24 线程, PM9A1 | 4,358 |
| **PUMP 全缓存** | **8 P-core, 990 PRO** | **20,367** |
| PUMP IO (cache=10K) | 8 P-core | 3,711 |

### 5.2 索引构建

SIFT1M R=27: 251s（PQ 181s + Vamana 70s），GPU PQ 训练 18s。

### 5.3 瓶颈

- **CPU**：process_node 占 65%（PQ 查表 + L2），已接近 -O3 极限
- **IO**：~112 IOs/query，单盘 990 PRO ~670K IOPS，2 核即打满
- **GPU 不适合搜索**：beam search 迭代依赖，每迭代计算量太小

## 6. 配置参数

### 6.1 构建

| 参数 | 默认值 | 说明 |
|------|--------|------|
| R | 64 | 最大图节点度数 |
| L | 100 | 构建时搜索列表大小 |
| alpha | 1.2 | RobustPrune 多样性 |
| n_chunks | ndims（0=auto） | PQ 子空间数 |
| max_candidates | 750 | RobustPrune 候选上限 |
| sample_size | 256000 | k-means 采样数 |
| kmeans_iters | 12 | Lloyd 迭代次数 |
| reorder | true | BFS 重排列 |

### 6.2 搜索

| 参数 | 默认值 | 说明 |
|------|--------|------|
| K | 10 | 返回结果数 |
| L (search_list) | 100 | 搜索队列容量 |
| W (beam_width) | 4 | beam 宽度 |
| concurrent | 512 | 并发查询数 |
| cache_nodes | 10000 | 缓存节点数 |

## 7. 实现进度

### Phase 1 ✅ 核心搜索
PQ 编解码、4K 磁盘布局、Beam Search Pipeline、write/search 子命令、格式转换工具

### Phase 2 ✅ 性能优化 + 多核
PQ 指针预计算、AVX2 SIMD、全局 cache_scheduler、PQ 合并 scheduler ❌放弃、多核 preemptive 分发

### Phase 3 ✅ 多盘条带化
节点级条带、per_core::queue local fast path、base_task bounce

### Phase 4 ✅ 索引构建
GPU PQ 训练（CPU fallback）、Vamana 图构建（inter_insert re-prune）、BFS 重排列、磁盘写入

### Phase 5: 暂不实现
RPC 搜索服务、标签过滤、动态索引。原版也不包含这些功能。

## 8. 文件结构

```
apps/aisaq/
├── main.cc              # 入口（build/write/search）
├── common/types.hh      # 基础类型
├── pq/codebook.hh       # PQ codebook + 距离表
├── index/               # 元数据、节点访问器、索引加载
├── search/              # candidate_queue、visited_set、beam_search pipeline
├── cache/               # global_node_cache + cache_scheduler
├── nvme/                # SPDK 初始化、sector_page、DMA pool
├── runtime/config.hh    # JSON 配置解析
├── build/               # PQ 训练、Vamana 图构建、磁盘写入
├── gpu/                 # CUDA kernels（PQ 训练/编码）
└── tools/               # convert_index、gen_test_index
```

## 9. 运行环境

### NVMe 设备

| PCIe | 型号 | 用途 |
|------|------|------|
| 02:00.0 | 990 PRO | 可用 |
| **03:00.0** | 980 PRO | **系统盘，不能用** |
| 04:00.0 | 980 PRO | 可用 |
| 06:00.0 | 980 PRO | 可用 |

### 注意事项

- 切换单盘/多盘后必须先 write 再 search
- NVMe 测试前建议 `nvme format --ses=1`（未 format 性能降 40%）
- CPU governor 设 `performance`
- Release 编译（Debug -O0 慢 40%）
