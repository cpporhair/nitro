# Inconel KV 概要设计（唯一规范）

> 状态：当前唯一规范。
>
> 作用：后续详细设计、实现、测试和评审一律以本文件为准。
>
> 范围：本文冻结 **单盘 v1** 的系统语义，但所有持久地址、allocator、WAL segment 标识和 I/O 接口都按可扩到多盘的抽象定义。这里的 `v1` 只表示**功能范围冻结**，不表示可以降低质量标准；任何已经纳入范围或已经进入 canonical path 的能力，都必须按 production-grade 实现。未来做多盘时，只允许替换“设备选择与并行策略”，不允许推翻本文的 batch / publish / read / recovery 基本模型。
>
> 关系：`ai_context/Inconel/nvme_layout.md`、`ai_context/Inconel/wal_segment_pool.md`、`ai_context/Inconel/design_overview.md`、`ai_context/inconel.codex/published_read_set_redesign.md` 可以保留为历史讨论或实现草稿，但不再是规范依赖。后续详细设计不要求交叉阅读这些文档；即使它们全部移除，本文也必须足够。若与本文冲突，以本文为准。

## 1. 系统定位与边界

Inconel 是一个基于 PUMP + SPDK 的持久化 KV 引擎。它的基本形态固定为：

- 前台：`per-front-scheduler WAL + per-front-scheduler memtable`
- 后台：`single logical Shadow CoW B+ Tree + Value Area`
- 提交点：`value object durable + canonicalized WAL(ref) durable + canonical memtable insert + durable_lsn publish`
- 恢复模型：`scan tree leaf records + replay surviving WAL refs + rebuild logical winners + merge/flush into clean tree + reconstruct value runtime state + clean start`

### 1.1 我们要做什么

这个项目要做的不是“另一个通用数据库”，而是一个面向：

1. `NVMe SSD`
2. `多核 share-nothing`
3. `pipeline runtime`
4. `有序 point read / range scan`

的持久化 KV 引擎。

它想同时拿到下面几件事：

1. 前台写延迟接近 `value object durable + WAL FUA` 级别，而不是等待后台树整理。
2. 同一 batch 原子提交，crash 后不丢 ACK 过的数据。
3. 读路径最终只面对一棵有序索引，而不是多层 SST。
4. 后台没有 LSM compaction 那种长期全局重写。
5. tree 侧不做传统“数据页原地更新 + 页级 WAL/doublewrite”。

换句话说，Inconel 要把：

```text
per-front-scheduler ingest path
+ single ordered index
+ crash-safe durability
```

放进同一个系统模型里。

### 1.2 要解决什么问题

它要解决的是三类经典方案在这个场景下各自的短板：

1. **传统就地修改 B+ Tree**
   - 优点是读好、结构直观。
   - 问题是页原地更新需要 undo/redo、doublewrite、latch/mutex，和 NVMe + 多核 share-nothing 模型冲突很大。

2. **LSM-Tree**
   - 优点是前台写吞吐高，WAL + memtable 模型自然。
   - 问题是 compaction 是全局后台任务，读路径要查多层，range read 和空间放大也更重。

3. **传统 CoW B+ Tree**
   - 优点是不做原地覆盖，天然没有 torn-page 问题。
   - 问题是普通更新也会触发从叶子到 root 的路径拷贝级联。

所以 Inconel 真正要解的是这个组合问题：

```text
如何在不做 LSM compaction、
不做原地页更新、
又不承受传统 CoW 路径级联的情况下，
保留一棵有序树作为后台物化形态。
```

### 1.3 为什么叫 Shadow CoW B+ Tree

名字里的 3 个词分别对应 3 个设计点：

1. **B+ Tree**
   - 后台最终形态是一棵有序 B+ Tree。
   - point lookup 和 range scan 都围绕这棵树定义。

2. **CoW**
   - 节点更新不覆盖旧页，而是写新版本。
   - 所以 tree 页天然规避 torn-page 问题，也不需要传统数据页 WAL。

3. **Shadow**
   - 一个逻辑节点不是只占一个物理 page，而是占一个预留的 `shadow range`。
   - 这个 range 里有多个 page slot；同一节点的新版本优先写到同一个 range 的下一个 slot。
   - 父节点保存的是这个 range 的 `base`，不是“当前生效 slot 的精确物理地址”。

可以把它理解成：

```text
一个节点有一串 shadow pages / slots
普通 CoW 先在这串影子页里滚动写
只有写满了，才搬家(consolidation)
```

这就是 “Shadow CoW” 的含义。  
它的关键不是“页会复制”本身，而是：

```text
把 CoW 从“每次都改父指针”
变成“先在本节点自己的 shadow range 内消化”
```

### 1.4 利用了什么

这个设计同时利用了 3 类东西：

1. **NVMe 持久化语义**
   - 前台 PUT 先把 value object durable 到 Value Area，再用 `FUA` 追加引用它的 WAL record。
   - 对 `class_size < lba_size` 的 sub-LBA value，v1 采用单个 logical block 粒度的 page-based writeback；这里依赖标准 NVMe 设备默认提供的 **1-LBA 原子写** 语义，而不是依赖厂商定制的大粒度高级原子写功能。
   - 后台 steady-state flush 用 `NVMe FLUSH` 固定 tree slot 的持久化顺序；v1 boot recovery 直接复用 winner `value_ref`，不重写 live value body。未来若引入 `value scrub / rewrite` 维护命令，仍遵守 value-before-tree。

2. **SSD FTL 的 TRIM / unmapped 语义**
   - slot 是否可用，不靠额外持久化 bitmap。
   - Shadow CoW 可以把旧 slot / old range 延迟 TRIM，并把“哪些位置已失效”交给设备侧映射层表达。

3. **CoW + CRC**
   - 新页写坏时，新页无效，旧页还在。
   - 所以 tree 页 crash safety 靠 `CoW + CRC`，不是靠数据页 redo/undo。

### 1.5 PUMP 在系统里起什么作用

PUMP 在这里不是“只是一个异步框架”，而是系统并发模型的一部分。

它提供了 4 件事：

1. **scheduler ownership**
   - 每类状态有明确 owner scheduler。
   - per-front-scheduler front state、tree reclaim、per-core NVMe qpair 都可以天然 share-nothing。

2. **fan-out / reduce 编排**
   - 一个 batch 可以自然地 fan-out 到多个 front schedulers 执行 `value object durable + canonical WAL(ref) + canonical memtable insert`，再 reduce 回来发布。

3. **跨执行域切换**
   - 前台逻辑、tree 逻辑、NVMe I/O 都在不同 scheduler 上显式切换，不需要隐式锁同步。

4. **队列顺序语义**
   - seal/release/reclaim 的很多正确性，都依赖“消息在 owner scheduler 上按顺序执行”。

所以 PUMP 的作用不是“代码写起来优雅一点”，而是：

```text
把 share-nothing + pipeline 直接变成系统的正确性边界
```

### 1.6 系统整体架构

系统的高层拓扑如下：

```text
                       client requests
                              |
                     +-----------------+
                     | coord_sched     |
                     | canonicalize    |
                     | assign lsn      |
                     | publish         |
                     +--------+--------+
                              |
                   +----------v-----------+
                   | value_alloc_sched    |
                   | leader-follower      |
                   | value alloc + write  |
                   | FUA on local nvme    |
                   +----------+-----------+
                              | value_ref
                  route by hash, fan-out
             +----------------+----------------+
             |                |                |
     +--------v-------+ +--------v-------+ +--------v-------+
     | front sched 0  | | front sched 1  | | front sched N  |
     | WAL FUA        | | WAL FUA        | | WAL FUA        |
     | memtable insert| | memtable insert| | memtable insert|
     +--------+-------+ +--------+-------+ +--------+-------+
             |                |                |
             +-------reduce---+----------------+
                              |
                     +--------v--------+
                     | tree_sched      |
                     | flush owner     |
                     | alloc / delta   |
                     | manifest write  |
                     +---+---------+---+
                         |         |
               flush map |         | write / trim
              read miss  |         |
         +---------------+         +------------+
         |                                        |
 +-------v--------+                     +---------v--------+
 | tree_lookup[]  |                     | tree_worker[]    |
 | point lookup   |                     | old leaf read    |
 | leaf mapping   |                     | candidate build  |
 +-------+--------+                     +---------+--------+
         |                                        |
         +----------------+   +-------------------+
                          |   |
                 +--------v---v-------+
                 | tree_read_domain[] |
                 | tree_node cache    |
                 +--------+-----------+
                          |
               +----------+-----------+
               |                      |
        +------v------+        +------v------+
        | logical B+  |        | value area  |
        | tree        |        | blobs       |
        +------+------|        +------+------+
               |                      |
               +----------+-----------+
                          |
                 +--------v--------+
                 | nvme schedulers |
                 +--------+--------+
                          |
                       SSD / FTL
```

读路径还有一条单独的旁路：point read / MultiGet 的 memtable miss 不在 `front_sched` 上直接走树，而是稳定路由到少量 `tree_read_domain` 执行 tree traversal。flush 中 old leaf read / candidate build 则路由到同一组 read_domain 的 worker arm。每个 `tree_read_domain` 持有一个 tree-node `readonly_frame_cache` shard，lookup 和 worker 在同一 `Cache` 类型上模板化共享该 shard；这些 shard 数量通常少于 `front_sched` 数量。

这张图里最重要的结构关系是：

1. 前台写先经 `value_alloc_sched` 统一持久化 value（leader-follower 合并并发 batch），拿到 `value_ref` 后再按 key hash fan-out 到各 `front_sched` 先写 WAL、all-WAL barrier 成功后再写 memtable。
2. `front_sched` 只负责 WAL 追加、memtable 维护，以及 point read 的前台入口与 memtable lookup；tree miss 通过 `current_shard_partitions()->route(key)` 路由到对应 `tree_read_domain`。
3. tree domain 由一个 `tree_sched` 单点（flush/allocator/manifest mutation）+ K 个 `tree_read_domain` 组成；每个 read_domain own 自己的 `tree_lookup_sched`（traversal / batch leaf mapping）和 `tree_worker_sched`（old leaf read / candidate build），两个 arm 共享 read_domain 的 cache shard（step 030 §2.3 / §6.5 G1）。
4. 后台物化是一棵逻辑树。
5. WAL 提交和 tree flush 是两条不同时间尺度的路径。
6. NVMe I/O 是单独 owner 的执行域，不和上层状态混在一起。

KV 层当前只提供以下保证：

1. **Atomic batch write**：同一 batch 内所有 key 要么全部可见，要么全部不可见。
2. **Statement-start stable Read Committed**：一次逻辑读调用内，memtable/tree 拓扑与可见上界固定；跨调用不提供快照。
3. **Crash-safe durability**：客户端收到 ACK 后，数据在进程或机器崩溃后不会丢失。
4. **Redo-only recovery**：`WAL(ref) + value object` 只承担逻辑 redo，不承担树页 undo/redo 双写。

KV 层明确不提供：

1. 显式 snapshot。
2. Snapshot Isolation / SSI。
3. 线性化读。
4. 客户端重试去重。
5. large-value 特殊路径（v1 不做）。
6. 多树分片；系统只有一棵逻辑树。

另一个全局不变量是：

```text
same key -> same front scheduler (within one runtime)
```

也就是：

1. 每个 entry 按完整 key 的 hash 路由到当前运行期中的唯一 owner front scheduler。
2. 相同 key 在同一次运行期间永远落在同一个 front scheduler 的前台 memtable 体系里。
3. 因此 point GET 只需要查一个 front scheduler 的 memtable，flush 时同 key 的前台版本裁决也不需要跨 scheduler 完成。
4. clean recovery 之后允许用不同的 front scheduler 数重新启动；旧运行期的 core 数、WAL stream 数和 memtable 拓扑不要求保留。

### 1.7 系统组件总览

从系统分层看，Inconel 由 8 类 scheduler 组成：

1. **batch scheduler**（`coord_sched`，单实例）
   - 接收或生成 canonical batch image
   - 仅对 canonicalization 成功、真正进入 durable path 的 batch，按当前运行期的 gap-free 顺序分配 `batch_lsn`
   - 计算 canonical `entry_count`
   - 协调 fan-out / reduce
   - 推进 `publish_catalog.durable_lsn`

2. **front scheduler**（`front_sched`，N 实例，`key_hash % N` 路由）
   - 追加 canonical WAL entry（FUA）并插入 canonical memtable entry
   - 维护本 owner 的 `active + sealed memtable gens`
   - 处理 point read 访问本 owner 的 memtable
   - 对 memtable miss 用 `current_shard_partitions()->route(key)` 分组后投递到对应 `tree_read_domain` 的 `lookup`（同 shard 的 key 必定落到同一 read_domain，见 §8.1）
   - 执行 `seal_active`

