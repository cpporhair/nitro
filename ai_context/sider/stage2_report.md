# Sider Stage 2 多核测试报告

> batch_route 跨核批量路由优化 + clean eviction（entry 级 NVMe 备份追踪）。

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
| CPU governor | performance |
| CPU 频率 | 4.4-4.8GHz（每轮间冷却 60 秒） |
| HUGEMEM | 8192 MB |
| Sider commit | 6b232eb |
| 测试工具 | sider_bench（基于 Valkey redis-benchmark，BSD-3-Clause） |
| 测试执行 | `apps/sider/scripts/bench.sh`（参数硬编码，禁止手动拼） |
| 测试标准 | `ai_context/sider/benchmark.md` |

## 测试方案

所有测试通过 `bench.sh` 脚本执行，参数从 `benchmark.md` 硬编码：

```bash
sudo ./apps/sider/scripts/bench.sh hot 1    # 1C 热读
sudo ./apps/sider/scripts/bench.sh cold 4   # 4C 冷读
```

### 固定参数

| 参数 | 热读 | 冷读 |
|------|------|------|
| `--memory` | 512M | 512M |
| `-d` | 256 | 256 |
| `-r` | 1,800,000 | 5,400,000 |
| `-n` SET | 18,000,000 | 54,000,000 |

### 多核客户端配置

| sider | -c | --threads | benchmark 绑核 |
|-------|-----|----------|---------------|
| 1C | 50 | — | 不绑 |
| 2C | 100 | 4 | `taskset -c 8,9,10,11` |
| 4C | 200 | 8 | `taskset -c 8,9,10,11,12,13,14,15` |

sider 核心: 1C=`[0]`, 2C=`[0,2]`, 4C=`[0,2,4,6]`

## Redis 对照（Valkey 8.1.4, 单线程, 同 sider_bench 同参数）

| 场景 | Redis |
|------|-------|
| P32 GET | 1.24M |
| P1 GET | 252K |
| P32 SET | 1.10M |

## 性能数据（纯内存）

### P32 GET

| | Redis | Sider 1C | Sider 2C | Sider 4C |
|--|-------|----------|----------|----------|
| 吞吐 | 1.24M | 1.80M | 3.99M | 6.38M |
| vs Redis | 1.00x | 1.45x | 3.22x | 5.15x |
| scaling | — | 1.00x | 2.22x | 3.55x |

### P1 GET

| | Redis | Sider 1C | Sider 2C | Sider 4C |
|--|-------|----------|----------|----------|
| 吞吐 | 252K | 251K | 444K | 666K |
| vs Redis | 1.00x | 1.00x | 1.76x | 2.64x |
| scaling | — | 1.00x | 1.77x | 2.65x |

## 性能数据（冷读）

sider 配置：`--memory 512M` + 3 NVMe，5.4M key（3 倍热读），约 2/3 冷读比例。
NVMe 启动时 format（非 TRIM），每次测试 SSD 回到出厂状态。
写入验证：redis-cli 采样 key 确认 257 字节（非 NIL）。

### P32 GET（冷）

| | Redis（纯内存） | Sider 1C | Sider 2C | Sider 4C |
|--|---------------|----------|----------|----------|
| 吞吐 | 1.24M | 963K | 1.60M | 2.46M |
| vs Redis | 1.00x | 0.78x | 1.29x | 1.98x |
| scaling | — | 1.00x | 1.66x | 2.55x |

### P1 GET（冷）

| | Redis（纯内存） | Sider 1C | Sider 2C | Sider 4C |
|--|---------------|----------|----------|----------|
| 吞吐 | 252K | 220K | 363K | 571K |
| vs Redis | 1.00x | 0.87x | 1.44x | 2.27x |
| scaling | — | 1.00x | 1.65x | 2.60x |

### 热/冷对比

| 场景 | 1C 热 | 1C 冷 | 冷/热 | 4C 热 | 4C 冷 | 冷/热 |
|------|-------|-------|-------|-------|-------|-------|
| P32 GET | 1.80M | 963K | 0.54x | 6.38M | 2.46M | 0.39x |
| P1 GET | 251K | 220K | 0.88x | 666K | 571K | 0.86x |

P1 冷读影响较小（单请求模式下 NVMe 延迟被 TCP RTT 稀释）。
P32 4C 冷/热比 0.39x（1C 为 0.54x），多核下每核内存更小（128MB），冷读比例更高。

## 验证

- 纯内存：SET 到 OOM 自动停止，GET 全部命中
- 冷读：SET fill BACKPRESSURE 自动重试，写入后 redis-cli 采样验证 key 有数据（257 bytes）
- 每轮测试前冷却 60 秒，确认 CPU 频率 ≥4GHz
- NVMe 每次启动 format，排除 GC 残留影响

## Clean eviction 特性

本版本新增 entry 级 NVMe 备份追踪（commit 092299e）：
- promote 时保留旧 ON_NVME 页引用，entry 记录 nvme_page_id/nvme_slot
- 淘汰时先退回 clean entry 到旧页，全 clean 页直接释放不写 NVMe
- 纯读稳态下 NVMe 写降为零（1C/4C 验证，5 分钟预热后 R2 阶段 write=0）
- P32/P1 吞吐零回归（同环境 A/B 对比确认）

## 已知问题

| 项 | 说明 |
|---|---|
| CPU 热降频 | 笔记本连续测试频率下降。每轮间冷却 60 秒 |
| 冷读 SET fill 尾部卡住 | NVMe 淘汰跟不上写入速率时 BACKPRESSURE 重试导致 rps 降为 0 |
| P32 4C 冷/热比 0.39x | 多核下 per-core 内存 128MB，热集合小，冷读比例高于 1C |
