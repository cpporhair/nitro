# Sider — NVMe-Extended Redis-Compatible Cache

## 1. 定位

纯缓存，定位与 Redis 一致。关机丢数据，不做持久化，不做存储。
核心价值：用 NVMe 扩展缓存容量到 TB 级，同时利用多核达到 Redis 同级别或更高的吞吐。

## 2. 兼容性

### 2.1 协议
- RESP2（Redis Serialization Protocol v2）
- 支持 inline 命令格式（redis-cli 交互模式）和 multibulk 格式（程序化客户端）
- 支持 pipelining：客户端可在一次 TCP send 中打包多条命令，服务端按序逐条处理并按序返回响应（redis-benchmark 默认使用 pipelining，Phase 0 必须支持）
- 现有 Redis 客户端（redis-cli、jedis、redis-py 等）可直接连接使用
- 不支持的命令统一返回 `-ERR unknown command` 错误响应（不断开连接）

### 2.2 命令 — 分阶段支持

**Phase 0（最小可运行，性能对比基准）：**

| 命令 | 语义 |
|------|------|
| `GET key` | 返回 value 或 nil |
| `SET key value` | 设置 key-value |
| `SET key value EX seconds` | 设置 + TTL（秒） |
| `SET key value PX milliseconds` | 设置 + TTL（毫秒） |
| `DEL key [key ...]` | 删除 |
| `PING` | 连通性检查 |
| `QUIT` | 关闭连接 |
| `COMMAND` / `COMMAND DOCS` | 客户端握手（返回最小兼容响应即可） |
| `CLIENT SETNAME` / `CLIENT GETNAME` | redis-cli 连接时可能发送，接受但可忽略 |

Phase 0 目标：能用 redis-cli 连上，跑 GET/SET，与 Redis 正面对比吞吐和延迟。

**Phase 1（常用子集）：**

| 类别 | 命令 |
|------|------|
| String | `MGET` `MSET` `INCR` `DECR` `INCRBY` `DECRBY` `APPEND` `STRLEN` `SETNX` `SETEX` `PSETEX` `GETSET` `GETDEL` |
| Key | `EXISTS` `TTL` `PTTL` `EXPIRE` `PEXPIRE` `PERSIST` `TYPE` `KEYS`(pattern) `SCAN` `RANDOMKEY` `RENAME` `UNLINK` |
| Hash | `HGET` `HSET` `HDEL` `HGETALL` `HMGET` `HMSET` `HEXISTS` `HLEN` `HKEYS` `HVALS` `HINCRBY` `HSCAN` |
| List | `LPUSH` `RPUSH` `LPOP` `RPOP` `LLEN` `LRANGE` `LINDEX` `LSET` `LREM` |
| Set | `SADD` `SREM` `SMEMBERS` `SISMEMBER` `SCARD` `SRANDMEMBER` `SPOP` `SUNION` `SINTER` `SDIFF` |
| Sorted Set | `ZADD` `ZREM` `ZSCORE` `ZRANK` `ZRANGE` `ZRANGEBYSCORE` `ZCARD` `ZCOUNT` `ZINCRBY` |
| Server | `DBSIZE` `FLUSHDB` `FLUSHALL` `INFO` `SELECT` `CONFIG GET` |

**不支持（明确排除）：**
- Lua 脚本（EVAL/EVALSHA）
- 事务（MULTI/EXEC/WATCH）— 多核 share-nothing 与事务语义冲突
- Pub/Sub — 不在缓存核心路径
- Cluster 协议 — 单机
- 持久化命令（BGSAVE/BGREWRITEAOF）
- Stream、HyperLogLog、Geo 等扩展类型

## 3. 数据模型

### 3.1 类型

| 类型 | Phase 0 | Phase 1 |
|------|---------|---------|
| String（bytes） | Y | Y |
| Hash | - | Y |
| List | - | Y |
| Set | - | Y |
| Sorted Set | - | Y |