3. **tree read domain**（`tree_read_domain<Cache>`，K 实例，每 core 一个，step 030 §6.5 G1）
   - 持 routing snapshot (`shared_ptr<const shard_partition_map>`)、`tree_node` `readonly_frame_cache` shard
   - own 一个 `tree_lookup_sched<Cache>` + 一个 `tree_worker_sched<Cache>`，通过 `advance()` 代驱两个 arm；PUMP runtime tuple 只注册 read_domain
   - `tree_lookup_sched` 负责 point lookup / `keys_to_leaf_groups()`，`tree_worker_sched` 负责 flush 的 old leaf read / decode / candidate page materialization
   - 两个 scheduler 都模板化 on 同一 `Cache`，通过 `read_domain_->node_cache` 访问共享 cache shard（零虚调用）
   - 路由由**全局 `shard_partition_map`** 决定：`shard_idx = current_shard_partitions()->route(key)`（单次二分，`log2(K)` 成本）。同 shard 的 key 永远落到同一 read_domain，因此一张 tree page 在整个读 + flush 链路里最多只在一个 read_domain 的 `tree_node` cache 驻留一份（INC-040 / step 030 §2.6 / §6.4 F2）。按 `front_owner % K` 的 hash 路由会把同 leaf 的 key 分散到不同 shard、在每个 shard 复制同一张 page，是明确禁止的反例。
   - `shard_partition_map` 由 builder 在启动时装入占位 (`shards=[{upper=+∞, idx=0}]`)；flush 完成后由 `tree_sched` 基于新 `leaf_order` 重建并 `install_shard_partitions()` 原子替换 (B1 目标设计，step 030 仅落地 API)

5. **wal space scheduler**（`wal_space_sched`，单实例）
   - 管理 WAL segment `free -> active -> sealed -> free`
   - 只在换段和回收时参与

6. **tree scheduler**（`tree_sched`，单实例）
   - 选择可 flush 的 sealed gens
   - 作为 tree-local flush round owner：fold 前台状态、汇总 lookup/worker 结果、规划 tree delta
   - 写 tree slots，构造 new manifest（含 `leaf_order`）
   - NVMe FLUSH 后推进 flush_max_lsn
   - 委托 `coord_sched` 执行 frontier switch（构造 G1/PRS2/CAT2）
   - 执行 tree-side reclaim（TRIM old slots/ranges，投递 value 回收到 `value_alloc_sched`）

7. **value alloc scheduler**（`value_alloc_sched`，单实例）
   - 集中管理 value page 分配、前台 value 持久化和 **tree-path value 读取**
   - 持有分配元数据：bump head、per-class `whole_page_pool` / `hole_page_list` / `extent_free_pool`、owner-local `generic_free_spans`
   - 持有全局唯一的 value_page `readonly_frame_cache`（读写共享），同时服务写侧 hole reuse 和读侧 `read_value`
   - 前台 PUT 的 value 写入采用 leader-follower 模式：advance() 合并当前并发 batch 的 PUT entries，leader 统一分配 slot、填充 DMA frame、提交 FUA 写到本核 nvme_sched；FUA 完成后所有 batch 拿到 `value_ref`，再 fan-out 到各 `front_sched` 先写 WAL、all-WAL barrier 成功后再写 memtable
   - `read_value(value_ref)`：Point GET 的 tree hit value 读取入口；`read_page_values(value_read_group)`：MultiGet / Scan 的批量 value 读取入口
   - 批量查询先在调用方按 value page 分组，再把同页 refs 交给 `value_alloc_sched`
   - `tree_sched` 把 dead `value_ref` 批量投递给 `value_alloc_sched::reclaim_values(...)`；页内聚合和 whole-free page 的 TRIM 完成态由 value owner 内部处理
   - 最佳实践是把它部署在独占核心上，但这属于部署建议，不是语义前提

8. **nvme scheduler(s)**（`nvme_sched`，每核心 × 每设备各一个实例）
   - 执行具体的 NVMe read / write / FLUSH / TRIM
   - 每个 `nvme_sched` 拥有独立的 SPDK qpair
   - 各 scheduler 使用本核心的 `nvme_sched`：`value_alloc_sched` 用其核心的 qpair 做 value FUA；`front_sched` 用其核心的 qpair 做 WAL FUA；`tree_read_domain` 的 lookup / worker 用其核心的 qpair 做 tree read；`tree_sched` 用其核心的 qpair 做 flush write / FLUSH / TRIM

### 1.8 端到端数据流

如果只看一条写和一条读，系统的主路径可以先压成下面这个图：

```text
write:
client
  -> coord_sched(canonicalize, assign lsn)
  -> value_alloc_sched(leader-follower: alloc slots, fill DMA, FUA write → value_ref)
  -> route by hash, fan-out to front schedulers
       -> canonical WAL append(FUA, carries value_ref)
       -> canonical active memtable insert(value_handle / tombstone)
  -> reduce
  -> publish durable_lsn on current CAT
  -> ACK

background:
sealed memtable gens
  -> tree-local flush pipeline
       -> fold memtable winners
       -> map keys to affected leaves
       -> read old leaf / build candidate pages
       -> write tree slots that reference existing value_ref
  -> NVMe FLUSH
  -> publish new tree_guard / new CAT
  -> reclaim old memtable / old tree data / old front-only values / old WAL segments
       -> tree_sched TRIM -> value_alloc_sched recycle

read:
client
  -> pin read_handle = {cat, read_lsn}
  -> batch cache
  -> front-scheduler memtable(active + imms)
  -> route miss to tree_read_domain.lookup via `current_shard_partitions()->route(key)` (shard-partition routing; INC-040 / step 030)
  -> tree hit value: route to value_alloc_sched(read_value → owning bytes)
```

这里最重要的结构关系是：

1. 前台写先进入 `all-WAL barrier + all-memtable barrier`，然后才可以提交。
2. 前台 PUT 的 value body 在 WAL 之前先 durable 到 Value Area；tree 是后台整理结果，不是前台提交点。
3. reader 读到的是“当前 `read_handle` 对应的 memtable + tree 拓扑”，不是“谁最新就抓谁”。

### 1.9 Tree 长什么样

Inconel 的后台不是 LSM，也不是前台每个 owner 各一棵树，而是：

```text
一棵 single logical Shadow CoW B+ Tree
```

它的基本形态是：

```text
internal node: separator key -> child range_ref.base
leaf node    : logical key -> leaf_record
leaf_record  : {data_ver, value_ref} | {data_ver, tombstone}
```

但和普通 CoW B+ Tree 不同，父节点保存的不是“子节点当前 slot 的精确地址”，而是子节点的 **shadow range base**。

这里的 `data_ver` 是同一逻辑 key 的持久化比较版本。它在语义上等价于该记录对应的 `batch_lsn`，但物理编码可以放在 versioned key 或 leaf payload 中；本文冻结“可比较语义”，不冻结具体编码细节。

每个节点在物理上长这样：

```text
shadow range:
  slot0  slot1  slot2  ... slot(X-1)
```

规则是：

1. 普通更新时，把节点新版本写到同一个 range 的下一个空 slot。
2. 父节点 child pointer 不变，因此普通更新不会层层级联到 root。
3. 只有 `split` 或 `consolidation` 改变 child `range base` 时，父节点才需要更新。

所以这棵 tree 的核心目标不是“完全没有级联”，而是：

```text
把绝大多数普通更新从“每次都级联”变成“只改本节点 slot”
```

### 1.10 为什么前台是分片的，后台却是一棵树

这个架构是刻意分层的：

1. 前台按运行期 front scheduler 分开，是为了让 `value object durable + canonical WAL append + canonical memtable insert` 走 share-nothing 快路径。
2. 后台保持一棵逻辑树，是为了让：
   - key 空间仍然全局有序
   - point lookup / range scan / compaction-free 物化模型都更简单
   - 未来多盘时也只是“同一棵树跨设备放置”，而不是把语义切成多棵树

因此可以把它理解成：

```text
front = runtime-sharded ingest
back  = single ordered index
```

### 1.11 三层持久形态

从“盘上到底有什么”来看，可以先分成三层：

1. **WAL 层**
   - 保存 logical redo record 与 batch 完整性信息
   - PUT record 只保存 `value_ref`，不内联原始 value bytes
   - 保证 ACK 后崩溃不丢数据

2. **Tree 层**
   - 保存有序索引结构
   - 叶子放 `logical key -> leaf_record`

3. **Value 层**
   - 保存 value 本体
   - foreground PUT 先写这里，再由 WAL / memtable / tree 通过 `value_ref` 引用

所以整个系统不是“WAL 直接变树页”，而是：

```text
WAL(ref) + Value object 共同承担提交后恢复
memtable 负责前台最新热状态
tree 负责后台有序物化
```

## 2. 已冻结的核心结论

本文先把最容易混淆的边界固定下来。

### 2.1 提交、flush、frontier switch 是三件不同的事

1. **提交 / publish**
   - 含义：某个 batch 从“已完成 value + all-WAL + all-memtable，但尚未对外可见”变成“对读者可见”。
   - 触发点：`durable_lsn` 被推进到该 batch 的 `batch_lsn`。

2. **flush**
   - 含义：把已经可见的 memtable 状态后台物化进 tree，并推进旧 value 的可回收边界；steady-state flush 不重写 value body。
   - 作用：减少 WAL replay 窗口、释放 memtable、回收旧值/旧 slot。
   - 不是提交点，不决定数据是否可见。

3. **frontier switch**
   - 含义：把读路径从 `old tree_guard + old imm set` 切到 `new tree_guard + reduced imm set`。
   - 作用：定义“新 reader 应该读哪一套前台/后台拓扑”。
   - 不是前台写的提交点。

### 2.2 运行时发布边界和崩溃存活边界不是一回事

运行时，reader 和 ACK 看到的是 `publish_catalog + durable_lsn` 这条发布边界；崩溃后，recovery 看到的是“仍然存活在 WAL 里的完整 batch”这条存活边界。

这两条边界通常重叠，但语义上不是同一个东西：

1. 对 live reader 来说，batch 在 `durable_lsn` 发布前不可见。
2. 对 crash recovery 来说，只要某个 batch 在 surviving WAL 中仍是完整 batch，它就会被 replay。
3. 因此，可能出现“崩溃前尚未 publish / 尚未 ACK 的 batch，在恢复后重新出现”的情况。
4. 这不破坏“ACK 后不丢”的保证；它只说明运行时可见性边界和 crash 后存活边界是两套不同判定。

### 2.3 数据可见性只看 `batch_lsn/read_lsn`，不看 tree slot 版本

1. live runtime 中，真正决定某个 reader 可见上界的是 `read_lsn`；它来自当前 catalog 已发布的 `durable_lsn`。
2. tree / memtable 中，同一逻辑 key 的记录还可以携带一个可持久化比较的 `data_ver`；它在语义上等价于该记录对应的 `batch_lsn`，用于 fold / recovery / tombstone winner 判定。
3. live runtime 的 tree 结构一致性，如果需要额外的 slot 版本/定位信息，只允许存在于内存 `tree_manifest` 中；它不服务于 live read 的数据可见性，也不承担同 key 数据 winner 的比较语义。
4. 因此，旧文档里的 `slot_lsn` 命名在语义上是误导性的；若详细设计保留这类 runtime-only 概念，统一称 `slot_seq`。

### 2.4 恢复不持久化独立 flush frontier

系统不持久化以下运行时前沿：

1. `flush_max_lsn`
2. `checkpoint_lsn`
3. `publish_catalog`
4. `published_read_set`
5. `checkpoint_guard.tree_manifest`

盘上恢复输入是：

1. `superblock A/B` 里的格式常量与区域边界、`root_base_paddr`
2. 从 `root_base_paddr` 遍历 tree 得到的 CRC-valid leaf records（recovery 不读任何 value body）
3. WAL area 中仍然存活的 segments

恢复从 superblock root 出发遍历 tree 收集 leaf records，再与 surviving WAL 合并重建逻辑 KV 状态。superblock root 可能是旧的（flush 改了 root 但 superblock 未更新），此时 WAL 中仍保留对应 entries（因为 recovery_safe_lsn 未推进，WAL 未回收）。

恢复不尝试复原崩溃前的 `active/imm/CAT` 拓扑，而是：

```text
traverse tree from superblock root → collect leaf records
-> replay surviving WAL refs
-> rebuild logical winners
-> merge/flush into clean tree(reuse winner value_ref)
-> reconstruct value allocator/free-state
-> TRIM old tree state / reclaim dead value extents
-> install clean runtime topology
```

### 2.5 持久化白名单（详细设计 / 编码停机线）

v1 把“允许写到盘上的状态”冻结成白名单。后续详细设计和实现都必须先回答一个问题：

```text
这个状态属于下面哪一类持久化对象？
如果 crash 后只剩这些盘面对象，recovery 是否仍能自洽？
```

如果遇到下面两种情况，必须停下来和人讨论，不能直接推进：

1. 为了正确性，发现需要新增一种本文未列出的持久化对象或持久化字段。
2. 计划删除、弱化、延迟写入本文已列出的持久化对象，导致它不再满足本文定义的提交 / recovery 语义。

v1 允许落盘的对象只有以下四类：

1. **格式入口对象**
   - `superblock A/B`
   - 作用：保存格式常量、区域边界、`root_base_paddr`、generation、crc。
   - 说明：`root_base_paddr` 是最新 clean root 信息或扫描 hint，不是 recovery 的唯一 correctness source。

2. **WAL 对象**
   - `segment header`
   - canonical `WAL entry`（PUT 记录的是 `value_ref`，不是原始 value bytes）
   - `sealed trailer`（只对已 seal segment；它是恢复定界加速信息，不是提交点）
   - 作用：提供 ACK 后不丢的 logical redo record，并在 segment 复用后区分不同 `segment_gen`。

3. **Tree 对象**
   - tree page image（包括 internal / leaf）
   - page / slot 自描述字段与 crc
   - leaf record 中的 `logical key + data_ver + kind(value/tombstone) + value_ref`
   - 作用：提供 live ordered index。
   - 说明：tree 对象会被持久化，但 recovery correctness 最终以 leaf records 为准，不以 internal 拓扑、root slot，或任何 runtime frontier 为准。

