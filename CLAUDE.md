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

## 参考实现

- `apps/kv/` — 完整 KV 存储引擎，展示五类 scheduler 协作、Leader/Follower 合并、跨域 pipeline 编排
- `apps/aisaq/` — NVMe 向量搜索引擎（DiskANN），展示 SPDK NVMe + pipeline beam search + GPU PQ 训练 + 多核 share-nothing 架构
