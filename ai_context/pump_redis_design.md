# PumpRedis 设计文档

> 请直接在 `[回复]` 标记处填写你的想法，或在任意位置添加评论。

## 1. 项目目标

用 PUMP 框架构建 Redis 兼容的 KV 存储：
- 多核 share-nothing 架构，充分利用多核
- 热数据内存，冷数据 NVMe，扩展容量（**NVMe 纯扩容，不持久化，重启丢数据**）
- 兼容 Redis 协议，可用 redis-cli / redis-benchmark 直连

## 2. 核心设计决策

### 2.1 兼容性范围

**RESP 协议**：实现 RESP2（`+OK`、`$6\r\nfoobar`、`*2\r\n...`），足够支撑 redis-cli 和 redis-benchmark。

**数据类型**：

| 类型 | 优先级 | 说明 |
|------|--------|------|
| String | P0 | GET/SET/DEL/MGET/MSET/INCR/SETNX/APPEND |
| Hash | P1 | HGET/HSET/HDEL/HGETALL |
| List | P1 | LPUSH/RPUSH/LPOP/RPOP/LRANGE/LLEN |
| Set | P2 | SADD/SREM/SMEMBERS/SISMEMBER |
| SortedSet | P2 | ZADD/ZREM/ZRANGE/ZSCORE/ZRANK |

[决定] 先做 String

**Pipeline 支持**：RESP 协议天然支持 pipeline（连续发多条命令，批量返回），建议支持。这也是 benchmark 的关键场景。

[决定] 支持 Pipeline

### 2.2 数据分层 — 内存 + NVMe

**定位**：NVMe 是纯内存扩展（大号 swap），不做持久化，重启数据全丢。

**Value 三层存储**：

| 层 | 条件 | 存储位置 | 淘汰？ |
|----|------|---------|--------|
| inline | val ≤ 48B | hash_entry 内部 | **永不淘汰** |
| slab | val > 48B，热 | per-core slab 分配 | 可淘汰到 NVMe |
| nvme | val > 48B，冷 | NVMe 页面 | 访问时加载回 slab |

**Hash Entry 设计**：

```
hash_entry (per key, 始终在内存):
┌──────────────────────────────────────────────┐
│ key_hash(8) │ key_len(4) │ val_len(4)        │
│ last_access(4) │ type(1) │ encoding(1)       │
│ expire_at(8)                                 │
│ key_ptr(8)  → key 字符串（始终在内存）         │
│ value_union:                                  │
│   encoding=inline → char[48] 直接存值          │
│   encoding=slab   → char* ptr（内存 slab）     │
│   encoding=nvme   → page_id + len（NVMe 上）   │
└──────────────────────────────────────────────┘
```

每个 core 独立一份 hash 表（按 key hash 路由到 core），无跨核共享。

[决定] 根据 key 调度到某个 core

**淘汰策略**：

[决定] **近似 LRU + 仅内存不足时触发**。每个 entry 加 `uint32_t last_access`（秒级时间戳），读写时更新（1 次 store，< 1ns）。内存超阈值时随机采样 8 个 entry，踢 `last_access` 最小的下沉到 NVMe。常态零额外数据结构开销。只淘汰 encoding=slab 的大 value，inline 小值永不淘汰。

**NVMe 层**：

- 复用 `env/scheduler/nvme/` 做 IO（SPDK 直接 IO）
- **不需要** KV 项目的 `fs_scheduler`（太重，有 Leader/Follower、持久化元数据等）
- 自建轻量 page allocator：纯内存 bitmap，启动时全空，4K 页面粒度
- 每个被淘汰的 value 占 ceil(val_len / 4K) 个连续页面
- 无需 fsync / WAL / 元数据持久化 — 重启全丢

**Per-core slab 分配器**：

大小分级：64B / 256B / 1K / 4K / 16K / 64K。每级独立 free list，O(1) 分配释放，无锁。

[回复]

### 2.3 多核架构

**方案 A：连接绑定到 core（推荐）**

```
Core 0: TCP accept → 按连接分配到 Core 1..N
Core 1: TCP 连接组 + 独立 hash 表（key 分片）
Core 2: TCP 连接组 + 独立 hash 表（key 分片）
Core N: TCP 连接组 + 独立 hash 表（key 分片）
```

- 每个 core 拥有一部分 key（按 hash 分片）
- 如果请求的 key 不在当前 core → 跨核路由（per_core::queue）
- 优点：大多数请求本地完成，无跨核开销
- 缺点：MGET 跨分片需要 scatter-gather

**方案 B：TCP core + 计算 core 分离**

```
Core 0-1: TCP 连接，解析 RESP，路由到计算 core
Core 2-N: 计算 core，各自独立 hash 表
```

- TCP 和计算分离
- 所有请求都要跨核一次
- 优点：TCP 处理和计算互不干扰
- 缺点：每个请求必跨核

