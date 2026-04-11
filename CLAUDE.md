# Nitro — 基于 PUMP 框架的高性能应用

## 最高优先级规则 — Production 实现阶段禁止读测试

**当你在实现或修改 production 代码以落地某个功能/修复时，禁止打开或搜索测试文件**：`test_*.cc`、`*_test.cc`、`tests/` 下任何文件、测试 fixture、benchmark 文件。

**如果你的当前角色是设计者、reviewer、测试维护者，或者任务本身就是修失败测试 / 补测试 / 校对测试语义，则允许读测试文件**；但必须先显式说明"我要读测试文件"，不要把“我顺手看一下测试”藏在实现流程里。

**所有结构决策必须对应设计文档或已有生产代码**，不能对应"能通过现有测试的最简结构"；即使允许读测试的场景，也不能拿测试反推 spec。

**背景**：把测试数据反推成 spec 是我的 fatal failure mode——inconel B+ tree 曾被我硬编码两层骗过所有测试，debt 累积到分支不可推进。规则防不住"我以为 spec 就是 tests"——所以对 production 实现阶段，要物理上切断测试文件可见性；而设计/review/补测试阶段则可以读测试，但仍不能让测试替代 spec。

**新增约束 A — 收窄实现必须显式声明并 fail-fast**：如果当前实现只覆盖特定 tree 形态、固定深度、特定 scheduler 拓扑、单一 batch 形态或其他受限前提，必须在命名、注释、类型或返回状态中显式写出限制；一旦输入超出覆盖范围，必须 fail-fast（返回 `unsupported_*`、抛错或断言失败，按模块层级选择），禁止 silent fallback，禁止"先按最常见 case 跑通"。

**新增约束 B — 通用命名必须对应通用语义**：只有满足设计文档定义的完整语义，才能使用 `tree_lookup`、`flush_phase2`、`write_restructured`、`range_scan`、`frontier_switch` 这类通用命名；如果只是阶段 seam、owner-local helper 或 shape-specific implementation，名字里必须带限制词，例如 `*_single_leaf_only`、`*_direct_leaf_root_only`、`*_root_stable_only`。

**新增约束 C — 设计缺口时禁止自行补最简 spec**：如果设计文档和已有生产代码不足以唯一决定结构，必须停下来说明缺口并请求确认；禁止用"最容易实现"、"最像当前 fixture"、"先满足当前输入形态"的结构自行补 spec。

---

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

### v1 语义（必须遵循）
- `v1` 对 Inconel 只表示**功能范围冻结**，不表示质量标准降低。允许少做未纳入范围的能力，但凡已经纳入范围、已经进入 canonical path、或已经落到 production 代码里的能力，都必须按 production-grade 标准实现；禁止用“v1 先糊一个能跑的版本”当理由降低正确性、容量边界、异常路径、回收、背压或热路径效率标准。

### 三阶段文档检查流程

对 `apps/inconel/` 的任何代码编写/修改，必须执行：

1. **实现前** — 读 `INDEX.md` 快速定位表，确定需查阅的文档章节，读完理解约束后再动手
2. **实现中** — 遇到 handle 签名/磁盘格式/pipeline 编排/LSN 语义时，回查对应章节确认
3. **实现后** — 对照 `cross_doc_contracts.md` 验证：handle 签名(§1)、struct 字段(§2)、pipeline 路径(§5)、三条红线

## 参考实现

- `apps/kv/` — 完整 KV 存储引擎，展示五类 scheduler 协作、Leader/Follower 合并、跨域 pipeline 编排
- `apps/sider/` — Redis 兼容 NVMe 扩展缓存，展示 share-nothing 多核、自定义 store_scheduler、batch pipeline、三级存储 + clean eviction
- `apps/aisaq/` — NVMe 向量搜索引擎（DiskANN），展示 SPDK NVMe + pipeline beam search + GPU PQ 训练 + 多核 share-nothing 架构
