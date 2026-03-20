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
| `kv/` | KV 存储引擎 — Leader/Follower 合并、五类 scheduler 协作 |
| `aisaq/` | NVMe 向量搜索引擎（DiskANN） — SPDK + pipeline beam search + GPU PQ |
| `ai_context/` | 应用层设计文档与分析 |

## 应用层规则

1. 应用之间可以互相**参考**实现模式，但不能互相**依赖**代码
2. 框架修改在 `pump/` submodule 中完成，独立提交到 pump repo
3. 新应用参考 `kv/` 的项目结构和 scheduler 设计模式

## 构建

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## 参考实现

- `kv/` — 完整 KV 存储引擎，展示五类 scheduler 协作、Leader/Follower 合并、跨域 pipeline 编排（详细分析见 `ai_context/kv_analysis.md`）
- `aisaq/` — NVMe 向量搜索引擎（DiskANN），展示 SPDK NVMe + pipeline beam search + GPU PQ 训练 + 多核 share-nothing 架构
