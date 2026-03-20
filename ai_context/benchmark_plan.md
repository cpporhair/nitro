# AiSAQ Benchmark 测试计划

## 测试目标

PUMP AiSAQ vs 原版 aisaq-diskann，公正对比搜索性能（QPS + Recall）。

## 测试环境

| 项目 | 配置 |
|------|------|
| CPU | i9-12900HX (8P+8E), performance governor |
| 内存 | 64GB DDR5 |
| NVMe | Samsung 990 PRO 1TB (02:00.0) — 单盘测试 |
| GPU | RTX 3070 Ti Laptop（仅用于索引构建 PQ 训练） |
| OS | Linux 6.19, Arch |
| 编译 | Release (-O3 -march=native) |

**测试前准备**：
- 3 块数据盘 `nvme format --ses=1`（已完成）
- CPU governor 设为 performance（已完成）
- 无残留进程（`pkill apps.aisaq`）

## 测试矩阵

**数据集**：SIFT1M（128 维，100 万向量，10000 查询）

| 配置 | R | n_chunks | spn | 说明 |
|------|---|----------|-----|------|
| R=27 | 27 | 128 | 1 | 单 sector/节点 |
| R=56 | 56 | 128 | 2 | 双 sector/节点 |

**搜索参数**：K=10, W=4, L=10,20,30,50,100

**对比双方**：

| 实现 | 线程 | IO 引擎 | 缓存 | 盘数 |
|------|------|---------|------|------|
| PUMP | 8 P-core | SPDK | cache=0（纯 IO） | 1 盘 (02:00.0) |
| aisaq-diskann | 8 线程, taskset 绑 P-core | io_uring | cache=0, pq_cache=0 | 1 盘 (02:00.0) |

> 双方都绑定相同的 8 个 P-core（`taskset -c 0,2,4,6,8,10,12,14`），消除 E-core 调度差异。原版用 `--use_aisaq` 模式（PQ 按需从磁盘读取，与 PUMP 的 DRAM-free 模式对等）。

## 测试步骤

### Step 0: 环境准备

```bash
sudo cpupower frequency-set -g performance
# 3 盘已 format
# SPDK 已绑定 (02:00.0, 04:00.0, 06:00.0)
pkill apps.aisaq 2>/dev/null; pkill search_disk_index 2>/dev/null
```

### Step 1: 构建 PUMP 索引（R=27 + R=56）

```bash
# R=27, GPU PQ
./cmake-build-release/apps/apps.aisaq /tmp/config_bench_r27.json build \
  test_data/aisaq-diskann-build/build/data/sift/sift_base.fbin

# R=56, GPU PQ
./cmake-build-release/apps/apps.aisaq /tmp/config_bench_r56.json build \
  test_data/aisaq-diskann-build/build/data/sift/sift_base.fbin
```

构建参数：L=100, alpha=1.2, max_candidates=750, n_chunks=128, sample_size=256K, kmeans_iters=12, GPU PQ。

### Step 2: 原版索引

已有预构建索引：
- `test_data/sift1m_b03_r27/index` — R=27
- `test_data/sift1m_b03_r56/index` — R=56

验证与我们参数一致（n_chunks=128, medoid 相同）。

### Step 3: PUMP 搜索测试

每个 R 值，先 write 到 NVMe，再跑不同 L 值的搜索。**cache=0，纯 NVMe IO**。

```bash
# 搜索配置模板（cache=0，单盘）
{
  "index_prefix": "/tmp/bench_rXX/sift1m",
  "main_core": 16,
  "search": { "k": 10, "search_list": L, "beam_width": 4, "num_probes": 1, "query_concurrent": 512 },
  "cache": { "max_nodes": 0 },
  "nvme": [ { "name": "nvme0", "pcie": "0000:02:00.0", "qpair_depth": 1024 } ],
  "cores": [
    { "id": 16, "cache": {} },
    { "id": 0,  "task": {}, "nvme": { "devices": ["nvme0"] } },
    ...8 P-cores...
  ]
}
```

对每个 R: write → search L=10,20,30,50,100（各跑 3 次取中位数）。

### Step 4: 原版 aisaq-diskann 搜索测试

```bash
SEARCH=test_data/aisaq-diskann-build/build/apps/search_disk_index

taskset -c 0,2,4,6,8,10,12,14 \
  $SEARCH --data_type float --dist_fn l2 \
  --index_path_prefix test_data/sift1m_b03_rXX/index \
  --query_file test_data/sift1m/query.fbin \
  --gt_file test_data/sift1m/gt100 \
  --recall_at 10 \
  -L 10 20 30 50 100 \
  -W 4 -T 8 \
  --use_aisaq \
  --pq_read_io_engine uring \
  --pq_cache_size 0 \
  --num_nodes_to_cache 0 \
  --result_path /tmp/res_orig_rXX
```

> 注意：原版 aisaq 模式需要 NVMe 在内核驱动下（不能在 SPDK 下）。所以原版测试和 PUMP 测试需要**交替进行 SPDK reset/bind**。

### Step 5: 交替测试流程

由于 PUMP 用 SPDK、原版用内核驱动，同一块盘不能同时被两者使用。测试顺序：

1. **SPDK 模式**：绑定 → PUMP write R=27 → PUMP search R=27 × 5个L → PUMP write R=56 → PUMP search R=56 × 5个L
2. **内核模式**：reset SPDK → 原版 search R=27 × 5个L → 原版 search R=56 × 5个L → 重新绑定 SPDK

原版索引不需要 write（直接从文件系统读）。

### Step 6: 结果汇总

输出表格：

| R | L | 实现 | QPS | Recall@10 | Mean IOs | 备注 |
|---|---|------|-----|-----------|----------|------|

绘制 Recall-QPS 曲线对比。

## 注意事项

- 每个搜索跑 3 次取中位数 QPS（消除波动）
- 原版用 `-T 8` 限制 8 线程（与 PUMP 8 P-core 对等）
- 原版 `--use_aisaq` + `--pq_cache_size 0` + `--num_nodes_to_cache 0`（无缓存）
- PUMP `cache: { max_nodes: 0 }`（无缓存）
- GT 文件：`test_data/sift1m/gt100`（10000 查询 × 100 近邻）
- 原版 aisaq 模式下，磁盘 IO 走 io_uring；PUMP 走 SPDK。这是设计差异，不是不公平——评测的就是不同 IO 引擎的效果
