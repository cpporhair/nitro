# Sider 基准测试标准

## 0. 执行方式

**所有测试必须通过 `bench.sh` 脚本执行，禁止手动拼命令。**

```bash
sudo ./apps/sider/scripts/bench.sh <hot|cold|set|backpressure> <1|2|4>
```

脚本硬编码本文档所有参数，输出包含实际参数和 CPU 频率。测试报告必须从脚本输出提取数据，不得手动填写。

## 1. 环境要求

| 项 | 要求 | 检查命令 |
|---|------|---------|
| CPU governor | **performance** | `cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor` |
| CPU 频率 | 确认未降频（≥3GHz） | `cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq` |
| 编译 | Release `-O3 -DNDEBUG -march=native -flto` | `cmake -B build -DCMAKE_BUILD_TYPE=Release` |
| 清理 | 测试前后 `pkill sider; pkill redis-server` | 残留进程占端口 |
| NVMe 绑定 | 冷读测试需要 SPDK | 见第 7 节 |

```bash
# 设置 governor（每次重启后需重设）
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

## 2. 工具选择：sider_bench

所有测试统一使用 **sider_bench**（基于 Valkey redis-benchmark，BSD-3-Clause）。

```bash
cmake --build build --target sider_bench -j$(nproc)
```

**为什么不用 redis-benchmark / memtier_benchmark**：
- redis-benchmark：遇到任何非 MOVED/ASK/CLUSTERDOWN 的错误直接 `exit(1)`，无法测试背压场景
- memtier_benchmark：Hits/Misses 统计不准（NIL 也算 hit），无 BACKPRESSURE 重试，GPL 许可

**sider_bench 的改进**：
- 遇到 `-BACKPRESSURE retry N` 自动等待并重试，保证 SET 最终写入成功
- 输出显示 `BP=N`（backpressure 事件计数）
- CLI 与 redis-benchmark 完全兼容（`-h -p -c -n -P -d -r -t -q` 等参数相同）

## 3. 参数规范

### 3.1 `-r` 规则（最重要）

不带 `-r` 时，key 是字面字符串 `key:__rand_int__`（不替换）。

**SET 和 GET 必须都带 `-r`，且范围一致。违反此规则的数据无效。**

### 3.1.1 零 miss 原则

除非特别说明，任何性能测试（包括 Redis 和 Sider）都必须保证 **GET 没有 miss**。
即：GET 的 `-r` 范围内的 key 必须在 SET 阶段全部写入，且未被淘汰。

**保证覆盖**：`-r N -n M` 随机写入时，每个 key 被写到的概率 = 1-(1-1/N)^M。
要求 SET 的 `-n` ≥ 10 × `-r`（10 倍过采样），使漏写概率 < 0.005%。

验证方法：SET 完成后，用 `redis-cli` 采样 GET 确认命中率 100%。

### 3.2 固定参数

| 参数 | 值 | 原因 |
|------|-----|------|
| `-d` | **256** | 小值（≤16B）走 inline 存储，测不出 slab + batch 优势 |
| `-r` | **1800000**（热）/ **5400000**（冷） | 热读填 ~90% 内存，冷读 3 倍（超出部分淘汰到 NVMe） |
| `-P` | **32** 或 **1** | P32 测 batch 吞吐，P1 测单请求延迟 |

### 3.3 多核客户端配置

单线程 benchmark 打多核 sider 会成为客户端瓶颈。

| sider 核心 | -c | --threads | benchmark 绑核 |
|-----------|-----|----------|---------------|
| 1C | 50 | 无 | 不绑 |
| 2C | 100 | 4 | `taskset -c 8,9,10,11` |
| 4C | 200 | 8 | `taskset -c 8,9,10,11,12,13,14,15` |

**绑核原则**：sider 用 `[0,2,4,6]`，benchmark 绑 `[8-15]`（不同物理核）。
禁止绑到 `[1,3,5,7]`——和 sider 是同一物理核的超线程兄弟。

### 3.4 运行量（保证 ≥2 秒稳态）

| 核心数 | P32 GET `-n` | P1 GET `-n` |
|--------|-------------|-------------|
| 1C | 3,000,000 | 500,000 |
| 2C | 4,000,000 | 1,000,000 |
| 4C | 8,000,000 | 2,000,000 |

## 4. 测试场景

所有场景通过 `bench.sh` 执行。脚本自动处理进程清理、SPDK 绑定/解绑、写入验证、参数填入。

### 场景 A：纯内存 GET（主基准）

`--memory 512M`，不挂 NVMe。1.8M key × 256B ≈ 填满 ~90% 内存。SET 遇到 OOM 自动退出。

```bash
sudo ./apps/sider/scripts/bench.sh hot 1   # 1C
sudo ./apps/sider/scripts/bench.sh hot 4   # 4C
```

### 场景 B：纯内存 SET

```bash
sudo ./apps/sider/scripts/bench.sh set 1   # 1C
sudo ./apps/sider/scripts/bench.sh set 4   # 4C
```

### 场景 C：冷读 GET（需 NVMe）

`--memory 512M` + NVMe。5.4M key（3 倍热读），超出内存部分淘汰到 NVMe。热读/冷读使用相同内存配置，唯一变量是 key 数量。

```bash
sudo ./apps/sider/scripts/bench.sh cold 1   # 1C
sudo ./apps/sider/scripts/bench.sh cold 4   # 4C
```

脚本自动：绑定 SPDK → 启动 sider（含 NVMe format）→ SET fill → 验证写入 → 等淘汰稳定 → P32 GET → P1 GET → 清理 → 解绑 SPDK。

### 场景 D：背压测试

`--memory 10M` + NVMe。

```bash
sudo ./apps/sider/scripts/bench.sh backpressure 1
```

**验证项**（脚本自动输出）：
- sider_bench 输出中 `BP=N` 大于 0
- 有 NVMe 时：SET 全部完成
- 无 NVMe 时：sider_bench 遇 OOM 自动退出

## 5. 多核配置

sider 核心配置（`cores` 选不同物理核，避免 HT 兄弟）：

| 核心数 | cores | accept_core |
|--------|-------|-------------|
| 1C | `[0]` | 0 |
| 2C | `[0, 2]` | 0 |
| 4C | `[0, 2, 4, 6]` | 0 |

## 6. Redis 对照

```bash
redis-server --port 6380 --daemonize yes --save "" --appendonly no --loglevel warning
# 用完全相同的 sider_bench 参数，改 -p 6380
pkill redis-server
```

## 7. NVMe 环境（冷读测试）

```bash
# 绑盘（03:00.0 是 OS 盘，绝对不碰）
sudo HUGEMEM=8192 PCI_ALLOWED="0000:02:00.0 0000:04:00.0 0000:06:00.0" spdk-setup config

