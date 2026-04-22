# Inconel 代码模块划分

> 本文档定义 `apps/inconel/` 的模块结构、职责边界和依赖关系。实现时据此确定代码归属。

## 模块总览

```
apps/inconel/
├── format/        L0 磁盘格式
├── core/          L0 运行时域对象 + scheduler 注册表
├── memory/        L1 DMA 池 + 帧管理
├── nvme/          L1 NVMe I/O（SPDK，正式环境）
├── mock_nvme/     L1 内存模拟 NVMe（测试环境）
├── coord/         L2 协调器 scheduler
├── front/         L2 前端 scheduler
├── tree/          L2 B+ Tree 域
├── wal/           L2 WAL 空间管理 scheduler
├── value/         L2 Value 分配与读取 scheduler
├── pipeline/      L3 顶层 pipeline 编排
├── recovery/      L3 启动恢复
└── runtime/       L3 配置 + 初始化 + main
```

## 依赖层次

```
        format (L0)     core (L0)
           ↘           ↙
         memory (L1)
        ↙       ↓       ↘
   nvme (L1)  mock_nvme (L1)
      ↘       ↙
  coord  front  tree  wal  value   (L2 — 互不依赖)
     ↘    ↓    ↙      ↙     ↙
      pipeline  recovery           (L3)
          ↘     ↙
         runtime                   (L3)
```

**关键约束：L2 的 5 个 scheduler 模块之间互不依赖。** 跨 scheduler 协作完全通过 L3 pipeline 层的 sender 编排完成，不通过直接调用。

**Scheduler 模块接口约束：每个 scheduler 模块对外只暴露 `sender.hh` 一个接口。** 其他模块使用该 scheduler 的功能时，只允许 `#include "模块/sender.hh"`。模块内部的 `scheduler.hh`、`device.hh` 等属于实现细节，不直接对外暴露。例外：`runtime/` 需要访问 scheduler 构造函数和 `advance()` 来完成初始化和主循环，这不受此约束限制。

此约束适用于所有含 scheduler 的模块：L1（nvme、mock_nvme）和 L2（coord、front、tree、wal、value）。

---

## L0 — 基础定义

### format/

磁盘格式定义。纯 POD 结构 + 常量 + 序列化/反序列化工具，零运行时逻辑。

| 职责 | 内容 |
|------|------|
| 地址类型 | `paddr`、`range_ref`、`value_ref` |
| Superblock | A/B 双槽布局、magic、format_version、格式化参数、CRC |
| WAL | segment header、entry header、trailer、跨 LBA 拼接规则 |
| Tree Page | slot header、internal/leaf 节点格式、shadow range、空 slot 检测 |
| Value Object | value object header、size class 布局、sub-LBA 策略 |
| 公共工具 | CRC-32C 计算、magic 常量、对齐辅助函数 |

**设计文档**：`on_disk_formats.md` 全文

**不包含**：任何运行时状态、引用计数、allocator 逻辑。

### core/

运行时域对象和 scheduler 注册表。定义多个 scheduler 共同引用的内存对象，以及 scheduler 实例的全局访问入口。

#### 域对象

| 类型 | 关键字段 | 引用方 |
|------|---------|--------|
| `publish_catalog` | `prs`, `durable_lsn` (atomic), `epoch` | coord(owns), pipeline(carries), 读路径(pins) |
| `published_read_set` | `tree_guard`, `fronts[]`, `epoch` | coord(构造), pipeline(快照), front/tree(查询) |
| `checkpoint_guard` | `manifest`, `retired` | tree(owns), coord(frontier switch), pipeline |
| `tree_manifest` | `root_slot`, `slot_map`, `leaf_order` | tree(owns/构造), tree_lookup(只读消费), tree_worker(只读消费) |
| `read_handle` | `cat`, `read_lsn` | coord(发放), pipeline(携带), front/tree/value(消费) |
| `memtable_gen` | `gen_id`, `state`, `front_owner_index`, `min/max_lsn`, `kv_arena`, `table`, `loser_durable_refs`（生命周期由 `std::shared_ptr<memtable_gen>` 管理，无内嵌 refcount） | front(owns), coord(seal), tree(fold / loser attach) |
| `memtable_entry` | `data_ver`, `kind`, `vh` | front(存储/查询), pipeline(传递) |
| `value_handle` | `durable: value_ref`, `hot: value_view`（POD，指向 owning gen `kv_arena` 切片） | front(memtable 内), pipeline（view 返回） |
| `gen_arena` | `chunks: vector<unique_ptr<char[]>>, bump_next, bump_end`；单 writer bump allocator，承载 key + value bytes | core（随 memtable_gen 生灭） |
| `value_view` | `data: const char*, len: uint32_t`；zero-copy 返回类型 | front(lookup 返回), pipeline |
| `front_read_set` | `active`, `imms`（均为 `std::shared_ptr<memtable_gen>`） | coord(seal 产出), pipeline(读路径携带) |
| `canonical_entry` | `op_type`, `key`, `value`, `allocated_vr` | coord(产出), front/value(消费) |
| `batch_ctx` | `batch_lsn`, `entry_count`, `canonical_entries[]`, `fragments[]` | coord(产出), pipeline(携带) |
| `fragment` | `owner`, `batch_lsn`, `entry_count`, `entries[]` | coord(构造), front(消费) |

