# Inconel — Shadow CoW B+ Tree 持久化 KV 引擎

> 基于 [PUMP](../../pump/) sender/scheduler 框架 + SPDK，面向 NVMe SSD 的多核 share-nothing 持久化 KV 引擎。

Inconel 把三件通常互相冲突的事放进同一个系统模型里：

```
per-front-scheduler ingest path   （前台写延迟接近 value durable + WAL FUA）
+ single ordered index            （读路径最终只面对一棵有序 B+ Tree，而非多层 SST）
+ crash-safe durability           （同一 batch 原子提交，crash 后不丢 ACK 过的数据）
```

它的目标场景是：**NVMe SSD + 多核 share-nothing + pipeline runtime + 有序 point read / range scan**。

---

## 设计要点

| 要点 | 一句话 |
|------|--------|
| [Shadow CoW B+ Tree](#shadow-cow-b-tree) | 后台单棵有序树；普通更新先在节点自己的 shadow range 内滚动写，不级联改父指针 |
| [全异步、无锁、share-nothing](#全异步无锁share-nothing) | 正确性靠「单线程 owner scheduler + 唯一状态归属」，热路径无锁无 latch |
| [多 batch 并发 in-flight](#多-batch-并发-in-flight) | coord 顺序定序，fan-out 后自由交错，durable_lsn 用 ready-bitmap 填连续前缀 |
| [WAL group commit](#wal-group-commit) | front idle 时把排队 fragment 聚合进同一 plan，FUA 次数从 per-batch 收敛到 per-group |
| [Value leader-follower 合并](#value-leader-follower-合并) | 并发 batch 的 PUT 合并分配、单次 FUA 落盘，再分发各自 value_ref |
| [per-front 分片](#per-front-分片same-key--same-front) | 写按 key_hash 固定路由 front，读按 shard 固定路由 read_domain，同 key 无并发竞争 |
| [value-before-WAL 提交语义](#提交语义value-before-wal) | value durable → WAL durable → memtable → publish，crash 后引用一定有盘 |

下面逐点展开。

---

## Shadow CoW B+ Tree

Inconel 不想接受三类经典方案在这个场景下的短板：

| 方案 | 优点 | 在本场景的短板 |
|------|------|----------------|
| 就地修改 B+ Tree | 读好、结构直观 | 页原地更新需 undo/redo、doublewrite、latch，与 NVMe + share-nothing 冲突 |
| LSM-Tree | 前台写吞吐高 | compaction 是全局后台重写，读查多层，range read / 空间放大重 |
| 传统 CoW B+ Tree | 无 torn-page | 普通更新也触发从叶子到 root 的路径拷贝级联 |

**Shadow CoW B+ Tree** 是 Inconel 区别于以上三者的全部价值来源，名字对应三个设计点：

- **B+ Tree** — 后台最终形态是一棵有序树，point lookup 和 range scan 都围绕它定义。
- **CoW** — 节点更新不覆盖旧页，写新版本；tree 页天然规避 torn-page，不需要数据页 WAL，crash safety 靠 `CoW + CRC`。
- **Shadow** — 一个逻辑节点占一个预留的 `shadow range`（含多个 page slot），新版本优先写到同一 range 的下一个 slot。父节点保存的是 range 的 **base**，不是当前生效 slot 的精确物理地址。

```
一个节点有一串 shadow pages / slots
普通 CoW 先在这串影子页里滚动写
只有写满了，才搬家 (consolidation)
```

关键不是“页会复制”，而是把 CoW 从「每次都改父指针」变成「先在本节点自己的 shadow range 内消化」——**普通 leaf update 不级联到父节点**。这正是它同时绕开「LSM 全局 compaction」「原地页更新的 latch / doublewrite」「传统 CoW 路径级联」三件事的支点。

> ⚠️ 两条顶层 invariant，违反任一则失去整个项目的存在理由：
> - **A（不级联）**：leaf 走 next-slot（`range_base` 不变）时**不得**触发父节点重建。
> - **B（child_base 是 range_base）**：internal page 里的 `child_base` 必须是 child 的 `range_base`，不是当前 slot paddr；lookup 端经 `manifest->resolve(child_base)` 过 slot_map 取 current slot。

---

## 全异步、无锁、share-nothing

所有跨域协作走 PUMP sender pipeline，**正确性边界不是锁，而是调度模型本身**：

- **单线程 scheduler 不变量** — 每个 scheduler 实例单线程运行 `advance()`，其 owner 状态天然互斥，无需 mutex/latch。
- **唯一状态归属** — 每类可变状态有且只有一个 owner scheduler（front 的 memtable/WAL stream、tree 的 manifest/allocator、value 的 space manager…），其它域只能通过该 scheduler 的 `sender.hh` 发请求，不直接访问。
- **跨核零竞争** — 跨域消息走 per-core SPSC 队列 + bitmap，无 CAS 争用；`coord_sched` 单线程串行化所有定序与 publish。

这正是为什么不能走传统就地修改 B+ Tree：页原地更新需要的 undo/redo、doublewrite、page latch 与「NVMe + 多核 share-nothing」模型直接冲突。Inconel 把并发正确性下沉到「谁拥有状态、消息按什么顺序到达 owner」，而不是靠加锁。

> 实现纪律：禁止给 `pump/` 框架或 scheduler 加锁；出现想加锁的冲动，根因几乎都是「scheduler 被多线程跑」或「状态归属错了」，要回溯调用链改归属，而不是上锁。

---

## 多 batch 并发 in-flight

写路径是流水线，多个 batch 可以同时在飞：

```text
coord_sched 视角：
  t0  assign_lsn(A) → lsn=1, fan-out
  t1  assign_lsn(B) → lsn=2, fan-out
  t2  assign_lsn(C) → lsn=3, fan-out
  ...
  t7  A 全 memtable 完成 → publish(1)
  t8  C 全 memtable 完成 → publish(3)   // C 先于 B 完成
  t9  B 全 memtable 完成 → publish(2)
```

- **定序严格、执行交错** — `coord_sched` 单线程，因此进入 durable path 的 batch 其 `assign_lsn` 严格有序；但 fan-out 之后，各 batch 在 value/front scheduler 上的执行彼此交错，互不阻塞。
- **durable_lsn 用 ready-bitmap 推进** — `publish_batch` / `release_batch` 即使乱序到达 coord，也只推进**连续前缀**：`publish(1)→durable=1`，`publish(3)→仍=1`（2 未 ready），`publish(2)→=3`。
- **失败填平而非留洞** — 某 batch 在 value/WAL 阶段失败走 `release_batch(X)`，同样把 `ready[X]` 置 1，填平前缀，不留永久 hole。

---

## WAL group commit

front WAL 在「不放大 crash 风险」与「合并小写」之间取平衡：

- **同一 tail LBA 只允许一个物理 `wal_append_plan` 在飞** — 避免同一整页被并发覆写造成乱序，这是 crash 安全的底线。
- **idle 时 FIFO coalesce** — front owner 空闲时，把 FIFO（`wal_pending_prepares_`）里**已排队**的多个 fragment 的 entries 按 FIFO 顺序合并进同一个 entry plan。
- **leader-follower 落盘** — plan 携带一组 participants：participant[0] 是 leader（发 FUA、commit/abort），其余 follower 的 prepare callback 被 park 在 front owner，leader commit 后按各自 cursor 唤醒；FUA 失败则把同一 device failure fan-out 给 group 内全部 batch，整组停在 memtable 前走 release。
- **效果** — 小 batch 的 WAL FUA 次数从「per-batch tail-page write」收敛到「per-group `ceil(total_group_wal_bytes / lba_size)`」量级。

段内因此可见跨 batch 交错，但 recovery 只按 `lsn + entry_count` 重组，**不依赖段内连续性**，契约不变。

---

## Value leader-follower 合并

`value_alloc_sched` 是 Value Area 的唯一写入 owner，对并发 batch 做合并：

- 合并多个并发 batch 的 PUT entries → 一次性分配 value slots → 填充 DMA frame → **单次 FUA** 写到本核 `nvme_sched` → value durable → 把各自的 `value_ref` 返回给对应 batch。
- 这把「N 个并发 batch 各写各的 value」收敛成「合并分配 + 合并落盘」，并为下游 WAL 提供已 durable 的 `value_ref`。

---

## per-front 分片（same-key → same-front）

整条读写路径靠**固定路由**消除同 key 的并发竞争，而不是靠锁：

- **写** 按 `route_to_front(key_hash) = key_hash % N` 路由到固定 front_sched，memtable 与 WAL stream 都是该 front 私有 —— 同一 key 的并发写永远落在同一个单线程 owner 上，天然串行。
- **读** 按 `current_shard_partitions()->route(key)`（单次二分）路由到固定 tree read_domain，每个 read_domain 持有独立的 node_cache shard；同 shard 的 key 必定同 read_domain。
- 因此「同 key 的并发访问」从不跨核共享可变状态，正确性由路由 + 单线程 owner 共同保证。

---

## 提交语义（value-before-WAL）

写路径分三阶段持久化，顺序是正确性的一部分：

```
value object durable (FUA)  →  canonical WAL(ref) durable (FUA)  →  canonical memtable insert  →  durable_lsn publish
```

- **value-before-WAL** 保证 crash 后 WAL 引用的 value 一定已经落盘——不会出现「WAL 说有这条、value 却没写完」。
- **读可见性** 用 `read_handle`（PRS 快照 + read_lsn）界定：winner 由 `max(data_ver)` 裁决，memtable 命中优先于 tree，tombstone = 不存在。

---

## 架构

8 类 scheduler，单线程 owner、跨域通过 PUMP sender pipeline 协作（无锁）：

```
                       client requests
                              |
                     +--------v--------+
                     | coord_sched     |  canonicalize / assign lsn / publish
                     +--------+--------+
                              |
                   +----------v-----------+
                   | value_alloc_sched    |  leader-follower value alloc + FUA write
                   +----------+-----------+
                              | value_ref，按 key_hash % N fan-out
             +----------------+----------------+
     +-------v-------+ +------v--------+ +------v--------+
     | front sched 0 | | front sched 1 | | front sched N |  WAL FUA + memtable insert
     +-------+-------+ +------+--------+ +------+--------+
             +-------reduce---+----------------+
                              |
                     +--------v--------+
                     | tree_sched      |  flush owner / alloc / manifest / reclaim
                     +---+---------+---+
              flush map |         | write / trim
              read miss |         |
         +--------------+         +--------------+
 +-------v--------+              +--------v---------+
 | tree_lookup[]  |              | tree_worker[]    |
 | point lookup   |              | old leaf read    |
 | leaf mapping   |              | candidate build  |
 +----------------+              +------------------+
        ( + wal_space_sched 管 WAL segment 池， nvme_sched 做 FUA/FLUSH/TRIM I/O )
```

| Scheduler | 实例数 | 路由 | 核心职责 |
|-----------|--------|------|----------|
| `coord_sched` | 1 | 固定 | LSN 分配、canonicalization、durable_lsn 推进、seal、frontier switch |
| `front_sched` | N | `key_hash % N` | per-front WAL stream + memtable 代际链 |
| `tree_sched` | 1 | 固定 | tree allocator、tree-local flush、reclaim、shard partition rebuild |
| `tree_read_domain` | K | `current_shard_partitions()->route(key)` | own `lookup`（查询/leaf mapping）+ `worker`（old leaf read/candidate build）+ node_cache shard |
| `wal_space_sched` | 1 | 固定 | WAL segment 池分配/回收 |
| `value_alloc_sched` | 1 | 固定 | Value Area 分配/读取/回收，`value_space_manager` 管空闲与 partial page |
| `nvme_sched` | per-core×device | 轮询/指定 | SPDK FUA write / FLUSH / TRIM |

### 后台流程

- **Seal / Flush / Frontier switch**：sealed memtable gen → tree-local flush（fold winner → leaf mapping → candidate build → 写 tree slot + NVMe FLUSH）→ 安装新 guard/CAT → 老 CAT/guard 引用归零链 → retire（TRIM + value 回收）。
- **Recovery = flush-like merge**：读 superblock → 遍历 tree leaf + 扫描 WAL → per-key winner 合并 → 增量 CoW 写 tree → 更新 superblock → 重建 allocator → 安装 clean runtime。

---

## 代码模块（L0–L3 四层）

```
apps/inconel/
├── format/        L0 磁盘格式（POD struct + 常量 + CRC，零运行时逻辑）
├── core/          L0 运行时域对象 + scheduler 注册表/路由
├── memory/        L1 DMA 池 + 统一帧抽象（frame state machine / pin）
├── nvme/          L1 NVMe I/O（SPDK qpair，per-core per-device）
├── coord/         L2 协调器 scheduler
├── front/         L2 前端 scheduler（WAL + memtable）
├── tree/          L2 B+ Tree 域（tree_sched + tree_read_domain[]）
├── wal/           L2 WAL 空间管理 scheduler
├── value/         L2 Value 分配/读取 scheduler
├── write_path/    L3 写路径 sender 组合
├── pipeline/      L3 顶层 pipeline 编排（写/读/seal/flush）
├── recovery/      L3 启动恢复
└── runtime/       L3 配置 + 初始化 + DB lifecycle + main
```

**关键约束**：L2 的 5 个 scheduler 模块互不依赖，跨 scheduler 协作只通过 L3 sender 编排层；每个 scheduler 模块对外只暴露 `sender.hh` 一个接口。

---

## 磁盘格式

只持久化四类对象（白名单，单盘 v1）：

| 对象 | magic | 对齐 | 说明 |
|------|-------|------|------|
| Superblock A/B | `0x494E434F4E454C31` | lba_size | 双槽 + CRC 选择，格式化参数 |
| WAL Segment | `0x57414C53` | wal_segment_size | header + entry + trailer，跨 LBA 拼接 |
| Tree Page | `0x54524545` | tree_page_size | slot header + slot directory，internal/leaf，shadow range |
| Value Object | `0x56414C55` | value_size_class | size class 布局 + sub-LBA 策略 |

核心地址类型：`paddr {device_id, lba}`、`range_ref {base, slot_count}`（shadow range）、`value_ref {base, byte_offset, len, flags}`。

---

## 硬约束（非谈判项）

- **容量下限：10 亿条 KV 起步**。所有 carrier/容器/算法复杂度/热路径成本按 10 亿 KV 校准；禁止只在小量级自洽、靠全局线性扫描或 per-request 物化蒙混的结构。
- **性能取向：`RocksDB × 5` 作为方向性锚点**。它是设计/review 用的性能基准，不是已验收的 benchmark 数字；禁止为“先跑通”接受明显拖慢热路径的结构。
- **v1 = 功能范围冻结，不是质量降级**。可少做外围能力（网络协议层、MERGE / batch-atomic / TTL / range-delete / compression / 加密 / column family）；但 split / consolidation / root-change / recovery 全流程 / value 回收 / WAL durability / flush / memtable seal 等核心引擎路径**必选**，不允许以 v1 为由省略或做成半成品。

---

## 构建与运行

Inconel 用 SPDK 操作真实 NVMe 盘，需 vendored SPDK + 独立构建目录：

```bash
# 配置（Release）
cmake -B build_real -DCMAKE_BUILD_TYPE=Release
cmake --build build_real -j$(nproc) --target inconel_ycsb

# 运行前绑定 scratch 盘到 SPDK（只绑非系统盘）
sudo PCI_ALLOWED=0000:04:00.0 /path/to/spdk/scripts/setup.sh

# YCSB 一致性场景（脚本硬编码参数，禁止手拼命令）
LD_LIBRARY_PATH=/path/to/spdk/build/lib:/path/to/spdk/dpdk/build/lib \
  apps/inconel/scripts/ycsb_consistency.sh all
```

> ⚠️ `0000:03:00.0` 是 OS 系统盘，禁止 SPDK 绑定；脚本对系统盘 BDF 会 fail-fast。scratch 盘默认 `0000:04:00.0`，可经 `INCONEL_YCSB_BDF` 覆盖。

主要构建目标：`inconel_ycsb`（workload 驱动）、`inconel_test_flush_e2e` / `inconel_test_steady_e2e` / `inconel_test_concurrent_runtime_e2e`（端到端测试，需 `--pci-addr BDF`）。

应用 root submit 边界统一在 `runtime/start_db.hh`；YCSB 等 app 只返回 workload sender。

---

## 设计文档

实现任何功能前**必须**先读设计文档索引并三阶段检查（实现前定位章节 / 实现中回查 / 实现后对照 contracts）：

| 文档 | 内容 |
|------|------|
| [`INDEX.md`](../../ai_context/inconel/design_doc/INDEX.md) | 分层索引 + 快速定位表（入口） |
| [`design_overview.md`](../../ai_context/inconel/design_doc/design_overview.md) | 唯一权威规范，所有语义源头 |
| [`code_modules.md`](../../ai_context/inconel/design_doc/code_modules.md) | 模块划分与职责边界 |
| [`on_disk_formats.md`](../../ai_context/inconel/design_doc/on_disk_formats.md) | 字节级持久化格式 |
| [`write_path_and_pipeline.md`](../../ai_context/inconel/design_doc/write_path_and_pipeline.md) | 写路径 |
| [`read_api_and_pipeline.md`](../../ai_context/inconel/design_doc/read_api_and_pipeline.md) | 读路径 |
| [`flush_and_frontier_switch.md`](../../ai_context/inconel/design_doc/flush_and_frontier_switch.md) | Flush 与前沿切换 |
| [`recovery_and_wal_reclaim.md`](../../ai_context/inconel/design_doc/recovery_and_wal_reclaim.md) | Recovery 与 WAL 回收 |
| [`runtime_state_machine.md`](../../ai_context/inconel/design_doc/runtime_state_machine.md) | Scheduler 状态机 |
| [`runtime_memory_and_cache.md`](../../ai_context/inconel/design_doc/runtime_memory_and_cache.md) | 内存与帧管理 |
| [`code_quality_standard.md`](../../ai_context/inconel/design_doc/code_quality_standard.md) | 实现与 review 质量红线 |
| [`cross_doc_contracts.md`](../../ai_context/inconel/design_doc/cross_doc_contracts.md) | 跨文档一致性检查表 + 三条红线 |