4. **Value 对象**
   - value blob 本体
   - 与 `value_ref` 对应的必要 header / 长度 / crc
   - 作用：承载 WAL / memtable / tree 通过 `value_ref` 引用的 value 数据。

除上述四类对象外，v1 明确不持久化、也不允许被详细设计偷偷变成 recovery 输入的状态包括：

1. `publish_catalog / published_read_set / checkpoint_guard / tree_manifest`
2. `durable_lsn / read_lsn / next_lsn / recovery_safe_lsn`
3. `flush_max_lsn / checkpoint_lsn / flush_seq / slot_seq`
4. active / imm memtable 集合、`front_sched_count`、WAL stream 拓扑及其运行时挂接关系
5. allocator head / free pools / WAL free list / active ownership
6. batch cache、fan-out / reduce 中间态、retire lists 等请求态或回收态对象
7. `memtable_gen.kv_arena` 内的 key / value bytes（由 gen 的 shared_ptr 整体保活），以及 `readonly_frame_cache` 的 clean frame 驻留

因此，详细设计的职责不是“再发明一批辅助持久化元数据”，而是：

1. 细化这四类持久化对象的字节格式、校验和更新顺序。
2. 证明只依赖这四类对象，系统就能完成提交、读路径和 clean recovery。
3. 若实现里想额外落盘某个 runtime 状态来“简化恢复”或“加速调试”，一律先回到概要设计层面讨论。

### 2.6 单盘只是设备数量为 1，不是接口模型为 1

v1 只落一块盘，但从现在开始：

1. 所有持久地址都必须带 `device_id`。
2. 所有 allocator / free_pool / WAL segment id 都必须是 device-aware 的抽象。
3. v1 只是 `device_count = 1`，即所有 `device_id == 0`。

这样未来扩到多盘时，变化只发生在：

1. 设备选择策略
2. 每设备的资源拥有者
3. 跨设备并行 I/O 编排

而不会反推 batch 语义、读发布模型或恢复模型。

## 3. 运行时与设备抽象

### 3.1 PUMP 运行时假设

本设计依赖以下 PUMP 语义：

1. 每个 scheduler 单线程串行执行；其内部状态不需要锁。
2. 跨 scheduler 协作通过消息投递或 sender 编排完成。
3. `reduce()` 是 fan-out/fan-in 的聚合点；只有所有目标分支完成后才结束。
4. scheduler 队列有顺序语义；同核上的 release 消息一定排在它之前已经入队的读请求之后。
5. 任意对象的 destructor 都不能直接发 NVMe 命令；只能 enqueue 给拥有对应 qpair 的 scheduler。

### 3.2 设备抽象

所有盘上对象都通过统一地址类型表示：

```cpp
struct paddr {
    uint16_t device_id;
    uint64_t lba;
};

struct range_ref {
    paddr base;
    uint32_t slot_count;
};

struct value_ref {
    paddr base;
    uint16_t byte_offset;
    uint32_t len;
    uint16_t flags;
};
```

约束：

1. tree child 指针保存 `range_ref.base`，不是某个具体 slot 的地址。
2. leaf 的 value record 保存 `value_ref`；leaf 的 tombstone record 不指向 value object。
3. superblock root 保存的是 `paddr root_base_paddr`。
4. v1 单盘时，所有地址都落在 `device_id = 0`。
5. `value_ref` 的物理起点是 `base.lba * lba_size + byte_offset`；`base` 表示该 value object 起始所在的 LBA。
6. 当 value class 本身按 LBA 对齐时，`byte_offset == 0`；只有 `class_size < lba_size` 的 sub-LBA 布局才需要它。
7. `value_ref` 在语义上指向一个稳定的 durable value object；只要 live WAL / memtable / tree / recovery 仍可能需要它，这个对象就不能被复用成别的 value。

### 3.3 运行时部署参数

前台 owner 拓扑不属于盘格式，而属于 runtime deployment。

v1 冻结如下规则：

1. `front_sched_count` 是运行时参数，不写入 superblock。
2. v1 中，`front_sched_count == wal_stream_count == memtable_lineage_count`。
3. `same key -> same front scheduler` 的含义只在**同一次运行期间**成立；它由当前运行期的 `front_sched_count` 与 key hash 共同决定。
4. clean recovery 完成后，新的运行期可以使用不同的 `front_sched_count`；旧运行期的物理 core 数、WAL stream 数和 memtable 拓扑都不要求保留。
5. 旧 WAL segment header 里的 `stream_id` 只表示“产生该 segment 的旧运行期前台 owner 编号”，用于恢复扫描、诊断和统计；它不是新运行期必须复原的拓扑承诺。

## 4. 盘面布局

单盘 v1 的物理布局如下：

```text
[ Metadata Prefix ]
  - superblock A
  - superblock B
  - reserved metadata pages

[ WAL Area ]
  - fixed-size segment pool

[ Data Area ]
  - tree allocator (front)
  - value allocator (back)
  - free pools / TRIMmed extents
```

本章只定义盘面入口、格式参数和恢复输入；与盘面直接相关但展开较多的内容分散在后续章节，分工如下：

1. **本章**
   - superblock
   - 格式化参数
   - 恢复入口
2. **第 10 章 Tree 与 Value Area 规范**
   - shadow range
   - Data Area allocator
   - value 持久化顺序
3. **第 11 章 WAL Segment Pool 规范**
   - WAL Area
   - segment 格式
   - append / reclaim / backpressure
4. **第 12 章 Recovery 规范**
   - 盘面对象如何在恢复时一起收敛到 clean state

也就是说，盘面相关信息不是只放在第 4 章，而是按“入口 / tree-value / WAL / recovery”四块拆开写，避免把物理布局、运行时对象和恢复流程混在同一章里。

### 4.1 格式化参数

以下参数一旦写入 superblock，就属于盘格式的一部分；除非重建整盘，否则不能在线修改：

1. `namespace_size`
2. `lba_size`
3. `tree_page_size`
4. `shadow_slots_per_range`
5. `wal_base_paddr`
6. `wal_segment_size`
7. `wal_segment_count`
8. `data_area_base_paddr`
9. `data_area_end_paddr`
10. `value_size_classes`

其中：

1. `shadow_slots_per_range` 直接决定 Shadow CoW 的摊销写放大与 consolidation 频率。
2. `value_size_classes` 是 Value Area allocator 的格式参数，而不是运行期随意热插拔的策略对象。
3. `front_sched_count` / WAL stream 数 / memtable 拓扑不属于盘格式；它们在每次 clean start 时作为 runtime state 重新建立。

### 4.2 Superblock A/B

superblock 是盘格式入口和最新 clean tree 根信息的记录，不是运行时发布对象，也不是 crash recovery 的唯一正确性来源。它至少包含：

1. format version
2. `namespace_size / lba_size`
3. `tree_page_size / shadow_slots_per_range`
4. `wal_base_paddr / wal_segment_size / wal_segment_count`
5. `data_area_base_paddr / data_area_end_paddr`
6. `value_size_classes`
7. `root_base_paddr`
8. generation
9. crc

规则：

1. superblock 采用 A/B 双槽位。
2. 恢复读取两份，选择 `CRC 正确且 generation 更大` 的一份。
3. 只有当 `root_base_paddr` 变化时，才需要 FUA 更新 inactive superblock。
4. 普通 flush 如果 root 不变，不产生额外 metadata IO。

### 4.3 没有独立 checkpoint record

v1 不引入独立的 checkpoint record 区域，也不把某次 flush 的 `max_lsn`、allocator head、运行时 `publish_catalog`、`checkpoint_guard.tree_manifest` 或独立的 `flush_seq/slot_seq` 记录持久化到盘上。

原因不是“不需要恢复边界”，而是：

1. 运行时发布边界由 `publish_catalog` 决定，它是纯内存对象。
2. 恢复时不追求重建旧的运行时拓扑，而是直接重建成 clean state。
3. 只要盘面格式可解析，且 Data Area 中的 leaf records、surviving WAL refs 与其中携带的 stable `value_ref` 足以恢复逻辑 KV 状态，旧 tree 结构差额就由 recovery 的增量 merge/flush 收敛进 clean tree。

### 4.4 运行时 tree frontier 只存在于内存 manifest

live runtime 仍然需要一份“当前 guard 应该沿哪套结构遍历树”的结构 frontier，但这份 frontier 不持久化到盘上，而是放在 `checkpoint_guard` 持有的 immutable `tree_manifest` 中。

本文冻结的语义是：

1. 每个 `checkpoint_guard` 都持有一份 immutable `tree_manifest`。
2. `tree_manifest` 必须能把“某个 range 在该 snapshot 下应读哪个具体 slot”解析成精确定位。
3. 这个定位在实现上可以落成 `range_base -> slot_index`、`range_base -> runtime slot_seq` 或 `range_base -> exact slot paddr`；三者语义等价，任选其一。
4. `tree_manifest` 在实现上还同时携带一份 runtime-only immutable `leaf_order`，供 flush 侧做 batch leaf mapping；它的覆盖性、有序性和不重叠不变量见 `runtime_state_machine.md` §4.5。
5. 具体 node page 可以因为内存压力被 cache 驱逐，但在最后一个 pin 该 guard 的 reader 释放前，这份 `tree_manifest` 不能丢。
6. crash 后所有旧 `checkpoint_guard/tree_manifest` 一起消失；因此 recovery 不尝试从盘上恢复旧 runtime 的 tree frontier。

因此：

1. 恢复不需要独立 checkpoint record。
2. 但 live reader 仍然有一份结构前沿；它锚定在内存 manifest，而不是盘上的 `slot_seq`。
3. recovery 的正确性来源是“所有可解析的 leaf records + surviving complete WAL batches + stable winner `value_ref` + `data_ver/tombstone` winner 规则”，而不是 root slot 派生出的结构前沿。
4. `root_base_paddr` 仍可保留在 superblock 中，作为最新 clean tree 根信息、磁盘检查和未来 fast-open 优化点，但它不是 v1 recovery correctness 的唯一锚点。

## 5. 核心运行时对象

### 5.1 `memtable_gen`

每个 front scheduler 的前台状态按代际管理：

```cpp
// Pointer+length view into a memtable_gen's kv_arena.
// POD; lifetime is tied to the owning gen's shared_ptr.
struct value_view {
    const char* data;
    uint32_t    len;
};

// POD payload of a memtable PUT entry. `hot` points into
// the owning gen's kv_arena (see §5.3 / §5.4). No heap
// owner, no refcount; the arena frees everything in one
// sweep when the last shared_ptr<memtable_gen> drops.
struct value_handle {
    value_ref  durable;
    value_view hot;
};

struct memtable_gen {
    uint64_t gen_id;
    enum class state { active, sealed } st;
    uint32_t front_owner_index;   // owning front index; UINT32_MAX = invalid
    uint64_t min_lsn;
    uint64_t max_lsn;
    // kv_arena + table + loser_durable_refs; see RSM §3.2.
    // kv_arena holds BOTH key bytes and value bytes for this
    // gen; table's key is std::string_view into kv_arena;
    // value_handle.hot is a value_view into kv_arena.
};
```

语义：

1. `active`：当前写入目标。
2. `sealed`：不再接收新写，但在被新 tree frontier 覆盖并且旧 catalog 释放前，仍必须参与读。
3. `front_owner_index` 记录该 gen 属于哪个 front；empty-delta flush 的 `flushed_gens_by_front` 由它分组构建。默认 `UINT32_MAX` 仅作 invalid sentinel，正式路径必须在 gen 创建时赋值。
4. `min_lsn/max_lsn` 只用于调度与 flush eligibility，不用于读可见性判断。
5. memtable 中的 PUT entry 保存的是 `value_handle = { durable_ref, value_view }`；DELETE 保存 tombstone。
6. memtable hit 的 value 来源是 owning `memtable_gen` 的 `kv_arena` 切片——`value_handle.hot` 就是指向这段切片的 view。gen 活则 arena 活则切片活；没有独立命名的 `hot_blob` 对象，也没有任何 refcount。只要对应 memtable entry 仍 live，value bytes 就不能被真正淘汰到需要 SSD 回读的状态。
7. memtable 被 flush、gen 被释放之后，同 key 后续读走 `tree_lookup → value_ref → value_alloc_sched.read_value()`，命中 `value_page readonly_frame_cache`；value 模块不维护额外的 `value_ref -> value bytes` materialized 索引。

### 5.2 `checkpoint_guard`

`checkpoint_guard` 是 tree 侧唯一的读安全与 GC 保护对象：

```cpp
struct tree_manifest {
    paddr root_slot;
    slot_locator_map slot_map;              // immutable: range_base -> exact slot locator
    small_vector<leaf_span, 0> leaf_order;  // runtime-only immutable leaf spans, key-ordered
};

struct checkpoint_guard {
    std::shared_ptr<const tree_manifest> manifest;
    retired_tree_objects retired;
};
```

职责只有两件：

1. **结构一致性**：reader 查 tree 时通过 `guard.manifest` 解析“该 snapshot 下每个 range 应该读哪个具体 slot”，而不是依赖盘上的 `slot_seq` 做选择。
2. **生命周期保护**：旧 slot / old tree-visible value / old range 挂在旧 guard 上，直到最后一个 reader 释放它后才能 TRIM。

补充约束：

1. `tree_manifest` 是 immutable snapshot；新 flush 只能创建新 manifest，不能原地修改旧 manifest。
2. 具体 node page 可以被 node cache 驱逐，但 `tree_manifest` 必须跟着 guard 活到最后一个 reader 释放。
3. 它不是跨调用 MVCC 快照；它只表达某次读调用 pin 住的 tree 结构拓扑。