建议方案 A。redis-benchmark 的典型模式是每连接操作随机 key，跨核路由概率取决于分片粒度。单连接如果操作的 key 分散，可以考虑"命令级路由"（解析完 key 后路由到目标 core，响应路由回来）。

[回复]

### 2.4 连接管理

**TCP accept 模型**：

选项：
1. **单 core accept**：Core 0 accept，round-robin 分配 fd 给其他 core（需跨核传 fd）
2. **SO_REUSEPORT**：每个 core 独立 listen + accept，内核负载均衡

建议选项 1，更可控。accept 频率远低于请求频率，跨核开销可忽略。

[回复]

### 2.5 过期（TTL）

Redis 的 TTL 机制：
- `SET key value EX seconds` / `EXPIRE key seconds`
- 惰性删除：访问时检查是否过期
- 定期删除：后台定时扫描一批 key

建议：
- 每个 hash_entry 带 `expire_at` 字段（0 = 不过期）
- 惰性删除：GET 时检查
- 定期删除：每个 core 的 advance 循环中，每 N 次扫描一小批 key

[回复]

### 2.6 持久化

[决定] **不持久化**。纯缓存模式，重启丢数据。NVMe 只做内存扩展（swap），不做持久存储。

### 2.7 与 KV 项目的关系

| KV 组件 | Redis 是否需要 | 说明 |
|---------|---------------|------|
| nvme_scheduler | ✅ 复用 | NVMe IO 层（SPDK） |
| fs_scheduler | ❌ 不需要 | 太重，自建轻量 page allocator |
| batch_scheduler | ❌ 不需要 | 无事务 |
| index_scheduler | ❌ 不需要 | 自建 hash 表 |
| MVCC / snapshot | ❌ 不需要 | 单 key 原子性 |
| TCP 层 | ✅ 复用 | tcp_scheduler + session |

独立项目 `apps/redis/`，底层直接用 `env/scheduler/nvme/` 和 `env/scheduler/net/tcp/`。

[回复]

## 3. 架构草图

```
                    ┌─────────────┐
                    │   Clients   │
                    └──────┬──────┘
                           │ RESP over TCP
                    ┌──────▼──────┐
                    │  TCP Accept  │  Core 0
                    │  (单点)      │
                    └──────┬──────┘
                           │ fd 分配
              ┌────────────┼────────────┐
              ▼            ▼            ▼
        ┌───────────┐┌───────────┐┌───────────┐
        │  Worker 1  ││  Worker 2  ││  Worker N  │
        │            ││            ││            │
        │ TCP 连接组  ││ TCP 连接组  ││ TCP 连接组  │
        │ RESP 解析   ││ RESP 解析   ││ RESP 解析  │
        │ Hash 表    ││ Hash 表    ││ Hash 表    │
        │ (分片)     ││ (分片)     ││ (分片)     │
        │ Slab 分配器 ││ Slab 分配器 ││ Slab 分配器 │
        │            ││            ││            │
        │ Core 1     ││ Core 2     ││ Core N     │
        └─────┬──────┘└─────┬──────┘└─────┬──────┘
              │             │             │
              ▼             ▼             ▼
        ┌─────────────────────────────────────┐
        │         NVMe Scheduler(s)           │
        │   冷数据读写（纯 swap，重启丢数据）    │
        │   轻量 page allocator（内存 bitmap）  │
        └─────────────────────────────────────┘
```

**请求处理流程（GET 热 key，本地 core）**：
1. TCP 收到 RESP 数据 → 解析出 GET + key
2. `hash(key) % N` → 目标 == 当前 core → 本地查找
3. hash 表命中 → inline/slab value → 编码 RESP 响应 → TCP 发送

**请求处理流程（GET 热 key，跨 core）**：
1. 解析出 key → `hash(key) % N` → 目标 != 当前 core
2. 路由到目标 core（per_core::queue）
3. 目标 core 查找 → 结果路由回来 → 编码 RESP → TCP 发送

**请求处理流程（GET 冷 key）**：
1. hash 表查到 key，encoding=nvme
2. on(nvme_scheduler) → 异步读 4K 页面
3. 回到 worker core → slab 分配 → 缓存到内存 → encoding 改为 slab → 响应

**SET 流程**：
1. 解析 key + value → 路由到目标 core
2. hash 表 upsert → 分配 slab（或 inline）→ 写入 value
3. 如果内存超阈值 → 采样淘汰（异步写 NVMe + 释放 slab）

[回复]

## 4. 待讨论

- [ ] 内存上限配置？超过后的行为（拒绝写入 / 淘汰 / OOM）？
- [ ] 是否支持 SELECT（多 DB）？建议不支持，只保留 DB 0
- [ ] 是否支持 Pub/Sub？建议不支持（首版）
- [ ] 是否支持 Lua 脚本？建议不支持
- [ ] 是否支持集群模式（Cluster）？建议不支持（首版）
- [ ] benchmark 目标？对标 Redis/Dragonfly 的什么指标？

[回复]