# 测完恢复
sudo PCI_ALLOWED="0000:02:00.0 0000:04:00.0 0000:06:00.0" spdk-setup reset
```

## 8. 已知陷阱

| # | 陷阱 | 症状 | 正确做法 |
|---|------|------|---------|
| 1 | SET 不加 `-r` | GET 全 miss，测 NIL 速度 | SET 和 GET 都加 `-r` |
| 2 | SET/GET 都不加 `-r` | 同 key L1 cache 命中，数字虚高 3 倍 | 加 `-r` |
| 3 | benchmark 不绑核 | 客户端和 sider 抢 CPU | `taskset` 绑独立物理核 |
| 4 | benchmark 绑 HT 兄弟核 | 同物理核争抢，性能反降 | 绑 `[8-15]` 不要 `[1,3,5,7]` |
| 5 | powersave governor | 频率不稳 | 设 performance |
| 6 | 连续多轮热降频 | 绝对值下降 | 每轮间冷却 30 秒，测前确认频率 ≥3GHz |
| 7 | 多核用单线程 benchmark | 客户端瓶颈 | 加 `--threads` |
| 8 | `-d 3`（默认） | slab 间接寻址劣势，batch 优势不明显 | 用 `-d 256` |
| 9 | 冷读测试不验证写入 | GET 全打 NIL，测的是 NIL 速度不是冷读 | SET 后 redis-cli 采样确认 key 有数据 |
| 10 | 无 NVMe 时 BACKPRESSURE 无限重试 | sider_bench 卡住不动 | 有 NVMe 才跑背压/冷读场景，或 Ctrl+C 中断 |
| 11 | 手动拼命令改参数 | 参数与标准不一致，报告数据不可信 | 必须用 `bench.sh`，禁止手动拼 |

## 9. 基准数据

### Stage 1.12 单核基线（commit d956653, 2026-03-23）

测试条件：`-r 1000000 -n_set 10000000 -d 256`，governor=performance，零 miss 验证通过。

> 注：此基线使用 redis-benchmark 测量。后续基线使用 sider_bench 重测。

| 场景 | Redis (Valkey 8.1) | Sider Stage 1.12 | Sider/Redis |
|------|-------|--------|------|
| P1 GET | 267K | 262K | 0.98x |
| P32 GET | 1.39M | 1.75M | 1.26x |
| P32 SET | 1.24M | 1.64M | 1.33x |