### 5.3 `published_read_set`

`published_read_set` 定义“这套发布边界下，reader 应该读哪套 memtable/tree 拓扑”：

```cpp
struct front_read_set {
    std::shared_ptr<memtable_gen> active;
    small_vector<std::shared_ptr<memtable_gen>, 4> imms;  // newest -> oldest
};

struct published_read_set {
    std::shared_ptr<checkpoint_guard> tree_guard;
    std::shared_ptr<const std::vector<front_read_set>> fronts;
    uint64_t epoch;
};
```

### 5.4 `publish_catalog`

`publish_catalog` 把“读拓扑”和“当前已发布到哪里”绑在一起：

```cpp
struct publish_catalog {
    std::shared_ptr<const published_read_set> prs;
    std::atomic<uint64_t> durable_lsn;
    uint64_t epoch;
};
```

关键约束：

1. reader pin 的是 `publish_catalog`，不是裸 `durable_lsn`。
2. 某个 catalog 被新 catalog 取代后，它自己的 `durable_lsn` 不再继续前进。
3. 因此旧 reader 不会拿着旧 topology 却看到只属于新 topology 的更晚 batch。

### 5.5 `read_handle`

每次逻辑读调用都固定一份请求级句柄：

```cpp
struct read_handle {
    std::shared_ptr<const publish_catalog> cat;
    uint64_t read_lsn;
};
```

获取规则：

```text
1. cat      = atomic_load(current_publish_catalog)
2. read_lsn = cat->durable_lsn.load(acquire)
```

语义：

1. `cat` 固定本次调用使用的前台/后台拓扑。
2. `read_lsn` 固定本次调用可见的 batch 上界。
3. `MultiGet`、range scan，以及任何上层把 `[key, key+1)` 当 point lookup 的逻辑，都必须整次调用共享同一份 `read_handle`。

### 5.6 `wal_segment`

WAL 共享容量池，但 live append point 是 per-front-scheduler 私有的：

```cpp
struct segment_id {
    uint16_t device_id;
    uint32_t index;
};

struct wal_segment_runtime {
    segment_id id;
    uint32_t owner_stream;
    uint32_t segment_gen;
    uint64_t min_lsn;
    uint64_t max_lsn;
    enum class state { free, active, sealed } st;
};
```

注意：

1. 共享的是 `free segment pool`，不是共享一个全局 append tail。
2. 不同 front schedulers 并发写不同 active segment。
3. 只有“申请新 segment / 回收旧 segment”才碰共享状态。

## 6. 序号、前沿与命名

系统里至少有以下几个容易混淆的序号/版本：

| 名称 | 单位 | 是否持久化 | 作用 |
|------|------|-----------|------|
| `batch_lsn` / `entry.lsn` | 数据版本 | 是（在 WAL entry 中） | 真正的数据版本号 |
| `data_ver` | 同 key 记录比较版本 | 是（在 memtable entry / tree leaf record 中；可编码进 versioned key） | 比较同一逻辑 key 的 value / tombstone winner |
| `durable_lsn` | 数据发布前沿 | 否 | 当前 catalog 下，哪些 batch 已对外可见 |
| `read_lsn` | 请求级可见上界 | 否 | 一次读调用最多能看到哪些 batch |
| `next_lsn` | 分配游标 | 否 | 下一个 batch 的 LSN |
| `recovery_safe_lsn` | 恢复输入清理下界 | 否 | 哪些更老的历史残影已经不可能再从 recovery 输入中出现 |
| `tree_epoch` | runtime tree snapshot 版本 | 否 | 调试、观测、帮助区分不同 manifest / guard |
| `epoch` | catalog / prs 版本 | 否 | 调试、防错、观测 |
| `segment_gen` | WAL segment 复用代数 | 是（segment header/entry） | 区分同一物理 segment 的不同复用轮次 |

额外约束：

1. `data_ver` 在语义上等价于其对应记录的 `batch_lsn`，但物理上可以编码在 versioned key 或 leaf payload 中。
2. live runtime 可以在 `tree_manifest` 内部使用 `slot_index`、runtime `slot_seq` 或 exact `slot_paddr` 作为 locator；它们都不是 on-disk recovery 输入。
3. `recovery_safe_lsn` 不是提交点，也不是“所有 `<= X` 的 live 数据都会消失”；它只表达“凡是 `data_ver <= X` 的历史旧版本残影，若它已经不是当前 durable winner，就不可能再从 surviving WAL 或 Data Area leaf scan 中出现”。
4. 因为 `data_ver` 在语义上等价于全局有序的 `batch_lsn`，所以 v1 用单个全局 `recovery_safe_lsn` 就足以表达 tombstone 物理删除的 recovery barrier；不需要 per-key barrier。
5. 在同一次运行期间，`batch_lsn` 是无间隙、单调递增的整数序列；`next_lsn` 每接纳一个进入 durable path 的 batch，恰好前进 1。
6. `durable_lsn = X` 表示当前 catalog 已经推进到 `X` 这个**连续前缀边界**；前缀中的每个 `batch_lsn` 槽位都必须已有终态（publish 或 release），绝不能跳过更早的未 resolved `batch_lsn`。
7. v1 定义一种受限的 clean abort：batch 一旦拿到 `batch_lsn`，如果它在 memtable phase 之前失败，可以走 `release_batch(batch_lsn)`，让该 LSN 槽位变成 resolved-empty slot；一旦进入 memtable phase 后失败，则仍必须运行期终止并由 recovery 处理其部分持久化痕迹。
8. `tree_epoch` 不是数据可见上界。
9. 运行态 `durable_lsn` 由 batch scheduler 连续前缀推进；恢复完成后，新的运行态仍恢复到连续前缀发布模型。

## 7. 前台写路径与提交语义

### 7.1 标准写路径

前台写的唯一规范流程是：

```text
1. 在 `coord_sched` 内、且进入 Inconel durable path 之前，按客户端 batch 内顺序把同 key 操作折叠成 canonical batch image
2. 如果 canonicalization / 参数校验失败，直接向客户端返回错误；只有成功的 batch 才继续消耗 `batch_lsn`
3. coord_sched 按当前运行期的 gap-free 顺序分配 batch_lsn
4. 计算该 canonical batch image 的全局 entry_count
5. value_alloc_sched（leader-follower）：
   - advance() 合并当前并发 batch 的 PUT entries
   - leader 统一分配 value slots、填充 DMA frame、FUA 写到本核 nvme_sched
   - FUA 完成后所有 batch 拿到 durable `value_ref`
6. 各 canonical entries 按 key hash 路由到目标 front scheduler
7. 各目标 front schedulers 并行写 WAL：
   - append canonical WAL entry (FUA, PUT 记录 `value_ref`)
8. all-WAL reduce 成功后，再并行执行 memtable insert：
   - PUT    -> `value_handle { durable = value_ref, hot = value_view }`（view 指向 owning gen 的 `kv_arena` 切片）
   - DELETE -> tombstone
9. all-memtable reduce()
10. publish(batch_lsn)
11. ACK
```

这里的 `canonical batch image` 不是实现优化，而是持久化语义本身。冻结规则如下：

1. 客户端看到的 batch 语义，仍按它提交的原始 batch 内顺序解释。
2. 进入 Inconel 的 WAL / memtable 之前，同一逻辑 key 的多步更新必须先被折叠成至多一条最终 KV 记录。
3. 这条最终 KV 记录在 v1 中只能是 `PUT(value)` 或 `DELETE(tombstone)`；Inconel v1 不持久化 unresolved `MERGE` / `INCREMENT` operand。
4. 因此，若上层存在简单 `MERGE` / `INCREMENT`，它必须在进入 durable path 之前先被折叠成与原始 batch 顺序等价的最终 `PUT` / `DELETE`。
5. 这种折叠只允许依赖“同一 batch 内、同一 key 的操作序列”；不得读取当前 DB / memtable / tree 状态。
6. `PUT / DELETE` 的 batch 内折叠规则是 last-op-wins。
7. 折叠完成后，`entry_count` 统计的是 canonical records 数，而不是原始客户端操作数。
8. durable boundary 之后的三种表示必须表达同一个 canonical 结果：
   - WAL PUT 看到的是 `PUT(value_ref)`
   - memtable PUT 看到的是 `PUT(value_handle { value_ref, value_view into kv_arena })`
   - recovery replay 看到的是 `PUT(key, data_ver, value_ref)`
   它们不能再回到原始 batch 内中间步骤。
9. 如果 PUT 的 value object 已 durable，但对应 WAL record 最终没有 durable 成功，则该 value object 尚未进入可见或可恢复状态；运行时可以立即回收它，crash recovery 也会把它当作未引用垃圾处理。

实现上，canonicalization 可以由调用方预先完成，也可以由 `batch scheduler` 在接到原始 batch 后完成；本文冻结的是 durable boundary 之后只允许看到 canonical image，而不冻结这步在软件栈中的具体归属。

补充不变量：

1. 同一个 batch 的所有 memtable inserts，必须全部落在同一套 active memtable gens 上。
2. 如果该 batch 在某次 seal 之前完成 front-side的两轮 fan-out（WAL phase + memtable phase），则它在所有目标 front schedulers 上都写入 seal 前的 `A*`，随后这些 `A*` 一起变成 `F*`。
3. 如果该 batch 在 `CAT1` 安装完成之后才开始 front-side 两轮 fan-out，则它在所有目标 front schedulers 上都写入 seal 后的新 `N*`。
4. 不允许同一个 batch 的一部分 entries 落进 `F*`，另一部分 entries 落进 `N*`。

这条约束不是实现细节，而是系统语义。它依赖：

1. `coord_sched` 对“write batch fan-out”和“seal round 发起”的单线程顺序；
2. 每个 `front(owner)` 自己队列内的顺序执行语义。

### 7.2 提交点

一个 batch 被视为“已提交”（这里特指对当前运行实例已发布并可 ACK），当且仅当：

1. 它的所有 PUT value objects 已 durable；
2. 它的所有 canonical WAL fragments 已 durable；
3. 它的所有 canonical memtable inserts 已完成；
4. 它的 `batch_lsn` 已被发布进当前 catalog 的 `durable_lsn`。

因此：

1. ACK 不等待 flush。
2. ACK 不等待 tree frontier switch。
3. ACK 不等待 superblock 更新。

这里要和 recovery 语义分开看：

1. 上面的“已提交”是 live runtime 的可见/应答边界。
2. crash 后到底会保留哪些 batch，仍然取决于 surviving WAL 里有哪些完整 batch 还没被安全回收。

### 7.3 Memtable 中允许存在未发布 entry

canonical memtable insert 发生在 publish 之前，因此 active 或 sealed gen 中都可能物理存在 `entry.lsn > read_lsn` 的数据。

这不影响正确性，因为读规则唯一且简单：

```text
entry 对该 reader 可见，当且仅当 entry.lsn <= read_lsn
```

系统不需要额外的 visible bit。

### 7.4 `publish()` 的唯一要求

`publish(batch_lsn)` 只能在“当前 catalog 的 `prs` 已覆盖该 batch 全部落点，且该 batch 已完成 all-memtable barrier”时推进。

与之并列，`release_batch(batch_lsn)` 表示该 batch 在 memtable phase 之前失败；它不会让任何数据变为可见，但会把该 LSN 槽位标记为已 resolved，从而允许连续前缀继续前进。

它推进的不是“某个离散 batch 已完成”的集合标记，而是：

```text
当前 resolved 批次槽位集合对应的最大连续 batch_lsn 前缀
```

也就是说：

1. 若 `batch_lsn = X + 1` 已 publish，但更早的 `X` 既没 publish 也没 release，则 `durable_lsn` 仍停在旧值，不能跳号。
2. 一旦较早 hole 被 `publish(X)` 或 `release_batch(X)` 补齐，前缀就可以继续推进。
3. 因为 v1 的 `batch_lsn` 分配语义本身是 gap-free 的，所以详细设计不定义“跳过永久缺号”的协议；hole 只能通过 publish/release 变成 resolved slot。

这条要求在两种场景下体现为：

1. 普通 steady-state：直接推进当前 `CAT.durable_lsn`。
2. 如果 publish 恰好撞上 seal：publish 必须在 `publish_gate` 上短暂等待，直到新的 `CAT1` 安装完成。

重要的是：

1. reader 不暂停；
2. 被挡住的是 publish，不是读路径；
3. 这不是 stop-the-world，只是 seal 边界上的一次短暂发布协调。

## 8. 读路径与可见性模型

### 8.0 最高准则

**`read_handle` 是一个完整的、自洽的拓扑 snapshot。读路径的每个步骤都使用 `read_handle` 内部的引用，不使用任何 scheduler 的当前可变状态。**

如果你的实现需要以下任何一项，说明理解错了：

1. ❌ 在读路径中访问 `front_sched.active` / `front_sched.imms`（应使用 `read_handle.cat->prs->fronts[owner]`）
2. ❌ 在读路径中访问 `tree_sched` 当前 manifest（应使用 `read_handle.cat->prs->tree_guard->manifest`）
3. ❌ 把 `tree_lookup` 串行到 `tree_sched` 上执行（应路由到少量 `tree_read_domain` 的 `lookup` 执行；绝不能依赖 `tree_sched` 的当前可变状态）
4. ❌ 需要额外机制保证"读操作期间不发生 seal / flush / frontier switch"（`read_handle` 的 `shared_ptr` pin 已经保证拓扑不变）

原因：seal / frontier switch / release_gens 会修改 scheduler 的当前可变状态，但 `read_handle` pin 住的 PRS snapshot 不受影响。两者在正常路径重合，在 frontier switch 后分裂——用 scheduler 当前状态的实现在分裂时丢数据。

