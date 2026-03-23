# Sider 开发计划

每个 Phase 可独立测试验收，完成后再进入下一个。

## Stage 1：单核（完成全部业务逻辑）

### Phase 1.1：TCP + RESP2 骨架

**目标**：redis-cli 能连上，PING/PONG 正常

- TCP accept + session（使用框架 TCP scheduler）
- RESP2 unpacker（inline + multibulk 解析）
- 响应格式化（Simple String、Error、Bulk String、Integer）
- 命令分发框架（switch on command type）
- 实现：PING、COMMAND/COMMAND DOCS、CLIENT SETNAME/GETNAME、QUIT、未知命令 → -ERR

**验收**：
- `redis-cli -p 6379` 连接成功，无报错
- `PING` 返回 `PONG`
- 输入任意命令返回 `-ERR unknown command`
- 连续多次 PING（pipelining 基础验证）

---

### Phase 1.2：哈希表 + Slab 分配器

**目标**：核心数据结构就位

- Slab 内存页分配器（size classes: 64B, 128B, 256B, 512B, 1KB, 2KB, 4KB）
  - DMA 安全内存池（spdk_dma_malloc）
  - bitmap 管理 slot 分配/释放
  - 按 size class 维护有空闲 slot 的页列表
- Page table（page_id → {state, mem_ptr, size_class, slot_bitmap, live_count}）
- 哈希表（开放寻址 + Robin Hood）
- Index entry（key_hash, key_data, page_id, slot_index, value_len, ...）

**验收**：
- 单元测试：slab 分配/释放正确，不同 size class 工作正常
- 单元测试：哈希表 insert/lookup/delete 正确，扩容正常
- 单元测试：page table 分配 page_id → 查找 → 释放

---

### Phase 1.3：GET / SET / DEL 命令

**目标**：基本命令正确工作，redis-benchmark 能跑

- GET：查哈希表 → page table → 读 slot → 返回 bulk string 或 nil
- SET：选 size class → 分配 slot → 写 value → 插入/更新哈希表
- SET 更新已有 key：如果 size class 不变 → 原地覆盖；size class 变了 → 释放旧 slot → 分配新 slot
- DEL：释放 slot → 从哈希表删除 → 返回 integer
- 内存记账（按页计数）

**验收**：
- `redis-cli` 手动 SET/GET/DEL 正确
- SET 同一 key 不同大小 value 反复更新，无泄漏
- `redis-benchmark -t set,get -n 100000` 能跑完，结果正确
- 对比 Redis 单线程吞吐

---

### Phase 1.4：TTL 过期

**目标**：SET EX/PX 过期行为正确

- entry 增加 expire_at 字段
- SET 解析 EX/PX 参数
- 惰性过期：GET/DEL 时检查 expire_at
- 主动过期：advance() 中周期性采样扫描
- 过期 key 释放 slab slot + 从哈希表删除

**验收**：
- `SET key val EX 2` → 2 秒后 `GET key` 返回 nil
- `SET key val PX 500` → 500ms 后过期
- 大量设置短 TTL key → 主动过期正常清理（内存不泄漏、slab slot 回收）

---

### Phase 1.5：内存淘汰（纯丢弃）

**目标**：内存满时自动淘汰，不 OOM

- 页级 hotness 跟踪（GET/SET 时更新所在页的 hotness）
- 水位线模型（evict_begin / evict_urgent / evict_max）
- 淘汰选择：采样最冷页
- 淘汰动作（此阶段无 NVMe）：丢弃最冷页上所有 key → 释放哈希表 entry + 释放内存页
- 配置：`--memory` 限制单核内存

**验收**：
- 设置 `--memory 10M`，持续 SET 不同 key → 内存稳定在 10M 附近，不 OOM
- 热 key（反复 GET）所在页不被淘汰，冷页被淘汰
- 水位线行为：低水位空闲淘汰，高水位积极淘汰
- 淘汰后 GET 已淘汰的 key → 返回 nil

---

### Phase 1.6：NVMe 淘汰写入 + DMA 内存池

**目标**：淘汰不再丢弃，而是写入 NVMe 保留；slab 页切换为 DMA 安全内存

#### 1.6a：DMA 内存池 + DPDK lcore 线程

将 slab 页从 `aligned_alloc` 切换为 `spdk_mempool`（DPDK `rte_mempool`）。

