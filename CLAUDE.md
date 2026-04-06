# Nitro — 基于 PUMP 框架的高性能应用

## 框架规范（通过 submodule 加载）
- @pump/CLAUDE.md
- @pump/ai_spec/RUNTIME_MODEL.md
- @pump/ai_spec/SENDERS_DETAIL.md
- @pump/ai_spec/CODING_GUIDE.md
- @pump/ai_spec/RPC_DETAIL.md

## 项目结构

| 目录 | 说明 |
|------|------|
| `pump/` | PUMP 框架（git submodule） |
| `apps/kv/` | KV 存储引擎 — Leader/Follower 合并、五类 scheduler 协作 |
| `apps/sider/` | Redis 兼容 KV 缓存 — NVMe 容量扩展、share-nothing 多核、三级存储 |
| `apps/aisaq/` | NVMe 向量搜索引擎（DiskANN） — SPDK + pipeline beam search + GPU PQ |
| `3rd/` | 第三方依赖 |
| `docs/` | 设计文档与分析 |

## 应用层规则

1. 应用之间可以互相**参考**实现模式，但不能互相**依赖**代码
2. 框架修改在 `pump/` submodule 中完成，独立提交到 pump repo
3. 新应用参考 `apps/kv/` 的项目结构和 scheduler 设计模式

## 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## 性能测试方法论

### 核心原则：测试结果必须可证伪

任何性能数据都要有验证手段。不能只看 benchmark 工具报出的数字就认为正确。

### 三层验证

1. **输入验证 — 数据是否真的写进去了**。benchmark 工具的参数组合可能产生意料之外的行为。写完之后必须采样确认数据存在。这不是测应用的正确性，是测"测试本身是否有效"。

2. **负载验证 — 瓶颈是服务端还是客户端**。多核测试时客户端可能先到瓶颈。增加客户端压力后吞吐还能涨 → 之前没打满；增加后不涨或反降 → 已打满。必须做这个对比才能确认数字代表的是服务端能力。

3. **环境验证 — 硬件状态是否一致**。CPU 频率、governor、核心绑定、热状态。相同代码在不同硬件状态下可以差 2-3 倍。每轮测试前确认环境，各轮间冷却。

### 操作规范

- **测试标准先于测试执行**。先写文档定义参数、流程、验证方法，review 通过后再跑。不要边跑边调参数。
- **基线必须用同一标准重测**。改了测试方法后，历史数据全部作废。必须用新标准重测基线。
- **绝对值不可跨工具对比，比值可以**。不同 benchmark 工具的绝对吞吐不同。跨系统对比只看比值。

### 应用级测试标准

- `apps/sider/` → `ai_context/sider/benchmark.md`

## Sider — Redis 兼容 NVMe 扩展缓存

### 定位

RESP2 协议兼容的 KV 缓存，用 NVMe SSD 透明扩展内存容量。NVMe 纯做容量层，不做持久化——无 FUA/WAL/crash consistency，重启数据全丢。

### 架构

Share-nothing 多核，每核独立运行：

```
TCP (RESP2, io_uring accept)
  → tcp_session_sched (per-core, batch recv/send)
  → store_scheduler (per-core, hash(key)%N 路由)
      - Robin Hood hash table + 7-class slab + page table
      - 三级存储: inline(≤16B) / slab(17B-4KB) / NVMe(冷数据)
      - 水位淘汰(90%/95%) + clean eviction(纯读零写)
      - TTL: lazy check + active scan
  → nvme_scheduler (SPDK, per-core per-disk)
```

### 关键设计

| 特性 | 说明 |
|------|------|
| 三级存储 | inline(entry 内 16B) → slab(DMA 页 7 size class) → NVMe(异步淘汰/促进) |
| Clean eviction | promote 保留旧 NVMe 备份，淘汰时 clean entry 直接退回旧页不写盘，纯读稳态 NVMe 写为零 |
| 背压 | 内存满返回 `-BACKPRESSURE retry N`，零服务端资源开销 |
| Batch pipeline | 一次 unpack 整批 RESP 命令 → concurrent 执行 → 批量响应，P32 吞吐 4C 6.38M ops/s |
| 跨核路由 | batch_route 按目标核心分组，本核 inline 执行，远程核心 ONE store_req，O(num_cores) |

### 文档与测试

- 设计文档: `ai_context/sider/design.md`
- 基准测试标准: `ai_context/sider/benchmark.md`
- 测试脚本: `apps/sider/scripts/bench.sh`（硬编码参数，禁止手动拼命令）
- Stage 2 报告: `ai_context/sider/stage2_report.md`

## Inconel 实现规范

### 设计文档（必须遵循）
- @ai_context/inconel/design_doc/INDEX.md — 分层索引，实现任何 Inconel 功能前先读此文件定位需要查阅的具体章节

### 三阶段文档检查流程

对 `apps/inconel/` 的任何代码编写/修改，必须执行：

1. **实现前** — 读 `INDEX.md` 快速定位表，确定需查阅的文档章节，读完理解约束后再动手
2. **实现中** — 遇到 handle 签名/磁盘格式/pipeline 编排/LSN 语义时，回查对应章节确认
3. **实现后** — 对照 `cross_doc_contracts.md` 验证：handle 签名(§1)、struct 字段(§2)、pipeline 路径(§5)、三条红线

## 参考实现

- `apps/kv/` — 完整 KV 存储引擎，展示五类 scheduler 协作、Leader/Follower 合并、跨域 pipeline 编排
- `apps/sider/` — Redis 兼容 NVMe 扩展缓存，展示 share-nothing 多核、自定义 store_scheduler、batch pipeline、三级存储 + clean eviction
- `apps/aisaq/` — NVMe 向量搜索引擎（DiskANN），展示 SPDK NVMe + pipeline beam search + GPU PQ 训练 + 多核 share-nothing 架构
