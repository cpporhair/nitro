# Sider

用 NVMe 扩展 Redis 容量，同时保持内存级速度。

Redis 的数据必须全部放在内存中——内存不够，数据就放不下。Sider 把冷数据透明地淘汰到 NVMe SSD，热数据留在内存直接访问，冷数据按需从 NVMe 异步读回。对客户端完全透明，协议兼容 RESP2。

基于 PUMP 异步管道框架，share-nothing 多核架构，SPDK 用户态 NVMe 驱动。4 核热读 6.38M ops/s，冷读 2.46M ops/s。纯读稳态下 NVMe 写为零（clean eviction）。

## 开发方式

代码由 [Claude Code](https://claude.ai/claude-code) (Opus) 编写。人类负责架构决策、需求定义、方案取舍和性能验证；AI 负责实现、调试和测试。PUMP 框架为预先存在的人类作品。

## 架构

```
客户端 (RESP2)
  │
  ▼
TCP accept (io_uring) ── 连接分配到 store core（round-robin）
  │
  ▼
batch_recv ── 一次 recv 解析多条命令（cmd_batch）
  │
  ▼
batch_route ── 按 hash(key) % N 分组，本核 inline / 远程核 ONE store_req
  │
  ▼
┌─────────────────────────────────────────┐
│  store_scheduler (per-core, lock-free)  │
│                                         │
│  hash_table ── Robin Hood open-addr     │
│  slab       ── 7 size classes (64B-4KB) │
│  page_table ── IN_MEMORY / EVICTING / ON_NVME │
│  eviction   ── water-level async NVMe   │
└─────────────────────────────────────────┘
  │
  ▼
NVMe scheduler (SPDK, per-core qpair) ── 冷数据落盘 / 冷读回填
```

### 三级存储

| 层 | 条件 | 延迟 |
|----|------|------|
| inline | value ≤ 16B | 直接在 hash entry 中 |
| slab (hot) | value ≤ 4KB, 内存充足 | DMA 页内 slot 寻址 |
| NVMe (cold) | 被淘汰到盘 | async read + promote (clean eviction 零写) |
| large | value > 4KB | 连续 DMA 页 |

### 背压与淘汰

- **60% (begin)**: 空闲时启动 async NVMe 淘汰
- **90% (urgent)**: 每轮 advance 启动 8 次 async 淘汰
- **100% (max)**: 有 NVMe 返回 `-BACKPRESSURE retry N`（客户端等待后重试），无 NVMe 返回 `-ERR OOM`
- 32 次 aggressive async 淘汰

### Clean eviction

entry 级 NVMe 备份追踪：promote 时保留旧 ON_NVME 页引用，淘汰时 clean entry 直接退回旧页，不写 NVMe。纯读稳态下 NVMe 写降为零。

设计文档: `ai_context/sider/clean_eviction_plan.md`
背压设计: `ai_context/sider/backpressure.md`

## 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

编译选项: `-O3 -DNDEBUG -std=gnu++26 -march=native -flto`

## 运行

```bash
# 纯内存模式（无 NVMe）
./build/sider --memory 4G --port 6379

# NVMe 模式（需 SPDK + sudo）
sudo PCI_BLOCKED="0000:03:00.0" /path/to/spdk/scripts/setup.sh
sudo ./build/sider --config apps/sider/sider.json
```

### 配置文件

```json
{
  "port": 6379,
  "memory": "512M",
  "evict_begin": 60,
  "evict_urgent": 90,
  "accept_core": 0,
  "cores": [0, 2, 4, 6],
  "nvme": ["0000:02:00.0", "0000:04:00.0", "0000:06:00.0"]
}
```

### CLI 参数

```
--port N          监听端口 (默认 6379)
--memory SIZE     内存限制 (如 4G, 512M, 0=无限制)
--config PATH     JSON 配置文件
--evict-begin N   淘汰开始阈值 % (默认 60)
--evict-urgent N  紧急淘汰阈值 % (默认 90)
--nvme ADDRS      NVMe PCIe 地址，逗号分隔
```

## 支持的命令

| 命令 | 说明 |
|------|------|
| `GET key` | 获取值（hot/inline 直接返回，cold 走 NVMe 异步读） |
| `SET key value [EX s] [PX ms]` | 设置值（支持过期时间） |
| `DEL key` | 删除 |
| `PING` | 返回 PONG |
| `QUIT` | 关闭连接 |
| `COMMAND` / `CLIENT` | 兼容性占位 |

## 性能

测试环境: Intel i9-12900HX, 128GB DDR5, 3x Samsung 980 PRO NVMe, GCC 15.2.1
测试执行: `sudo ./apps/sider/scripts/bench.sh <hot|cold> <1|2|4>`

### 纯内存 (--memory 512M, 无 NVMe)

| 场景 | Redis (Valkey 8.1) | Sider 1C | Sider 2C | Sider 4C |
|------|-------------------|----------|----------|----------|
| P32 GET | 1.24M | 1.80M | 3.99M | 6.38M |
| P1 GET | 252K | 251K | 444K | 666K |
| P32 SET | 1.10M | — | — | — |

### 冷读 (--memory 512M + 3 NVMe, -r 5400000, ~2/3 冷读比例)

| 场景 | Redis（纯内存） | Sider 1C | Sider 2C | Sider 4C |
|------|---------------|----------|----------|----------|
| P32 GET | 1.24M | 963K | 1.60M | 2.46M |
| P1 GET | 252K | 220K | 363K | 571K |

Sider 用 1/3 的内存承载 3 倍数据量，2/3 请求走 NVMe 冷读。**2 核起 P1 冷读超越 Redis 纯内存，4 核 P32 冷读达 Redis 2 倍**。

纯读稳态下 NVMe 写为零（clean eviction，5 分钟预热后验证）。

测试标准: [benchmark.md](../../ai_context/sider/benchmark.md)
测试报告: [stage2_report.md](../../ai_context/sider/stage2_report.md)
Clean eviction 设计: [clean_eviction_plan.md](../../ai_context/sider/clean_eviction_plan.md)

## 目录结构

```
apps/sider/
├── main.cc              # 入口：runtime 初始化、TCP accept、NVMe 设备
├── config.hh            # 配置解析（JSON / CLI）
├── test_store.cc        # 存储引擎单元测试（66 tests）
├── sider.json           # 示例配置
├── store/
│   ├── types.hh         # size class、page 常量、DMA 分配器接口
│   ├── entry.hh         # hash entry（48B, inline value union）
│   ├── hash_table.hh    # Robin Hood open-addressing hash table
│   ├── page_table.hh    # page 状态机（FREE→IN_MEMORY→EVICTING→ON_NVME）
│   ├── slab.hh          # slab 分配器（7 size classes, partial page list）
│   ├── store.hh         # kv_store: GET/SET/DEL、过期扫描、淘汰
│   ├── scheduler.hh     # store_scheduler: PUMP 自定义 scheduler
│   └── scheduler_impl.hh # advance(): 请求处理、淘汰水位线
├── resp/
│   ├── parser.hh        # RESP2 命令解析
│   ├── batch.hh         # cmd_batch + resp_slot（零拷贝响应，含 BACKPRESSURE 类型）
│   ├── response.hh      # RESP2 序列化
│   └── unpacker.hh      # TCP ring buffer 解包
├── server/
│   ├── session.hh       # TCP session 组合（layers）
│   └── handler.hh       # 命令分发、batch_route、NVMe 冷读、背压
├── bench/               # sider_bench 测试工具（基于 Valkey，BSD-3-Clause）
│   ├── sider-benchmark.c # 主文件（新增 BACKPRESSURE 重试 + OOM 退出）
│   └── ...              # Valkey 依赖文件
└── nvme/
    ├── init.hh          # SPDK 环境初始化、DMA 内存池
    ├── page.hh          # sider_page（PUMP NVMe page_concept）
    └── allocator.hh     # LBA 分配器（bitmap + MPMC 中央池）
```

## 测试

```bash
# 单元测试
cmake --build build --target sider_test_store
./build/sider_test_store

# 构建 sider_bench
cmake --build build --target sider_bench

# 基准测试（参考 ai_context/sider/benchmark.md）
# 热读
./build/sider_bench -h 127.0.0.1 -p 6379 -t set -c 50 -n 18000000 -P 32 -q -d 256 -r 1800000
./build/sider_bench -h 127.0.0.1 -p 6379 -t get -c 50 -n 3000000 -P 32 -q -d 256 -r 1800000

# 冷读（需 NVMe，sider_bench 遇 BACKPRESSURE 自动重试）
./build/sider_bench -h 127.0.0.1 -p 6379 -t set -c 50 -n 54000000 -P 32 -q -d 256 -r 5400000
./build/sider_bench -h 127.0.0.1 -p 6379 -t get -c 50 -n 3000000 -P 32 -q -d 256 -r 5400000
```