值为任意 bytes，与 Redis 一致。单个 value 最大大小待定（Redis 默认 512MB，我们可以先限制更小）。

### 3.2 TTL / 过期

- 支持 key 级别的 TTL（毫秒精度）
- 惰性过期：访问时检查，过期则删除返回 nil
- 主动过期：后台周期性扫描清理过期 key（避免内存/NVMe 空间泄漏）

### 3.3 淘汰策略

缓存必须处理容量满的情况：
- 至少支持 LRU 或近似 LRU（与 Redis 的 `allkeys-lru` 对标）
- NVMe 空间满时淘汰冷 key

## 4. 架构约束

### 4.1 PUMP 框架
- 基于 PUMP sender/pipeline 构建
- share-nothing 多核架构，每核独立处理
- 无锁，无共享可变状态

### 4.2 多核模型
- key 按 hash 分片到各核心，每核独立持有一部分 key 空间
- 客户端连接可以落在任意核心，跨核请求通过 per-core SPSC queue 路由
- 单个 key 的所有操作由同一核心处理（天然串行，无锁）
- 每核独立内存记账，内存上限 = 总上限 / N 核，淘汰由各核独立决策

### 4.3 跨核请求与响应保序
- 连接核心解析命令后，按 key hash 路由到目标核心执行
- Pipelining 保序：连接核心维护 slot reorder buffer，每条请求分配 slot 序号，响应回来写入对应 slot，从头扫描连续已完成 slot 依次写 TCP 响应
- 多 key 命令（DEL k1 k2 k3、MGET 等）：拆分为多个单 key 请求分发到各核心，汇聚结果后组装响应

### 4.4 网络
- TCP，使用 PUMP 的 TCP scheduler
- RESP2 协议解析在 TCP 接收路径上完成

### 4.5 配置
- 命令行参数：`--port --memory --evict-begin --evict-urgent --nvme --cores`
- 示例：`sider --port 6379 --memory 8G --evict-begin 60 --evict-urgent 90 --nvme /dev/nvme1n1,/dev/nvme2n1 --cores 8`
- `--evict-begin` / `--evict-urgent`：内存水位百分比，控制后台淘汰节奏（见 6.4）

## 5. 性能目标

### 5.1 吞吐
- 单核：≥ Redis 单线程吞吐（~10-15 万 QPS，GET/SET 混合）
- 多核线性扩展：N 核 ≈ N × 单核吞吐
- 目标：8 核机器上 GET/SET 混合 ≥ 100 万 QPS

### 5.2 延迟
- 内存命中：p99 < 100μs（对标 Redis）
- NVMe 命中：p99 < 200μs（含 NVMe 读延迟）
- 对比参考：Redis p99 通常在 100-300μs

### 5.3 容量
- 远超物理内存：TB 级 NVMe 上的有效缓存容量
- 内存用于索引 + 热数据缓存

## 6. 存储模型：内存为主，NVMe 溢出

```
┌───────────────────────────────┐
│         Memory (热数据)        │  ← 容量 = 可配置内存上限
│   hash index (全量 key)        │
│   value (热 key 的 value)      │
└──────────┬────────────────────┘
           │ 内存满时，按 LRU 选冷 key 的 value 淘汰到 NVMe
           ▼
┌──────────┬──────────┬──────────┐
│  NVMe 0  │  NVMe 1  │  NVMe N  │  ← 多盘，容量 = 所有盘之和
│  冷 value │  冷 value │  冷 value │
└──────────┴──────────┴──────────┘
```

### 6.1 读写路径

| 操作 | 热 key（value 在内存） | 冷 key（value 在 NVMe） |
|------|----------------------|------------------------|
| GET | 内存读 ~1μs，与 Redis 一致 | 内存 index 定位 → NVMe 读 ~10-20μs → value 留内存（提升为热） |
| SET | 内存写 ~1μs，与 Redis 一致 | 更新 index + 内存写 value（变热）；如内存满则先淘汰一个冷 key 到 NVMe |
| DEL | 内存删 index + value/NVMe 位置回收 | 同左 |