#### Scheduler 注册表与路由

| 内容 | 说明 |
|------|------|
| 各类 scheduler 实例列表 | `by_core[]` 数组 + `list` vector，同 KV 的 `scheduler_objects.hh` 模式 |
| `route_to_front(key_hash)` | `key_hash % front_sched_count` |
| `current_shard_partitions()->route(key)` | key → `shard_idx`（单次二分，成本 `log2(shard_count)`）。同 shard 的 key 必定同 read_domain（INC-040 / step 030；**不是** `front_owner % K` 的 hash 映射，也**不是** `leaf_order.find_leaf_for_key(key) % K` 的 ordinal 映射） |
| `install_shard_partitions(m)` | 原子替换 `shared_ptr<const shard_partition_map>`；builder 启动时装占位，`tree_sched` 每次 flush 完成后基于新 `leaf_order` 重建 |
| `tree_read_domain_at(idx)` / `local_tree_read_domain()` | read_domain 索引入口；访问 scheduler 走 `.lookup_sched` / `.worker_sched` base pointer |
| `local_nvme()` | 当前核心的 nvme_sched 实例 |
| singleton 访问 | coord_sched、tree_sched、wal_space_sched、value_alloc_sched |

**设计文档**：`design_overview.md` §5（核心运行时对象）、§1.7（系统组件）、`runtime_state_machine.md` §1（scheduler 总览）、`cross_doc_contracts.md` §2（struct 字段）

---

## L1 — 共享基础设施

### memory/

DMA 内存池和统一帧抽象。所有做 I/O 的 scheduler 共同使用。

| 职责 | 内容 |
|------|------|
| DMA Pool | 按 size + NUMA 分池，per-core cache，spdk_mempool / spdk_dma_malloc_socket |
| 帧类型 | `frame_id`、`frame_state`、`page_frame`、`value_page_frame` |
| 帧状态机 | `dirty_append` → `writeback_inflight` → `clean_readonly` → evicted |
| Pin 管理 | `frame_pin` RAII，pin_count 控制逐出 |
| Frame Cache | readonly frame cache 抽象（`tree_read_domain` 持有，lookup/worker 共享；`value_alloc_sched` 单独持有 value_page 实例） |

**设计文档**：`runtime_memory_and_cache.md` §5-7, §11, §14

### nvme/

正式 NVMe I/O scheduler。每核心每设备一个实例，拥有独立 SPDK qpair。

| 操作 | 说明 |
|------|------|
| FUA write | 写入 + 强制落盘，用于 value object / WAL entry / tree slot |
| read | 读取 tree page / value page |
| FLUSH | NVMe FLUSH 命令，用于 flush 阶段确认 tree slot 持久化顺序 |
| TRIM | deallocate 旧 slot/range/value page |

**设计文档**：`runtime_state_machine.md` §7

### mock_nvme/

内存模拟 NVMe scheduler。与 nvme/ 提供相同的 sender 接口，底层用连续内存替代 SPDK。

| 职责 | 内容 |
|------|------|
| 模拟存储 | `malloc` 分配的连续内存块，按 LBA 寻址 |
| 接口一致 | FUA write / read / FLUSH / TRIM 语义等价于 nvme/，业务代码不感知差异 |
| 测试辅助 | `test_read_raw(lba, len)` — 绕过正常读路径直接访问原始字节，验证格式和数值正确性 |
| 切换方式 | 编译期模板参数或 build 开关，L2 scheduler 对 nvme 类型参数化 |

**用途**：开发阶段功能验证。所有模块的单元测试和集成测试基于 mock_nvme 运行，无需真盘。最终上真盘时切换到 nvme/。

---

## L2 — Scheduler 模块

每个模块对应一种（或一组）scheduler，拥有其内部状态、请求类型和 handle 逻辑。模块之间互不依赖。

### coord/