- SPDK 环境初始化（`spdk_env_init`，配置核心掩码）
- 通过框架 `share_nothing.hh` 的 DPDK launcher 启动 lcore 线程（per-core cache 依赖 `rte_lcore_id()`）
- `spdk_mempool_create` 创建页池（hugepage DMA 内存 + per-lcore 本地缓存）
- `types.hh` 替换 `alloc_page()`/`free_page()` → `spdk_mempool_get`/`put`，上层零改动
- Phase 1.6 单核运行验证，但逻辑上完整支持多核

#### 1.6b：NVMe 淘汰写入

- NVMe scheduler 初始化（SPDK probe + qpair）
- NVMe 空间分配器（free page 栈，O(1)）
- sider_page 类型（payload 直接指向 slab 内存页，零拷贝 DMA）
- page_table 增加 ON_NVME 状态 + EVICTING 状态，记录 {disk_id, lba}
- 淘汰改为：整页 DMA 写 NVMe（异步）→ 写完回调更新 page_table → 释放内存页
- EVICTING 期间的并发访问处理：
  - GET 命中 EVICTING 页 → 直接读内存（页还在）
  - SET 命中 EVICTING 页上的 key → 正常更新（size class 变则迁移到新页）
  - DEL 命中 EVICTING 页上的 key → 正常删除，live_count--

**验收**：
- DMA 池分配/释放正确，`spdk_mempool_get`/`put` 正常工作
- 设置 `--memory 10M`，持续写入 > 10M 数据 → 淘汰的页写到 NVMe
- NVMe 使用量随淘汰增长
- EVICTING 期间 GET/SET/DEL 正确（手动控制时序或高并发压测）
- 此阶段 GET 已淘汰 key → 返回 nil（冷读尚未实现）
- Phase 2 多核时 DMA 池和 lcore 线程零改动可用

---

### Phase 1.7：冷 GET 读取

**目标**：冷 key 可从 NVMe 读回

- entry 增加 version 字段
- lookup 返回 variant<hot_result, cold_result, nil_result>
- cold_result 携带 {page_id, slot_index, size_class, version}
- 冷 GET pipeline：NVMe 读 4KB 页 → 提取 slot value → promote 到新热 slot（version 校验）
- promote：分配新热 slot → 拷贝 value → 更新 entry 的 page_id/slot_index → 旧 NVMe 页 live_count--
- NVMe 页 live_count 归零 → 释放 NVMe 页

**验收**：
- GET 冷 key 返回正确 value
- GET 冷 key 后变热（再次 GET 不走 NVMe）
- SET 冷 key → 正确更新为热，旧 NVMe 页 live_count 递减
- DEL 冷 key → 正确删除 + NVMe 页 live_count 递减
- 冷读竞态：GET 冷 key 的 NVMe 读期间 SET 同一 key → GET 返回旧值，promote 因 version 不匹配被丢弃，新值不受影响

---

### Phase 1.8：冷热混合压测

**目标**：冷热完整路径跑通，长时间稳定

- 混合压测：GET/SET 随机 key，数据量 >> 内存
- 验证淘汰→NVMe写入→冷读→promote→再淘汰 的完整循环
- 验证 NVMe 页 live_count 生命周期：分配→淘汰写入→冷读/删除递减→归零释放→再分配
- 内存和 NVMe 空间均不泄漏

**验收**：
- `--memory 10M`，写入 100M+ 随机数据，持续 GET/SET 30 分钟
- 内存稳定在 10M 附近
- NVMe 使用量稳定（有分配有释放，不单调增长）
- 无崩溃、无错误响应

---

### Phase 1.9：大 value 支持

**目标**：支持 > 4KB 的 value

- 大 value（> 4KB）不走 slab，独立分配连续内存
- Index entry 标记大 value 标志
- 淘汰大 value：分配多个连续 NVMe 页，DMA 写入
- 冷读大 value：读取多个连续 NVMe 页
- 大 value 不参与页级淘汰（独立淘汰，按 key 的 LRU 选择）

**验收**：
- SET 8KB/16KB/1MB value → GET 返回正确
- 大 value 淘汰到 NVMe → 冷读回来正确
- 大小 value 混合压测正确

---

### Phase 1.10：多盘支持

**目标**：支持多块 NVMe 盘

- 多盘配置（`--nvme dev1,dev2`）
- 选盘策略（轮询 / 容量均衡）
- page_table nvme 字段记录 disk_id
- 边界：单盘满 → 用另一盘；所有盘满 → 降级为纯丢弃

**验收**：
- 两块盘配置，数据分布在两块盘上
- 单盘空间满 → 自动使用另一块盘
- 所有盘满 → 降级为纯淘汰

