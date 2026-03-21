# Sider 基准测试注意事项

## 测试环境

- CPU: i9-12900HX（笔记本，容易热降频）
- OS: Linux 6.19.6-zen1-1-zen
- 编译: `-O3 -DNDEBUG -march=native -flto`（CMake Release）
- CPU governor: 必须设为 performance
  ```bash
  echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
  ```
- NVMe 绑定（冷读测试需要）:
  ```bash
  sudo HUGEMEM=4096 PCI_ALLOWED="0000:02:00.0 0000:04:00.0 0000:06:00.0" /home/null/work/kv/spdk/scripts/setup.sh
  ```
- **0000:03:00.0 是 OS 盘，禁止绑定**

## 标准测试命令

### 纯内存 P32 GET（主基准）

```bash
# SET: 必须 -d 256
redis-benchmark -p 6379 -t set -c 50 -n 3000000 -P 32 -q -d 256

# GET: -c 50 -P 32, 无 --threads
redis-benchmark -p 6379 -t get -c 50 -n 2000000 -P 32 -q -r 3000000
```

### Redis 对照组

```bash
redis-server --port 6380 --daemonize yes --save "" --appendonly no --loglevel warning

# 同样参数
redis-benchmark -p 6380 -t set -c 50 -n 3000000 -P 32 -q -d 256
redis-benchmark -p 6380 -t get -c 50 -n 2000000 -P 32 -q -r 3000000
```

### 2/3 冷读（NVMe）

```bash
# 512M 内存 + 6M keys × 256B ≈ 1.5GB → 约 2/3 在 NVMe
sudo ./build/sider --config apps/sider/sider.json
redis-benchmark -p 6379 -t set -c 50 -n 6000000 -P 32 -q -d 256
redis-benchmark -p 6379 -t get -c 50 -n 2000000 -P 32 -q -r 6000000
```

## 参数说明

| 参数 | 值 | 原因 |
|------|-----|------|
| `-d` | **256** | Sider 优势在 batch pipeline 发送端，小值（≤16B）发送开销趋零、slab 间接寻址劣势暴露，测不出真实优势 |
| `-c` | 50 | redis-benchmark 默认值 |
| `-P` | 32 | pipeline 深度，体现 batch 优势 |
| `-r` | 3000000 | 随机 key 范围，避免单 key 热缓存假象 |
| `--threads` | 不加 | 单线程 benchmark，减少客户端侧干扰 |
| `-n` | ≥1000000 | 足够长，让 rps 稳定 |

## 常见陷阱

1. **`-d 3`（默认值）会让 Sider 比 Redis 慢**：3B 值在 slab SC_64 里，GET 路径多两次间接寻址（page_table + slab ptr），而 Redis 用 SDS embstr 内联存储，查找后数据就在手边。batch pipeline 的发送端优势在小响应（~350B/batch）下体现不出来。

2. **连续跑多轮会热降频**：i9-12900HX 笔记本 CPU 连续负载后频率从 4.9GHz 降到 <1GHz。降频影响绝对值但不影响比值。如果比值异常，不是降频问题，去查参数。

3. **不带 `-r` 的 GET 是单 key 热缓存测试**：所有 client 读同一个 key，hash table entry + slab page 常驻 L1。可以测极限吞吐但不代表真实工作负载。

4. **测试后清理进程**：`pkill sider; pkill redis-server`，残留进程占端口导致下次启动失败。

## 历史基准数据（commit abf8f40, 2026-03-21）

| 场景 | Redis | Sider | 比值 |
|------|-------|-------|------|
| P1 GET 纯内存 | 258K | 257K | 1.00x |
| P32 GET 纯内存（无 -r） | 2.87M | 4.87M | 1.70x |
| P32 GET 纯内存（-r 3M, -d 256） | 3.34M | 4.44M | 1.33x |
| P32 GET 2/3冷读 3盘NVMe | 3.24M | 3.60M | 1.11x |