协调器 scheduler。全局单实例，系统的中枢。

| 职责 | handle |
|------|--------|
| LSN 分配 + canonicalization | `assign_lsn(raw_batch)` → `(batch_lsn, canonical_entries, entry_count, route_table)` |
| 发布推进 | `publish_batch(batch_lsn)` → void（推进 durable_lsn 连续前缀） |
| 受限 clean abort | `release_batch(batch_lsn)` → void（LSN 槽位 resolved-empty） |
| 读 handle 发放 | `acquire_read_handle()` → `read_handle { cat, read_lsn }` |
| Seal 触发 + gate | `close_gate()` / `open_gate()`，seal 条件判定 |
| Frontier switch | `frontier_switch(old_guard, new_manifest, retired, flushed_gens)` → 构造安装 CAT2 |
| Catalog 安装 | `install_cat(new_catalog)` |
| Flush frontier 捕获 | `capture_flush_frontier()` → `{ durable_lsn, old_guard }` |

**Owner 状态**：`coord_state { next_lsn, gate, current_cat, ready_set, seal_in_progress, cat_epoch }`

**设计文档**：`runtime_state_machine.md` §2

### front/

前端 scheduler。N 实例，`key_hash % N` 路由。每个实例独立拥有一条 WAL stream 和 memtable 代际链。

| 职责 | handle |
|------|--------|
| WAL 写入 | `write_wal_entries(batch_lsn, entry_count, entries[])` → FUA append |
| Memtable 插入 | `insert_memtable_entries(batch_lsn, entries[])` → CPU memtable insert |
| Seal | `seal_active()` → `front_read_set`（rotate active→sealed，创建新 active） |
| Memtable 查询 | `lookup_memtable(key, read_lsn, front_read_set)` → `variant<value_view, tombstone, miss>` |
| 批量查询 | `batch_lookup(keys[], read_lsn, front_read_set)` → results[] |
| Range scan | `scan_memtable(begin, end, read_lsn, front_read_set)` → `scan_result_set` |
| Flush 支持 | `collect_eligible_gens(durable_lsn)` → eligible_gens[]，`release_gens(gen_ids[])` |
| WAL segment rotation | 当前 segment 写满时向 wal_space_sched 申请新 segment |

**Owner 状态**：`front_state { owner_id, active, imms[], wal_stream }`

**设计文档**：`runtime_state_machine.md` §3

### tree/

B+ Tree 域。包含 `tree_sched`（单实例，状态拥有者）和 K 个 `tree_read_domain<Cache>`（每 core 一个，own lookup/worker）。每个 read_domain 通过 `advance()` 代驱所属 `lookup` 和 `worker`，PUMP runtime tuple 只注册 read_domain（step 030 §6.5 G1）。

| Scheduler | 职责 | handle |
|-----------|------|--------|
| tree_sched | Tree-local flush round owner | `tree_flush(tree_flush_request)` → `tree_flush_result` |
| tree_sched | Tree slot 写入 | shadow slot 选择、consolidation（leaf/internal/root split） |
| tree_sched | Manifest 构造 | deep_copy old → 更新 `slot_map + leaf_order` → 替换 root（如变更） |
| tree_sched | Allocator 管理 | tree_allocator { head, free_ranges, shared_heads } |
| tree_sched | Checkpoint guard | 构造 new guard、挂载 retired list |
| tree_sched | Reclaim | TRIM old slots/ranges、投递 value 回收到 value_alloc_sched |
| tree_sched | Shard partition rebuild | flush 完成后基于新 `leaf_order` `build_initial_shard_partition_map` 并 `install_shard_partitions` 原子替换（B1 目标设计，step 030 暂未触发） |
| tree_read_domain.lookup | Tree 查询 | `tree_lookup(key, manifest)` → `variant<leaf_value, leaf_tombstone, absent>` |
| tree_read_domain.lookup | Leaf mapping | `keys_to_leaf_groups(flush_lookup_req)` → `flush_leaf_group_result[]` |
| tree_read_domain.worker | Candidate build | `build_leaf_candidates(flush_worker_req)` → `flush_candidate_batch` |
| tree_read_domain | Frame cache shard | `Cache node_cache`（lookup / worker 在同一 `Cache` 类型上模板化共享） |

**Owner 状态**：tree_sched `{ tree_allocator, checkpoint_guard, flush_state }`；tree_read_domain `{ partitions: shared_ptr<const shard_partition_map>, node_cache: Cache, lookup: unique_ptr, worker: unique_ptr }`；lookup / worker 持 `tree_read_domain<Cache>*` back-ref，访问 cache 和 `read_domain_index` 全部走 back-ref。