### 8.1 Point GET

这里的 `batch cache` 指当前请求上下文中的临时写集合，只用于同一逻辑 batch 内的 read-your-own-writes；它不持久化，也不参与恢复。

在 v1 canonical batch 模型下，`batch cache` 对同一逻辑 key 暴露的也必须是该 batch 的 canonical final image，而不是原始中间步骤。

Point GET 的规范顺序如下：

```text
1. 先查 batch cache
2. miss -> key hash -> 目标 front scheduler
3. 用 read_handle.read_lsn 查 cat->prs 对应的 active + imms
4. 只有 memtable 全 miss，才把 `(key, manifest)` 交给 `tree::lookup` sender；
   sender 内部用
       shard_idx = core::registry::current_shard_partitions()->route(key)
       home      = core::registry::tree_read_domain_at(shard_idx)
   把 batch 按 home shard 分组 fan-out 到对应 read_domain 的 `lookup_sched`
   （INC-040 / step 030 §2.6 / §6.4 F2）。
5. 如命中 leaf record：
   - `kind = value`     -> 路由到 `value_alloc_sched` 执行 `read_value(value_ref)` 读 value body
   - `kind = tombstone` -> 返回 not found
```

规则：

1. 同 key 在 `active + imms` 中取 `lsn <= read_lsn` 的最大版本。
2. tombstone 与 PUT/value 按同一套 winner 规则比较；winner 为 tombstone 时，对上层返回 not found。
3. 只要 memtable 命中，无论命中的是 value 还是 tombstone，都不再回退到 tree。
4. memtable 命中 value 时，必须直接返回 `value_handle.hot`（POD `value_view` 指向 owning gen 的 `kv_arena` 切片，见 `runtime_state_machine.md` §3.7）；它绝不能退化成一次 SSD 读，也不走任何 value page cache。
5. value bytes 住在 owning `memtable_gen` 的 `kv_arena` 里，没有独立的 `hot_blob` 对象；`value_alloc_sched` 不维护 `value_ref -> value bytes` 索引，memtable hit 的 view 生命周期完全由 `read_handle` 的 pin 链（经由 shared_ptr<memtable_gen>）保证。
6. tree 侧可以有少量 `tree_read_domain` owner 的 node cache shard；它只是性能优化，不改变 `read_handle`、`tree_guard` 或可见性规则。

读路径 handle 数据源断言（详细设计展开 handle 签名时必须对照）：

| handle | 数据输入 | 来源 | ❌ 不是 |
|--------|---------|------|--------|
| `lookup_memtable` | active, imms | 请求携带的 `read_handle.cat->prs->fronts[owner]`（snapshot） | `front_sched` 当前 active/imms |
| `scan_memtable` | active, imms | 同上 | 同上 |
| `tree_lookup` | manifest | 请求携带的 `read_handle.cat->prs->tree_guard->manifest`（snapshot） | `tree_sched` 当前 manifest |

原因：`release_gens` / frontier switch 后，scheduler 自有状态已更新，但旧 reader pin 的 PRS snapshot 仍指向旧 gens/旧 guard。用 self.state 会导致旧 reader 丢数据。

### 8.2 MultiGet / range scan

`MultiGet` 与 range scan 不是“每个 key 临时抓最新 catalog”，而是整次调用共享一份 `read_handle`。

原因：

1. 否则一次逻辑读调用会混到不同的 memtable/tree frontier。
2. 上层数据库常把一个逻辑 point lookup 实现成 `[key, key+1)` 的 scan；如果这类 scan 混到不同 frontier，就可能得到协议级不可能状态。

因此本文冻结以下 API 约束：

1. `MultiGet(k1..kn)`：整次调用共享一个 `read_handle`。
2. `Scan(begin, end)`：整次调用共享一个 `read_handle`。
3. 上层若把一个逻辑操作拆成多个物理读，这些物理读必须显式共享同一个 `read_handle`。
4. tree tombstone 只是一种内部 winner 表达；`GET / MultiGet / Scan` 对上层都只输出 live value，不输出 tombstone。
5. `MultiGet / Scan` 在 tree hit value 后，不应按 `value_ref` 一条条调用 `read_value()`；而应先按 value page 分组，再批量调用 `read_page_values()`。

对 `MultiGet / Scan` 的结果合并，本文再冻结以下可观察规则：

1. 每个 `front(owner)` 必须先在本 owner 的 `active + imms` 内，为同一逻辑 key 折叠出一个 memtable winner，再参与全局合并。
2. 全局结果按逻辑 key 有序输出。
3. 若某个 key 同时出现在 memtable winner 与 tree record 中，则以 memtable winner 为准，tree 命中必须被遮蔽。
4. 若最终 winner 为 tombstone，则该 key 不出现在 API 输出中。
5. 因为系统冻结了 `same key -> same front scheduler`，所以不同 front schedulers 的 memtable 结果不会为同一 key 再次竞争；全局冲突只发生在“某 owner 的 memtable winner”和“tree record”之间。

### 8.3 语义级别

KV 层提供的是：

```text
statement-start stable Read Committed
```

也就是：

1. 一次调用内，读拓扑与 `read_lsn` 固定。
2. 跨调用不固定。
3. 不承诺 snapshot isolation。

## 9. Seal、flush、frontier switch 与回收

### 9.1 三阶段交接模型

运行态以 `CAT0 -> CAT1 -> CAT2` 方式交接：

1. `CAT0`
   - old tree guard `G0`
   - old active memtables `A*`
   - 当前 `durable_lsn = D0`

2. `CAT1`
   - 仍然使用 `G0`
   - active 切到 `N*`
   - old active 变成 `F*`，进入 `imms`
   - `durable_lsn` 从 `D0` 继续前进

3. `CAT2`
   - tree guard 切到 `G1`
   - 被该轮 flush 覆盖的 `F*` 从 `imms` 中摘掉
   - 安装时把切换瞬间当前 catalog 已发布到的 `durable_lsn` 原样继承为 `D1`
   - 新 reader 从此读 `G1 + reduced imm set`

### 9.2 Seal / rotate 的规范步骤

一次 seal 的规范步骤如下：

```text
1. close publish_gate
2. 向每个 front scheduler 投递 seal_active
3. 每个 front scheduler 本地执行：
   - A -> F
   - install N as new active
4. 协调器汇总各 front schedulers 的新读集合，组装 PRS1
5. 发布 CAT1 = { prs = PRS1, durable_lsn = D0 }
6. open publish_gate
7. 之后 post-seal batch 才允许把自己的 LSN 推进进 CAT1.durable_lsn
```

关键点：

1. 旧 reader 继续 pin `CAT0`。
2. 新 reader 只会拿到 `CAT1`。
3. 如果某个 batch 的 publish 撞上 seal，它等待的是 gate，不是 reader 暂停。
4. 同一个 batch 不得跨过这次 `A -> F / install N` 的边界裂成两代 memtable；它要么整体属于 pre-seal 的 `A*`，要么整体属于 post-seal 的 `N*`。
5. 因此，`publish_gate` 的职责只是阻止“旧 topology 上的 publish 越过 seal 边界”；batch 不跨代的保证仍来自 `coord_sched` 与各 `front(owner)` 的队列顺序。

### 9.3 sealed gen 的 flush eligibility

为了避免“同一个 gen 只 flush 一半”的复杂度，本文冻结如下规则：

```text
一个 sealed memtable_gen 只有在 gen.max_lsn <= 当前已发布 durable_lsn 时，才允许被 flush 选中。
```

这意味着：

1. flush 不做 partial-gen flush。
2. 一个 gen 要么整代参与本轮 flush，要么整代继续留在 `imms` 中。
3. 如果 seal 时该 gen 内还有尚未发布的尾部 entries，它只是暂时不可 flush，但仍然必须参与读。

### 9.4 Flush 的规范步骤

一次成功 flush 必须满足以下顺序：

```text
1. 选择一批 flush-eligible sealed gens
2. 构造 `tree_flush_request { base_guard, sealed_gens[], recovery_safe_lsn }`
3. 在 tree-local flush pipeline 内：
   - fold 这些 gens 的 visible state
   - 计算每个逻辑 key 的 memtable winner
   - 用 `manifest.leaf_order` 把 sorted key groups 映射到 affected leaves
   - 读取 old leaf、生成 candidate pages
   - 写 tree slots（leaf/internal 写 value record 或 tombstone record），并在内存中构造新的 immutable `tree_manifest`
4. 发 NVMe FLUSH
5. 产出 `tree_flush_result.success`
6. 构造新的 checkpoint_guard = G1 { manifest = M1 }
7. 在 `coord_sched` 上读取安装瞬间当前 catalog 的 `durable_lsn = D1`，发布 `CAT2 = { prs = PRS2, durable_lsn = D1 }`，使新 reader 开始使用 G1，并把本轮已覆盖的 gens 从 prs.imms 中摘掉
8. 如 root_base_paddr 变化，再异步 FUA 更新 superblock A/B
```

约束：

1. flush 只处理已经对外发布的数据。
2. flush 不是提交点。
3. frontier switch 只在 tree slots durable 之后发生；steady-state flush 不额外创造新的 value body。
4. old reader 只要还 pin 着旧 CAT / 旧 guard，对应旧 memtable / old tree data 就不能回收。
5. `tree_local_flush` 的正式边界停在 `tree_flush_result`；frontier switch 与 gens release 是它的消费者，不属于该子模块内部。
6. 本轮 flush 产出的 tree 状态，必须等价于“把本轮选中的 sealed gens 中所有已发布更新，按 `batch_lsn` 顺序应用到 old tree 上”的结果。
7. old reader 若在 flush 后发生 node cache miss，仍必须能通过它 pin 住的旧 `tree_manifest` 走到旧结构；因此 page image 可以被驱逐，但旧 manifest 不能提前释放。
8. fold winner 在逻辑 key 维度上按 `data_ver` 比较；`data_ver` 在语义上等价于该记录对应的 `batch_lsn`。
9. winner 为 value -> 写引用既有 `value_ref` 的 leaf value record，而不是重写 value body。
10. winner 为 tombstone 时：
   - 如果 old tree-visible 状态是 value，则本轮 flush 必须写 tombstone record，不能第一次 DELETE 就直接 absent。
   - steady-state 中不为旧 tombstone 维护单独的全局 revisit 队列。只有当其所在 leaf 因本轮 flush 被重写时，才会在生成新的 leaf image 时 opportunistically 检查：若 `tombstone.data_ver <= recovery_safe_lsn`、也就是所有更老旧版本都不可能再从 recovery 输入中出现，则可以直接省略该 record（tombstone -> absent）；否则本轮仍保留 tombstone record。
11. `CAT2.durable_lsn` 不是 flush 重新计算出的新前沿，而是 install `CAT2` 那一刻旧 current catalog 已连续发布到的位置的原样继承。

### 9.5 回收规则

回收分四类：

1. **old memtable_gen**
   - 跟着旧 `publish_catalog` 的引用走。
   - 最后一个 pin 旧 CAT 的 reader 释放后，相关 gens 才能 free。
   - `memtable-only loser durable_ref` 在 flush fold 期间直接挂到其 owning `memtable_gen` 的 retire list 上；只有该 gen 不再被任何 published CAT 触达后，它才允许进入物理 value reclaim 判定。若 unfinished round 在同一 sealed gen 上重试，允许先 clear 再按本轮 fold 结果重建。

2. **old tree slot / old tree-visible value / old range**
   - 挂在旧 `checkpoint_guard` 的 retire list 上。
   - 最后一个持有旧 guard 的 reader 释放后，destructor 只 enqueue 回收任务；真正的 TRIM 由 tree scheduler 发起。

3. **pre-WAL orphan value object**
   - 指 PUT 的 value object 已 durable，但对应 WAL record 最终没有 durable 成功。
   - 在该 WAL record durable 之前，它不属于 live read，也不属于 recovery 输入。
   - front owner 可以立即回收，或延后到后台 sweep；crash recovery 会把它当成未引用垃圾清掉。

4. **WAL segment**
   - 只允许 segment 级回收，不做中间打洞。
   - 只有 sealed segment 才可能回收。
   - 只有当 tree/WAL 协调器确认“即使该 segment 消失，恢复也不会丢失其中 batch”时，才允许放回 free_pool。

对 `memtable-only loser durable_ref` 再补充一条：

- 它的物理 recycle 不只看 owning `memtable_gen` 是否释放；还必须满足“它已经不可能在 recovery 中重新成为 winner”。
- v1 可以保守地用 `candidate.data_ver <= recovery_safe_lsn` 表达这条条件；若详细设计能证明一个更紧但等价的条件，也可以替换。

这里最后一条非常重要：

- WAL 回收看的是 **recovery safety**，不是单纯看 `durable_lsn`。
- 如果某轮 flush 改了 root，而 superblock 还没更新，相关 WAL 不能提前回收。
- v1 可把 tree/WAL 两侧的 recovery 清理进度收敛成一个全局 `recovery_safe_lsn`；实现上也可以分成 `wal_safe_lsn`、`leaf_safe_lsn` 等内部量，只要其对外语义等价即可。
- 这个 watermark 是新 runtime 启动后的运行时量，不要求跨 crash 持久化；boot recovery 本身必须采用不依赖旧 runtime `recovery_safe_lsn` 的保守规则。

## 10. Tree 与 Value Area 规范

### 10.0 最高准则