---

### Phase 1.11：小 value 内联

**目标**：极小 value 不走 slab，减少间接寻址开销

- Index entry 内联存储极小 value（≤16B），不分配 slab slot
- 内联 value 无 page_id/slot_index，直接在 entry 内
- GET/SET 热路径少一次 page table 查找
- 淘汰时内联 value 不占页，不被页级淘汰误伤
- 内联 value 不写 NVMe（太小，不值得 IO）

**验收**：
- 小 value SET/GET 正确
- redis-benchmark 小 value 场景吞吐有提升
- 大 value 行为不变

---

### Phase 1.12：单核稳定性 + 性能基准

**目标**：全功能单核版本，长时间稳定，建立性能基准

- 长时间压测（1 小时+）：混合 GET/SET，不同 value 大小，带 TTL
- 各种边界组合：内存满 + NVMe 满 + TTL 过期 同时发生
- 性能基准：对比 Redis 单线程，记录吞吐和延迟分布

**验收**：
- 1 小时压测：内存稳定、NVMe 空间不泄漏、无崩溃
- 性能数据：对比 Redis 的 QPS 和 p99 延迟

---

## Stage 2：多核扩展

### Phase 2.1：多核启动 + 连接分配

- share-nothing 多核启动（每核独立 scheduler 实例）
- 连接分配：accept 分发到各核 session scheduler
- 每核独立 store_scheduler + NVMe 分区
- 此阶段不做跨核路由：每个连接的 key 只访问本核 store（功能受限但可测）

**验收**：
- N 核启动，每核独立接受连接
- 同一连接内 SET/GET 正确（key 都在本核）
- N 核总吞吐 ≈ N × 单核

---

### Phase 2.2：跨核 key 路由

- key 分片：hash(key) % N → 目标核心
- 跨核请求路由：per_core::queue 投递到目标 store_scheduler
- 响应通过 tcp::send 自动回送（session_scheduler 队列）

**验收**：
- 连接在 core 0，SET key 落在 core 3 → 另一连接在 core 1 GET 同一 key → 正确
- redis-benchmark 多核跑通
- 吞吐线性扩展

---

### Phase 2 已知问题

#### 问题 1：UAF crash（多客户端同时断连）— 已修复

- 根因：`handle_session_error()` 中 `broadcast(on_error)` 同步触发 pipeline 终止 → `conn_state::~conn_state()` delete session → 返回后访问已释放 session
- Session 有两个独立使用者：pipeline（conn_state）和 io_uring read chain（uring_req->user_data）
- 修复：框架新增 `session_lifecycle` 层（`pump/src/env/scheduler/net/common/session_lifecycle.hh`），追踪 `pipeline_active` 和 `read_active` 两个状态，最后一个归零的触发 `do_close + delete`
  - `conn_state::~conn_state()` → `broadcast(pipeline_end)` 清除 pipeline_active
  - CQE handler read chain 结束时 → `broadcast(read_end)` 清除 read_active
  - `handle_session_error()` 只做 `broadcast(on_error) + invoke(do_close)`，不 delete
  - 应用按需将 `session_lifecycle` 组合进 session（不加则 broadcast 自动跳过，不影响其他项目）
- A/B 测试：单核 P32 GET 5.60M → 5.42M（-3%，CPU 频率波动范围内），无性能回归

#### 问题 2：多核跨核路由性能低于单核

- Phase 2.2 双核 P32 GET 吞吐低于单核（4.01M vs 5.33M with bypass）
- FNV-1a `% N` 对 redis-benchmark key 格式分布严重偏斜（实测 4:1，但 Python 验证是 50/50）
- 偏斜导致一个 store 核心过载，另一个空闲
- 需要先解决 hash 分布偏斜问题，才能准确测量跨核路由的真实性能开销

---

### Phase 2.3：多核稳定性

- 跨核淘汰/过期独立运行
- 每核独立 NVMe 分区 + 多盘
- 长时间多核压测

**验收**：
- 8 核 + 2 盘，长时间混合压测稳定
- 对比 Redis：吞吐显著领先

---

## Stage 3：功能完善

- Phase 1 命令集（MGET/MSET/Hash/List/Set/Sorted Set）
- 多 key 命令跨核聚合（DEL k1 k2 k3、MGET）
- 冷读 waiter 合并（重复冷读同一 NVMe 页 → 一次 IO）
- INFO / DBSIZE / SCAN 等运维命令