**设计文档**：`runtime_state_machine.md` §4，`flush_and_frontier_switch.md` §3/§8/§9

### wal/

WAL 空间管理 scheduler。全局单实例，管理 WAL segment 池。

| 职责 | handle |
|------|--------|
| Segment 分配 | `alloc_segment(stream_id, sealed_info?)` → `segment_runtime*` |
| 回收检查 | `reclaim_check(recovery_safe_lsn)` → 释放可回收 segment |

**Owner 状态**：`{ segment_free_pool, alloc_head }`

**设计文档**：`runtime_state_machine.md` §5，`design_overview.md` §11

### value/

Value 分配与读取 scheduler。全局单实例，集中管理 Value Area 的分配、写入、读取和回收。

| 职责 | handle |
|------|--------|
| 写入分配 | `persist_put_values(batch PUT entries)` → `value_ref[]`（leader-follower 合并） |
| 单条读取 | `read_value(value_ref)` → owning bytes |
| 批量读取 | `read_page_values(value_read_group)` → owning bytes[] |
| Value 回收 | `reclaim_values(dead_value_refs[])` |
| TRIM drain | `drain_trim_pending()` |

**Owner 状态**：`value_alloc_state { bump_head, per-class pools, open_frames, dirty_pages, deferred_freed, trim_pending_pages, readonly_frame_cache }`

**设计文档**：`runtime_state_machine.md` §6，`write_path_and_pipeline.md` §5，`runtime_memory_and_cache.md` §9

---

## L3 — 编排与启动

### pipeline/

顶层 pipeline 编排。纯 sender 组合，不持有任何状态。将 L2 各 scheduler 的操作串联为端到端流程。

| Pipeline | 路径 |
|----------|------|
| 写 | coord(assign_lsn) → value(persist) → fan-out front[](WAL FUA) → reduce → fan-out front[](memtable) → reduce → coord(publish) |
| Point GET | coord(acquire_read_handle) → front(lookup_memtable) → [miss] → tree_lookup(tree_lookup) → [hit] → value(read_value) |
| MultiGet | coord(acquire_read_handle) → fan-out front[](batch_lookup) → reduce → group misses → fan-out tree_lookup[](batch) → reduce → group by page → fan-out value(read_page_values) → merge |
| Range Scan | coord(acquire_read_handle) → fan-out front[](scan_memtable) + tree_lookup(tree_scan) → merge |
| Seal | coord(close_gate) → fan-out front[](seal_active) → reduce → coord(build_prs, install_cat, open_gate) |
| Flush | tree(check) → coord(capture_frontier) → fan-out front[](collect_gens) → reduce → tree(tree_local_flush) → tree(update_max_lsn) → coord(frontier_switch) → fan-out front[](release_gens) |

**设计文档**：`write_path_and_pipeline.md` §2，`read_api_and_pipeline.md` §4-6，`flush_and_frontier_switch.md` §8，`design_overview.md` §14

### recovery/

启动恢复。一次性线性流程，从磁盘状态重建 clean runtime。

| 步骤 | 内容 |
|------|------|
| 1. 读 Superblock | CRC 选择 A/B，提取格式化参数 |
| 2-3. 收集数据 | 从 root 遍历 tree 收集 leaf records + 扫描 WAL segments 收集 entries |
| 4-5. 合并重建 | 识别完整 batch、per-key winner merge（tree vs WAL）、提取 live_value_refs |
| 6-7. 增量 flush | flush-like CoW merge 写 tree → NVMe FLUSH → 更新 superblock → TRIM old tree |
| 8-9. Allocator 重建 | tree allocator（head + free_ranges）、value allocator（bump_head + class pools from live_value_refs） |
| 10+. 安装 runtime | 构造 CAT_clean、设置 next_lsn、建立 front 拓扑 |

**设计文档**：`recovery_and_wal_reclaim.md` 全文

### runtime/

配置、初始化和 main 入口。

| 职责 | 内容 |
|------|------|
| 格式化参数 | namespace_size, lba_size, tree_page_size, shadow_slots_per_range, value_size_classes 等 |
| 运行时部署参数 | front_sched_count, tree_read_domain_count（== K shard_partition slots）, 核心绑定策略 |
| 初始化流程 | 创建各 scheduler 实例 → 注册到 core/ 注册表 → 运行 recovery（或格式化新盘）→ 启动 share_nothing 主循环 |
| main 入口 | 参数解析、SPDK 初始化、启动/停止 |

**设计文档**：`design_overview.md` §3.3，§4.1
