# Sider 基准测试标准

## 1. 环境要求

| 项 | 要求 | 检查命令 |
|---|------|---------|
| CPU governor | **performance** | `cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor` |
| 编译 | Release `-O3 -DNDEBUG -march=native -flto` | `cmake -B build -DCMAKE_BUILD_TYPE=Release` |
| 清理 | 测试前后 `pkill sider; pkill redis-server` | 残留进程占端口 |
| NVMe 绑定 | 冷读测试需要 SPDK | 见第 6 节 |

```bash
# 设置 governor（每次重启后需重设）
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

## 2. redis-benchmark 参数规范

### 2.1 `-r` 规则（最重要）

redis-benchmark 不带 `-r` 时，key 是字面字符串 `key:__rand_int__`（不替换）。

**SET 和 GET 必须都带 `-r`，且范围一致。违反此规则的数据无效。**

### 2.1.1 零 miss 原则

除非特别说明，任何性能测试（包括 Redis 和 Sider）都必须保证 **GET 没有 miss**。
即：GET 的 `-r` 范围内的 key 必须在 SET 阶段全部写入，且未被淘汰。

**保证覆盖**：`-r N -n M` 随机写入时，每个 key 被写到的概率 = 1-(1-1/N)^M。
要求 SET 的 `-n` ≥ 10 × `-r`（10 倍过采样），使漏写概率 < 0.005%。

验证方法：SET 完成后，采样 GET 确认命中率 100%。如有 miss，加大 SET 的 `-n` 或减小 `-r`。

```bash
# ✅ 正确：SET -r 3000000 → 创建 3M 不同 key → GET -r 3000000 → 随机读
redis-benchmark -t set ... -r 3000000
redis-benchmark -t get ... -r 3000000

# ❌ 错误 1：SET 无 -r → 只写一个 key → GET 有 -r → 全 miss → 测 NIL 速度
# ❌ 错误 2：SET 和 GET 都无 -r → 同一 key 反复读 → L1 cache → 虚高 3 倍
```

### 2.2 固定参数

| 参数 | 值 | 原因 |
|------|-----|------|
| `-d` | **256** | 小值（≤16B）走 inline 存储，测不出 slab + batch 优势 |
| `-r` | **1000000** | 1M key 范围（~340MB 工作集，远超 L3 30MB） |
| `-P` | **32** 或 **1** | P32 测 batch 吞吐，P1 测单请求延迟 |

### 2.3 多核客户端配置

单线程 benchmark 打多核 sider 会成为客户端瓶颈。

| sider 核心 | benchmark 连接 | benchmark 线程 | benchmark 绑核 |
|-----------|---------------|---------------|---------------|
| 1C | `-c 50` | 无 `--threads` | 不绑（单核不争抢） |
| 2C | `-c 100` | `--threads 4` | `taskset -c 8,9,10,11` |
| 4C | `-c 200` | `--threads 8` | `taskset -c 8,9,10,11,12,13,14,15` |

**绑核原则**：sider 用 `[0,2,4,6]`，benchmark 绑 `[8-15]`（不同物理核）。
禁止绑到 `[1,3,5,7]`——和 sider 是同一物理核的超线程兄弟。

### 2.4 运行量（保证 ≥2 秒稳态）

| 核心数 | P32 GET `-n` | P1 GET `-n` |
|--------|-------------|-------------|
| 1C | 3,000,000 | 500,000 |
| 2C | 4,000,000 | 1,000,000 |
| 4C | 8,000,000 | 2,000,000 |

## 3. 测试场景

### 场景 A：纯内存 GET（主基准）

sider 配置：`--memory 4G`（足以放 1M×256B ≈ 340MB，无淘汰）

```bash
# SET 填充（-n = 10× -r，保证全覆盖）
redis-benchmark -p $PORT -t set -c 50 -n 10000000 -P 32 -q -d 256 -r 1000000
# P32 GET
redis-benchmark -p $PORT -t get -c 50 -n 3000000 -P 32 -q -d 256 -r 1000000
# P1 GET
redis-benchmark -p $PORT -t get -c 50 -n 500000 -P 1 -q -d 256 -r 1000000
```

### 场景 B：纯内存 SET

```bash
redis-benchmark -p $PORT -t set -c 50 -n 3000000 -P 32 -q -d 256 -r 1000000
```

### 场景 C：冷读 GET（需 NVMe）

sider 配置：`--memory 512M` + NVMe（3M keys × 256B ≈ 750MB > 512M → 部分在 NVMe）

```bash
# SET 填充（-r 3M, -n = 10× -r）
redis-benchmark -p $PORT -t set -c 50 -n 30000000 -P 32 -q -d 256 -r 3000000
# P32 GET
redis-benchmark -p $PORT -t get -c 50 -n 3000000 -P 32 -q -d 256 -r 3000000
```

**注意**：此场景零 miss 原则例外。高压 SET 时 sync discard 会丢弃部分 key（已知问题，待修复）。
测试后采样验证命中率，要求 ≥ 85%。命中率需记录在报告中。

## 4. 多核配置

sider 核心配置（`cores` 选不同物理核，避免 HT 兄弟）：

| 核心数 | cores | accept_core |
|--------|-------|-------------|
| 1C | `[0]` | 0 |
| 2C | `[0, 2]` | 0 |
| 4C | `[0, 2, 4, 6]` | 0 |

## 5. Redis 对照

```bash
redis-server --port 6380 --daemonize yes --save "" --appendonly no --loglevel warning
# 用完全相同的 benchmark 参数
pkill redis-server
```

## 6. NVMe 环境（冷读测试）

```bash
# 绑盘（03:00.0 是 OS 盘，绝对不碰）
sudo HUGEMEM=8192 PCI_ALLOWED="0000:02:00.0 0000:04:00.0 0000:06:00.0" spdk-setup config

# 测完恢复
sudo PCI_ALLOWED="0000:02:00.0 0000:04:00.0 0000:06:00.0" spdk-setup reset
```

## 7. 已知陷阱

| # | 陷阱 | 症状 | 正确做法 |
|---|------|------|---------|
| 1 | SET 不加 `-r` | GET 全 miss，测 NIL 速度 | SET 和 GET 都加 `-r` |
| 2 | SET/GET 都不加 `-r` | 同 key L1 cache 命中，数字虚高 3 倍 | 加 `-r` |
| 3 | benchmark 不绑核 | 客户端和 sider 抢 CPU | `taskset` 绑独立物理核 |
| 4 | benchmark 绑 HT 兄弟核 | 同物理核争抢，性能反降 | 绑 `[8-15]` 不要 `[1,3,5,7]` |
| 5 | powersave governor | 频率不稳 | 设 performance |
| 6 | 连续多轮热降频 | 绝对值下降 | 比值稳定则正常；间隔冷却 |
| 7 | 多核用单线程 benchmark | 客户端瓶颈 | 加 `--threads` |
| 8 | `-d 3`（默认） | slab 间接寻址劣势，batch 优势不明显 | 用 `-d 256` |

## 8. 基准数据

### Stage 1.12 单核基线（commit d956653, 2026-03-23）

测试条件：`-r 1000000 -n_set 10000000 (10x覆盖) -d 256`，governor=performance，零 miss 验证通过。

| 场景 | Redis (Valkey 8.1) | Sider Stage 1.12 | Sider/Redis |
|------|-------|--------|------|
| P1 GET | 267K | 262K | 0.98x |
| P32 GET | 1.39M | 1.75M | 1.26x |
| P32 SET | 1.24M | 1.64M | 1.33x |