**运行时 tree 结构一致性来自 `checkpoint_guard` 持有的 immutable `tree_manifest`，不来自盘上的 slot 扫描。`manifest` 精确告诉每个 range 应该读哪个 slot。**

如果你的实现需要以下任何一项，说明理解错了：

1. ❌ 运行时 cache miss 时扫描 shadow range 的多个 slot 选最新（应由 `manifest.resolve(range_base)` 直接给出精确 slot 地址）
2. ❌ 需要"每个 range 只剩一个 slot"才能正常读服务（多个 slot 共存是常态，`manifest` 指定当前 snapshot 该读哪个）
3. ❌ 把 flush / recovery 的目标理解为"重写回 slot 0"（flush 是 CoW 写 next slot；recovery 是一次与 steady-state 同构的增量 flush）

只有 boot recovery 没有旧 manifest 时才扫描 shadow range 找最新 CRC-valid slot（从高 index 向低 index），这是 recovery 特有的 bootstrap 步骤，不是运行时常态。

### 10.1 Shadow CoW B+ Tree

单棵逻辑树的基本规则：

1. 每个节点拥有一个 shadow range。
2. 父节点保存子节点 `range_ref.base`，不是某个具体 slot 地址。
3. 普通更新写到当前 range 的下一个空 slot，不更新父指针。
4. 只有 split / consolidation 才会改变 child `base`，因此才可能向上级联。

### 10.2 runtime slot 解析规则

每个 slot header 至少包含：

1. magic / format version
2. node type
3. payload crc

reader 持有某个 `checkpoint_guard` 时，对每个 range 的唯一选择规则是：

```text
先由 guard.manifest 解析该 snapshot 下应读的 exact slot
再读取该 slot 并校验 CRC
```

注意：

1. 这是**树结构一致性**规则。
2. 它不依赖盘上的 `slot_seq`，而依赖 runtime pin 住的 immutable manifest。
3. 它不是数据可见性规则。
4. tree 中是否应该命中某个 key，仍由 `read_lsn` 与 memtable 优先级共同决定。

实现上，在 node cache miss 时不需要扫描整个 shadow range 选 slot；exact slot 由 manifest 给出。manifest 内部可以用 `slot_index`、runtime `slot_seq` 或 exact `slot_paddr` 表示，语义等价。正确性唯一依赖：

1. manifest 是否仍被对应 guard 持有；
2. manifest 解析到的 slot header 是否可解析；
3. payload CRC 是否通过。

### 10.3 DELETE 的 tree 表现

DELETE 在前台和 WAL 中表现为 tombstone；在 tree 中，本文冻结“两阶段删除”语义。

规则如下：

1. 若某逻辑 key 的 old tree-visible 状态是 value，而本轮 winner 是 DELETE，则本轮 flush **必须**把它物化为 tree-side tombstone。
2. 若某逻辑 key 的 tree-side 当前记录已经是 tombstone，则只有当其所在 leaf 本轮被重写，且 `tombstone.data_ver <= recovery_safe_lsn` 时，才允许在新的 leaf image 中省略它，物理删除为 absent。
3. 因此，DELETE 的 tree-side 物化路径是：

```text
value -> tombstone -> absent
```

4. v1 的 steady-state tombstone GC 是 page-local opportunistic compaction，不是单独的全局 sweep；未被本轮 flush 触达的 leaf 会继续保守保留其中的 tombstone，等待未来某次该 leaf 被重写时再清理。
5. reader / recovery / flush fold 在比较同一逻辑 key 的 winner 时，tombstone 与 value 一样按 `data_ver` 参与比较；winner 为 tombstone 时，逻辑结果为 not found。

### 10.4 Data Area allocator

Data Area 是一个连续区域，但内部逻辑上分成两端：

```text
data_area_base_paddr                                   data_area_end_paddr
tree allocator -> [shadow ranges ... free space ... value slabs] <- value allocator
```

冻结规则：

1. Data Area 不预先切出固定的 tree/value 比例；两端共享同一连续空间。
2. tree allocator 从低地址向高地址分配，单位是整个 shadow range；`range_size = tree_page_size * shadow_slots_per_range`，并按该粒度天然对齐。
3. value allocator 从高地址向低地址分配，单位是 `value_size_classes` 定义的 size class / slab。
4. 两端在中间相遇就表示 Data Area 已满。碰撞检测通过 per-device 的 `std::atomic<uint64_t>` 实现：`tree_sched` 分配新 range 后 relaxed store 更新 `tree_alloc_head`，`value_alloc_sched` bump 分配时 relaxed store 更新 `value_alloc_head`；两侧分配前各自 relaxed load 对方的 head 做本地检查。两个 head 都是单调的（tree 只增，value 只减），读到略旧的值只会导致少分配一点后重试，不破坏正确性。
5. tree 和 value 都有各自的 free pool；真正回收到可重用状态，要等各自对应的读/恢复屏障满足后再由 owner scheduler 处理。
6. tree free pool 和 value free pool 不混用；tree 侧回收的是整个 shadow range，value 侧回收的是按 size class 组织的 value extent/slab。
7. value free pool 的来源有两类：
   - old tree-visible value，经由 old `checkpoint_guard`
   - `memtable-only loser durable_ref`，经由 owning `memtable_gen`
8. 当 `class_size < lba_size` 时，value allocator 可以在一个 LBA 内放多个 sub-LBA slots；对上层暴露的稳定定位符仍统一是 `value_ref { base, byte_offset, len, flags }`。
9. allocator head 与 free pool 都不持久化；它们是运行时状态，不是恢复入口。

这样做的目的不是“布局好看”，而是：

1. tree side 固定范围分配，range 对齐简单，consolidation 易处理。
2. value side 可以按 size class 回收，避免 variable-length value 直接把 tree allocator 的地址空间搅碎。
3. 多盘扩展时，只是把 per-device allocator 实例数组化；分配方向与 publish/recovery 语义不变。

### 10.5 Value Area

系统里同一个逻辑 value 在三处有三种不同表示：

```text
value object  : durable bytes in Value Area
value_ref     : stable durable locator to that object
value_handle  : runtime { durable_ref, value_view into kv_arena }
```

PUT 的 steady-state 规则是：

```text
persist value object
-> append WAL PUT(value_ref)
-> insert memtable PUT(value_handle)
-> later flush writes tree leaf { data_ver, value_ref }
```

也就是说，steady-state 下：

1. 前台 PUT 先把 value body durable 到 Value Area。
2. WAL PUT record 只记录 `value_ref`，不再内联原始 value bytes。
3. memtable PUT entry 持有 `value_handle { durable_ref, value_view into kv_arena }`。
4. steady-state flush 只把 winner 的 `value_ref` 物化进 tree；它不再重复写 value body。

这里在概要层再冻结一条抽象边界：

1. `value_ref` 的稳定定位格式是 `{ base, byte_offset, len, flags }`。
2. 当 `class_size < lba_size` 时，一个 LBA 内可以承载多个 value slots，由 `byte_offset` 区分。
3. WAL / memtable / tree / recovery 只传播 `value_ref` 抽象，不依赖 value allocator 在盘上的具体 slot 组织方式。

这里再补一条设备前提：

4. 当 `class_size < lba_size` 时，v1 的写法是对该 value page 所在的 **单个 logical block** 做整块 writeback。这个模型依赖标准 NVMe 设备默认提供的 **1-LBA 原子写** 契约，因此后续对同一 value page 的整块写回不会把更早已经 durable 的 sibling slot 暴露成 torn state。
5. 这里依赖的是基础的 LBA 级原子写语义，不是厂商定制的大粒度高级原子写能力。若目标设备不满足这个前提，不能直接沿用 v1 的 sub-LBA value page writeback 模型。

tree 叶子保存的是：

```text
logical key -> leaf_record
leaf_record = { data_ver, kind = value, value_ref }
           or { data_ver, kind = tombstone }
```

其中：

1. `data_ver` 用于比较同一逻辑 key 的 value / tombstone winner；它在语义上等价于该记录对应的 `batch_lsn`。
2. `data_ver` 的物理编码可以落在 versioned key 或 leaf payload 中；本文冻结比较语义，不冻结具体编码细节。
3. tombstone record 不包含 `value_ref`，也不指向 value object。

steady-state 前台 PUT 的持久化顺序必须是：

```text
write value object
-> make value object durable
-> append WAL PUT(value_ref)
-> insert memtable PUT(value_handle)
-> publish batch durable_lsn
```

这样 crash 后 WAL 中不会先出现一个指向未 durable value 的 `value_ref`；而 DELETE/tombstone 本身不依赖 value object。

steady-state flush 的持久化顺序必须是：

```text
write new tree slots that reference existing value_ref or carry tombstone records
-> NVMe FLUSH
-> publish new tree guard
```

这样新 tree frontier 永远不会先发布一个还没 durable 的 leaf record；而 leaf 中的 `value_ref` 指向的 value body 早在前台 PUT 阶段就已 durable。

补充回收语义：

1. 若某逻辑 key 在本轮 flush 前的 tree-visible winner 是 `old value_ref = V_old`，而本轮 flush 后 winner 变成：
   - 新 `value_ref = V_new`，或
   - tombstone
   则 `V_old` 从这一轮开始就不再属于新 frontier，必须挂到旧 `checkpoint_guard` 的 retire list 上。
2. `V_old` 不能在 install 新 guard 后立刻复用；只有最后一个 pin 旧 guard 的 reader 释放后，tree scheduler 才能把它真正回收到 value free pool。
3. 若某个 `value_ref` 从未进入 tree、只在 memtable/WAL 层出现过，但在 flush fold 中已经输给了更新的 durable winner，则它在 fold 期间直接挂到 owning `memtable_gen` 的 retire list；在该 gen 释放、且它不可能再成为 recovery winner 之后，才允许回收。若 unfinished round 在同一 sealed gen 上重试，允许对该 list clear+rebuild。
4. memtable 中的 value bytes（住在 owning gen 的 `kv_arena`）只看 gen 的 shared_ptr；它不承担 recovery 正确性。只要 gen 还被 PRS / front_state / flush round 中任一方引用，整个 `kv_arena` 就不能释放；最后一个 shared_ptr 归零时，`kv_arena` 的所有 chunk 一次 sweep 释放，durable `value_ref` 的回收走独立的 retire list 路径。
5. `recovery_safe_lsn` 约束的是“哪些更老历史版本不可能再从 recovery 输入中翻盘”；对 `memtable-only loser durable_ref`，v1 可以保守地用它作为物理 recycle barrier。它不是 memtable value bytes 的回收条件——value bytes 的回收条件是 "owning gen 的所有 shared_ptr 都已释放"。
6. 详细设计必须保证每个被覆盖或被删除的 `value_ref`：
   - 恰好进入一次 retire 流程；
   - 不会因为重复挂接而 double free；
   - 不会因为漏挂接而永久滞留。

因此，steady-state 中可能出现“空间暂时回不来”的现象，例如长读一直 pin 住旧 guard，或 `memtable-only loser durable_ref` 还没越过 recovery-safe barrier；但这属于延迟回收，不属于设计上的永久泄漏。

### 10.6 v1 不做 large-value 特殊路径

v1 只实现“一个逻辑 value -> 一个 `value_ref`”。

但为了不给后续 large-value / 多盘 value 条带化挖坑，详细设计应当：

1. 把 leaf payload 的 value 分支定义成 `value_ref` 抽象，而不是裸 `lba/byte_offset/len` 组合在各层局部散落；
2. 允许未来把 `value_ref.flags` 扩展成外部 blob / extent list 的解释方式；
3. 不让 publish、flush、recovery 语义依赖“value 一定内联为单 extent”。

### 10.7 Consolidation 的摊销写放大

Shadow CoW 消除的是“普通更新时每次都向父传播”的地址级联，但并不消除 consolidation 本身。

设每个节点有 `X = shadow_slots_per_range` 个 slots，则：

1. 节点大约吸收 `X-1` 次本地版本更新后，才需要做一次 consolidation。
2. 一次 consolidation 最多向父节点传播 1 次 child base 变化。
3. 这条传播链在层级上按几何级数衰减。

因此其理想摊销上界是：

```text
WA = X / (X - 1)
```

这条结论的用途有两个：

1. 它解释了为什么 `shadow_slots_per_range` 是格式参数，而不是随手可调的小优化。
2. 它给详细设计一个明确目标：不要引入额外机制，把本来 `X/(X-1)` 的结构写放大又推回传统 CoW 的路径级联。

### 10.8 Crash safety 与 torn-page 免疫

这一节需要区分 **tree side** 和 **value side**：

1. `tree slot` 的 crash safety 不依赖设备原子写；它依赖 `CoW + CRC + NVMe FLUSH`。
2. `Value Area` 不走 tree 的 CoW 模型。对 `class_size < lba_size` 的 sub-LBA value，v1 允许一个 logical block 内承载多个 value slots，并通过整块 writeback 更新该页像；这里依赖的是标准 NVMe 设备默认提供的 **1-LBA 原子写** 语义。
3. 因此，“不依赖原子写”这句话只适用于 tree page 不依赖**大粒度**原子写能力，不应被理解成“整个系统完全不依赖任何 LBA 级原子写语义”。

tree slot 从不原地覆盖；新版本永远写到先前未作为“当前有效版本”使用的 slot 上。因此：

1. 新 slot 完整写入并 CRC 正确 -> 新版本有效。
2. 新 slot 半写、torn 或损坏 -> 新 slot 无效，旧 slot 仍可用。
3. 旧 slot / old value 是否已经 TRIM，只影响回收和空间复用，不改变已发布版本的判定。

因此 v1 的 tree side crash safety 依赖的是：

