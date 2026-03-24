# Sider Stage 2 多核测试报告

> batch_route 跨核批量路由优化后的多核 scaling 基准。

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
| CPU 频率 | 4.4-4.7GHz（每轮测试前确认，冷却 30 秒） |
| HUGEMEM | 8192 MB |
| Sider commit | 6c776e8 |
| 测试工具 | sider_bench（基于 Valkey redis-benchmark，BSD-3-Clause） |
| 测试标准 | `ai_context/sider/benchmark.md` |

## 测试方案

### 设计原则

热读/冷读使用**相同内存配置** `--memory 512M`，唯一变量是 key 数量：
- 热读 1.8M key（填满 ~90% 内存），无 NVMe，OOM 后自动停止
- 冷读 5.4M key（3 倍），有 NVMe，超出部分淘汰到 NVMe

### 纯内存（热读）

```bash
# sider: --memory 512M, 无 NVMe
sider_bench -h 127.0.0.1 -p $PORT -t set -c $C --threads $T -n 18000000 -P 32 -q -d 256 -r 1800000
sider_bench -h 127.0.0.1 -p $PORT -t get -c $C --threads $T -n $N -P 32 -q -d 256 -r 1800000
sider_bench -h 127.0.0.1 -p $PORT -t get -c $C --threads $T -n $N -P 1 -q -d 256 -r 1800000
```

### 冷读

```bash
# sider: --memory 512M + 3 NVMe（3 倍 key，~2/3 冷读比例）
sider_bench -h 127.0.0.1 -p $PORT -t set -c $C --threads $T -n 54000000 -P 32 -q -d 256 -r 5400000
# 验证写入：redis-cli GET key:000000500000 确认有数据
sleep 5  # 等淘汰稳定
sider_bench -h 127.0.0.1 -p $PORT -t get -c $C --threads $T -n $N -P 32 -q -d 256 -r 5400000
sider_bench -h 127.0.0.1 -p $PORT -t get -c $C --threads $T -n $N -P 1 -q -d 256 -r 5400000
```

### 多核客户端配置

| sider | -c | --threads | benchmark 绑核 |
|-------|-----|----------|---------------|
| 1C | 50 | — | 不绑 |
| 2C | 100 | 4 | `taskset -c 8,9,10,11` |
| 4C | 200 | 8 | `taskset -c 8,9,10,11,12,13,14,15` |

sider 核心: 1C=`[0]`, 2C=`[0,2]`, 4C=`[0,2,4,6]`

## 性能数据（纯内存）

### P32 GET

| | Sider 1C | Sider 2C | Sider 4C |
|--|----------|----------|----------|
| 吞吐 | 1.79M | 4.00M | 6.37M |
| scaling | 1.00x | 2.24x | 3.56x |

### P1 GET

| | Sider 1C | Sider 2C | Sider 4C |
|--|----------|----------|----------|
| 吞吐 | 241K | 444K | 666K |
| scaling | 1.00x | 1.84x | 2.76x |

### P32 SET

| | Sider 1C |
|--|----------|
| 吞吐 | 1.78M |

## 性能数据（冷读）

sider 配置：`--memory 512M` + 3 NVMe，5.4M key（3 倍热读），约 2/3 冷读比例。

写入验证：redis-cli 采样 key 确认 257 字节（非 NIL），BACKPRESSURE 自动重试生效。

### P32 GET（冷）

| | Sider 1C | Sider 2C | Sider 4C |
|--|----------|----------|----------|
| 吞吐 | 995K | 2.00M | 3.55M |
| scaling | 1.00x | 2.01x | 3.57x |

### P1 GET（冷）

| | Sider 1C | Sider 2C | Sider 4C |
|--|----------|----------|----------|
| 吞吐 | 249K | 400K | 615K |
| scaling | 1.00x | 1.61x | 2.47x |

### 热/冷对比

| 场景 | 1C 热 | 1C 冷 | 冷/热 | 4C 热 | 4C 冷 | 冷/热 |
|------|-------|-------|-------|-------|-------|-------|
| P32 GET | 1.79M | 995K | 0.56x | 6.37M | 3.55M | 0.56x |
| P1 GET | 241K | 249K | 1.03x | 666K | 615K | 0.92x |

P32 冷读约为热读的 56%。P1 冷读影响较小（单请求模式下 NVMe 延迟被 TCP RTT 稀释）。

## 验证

- 纯内存：SET 到 OOM 自动停止，GET 全部命中
- 冷读：SET fill BACKPRESSURE 重试（BP=43），写入后 redis-cli 采样验证 key 有数据
- 每轮测试前确认 CPU 频率 ≥4GHz，测试中监控频率稳定

## 已知问题

| 项 | 说明 |
|---|---|
| CPU 热降频 | 笔记本连续测试频率下降。每轮间冷却 30 秒，测前确认频率 |
| 冷读 SET fill 尾部卡住 | NVMe 淘汰跟不上写入速率时 BACKPRESSURE 重试导致 rps 降为 0，最终完成但耗时更长 |
| P32 冷/热比 0.56x | 低于预期 0.70x，待分析。可能与冷读路径额外开销（DMA 分配 + NVMe RTT + promote）有关 |

## 变更说明

- 测试工具：sider_bench（基于 Valkey redis-benchmark，BSD-3-Clause，新增 BACKPRESSURE 重试 + OOM 退出）
- 背压响应：有 NVMe 返回 `-BACKPRESSURE retry 10`，无 NVMe 返回 `-ERR OOM`
- 测试方案：热读/冷读使用相同 `--memory 512M`，唯一变量是 key 数量（公平对比）
