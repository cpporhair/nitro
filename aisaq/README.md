# AiSAQ — DiskANN on PUMP

NVMe 直通的高性能向量搜索引擎。基于 DiskANN/Vamana 算法，使用 PUMP 框架的 SPDK NVMe scheduler 实现用户态 DMA 直读，搜索全程无 syscall。

AiSAQ 是完全由claude code实现的项目,单纯就代码来说,人类只写了几个pump pipeline针对AiSAQ特殊场景的最佳实践示例.这些例子没有出现在任何提交中

这是 claude code 根据项目周期的对话,自己总结的经验心得 [claude-code-best-practices.md](claude-code-best-practices.md)

## 性能

SIFT1M, 8 P-core (i9-12900HX), 3 盘条带化, 无缓存, vs aisaq-diskann (同硬件同绑核):

| R | L | QPS | Recall@10 | vs 原版 |
|---|---|-----|-----------|--------|
| 27 | 50 | 25,253 | 96.4% | **11.6x** |
| 27 | 100 | 16,367 | 98.8% | **11.3x** |
| 56 | 50 | 16,722 | 98.8% | **13.1x** |
| 56 | 100 | 10,374 | 99.7% | **12.8x** |

详细数据和场景分析见 [benchmark_report.md](benchmark_report.md)。

## 快速开始

### 编译

```bash
cmake --build cmake-build-release --target apps.aisaq

# GPU PQ 训练需要编译 CUDA kernel
nvcc --ptx apps/aisaq/gpu/kernels.cu -o cmake-build-release/apps/aisaq_kernels.ptx
```

### 构建索引

```bash
./apps.aisaq config_build.json build base_vectors.fbin
```

`config_build.json`:
```json
{
  "index_prefix": "path/to/index",
  "build": {
    "R": 27, "L": 100, "alpha": 1.2,
    "n_chunks": 128, "sample_size": 256000,
    "gpu_device": 0,
    "ptx_path": "cmake-build-release/apps/aisaq_kernels.ptx"
  },
  "cores": [
    { "id": 0, "task": {} }, { "id": 2, "task": {} },
    { "id": 4, "task": {} }, { "id": 6, "task": {} }
  ]
}
```

### 写入 NVMe + 搜索

```bash
# SPDK 绑定（排除系统盘）
sudo PCI_ALLOWED="0000:02:00.0" /usr/bin/spdk-setup

# 写入
sudo ./apps.aisaq config_search.json write

# 搜索
sudo ./apps.aisaq config_search.json search query.fbin [groundtruth]
```

`config_search.json`:
```json
{
  "index_prefix": "path/to/index",
  "main_core": 16,
  "search": { "k": 10, "search_list": 100, "beam_width": 4, "query_concurrent": 512 },
  "cache": { "max_nodes": 100000 },
  "nvme": [ { "name": "nvme0", "pcie": "0000:02:00.0" } ],
  "cores": [
    { "id": 16, "cache": {} },
    { "id": 0, "task": {}, "nvme": { "devices": ["nvme0"] } },
    { "id": 2, "task": {}, "nvme": { "devices": ["nvme0"] } }
  ]
}
```

## 架构

```
query → precompute PQ dist_table
      → beam search loop (协程早停):
          select W unexpanded → cache hit? → direct process
                              → cache miss? → SPDK NVMe DMA read
          → process_node: PQ distance (inline codes) + exact L2
          → update candidates
      → top-K results
```

- **SPDK NVMe**：用户态 DMA，bypass 内核，per-core qpair 无锁
- **Inline PQ**：邻居 PQ 码嵌入节点 sector，搜索完全 DRAM-free
- **Share-nothing**：每核独立 scheduler，preemptive 跨核分发
- **多盘条带化**：`node_id % N` 路由，线性扩展 IO 带宽

## 关键参数

| 参数 | 构建 | 搜索 | 说明 |
|------|------|------|------|
| R | 27/56 | - | 图节点度数。R=27 单 sector，R=56 双 sector |
| L | 100 | 10-100 | 候选队列大小。越大 Recall 越高，QPS 越低 |
| W | - | 4 | beam 宽度（并发 IO 数） |
| n_chunks | 128 | - | PQ 子空间数 |
| cache | - | 0-1M | 缓存节点数。100% 缓存 QPS 约 3x |

## 文件格式

兼容 [aisaq-diskann](https://github.com/antifreeze53/aisaq-diskann) 索引格式，可加载其构建的索引（需 `tools/convert_index` 转换磁盘布局为 4K 对齐）。

自建索引输出：
```
{prefix}_disk.index                    # 主索引（4K 对齐节点 sector）
{prefix}_pq_pivots.bin                 # PQ codebook
{prefix}_pq_compressed.bin             # PQ 压缩码
{prefix}_disk.index_medoids.bin        # 入口点
{prefix}_disk.index_centroids.bin      # medoid 向量
{prefix}_disk.index_entry_points.bin   # 多入口点
```