1. `CoW + CRC`
2. foreground `value-before-WAL` 的 durable 顺序
3. steady-state 下 tree 只引用已 durable 的 `value_ref`
4. v1 boot recovery 直接复用 stable winner `value_ref`；未来若做 `value scrub / rewrite` 维护命令，仍遵守 value-before-tree
5. `NVMe FLUSH` 固定 tree durable frontier

而不是：

1. 数据页 redo/undo
2. doublewrite buffer
3. `tree page` 依赖厂商定制的大粒度原子写功能才能避免 torn page

## 11. WAL Segment Pool 规范

### 11.1 核心思想

WAL area 是一个共享容量池，但 live append point 不是共享的。

规范模型：

1. WAL area 被切成固定大小的 segments。
2. 每个 wal stream 在任一时刻只写自己的 active segment。
3. 不同 front schedulers 并发写不同 segments。
4. segment 用完后 seal，再从 free_pool 申请下一个。

单盘 v1 中：

1. `device_id = 0`
2. `stream_id` 可以直接等于当前运行期 front scheduler 的 owner index

### 11.2 盘上对象

每个 segment 至少有：

1. **segment header**
   - `magic`
   - `format_version`
   - `segment_id`
   - `stream_id`
   - `segment_gen`
   - `crc`

2. **WAL entries**
   - `len`
   - `segment_gen`
   - `lsn`
   - `entry_count`（该 `batch_lsn` 的 canonical record 总数）
   - `op_type`（v1 为 canonical `PUT` / `DELETE`；为 future large-value 预留扩展值）
   - `key_len`
   - `value_ref`（仅 PUT；DELETE 不带）
   - payload（key bytes；v1 PUT 不再内联原始 value bytes）
   - `crc32`

3. **sealed trailer（sealed segment 才有）**
   - `segment_gen`
   - `write_end`
   - `min_lsn`
   - `max_lsn`
   - `sealed`
   - `crc`

这里把 `segment_gen` 放进 entry 的目的只有一个：

- segment 被复用后，recovery 能识别“当前 generation 的有效前缀”和“旧 generation 残留字节”，而不依赖任何持久化 flush frontier。

补充说明：

- v1 不定义独立的持久化 `segment_seq`。同一 stream 内“这是第几次换段”的顺序若未来需要，只能作为诊断/观测字段另行讨论，不能让 recovery correctness 依赖它。
- `segment_gen` 的语义是“同一物理 `segment_id` 被重新投入 ACTIVE 的复用代数”，它和 stream 内逻辑换段顺序不是一回事。

额外约束：

1. segment header 是固定大小前缀；entries 从 header 之后紧密排列。
2. entry 可以跨 `lba_size` 页边界，但不能跨 segment 边界。
3. 同一 segment 可以承载多个不同 `batch_lsn` 的 entries。
4. 同一 batch 也可以散落到多个 segments；recovery 一律按 `lsn + entry_count` 重组，而不是按 segment 解释 batch 边界。
5. `wal_segment_size` 扣除固定 segment header 前缀后，必须仍大于 v1 单条 entry 的最大编码长度。
6. WAL 中保存的是 canonical batch image，而不是原始 batch 内的中间步骤；因此同一 `batch_lsn` 在 WAL 中对同一逻辑 key 最多出现一次。
7. v1 的 WAL PUT record 是 `key + data_ver(lsn) + value_ref`，不是 `key + raw value bytes`。

### 11.3 Per-front-scheduler append 运行时

每个 wal stream 至少维护以下运行时状态：

1. 当前 `active segment`
2. 当前 `write_offset`
3. 当前 tail page 的内存镜像 `tail_buf`
4. 当前 active segment 的 `seg_max_lsn`
5. 已 seal 但尚未回收的旧 segments 列表

其热路径规则是：

1. entry 永远顺序追加到本 stream 的 active segment。
2. FUA 写的设备粒度按 `lba_size` 页对齐；因此当前 tail page 需要在内存中维护一份 page image。
3. 同一 tail page 在 entry 追加过程中可以被多次整页重写；这是幂等的，因为更早的字节已经属于同一页 image 的前缀。
4. 只有当 entry 放不进当前 segment 的剩余空间时，才触发换段。

换段时：

1. 当前 segment 进入 `SEALED`。
2. 如有机会，补写 sealed trailer。
3. 旧 segment 连同其 `min_lsn/max_lsn` 进入待回收集合。
4. 再申请新 segment，写入 segment header，继续追加。

### 11.4 Segment 分配与反压

segment 分配的唯一允许路径是：

```text
free_pool.try_dequeue()
-> 否则 alloc_head.fetch_add()
-> 仍失败则进入 WAL backpressure
```

规则：

1. 反压发生在“本 stream 无法获得新的 active segment”时。
2. 不允许把一个 front scheduler 的新写借道到别的 front scheduler 的 active segment。
3. 不允许为了避免反压而破坏 `same key -> same front scheduler` 或 `single-writer-per-segment`。
4. 详细设计可以决定 backpressure 是挂起 sender、排队等待，还是显式返回 busy，但不能改这条语义边界。
5. 如果选择显式返回 `busy`，它必须发生在 batch 被分配 `batch_lsn` 之前；一旦 batch 已进入 gap-free `batch_lsn` 序列，后续 WAL 压力只能让它等待、排队，或触发运行时终止，不能留下永久 LSN hole 后继续服务。

### 11.5 Segment 生命周期

每个 segment 的状态机固定为：

```text
FREE -> ACTIVE -> SEALED -> FREE
```

规则：

1. ACTIVE 段只有一个 owner stream，可以顺序追加。
2. SEALED 段不再接受新 entry。
3. 回收粒度永远是整个 segment。
4. 绝不允许多个 front schedulers 并发写同一个 ACTIVE segment。

### 11.6 恢复扫描规则

恢复扫描 WAL area 时：

1. 遍历所有 segments。
2. header 不合法 -> 直接跳过。
3. header 合法 -> 按 `segment_gen` 扫描 entries。
4. 如果有合法 sealed trailer，可用 `write_end` 快速定界；否则按“首条坏 entry / generation 不匹配处停止”处理 crashed ACTIVE 段。
5. 把所有 CRC 通过的 entries 收集出来，按 `lsn` 分组。
6. 只有 `actual_count == entry_count` 的 batch 才算完整 batch。

这里的 `actual_count / entry_count` 比较的是 canonical records 数；因为 WAL 中不保存原始 batch 内中间步骤，所以 recovery 不需要额外的 intra-batch 顺序号。

对恢复可用性再补充一条：

- complete batch 的判定仍只看 `actual_count == entry_count`；v1 boot recovery 本身不要求读取或校验 PUT 对应的 value body，但详细设计必须保证 entry 中的 `value_ref` 指向一个在 recovery 期间不会被提前复用的 stable durable value object。

### 11.7 共享元数据只出现在换段与回收时

WAL 快路径不允许碰全局共享 tail。共享状态只包括：

1. `free segment pool`
2. `active segment ownership`
3. `sealed segments waiting for reclaim`

因此：

1. 日常 append 是 per-front-scheduler 私有快路径。
2. 只有换段申请和 segment 回收才碰共享空间管理。

## 12. Recovery 规范

### 12.0 最高准则

**Recovery 就是一次在 boot 时执行的 flush。** 如果你的理解需要以下任何一项才能成立，说明理解错了：

1. ❌ 扫描或读取 Value Area（哪怕一个 value body / value header）
2. ❌ 把 tree 重写回 slot 0 或从 scratch 重建
3. ❌ 需要 `dead_value_refs` 才能保证 allocator 不泄漏（`live_value_refs` 是占用状态的唯一真相；不在 live 中的 slot 就是 free，无论是已知 dead、orphan、还是从未写入）
4. ❌ 引入盘面白名单之外的持久化元数据才能完成恢复

Recovery 的全部工作：
1. 确定 logical winners = merge(tree leaf records, complete WAL batches)
2. 做一次与 steady-state 同构的增量 flush（CoW 写新 slot，直接复用 winner 的 `value_ref`，不碰 value body）
3. TRIM 旧 slots / 旧 ranges / dead values，重建 allocator runtime state（这些是清理和优化，不是正确性前提）

`dead_value_refs`（从 all_referenced - live 以及 incomplete batches 中得到）只是辅助信息，用于加速 class 识别和 TRIM 判定。即使完全丢弃 `dead_value_refs`，allocator 仍可以仅从 `live_value_refs` 反推出完整的占用/空闲状态。

### 12.1 恢复目标

恢复完成后的系统状态必须是：

1. WAL 空或已重建成新的空 active segments。
2. 没有旧 `publish_catalog` / `imm` / `checkpoint_guard` 继续残留。
3. tree 回到 clean frontier：每个 range 只保留当前有效 slot；若某些 key 的当前 winner 仍是必须保留的 tombstone，则 clean tree 仍可以包含 tombstone。
4. `current_publish_catalog` 指向新的 clean state。
5. Value Area 不要求在 boot recovery 时被全量重写或 compact；只要求 live `value_ref` 集合与恢复后重建出的 value allocator / free pool 运行时状态自洽。

### 12.2 恢复步骤

恢复流程固定为：

```text
1. 读取 superblock A/B，取出格式常量与区域边界
2. 从 `superblock.root_base_paddr` 出发递归遍历 tree，收集所有 CRC-valid leaf records；root 为 null 则 leaf record pool 为空。遍历只读 tree pages，不读任何 value body
3. 扫描全部 WAL segments，收集所有完整 canonicalized batches（PUT 记录 `value_ref`）
4. 将 leaf records 与 surviving complete canonicalized batches 按逻辑 key / `data_ver` fold，先重建 crash 后应存活的最新逻辑 KV winner 集合
5. 在已遍历的 tree 基础上增量合并 WAL winners（本质上与 steady-state flush 同构）：
   - 不变的 leaf page → 保留现有 slot，零写入
   - 有变化的 leaf page → CoW 写入同一 range 的 next slot
   - 结构变化（split/新 key）→ 正常分配新 range
   - winner 为 value → 直接复用 winner `value_ref`；不读取、不校验、也不重写旧 value body
   - winner 为 tombstone → boot recovery 一律保守保留 tombstone，不在此阶段直接变成 absent
   - 如果 WAL 为空（无变化），零 tree page 写入
   - 在内存中构造新的 clean `tree_manifest`（每个 range 记录当前有效 slot_index）
   - 有写入时才 NVMe FLUSH
6. 必要时更新 superblock A/B 中的 clean root 信息
7. 生成新的 clean checkpoint_guard = G_clean
8. TRIM old tree slot / old tree range，并依据 live 与 dead `value_ref` 集合回收 definitively-dead value extents（sub-LBA dead slot 不逐个 TRIM，只在整页全 dead 时才 TRIM）
9. 从 rebuilt clean tree、live `value_ref` 集合以及可选的 dead `value_ref` hint 重建 allocator runtime state：
   - tree allocator = 最大 live range 之后的第一个可分配位置
   - value allocator head = 最低地址 live value 之前的 fresh bump 位置；若无 live value，则回到 `data_area_end_paddr`
   - `occupied` 由 live `value_ref` 集合唯一确定；`free` 是整个 Value Area 对 `occupied` 的语义补集，而不是 `live ∪ dead` 这类“被 surviving refs 看见的页面集合”的补集
   - 对有 live sibling 的 sub-LBA page，hole mask 由 `~live_slots` 直接反推；对 whole-free region，`dead_value_refs` 只用于 class / TRIM hint，没有 surviving ref 的 region 也必须在 recovery 后立即可分配
   - allocator free pools 全部在内存重建，不持久化
10. 重置 WAL segment pool，安装新的空 active memtables
11. 设置 recovered_max_lsn = max( surviving complete WAL batches 的最大 batch_lsn,
                                  rebuilt clean logical state 的最大 data_ver )
12. 组装 PRS_clean = { G_clean, fresh active sets, no imms }
13. 组装 CAT_clean = { durable_lsn = recovered_max_lsn, prs = PRS_clean }
14. 设置 current_publish_catalog = CAT_clean
15. 设置 next_lsn = recovered_max_lsn + 1，开始服务
```

### 12.3 恢复语义说明

这里有十一个关键点：

1. recovery 不尝试恢复旧 runtime frontier，只恢复成新的 clean runtime；这里的 clean 指 tree frontier 与 runtime ownership 收敛，不要求 boot 时全量重写 live values。
2. crash 后旧 `checkpoint_guard/tree_manifest` 全部消失，因此 recovery correctness 不依赖持久化 `slot_seq`，也不依赖 root-slot-derived frontier。
3. recovery 的 source of truth 是”从 superblock root 遍历 tree 得到的 CRC-valid leaf records + ALL surviving complete WAL batches + stable winner `value_ref` 集合”。recovery 全程不读任何 value body。
4. internal nodes 在 recovery 中用于遍历导航（从 root 下降到 leaf），不是独立的 correctness source。旧 runtime manifest 不参与 recovery。
5. surviving WAL 中的每个完整 batch 保存的都是 canonical batch image，而不是原始 batch 内中间步骤；因此 recovery replay 不需要再解释 batch 内顺序，只需要按 `batch_lsn` 比较这些 canonical records。
6. `recovered_max_lsn` 不能只看 surviving WAL；它还必须覆盖 rebuilt clean logical state 中的最大 `data_ver`。否则当 WAL 已被清空而 tree 中仍保留更老但仍 live 的记录时，恢复后的 `durable_lsn/next_lsn` 会倒退。
7. recovery 收敛到 clean tree 的过程在逻辑 key 维度上按 `data_ver` 幂等；`data_ver` 在语义上等价于该记录对应的 `batch_lsn`，因此某个 batch 即使已经体现在旧 leaf records 中，再次 replay 也不能改变最终可见状态。
8. recovery 在 winner 判定与 clean tree 重建阶段都不需要读取任何 value body；v1 boot recovery 直接复用 stable winner `value_ref`，不做 value scrub，也不做 full-value rewrite。若未来引入维护命令，再单独定义其校验 / 重写语义。
9. boot recovery 不依赖旧 runtime 的 `recovery_safe_lsn`；因此 winner 为 tombstone 时，clean tree 必须保守保留 tombstone。是否允许后续变成 absent，留给新 runtime 在重新建立 `recovery_safe_lsn` 后、于后续被重写的 leaf 上 opportunistically 决定。
10. 运行时之所以不持久化 allocator head，是因为 recovery 可以从 rebuilt clean tree 与 live `value_ref` 集合重新推导 tree/value allocator runtime state；free pools 也在内存中重建，而不是靠 boot 时重写 live values 获得。
11. 运行时之所以需要 old CAT / old guard / old imm，是为了保护并发 reader；recovery 本身没有并发 reader，因此可以直接收敛到 clean state。

