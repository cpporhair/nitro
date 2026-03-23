# Sider Stage 2 多核测试报告

> batch_route 跨核批量路由优化后的多核 scaling 基准。作为后续优化（背压等）的回归对照。

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
| Sider commit | cfccaa2 |
| 测试标准 | `ai_context/sider/benchmark.md` |

## 测试参数

### 纯内存

```bash
# sider: --memory 4G, cores 按配置
# SET 填充（10x 覆盖，零 miss）
redis-benchmark -p $PORT -t set -c $C -n 10000000 -P 32 -q -d 256 -r 1000000
# P32 GET
redis-benchmark -p $PORT -t get -c $C -n $N -P 32 -q -d 256 -r 1000000
# P1 GET
redis-benchmark -p $PORT -t get -c $C -n $N -P 1 -q -d 256 -r 1000000
```

### 冷读

```bash
# sider: --memory 512M + 3 NVMe
# SET 填充（10x 覆盖）
redis-benchmark -p $PORT -t set -c 50 -n 30000000 -P 32 -q -d 256 -r 3000000
# GET
redis-benchmark -p $PORT -t get -c $C -n $N -P 32 -q -d 256 -r 3000000
```

### 多核客户端配置

| sider | benchmark 连接 | benchmark 线程 | benchmark 绑核 |
|-------|---------------|---------------|---------------|
| 1C | `-c 50` | 无 | 不绑 |
| 2C | `-c 100` | `--threads 4` | `taskset -c 8,9,10,11` |
| 4C | `-c 200` | `--threads 8` | `taskset -c 8,9,10,11,12,13,14,15` |

sider 核心: 1C=`[0]`, 2C=`[0,2]`, 4C=`[0,2,4,6]`

## 性能数据

### P1 GET

| | Redis | Sider 1C 热 | Sider 1C 冷 | Sider 2C 热 | Sider 2C 冷 | Sider 4C 热 | Sider 4C 冷 |
|--|-------|------------|------------|------------|------------|------------|------------|
| 吞吐 | 268K | 256K | 228K | 444K | 400K | 666K | 615K |
| scaling | — | 1.00x | 1.00x | 1.74x | 1.75x | 2.60x | 2.70x |

### P32 GET

| | Redis | Sider 1C 热 | Sider 1C 冷 | Sider 2C 热 | Sider 2C 冷 | Sider 4C 热 | Sider 4C 冷 |
|--|-------|------------|------------|------------|------------|------------|------------|
| 吞吐 | 1.40M | 2.01M | 1.23M | 3.99M | 2.28M | 7.23M | 3.75M |
| scaling | — | 1.00x | 1.00x | 1.99x | 1.86x | 3.60x | 3.05x |

### 对 Redis 比值（纯内存）

| 场景 | Sider 1C | Sider 4C |
|------|---------|---------|
| P1 GET | 0.96x | 2.49x |
| P32 GET | 1.44x | 5.16x |

### 对 Stage 1.12 单核比值

| 场景 | Stage 1.12 | 当前 1C | 变化 |
|------|-----------|--------|------|
| P1 GET | 256K | 256K | 持平 |
| P32 GET | 1.73M | 2.01M | +16% |
| P32 SET | 1.63M | 1.92M | +18% |

## 验证

- 纯内存：SET 后采样 100% 命中（零 miss 验证通过）
- 冷读：SET 后采样 100% 命中（512M + 3 NVMe, -r 3M）
- 功能正确性：跨核 SET/GET/DEL 验证通过

## 已知问题

| 项 | 说明 |
|---|---|
| CPU 热降频 | 笔记本连续测试频率下降，各轮间需冷却 30 秒。4C 热 P32 冷却后 7.23M vs 降频时 6.38M |
| 冷读 batch path | batch_route 中冷读通过 launch_cold_read 子 pipeline 处理，pending_state 原子计数器追踪完成 |
| sync discard | 内存满时 set() 同步丢弃整页 key（待修复），当前测试条件下未触发 |
| batch_send 跨核 | 远程 batch 完成后 batch_send 可能在非 session 核心执行，tcp::send 跨核安全但多一次 per_core::queue hop |