### 6.2 关键语义

- **index 始终在内存**：全量 key 的 hash index 常驻内存，保证任何 key 的查找都是 O(1) 内存操作
- **value 位置二态**：每个 key 的 value 要么在内存，要么在 NVMe，由 index entry 中的标志位区分
- **SET 总是写内存**：新写入/更新的 value 始终放内存（热数据），如果该 key 之前在 NVMe 则释放 NVMe 空间
- **GET 冷 key 提升为热**：读到的 value 留在内存，标记为热。NVMe 上的旧副本惰性回收（淘汰时空间自然复用），无额外 NVMe 写开销。提升可能触发淘汰（内存满时先淘汰另一个冷 key）

### 6.3 多盘支持

- 支持配置多块 NVMe 盘，总冷数据容量 = 所有盘容量之和
- 每块盘对应独立的 NVMe scheduler 实例
- 淘汰写入时选盘策略：轮询 / 容量均衡（优先写剩余空间多的盘）
- 读取时由 index entry 记录所在盘 + 偏移，直接定位
- 单盘故障：缓存场景不需要冗余，丢失该盘上的冷数据即可（等同于 key 被淘汰）

### 6.4 淘汰策略

**水位线模型（每核独立）：**

| 水位 | 默认值 | 行为 |
|------|--------|------|
| 低水位 `evict_begin` | 用户配置 | 后台开始淘汰，仅在 advance 空闲时执行，对前台零影响 |
| 高水位 `evict_urgent` | 用户配置 | 加速淘汰，每轮 advance 积极淘汰多个 key |
| 硬上限 `evict_max` | 高水位 + 固定余量 | 前台 SET 触发同步淘汰（保底，正常运行不应到达） |

- 淘汰粒度：单个 key 的 value（从内存移到 NVMe）
- 淘汰选择：近似 LRU（类 Redis 采样淘汰：随机采样 N 个 key，淘汰最久未访问的）
- 后台淘汰通过 pipeline 异步写 NVMe，不阻塞前台请求路径
- NVMe 空间也满时：直接丢弃最冷 key（内存 + NVMe 都删），等同于 Redis 内存满时的 eviction

## 7. 基准测试计划

### 7.1 工具
- `redis-benchmark`（Redis 自带，标准对比工具）
- `memtier_benchmark`（更灵活的负载生成）

### 7.2 对比场景

| 场景 | 说明 |
|------|------|
| SET only | 纯写吞吐 |
| GET only（全命中） | 纯读吞吐，热数据 |
| GET only（冷数据） | 数据量 > 内存，测 NVMe 读性能 |
| GET/SET 1:1 | 混合负载 |
| GET/SET 9:1 | 典型读多写少 |
| 不同 value 大小 | 64B / 256B / 1KB / 4KB / 16KB |
| 不同并发连接数 | 1 / 10 / 100 / 1000 |

### 7.3 指标
- QPS（吞吐）
- 延迟分布：avg / p50 / p95 / p99 / p999
- 内存占用
- CPU 利用率

## 8. 非目标（明确不做）

- 持久化 / 数据安全
- 集群 / 分布式
- 主从复制
- Lua 脚本
- 事务
- ACL / 认证（Phase 0 不做，后续可加）

## 9. 开放问题（设计阶段解决）

1. **内存索引结构** — 开放寻址 hash？每核独立 hash table？
2. **NVMe 空间管理** — slab 分配？log-structured？
3. **大 value 处理** — 跨多个 NVMe page 的 value 如何管理？
4. **RESP2 解析** — 零拷贝解析可行性？与 TCP ring buffer 的集成方式？
5. **NVMe 惰性空间回收** — 冷 key 被 GET 提升为热后，NVMe 旧副本何时回收？淘汰时覆盖写 or 后台 GC？
6. **value 最大大小** — Redis 默认 512MB，我们限制多少？影响 NVMe 空间管理设计