## 13. 单盘 v1 对未来多盘的兼容约束

下面这些约束是为了保证以后扩多盘时不需要推倒重来：

1. **单棵逻辑树不变**
   - 多盘也仍是一棵逻辑树。
   - 节点和 value 只是在不同 `device_id` 上分配。

2. **地址类型从一开始就是 device-aware**
   - 所有 child pointer / value pointer / root pointer 都是 `paddr`。

3. **WAL segment id 从一开始就是 `(device_id, index)`**
   - v1 虽然只有一块盘，但 future multi-disk WAL 不需要换类型。

4. **allocator / free_pool / TRIM 调用都显式带 device 归属**
   - v1 只有一个实例。
   - 多盘时扩成 per-device 实例数组即可。

5. **publish/read/recovery 语义与设备数无关**
   - `publish_catalog`
   - `read_handle`
   - `checkpoint_guard/tree_manifest`
   - `recovery clean rebuild`
   这些都不能因为设备数增加而改语义。

未来多盘要新增的只是：

1. tree/value 的放置策略
2. WAL segment 的设备选择策略
3. 跨设备并行 I/O 计划
4. root / superblock 的冗余策略

它们都属于详细设计层，不改变本文冻结的系统语义。

## 14. 典型 PUMP Pipeline（非规范伪码）

本章只描述主要路径在 PUMP 中应当呈现出的执行形状，帮助后续详细设计统一 owner、切换点和 fan-out/fan-in 骨架。

这里的伪码不是规范 API，真正的系统语义仍以前文各章为准。

记号约定：

1. `A >> B`：表示前一步完成后，把结果继续交给后一步，相当于 `bind_back`
2. `on(S, X)`：切到 scheduler `S` 执行 `X`
3. `when_all(...)`：并发分叉并等待全部完成
4. `if_miss(X)`：仅当前一步 miss 时才继续执行 `X`

执行域约定：

1. `coord_sched`：`batch_lsn` 分配、`publish_gate`、`current_publish_catalog` 的 owner
2. `front(owner)`：当前运行期某个前台 owner，负责 WAL append、memtable、seal/release
3. `tree_sched`：tree-local flush round、allocator、manifest 构造和 tree-side reclaim 的 owner
4. `tree_read_domain(shard_idx)`：tree traversal / `keys_to_leaf_groups()` 和 old leaf read / candidate build 的 owner。own `lookup` 和 `worker` 两个 arm，两者共享 read_domain 的 `node_cache`。
5. `value_alloc_sched`：value write/read、allocator metadata 与 value-page cache 的 owner
6. `wal_space_sched`：WAL segment 空间与回收的 owner
7. `nvme(dev)`：设备 owner

### 14.1 前台写入 / 提交

```text
write_batch(req)
  = on(coord_sched, prepare_canonical_entries_and_assign_batch_lsn(req))
 >> on(coord_sched, route_canonical_entries())
 >> on(value_alloc_sched, persist_put_values())          // leader-follower: alloc + fill DMA + FUA
 >> when_all(                                             // value durable 后 fan-out 到 front_scheds
      on(front(f0), wal_append_ref_fua(frag0) >> memtable_insert_handle(frag0)),
      on(front(f1), wal_append_ref_fua(frag1) >> memtable_insert_handle(frag1)),
      ...
    )
 >> on(coord_sched, await_publish_gate_if_needed(batch_lsn))
 >> on(coord_sched, publish_durable_lsn(batch_lsn))
 >> on(coord_sched, ack(req))
```

其中 WAL 追加在骨架上可继续展开成：

```text
wal_append_ref_fua(frag)
  = ensure_active_segment()
 >> append_to_segment(frag)
 >> fua_write_tail_page()
```

而：

```text
ensure_active_segment()
  = try_use_current_segment()
 >> if_full_then(on(wal_space_sched, alloc_next_segment()))
```

### 14.2 Point GET

```text
point_get(key)
  = on(coord_sched, acquire_read_handle())
 >> on(front(owner_front(key)), lookup_memtable(key, read_lsn, prs.fronts[owner]) >> if_memtable_value_then_return_view())
     >> if_miss(
      // tree::lookup sender picks the shard internally via
      //   shard_idx = core::registry::current_shard_partitions()->route(key)
      //   home      = core::registry::tree_read_domain_at(shard_idx)
      // so the caller never hand-picks a tree scheduler pointer
      // (INC-003 / INC-040 / step 030 §2.6 / §6.4 F2).
      tree::lookup({key}, tree_guard.manifest)
      >> decode_leaf_record()
      >> if_value_then_read_value_ref_else_not_found()
    )
```

### 14.3 MultiGet

```text
multi_get(keys)
  = on(coord_sched, acquire_read_handle())
 >> on(coord_sched, group_by_owner_front(keys))
 >> when_all(
      on(front(f0), lookup_memtables(keys_on_f0, read_lsn, prs.fronts[f0])),
      on(front(f1), lookup_memtables(keys_on_f1, read_lsn, prs.fronts[f1])),
      ...
    )
 >> tree_fill_misses_via_tree_read_domains(tree_guard)
 >> group_tree_value_refs_by_value_page()
 >> value_fill_tree_hits_via_value_alloc_sched_grouped_by_page()
 >> merge_results_and_hide_tombstones()
```

### 14.4 Range Scan

```text
range_scan(begin, end)
  = on(coord_sched, acquire_read_handle())
 >> when_all(
      on(front(f0), scan_memtable(begin, end, read_lsn, prs.fronts[f0])),
      on(front(f1), scan_memtable(begin, end, read_lsn, prs.fronts[f1])),
      ...,
      on(tree_read_domain(route_tree_scan(begin, end)).lookup,
         tree_scan(tree_guard, begin, end))
    )
 >> merge_memtable_over_tree_and_hide_tombstones()
 >> group_tree_value_refs_by_value_page()
 >> value_fill_tree_hits_via_value_alloc_sched_grouped_by_page()
 >> stream_out()
```

### 14.5 Seal / Rotate

```text
seal_round()
  = on(coord_sched, close_publish_gate())
 >> when_all(
      on(front(f0), seal_active_and_install_new_active()),
      on(front(f1), seal_active_and_install_new_active()),
      ...
    )
 >> on(coord_sched, build_prs1_and_install_cat1())
 >> on(coord_sched, open_publish_gate())
```

### 14.6 Flush / Frontier Switch

```text
flush_round()
  = on(tree_sched, choose_flush_eligible_gens())
 >> when_all(
      on(front(f0), pin_selected_imms()),
      on(front(f1), pin_selected_imms()),
      ...
    )
 >> on(tree_sched, tree_local_flush({
      .base_guard = old_guard,
      .sealed_gens = pinned_gens,
      .recovery_safe_lsn = recovery_safe_lsn,
    }))
 >> on(tree_sched, update_flush_max_lsn(tree_flush_result.flushed_max_lsn))
 >> on(coord_sched, build_g1_prs2_and_install_cat2())
 >> // retired 由旧 G0 析构时自动投递到 tree_sched，不在此处 enqueue（见 flush_and_frontier_switch.md §8.1）
 >> maybe_root_change(
      on(tree_sched, update_superblock_async(new_root_base_paddr, covered_lsn))
    )
```

### 14.7 Recovery / Clean Start

```text
recover()
  = read_superblock_a_b()
 >> when_all(
      traverse_tree_collect_leaf_records(root_base_paddr),
      on(nvme(d0), scan_wal_segments())
    )
 >> on(coord_sched, assemble_complete_batches())
 >> on(tree_sched, rebuild_latest_logical_winners_by_data_ver())
 >> on(tree_sched, merge_winners_into_existing_tree_and_build_clean_manifest_reusing_winner_value_refs())
 >> on(tree_sched, reclaim_dead_value_extents_and_rebuild_value_runtime_state())
 >> on(tree_sched, trim_old_tree_and_reset_wal())
 >> on(coord_sched, install_clean_cat_and_next_lsn())
```

### 14.8 WAL Segment 回收

```text
wal_reclaim_round()
  = on(tree_sched, compute_recovery_safe_segments())
 >> on(wal_space_sched, recycle_segments_to_free_pool())
```

## 15. 详细设计必须回答的五个模块

在本文基础上，详细设计应拆成下面五份，不要再回到“重新讨论总语义”：

额外边界：

1. `on_disk_formats.md` 只能细化第 `2.5` 节列出的持久化白名单对象。
2. 如果详细设计需要新增新的持久化对象类型，或把第 `2.5` 节明确禁止持久化的 runtime 状态落盘，必须先回到本文修改概要语义，再继续实现。

1. **runtime_state_machine.md**
   - `batch scheduler`
   - batch canonicalization
   - front scheduler
   - `value object durable -> WAL(ref) durable -> memtable insert` 的前台顺序
   - tree scheduler
   - wal_space scheduler
   - `checkpoint_guard / tree_manifest`
   - `publish_gate` / seal 协议
   - `ValueHandle / kv_arena` 生命周期（per-gen arena 承载 kv bytes，shared_ptr<memtable_gen> 是唯一 pin gate）与 `value_page readonly_frame_cache` 的读路径集成

2. **on_disk_formats.md**
   - superblock A/B
   - WAL segment header / canonical ref entry / trailer
   - tree slot header
   - tree leaf record / tombstone encoding
   - value object header

3. **flush_and_frontier_switch.md**
   - sealed gen 选择
   - fold 算法
   - `tree_manifest` 增量构造
   - old tree-visible `value_ref` 何时挂到 old guard 的 retire list
   - `memtable-only loser durable_ref` 何时挂到 owning `memtable_gen`
   - root-change / root-stable 两类 flush 的处理
   - old CAT / old guard 的回收挂接

4. **recovery_and_wal_reclaim.md**
   - Data Area leaf scan
   - WAL ref 扫描实现
   - canonical batch 完整性判定
   - live `value_ref` 集合、value allocator 与 value free pool 的恢复重建
   - 在现有 tree 上 merge/flush WAL winners，得到 clean tree
   - tombstone 何时允许物理删除
   - value free pool / allocator head 在 steady-state 与 boot recovery 下如何闭环，避免 value 泄漏
   - `recovery-safe` / WAL reclaim 条件的具体运行时表示

5. **read_api_and_pipeline.md**
   - GET / MultiGet / Scan 的 `read_handle` 生命周期
   - batch cache
   - memtable hit 如何经由 `value_view` (指向 owning gen `kv_arena`) 返回且不退化成 SSD 读
   - memtable miss -> `tree_manifest` 驱动的 tree lookup sender 编排
   - tree tombstone 的读语义
   - 长读拖住 old CAT / old guard 的资源上界策略

## 16. 一页结论

Inconel 的最终概要模型可以压缩成下面几句：

1. **写的逻辑提交点是 `value object durable + canonicalized WAL(ref) durable + canonical memtable insert + durable_lsn publish`。**
2. **flush 只是后台物化，不决定可见性。**
3. **reader pin 的不是一个裸全局 LSN，而是一份 `publish_catalog`。**
4. **一次逻辑读调用共享一个 `read_handle = {cat, read_lsn}`。**
5. **tree 的结构一致性由 runtime 的 `checkpoint_guard/tree_manifest` 保证；它不要求持久化 `slot_seq`。**
6. **DELETE 在 tree 中采用 `value -> tombstone -> absent` 的两阶段物化；tombstone 参与数据 winner 比较，物理删除在后续 leaf rewrite 时按 `recovery_safe_lsn` opportunistically 完成。**
7. **WAL PUT 只保存 `value_ref`；memtable PUT 持有 `ValueHandle { durable_ref, value_view into kv_arena }`，因此 memtable hit 永远不退化成 SSD 读。**
8. **WAL 用共享 segment pool 提供共享容量，但并发只发生在不同 segment 之间。**
9. **恢复不重建旧 runtime frontier，而是 `scan tree leaf records + surviving WAL refs -> rebuild logical winners -> merge/flush into clean tree(reuse winner value_ref) -> reconstruct value runtime state -> clean start`。**
10. **持久化白名单只有 `superblock / WAL / tree / value` 四类对象；所有 publish/frontier/reclaim 状态都是运行时对象。**
11. **`front_sched_count` / WAL stream 数 / memtable 拓扑都是运行时部署参数；clean recovery 后允许和上一次运行不同。**
12. **单盘 v1 只是 `device_count = 1`，不是把接口写成只支持一块盘。**
