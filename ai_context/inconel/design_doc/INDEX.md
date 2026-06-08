# Inconel 设计文档索引

> **必读规则：实现 `apps/inconel/` 的任何功能前，必须完整读取本索引引用的所有设计文档（不是"按需查阅部分章节"，而是全部读取）。** 跳过任何一份文档或只读其中几节，会导致对系统约束的理解不完整，进而做出违反设计的结构决策。

---

## 项目硬约束与性能取向（最高优先级，非谈判项）

Inconel 的定位是**生产级 KV 存储引擎**，不是研究原型，也不是"先跑通再说"的脚手架。这里的 `v1` 只表示**功能范围冻结**，**不表示质量等级下降**：允许少做部分外围或 future 功能，但凡已经纳入范围、已经进入 canonical path、或已经落到 production 代码里的能力，都必须按 production-grade 标准实现。这里冻结的是一条容量硬指标和一条性能取向：

1. **容量下限：10 亿条 KV 起步**。所有设计、carrier 选型、容器选型、算法复杂度、热路径成本都必须按 10 亿 KV 做校准。spec 文本里出现的 "1 亿"、"每 leaf 64 条" 之类数字只是算术举例，不是目标；任何设计都不能只在小量级上自洽，也不能靠全局线性扫描、per-request 物化或其他只在小规模下勉强可用的结构蒙混过关。
2. **性能取向：以 `RocksDB × 5` 作为方向性锚点。** 这不是当前阶段已经承诺可验收的 benchmark 数字，也不是没有标准 workload / 硬件 / 测法时就能直接宣称达成的合同条款；它的作用是强制所有实现按**极速 KV 系统**的标准审视 canonical 读/写路径，禁止为了"先跑通"接受明显拖慢热路径、放大 allocation/copy/queue hop/I/O 成本的结构。

### 不可简化约束

- 任何已经证明会伤害、或当前无法证明不伤害容量下限与性能取向的"简化"**一律禁止**，无论理由多充分、无论是不是"临时的"。不能先按最简单形状落代码，再把容量/效率验证留给以后。
- `v1` 的合法含义只有**少做功能**，没有"把已做功能做成半成品"这层含义。允许暂不支持未纳入范围的能力；但对已经宣称支持、已经接入 canonical path、或已经写进 production 代码的能力，不允许只覆盖 happy path、不允许靠 mock/stub/seam 形态冒充完成、不允许把异常路径/背压/资源回收/边界条件留成"以后再补"。
- "少做功能"的合法含义是**引擎外围能力可以不做**：比如暂不暴露网络协议层、不实现 `MERGE` / batch-atomic / TTL / range-delete / compression / encryption-at-rest / column family 这类可以挂在核心 KV 引擎之外的附加能力。这些省掉不影响存储引擎本身的容量与吞吐。
- "少做功能"的**非法含义**：把"核心引擎组成部分"当 v1 可选简化。以下这些是**必选的生产能力**，任何阶段不允许以"v1 先不做"为由省略：
  - **B+ tree shape-changing 路径的全部**：leaf split / parent rewrite / root change / consolidation。没有 split 树长不大，没有 consolidation 写入就卡死，没有 root-change 高度就封顶——这三条缺任何一条，10 亿 KV 都做不到。
  - **Recovery 全流程**：崩溃后完整重建 CAT clean runtime。
  - **Value allocator / placement / 回收**：sub-LBA / class pools / hole page / deferred reclaim。
  - **WAL / durable_lsn / publish gate**：所有 durability 语义。
  - **Flush / frontier switch / retire list**：后台物化管线和老 guard 回收链。
  - **Memtable gen / seal / 跨 gen 合并**：前台写入骨架。
- v1 **不允许**在容量、或与 canonical 热路径效率直接相关的结构成本上打折扣，即使临时折扣也不行。"先做差一点，后面再优化"这类措辞只要落在会影响 10 亿 KV 可行性，或把实现明显拉离"极速 KV 系统"这个性能取向的位置，**直接视为评审否决项**。
- 阶段拆分是**开发顺序**工具，不是"v1 ship 哪些" 的划分线。允许先做 Phase 7（root-stable 子集）再做 Phase 8（shape-changing 子集）作为实现顺序，**但 v1 发布前两者必须都完成**；不允许 Phase 7 单独作为 "v1" 交付。
- `RocksDB × 5` 在当前阶段是**设计和 review 用的性能锚点**，不是一句可以随口喊的结论。没有独立 benchmark 标准文档前，任何人都不能声称"已经达到这条线"；但也不能拿"反正还没 benchmark"当理由，把明显低效的结构放进 canonical 路径。
- 任何可能触及这两条约束的决定，必须在所在阶段就按 10 亿 KV 的容量基线和"极速 KV 系统"的热路径标准把代价说清楚再落代码；算不清、证据不足或只给口头直觉，等同于不过关。
- Review 时只要发现热路径 / carrier / 容器选择存在"小量级自洽、大量级代价爆炸"的结构性问题，直接退回重设计。

### 容量快速校准参考

下面这张表不是 spec 常量，只是让任何新设计在动笔前有一个量级参照：

| 场景 | 4K tree page | 16K tree page |
|---|---|---|
| 每 leaf 记录数（32B key） | ~64 | ~259 |
| 10 亿 KV 下 leaf 数 | ~15.6 M | ~3.86 M |
| Tree 总页数 | ~15.8 M | ~3.87 M |
| Tree 磁盘占用 | ~63 GB | ~62 GB |

任何新增 runtime carrier（比如 `leaf_order` / `slot_map` / round state）在动笔前都要先按这两列算一次 per-manifest 内存占用，写进对应 step 文档或变更说明的"容量估算"节；量级评估不过关的结构不允许进代码。

---

## 代码模块

### [code_modules.md](code_modules.md) — 代码模块划分与职责

定义 `apps/inconel/` 的 13 个模块（L0-L3 四层）、职责边界、依赖关系。确定代码归属时查阅此文档。

---

## Layer 0 — 总纲与约束

### [design_overview.md](design_overview.md) — 唯一权威规范

整个 Inconel v1 的顶层设计，所有其他文档的语义源头。

| 章节 | 内容 | 实现时查阅场景 |
|------|------|---------------|
| §1 系统定位与边界 | 8 种 scheduler、实例数、路由策略 | 新建 scheduler 或确认职责分工 |
| §2 冻结结论 | 持久化白名单（superblock/WAL/tree/value 四类）、单盘约束 | 任何涉及"要不要落盘"的决策 |
| §6 序号与前沿 | batch_lsn, data_ver, durable_lsn, read_lsn, recovery_safe_lsn 语义 | 任何 LSN 相关逻辑 |
| §7 写路径提交语义 | value FUA → WAL FUA → memtable → durable_lsn 三阶段 | 写路径持久化顺序 |
| §8 读路径可见性 | read_handle + PRS 快照 + data_ver 比较 | 读路径正确性论证 |
| §9 Seal/Flush/Switch | 三阶段 flush、frontier switch、retire 链 | flush 流程设计 |
| §10 Tree 规范 | shadow CoW、slot_map、consolidation 触发 | tree 节点操作 |
| §11 WAL 段池 | segment 分配/回收/rotation | WAL 空间管理 |
| §12 Recovery | 启动恢复 = flush-like merge | recovery 实现 |
| §14 典型 Pipeline | 写/读/seal/flush/recovery 的 PUMP sender 链 | pipeline 编排参考 |

### [code_quality_standard.md](code_quality_standard.md) — 实现与 Review 质量标准

定义热路径预算、ownership / lifetime、抽象边界、PUMP sender 结构、设备访问边界的质量红线。修改 `apps/inconel/` 代码时，用本文审查“实现质量是否过关”。

### [cross_doc_contracts.md](cross_doc_contracts.md) — 跨文档一致性检查表

**不是实现参考，是验证工具。** 改完代码后对照此表检查是否违反约束。

| 内容 | 用途 |
|------|------|
| scheduler handle 签名表 | 确认接口参数不漂移 |
| 关键 struct 字段映射 | 确认字段在定义/引用处一致 |
| Pipeline 跳转路径（5 条） | 确认 scheduler 间数据流无遗漏 |
| **三条红线** | 实现前必读，防止走弯路 |

**三条红线速览：**
- 读路径：不访问 front_sched 当前 active/imms，不访问 tree_sched 当前 manifest，不串行化 tree_lookup 到 tree_sched
- Tree 运行时：cache miss 不扫描 shadow range 多个 slot
- Recovery：不扫描 Value Area，不重写 tree 到 slot 0，不需要 dead_value_refs 防泄漏

---

## Layer 1 — 磁盘格式（实现 I/O 前必读）

### [on_disk_formats.md](on_disk_formats.md) — 字节级持久化格式

| 章节 | 定义内容 | 关键常量 |
|------|---------|---------|
| §2 Superblock A/B | 双槽超级块、CRC 选择、元数据页 | magic=0x494E434F4E454C31, 占 2 LBA |
| §3 WAL Segment | header + entry + trailer、跨 LBA 拼接 | magic=0x57414C53 |
| §4 Tree Page | slot header + full slot directory、internal/leaf 节点、shadow range、空 slot 检测 | magic=0x54524545, 典型 16KiB |
| §5 Value Object | 布局、size class、sub-LBA 策略 | magic=0x56414C55, ≤16 个 size class |
| §6 参数关系 | 对齐约束、容量估算 | 16K leaf ~259 条 / 4K leaf 64 条 (32B key)，16K internal ~356 children |

**核心地址类型：**
```
paddr          {device_id, lba}                   — 10B, 磁盘地址
range_ref      {base: paddr, slot_count}          — 14B, shadow range
value_ref      {base: paddr, byte_offset, len, flags} — 18B, 值定位
```

**对齐规则：** superblock→lba_size, WAL segment→wal_segment_size, tree page→tree_page_size, value object→value_size_class

---

## Layer 2 — 数据路径（写/读 pipeline 实现）

### [write_path_and_pipeline.md](write_path_and_pipeline.md) — 写路径

| 章节 | 内容 | 实现时查阅场景 |
|------|------|---------------|
| §2 PUMP Sender 链 | 完整写 pipeline + batch_ctx 结构 | 编写写 pipeline |
| §3 Canonicalization | 去重折叠算法、entry_count 定义 | 实现 assign_lsn |
| §4 路由与分片 | key_hash % front_count | fragment 构造 |
| §5 Value Allocation | placement 策略、leader-follower 合并、DMA 填充 | 实现 persist_put_values |
| §6 两阶段处理 | WAL append → memtable insert（全 WAL 屏障 → 全 memtable 屏障） | 理解阶段依赖 |
| §7 WAL Segment Rotation | rotation 触发、pipeline 集成、背压 | WAL 空间不足处理 |
| §8 多 batch 并发 | WAL 交错、durable_lsn 推进、并发上限 | 并发写正确性 |
| §9 Batch 不跨 Seal | publish_gate 机制 | seal 与写的交互 |
| §10 异常处理 | 三阶段失败分类、orphan 恢复 | 错误路径设计 |
| §11 持久化顺序论证 | value-before-WAL 正确性 | 审查持久化 |

**写 pipeline 关键路径：**
```
coord: canonicalize → assign_lsn
 → value_alloc: leader-follower persist_put_values (FUA)
 → fan-out front[]: write_wal_fragment (FUA) → reduce
 → fan-out front[]: write_memtable_fragment → reduce
 → coord: publish_batch → ACK
```

**关键结构：** `batch_ctx {batch_lsn, entry_count, canonical_entries[], fragments[]}`

### [read_api_and_pipeline.md](read_api_and_pipeline.md) — 读路径

| 章节 | 内容 | 实现时查阅场景 |
|------|------|---------------|
| §2 read_handle 生命周期 | acquire/hold/release、PUMP context | 读 handle 管理 |
| §4 Point GET | 完整流程 + pipeline | 实现单点查询 |
| §5 MultiGet | 批量查询、按 owner 分组、miss 收集、合并 | 批量读优化 |
| §6 Range Scan | memtable scan + tree scan + merge | 范围查询 |
| §7 Tombstone 读语义 | 不穿透、遮蔽、range scan 影响 | tombstone 处理 |
| §8 长读资源限制 | 超时/代距/内存背压策略 | 防资源泄漏 |
| §9 Page/Frame Cache 模型 | tree-node cache、value read、memtable kv_arena 独立 | 缓存集成 |
| §11 可见性判定总结 | 单条 winner 规则、永不回退 | 正确性审查 |

**Point GET 关键路径：**
```
coord: acquire_read_handle → (cat, read_lsn)
 → front[owner]: lookup_memtable (PRS 快照的 active+imms)
 → miss → tree_read_domain[shard_idx].lookup: tree_lookup (manifest 快照)
       shard_idx = core::registry::current_shard_partitions()->route(key)
 → tree hit → value_alloc: read_value → copy-out
```

**可见性规则：** memtable 内 max(data_ver) ≤ read_lsn 胜出；memtable vs tree 取 max(data_ver)；tombstone = 不存在

---

## Layer 3 — 后台流程（Flush / Recovery / WAL 回收）

### [flush_and_frontier_switch.md](flush_and_frontier_switch.md) — Flush 与前沿切换

| 章节 | 内容 | 实现时查阅场景 |
|------|------|---------------|
| §2 Phase 1: 选 gen | 资格规则 (gen.max_lsn ≤ durable_lsn)、选择策略 | flush 触发判定 |
| §3 Phase 2: Tree-Local Flush | tree-local flush sub-pipeline：fold / leaf mapping / candidate build / plan+write | 核心 flush 逻辑 |
| §4 Phase 3: Frontier Switch | new guard 构造、PRS2、CAT2 安装、front 通知 | 前沿切换实现 |
| §5 Retire List Hooks | 旧 tree value、旧 slot/range、memtable loser、orphan、双 hook 防护 | 资源回收入口 |
| §6 Root-Change vs Root-Stable | 条件判定、superblock 更新时机 | root 变更处理 |
| §7 Old CAT/Guard Reclaim Chain | 引用计数归零链、value 回收判定、延迟扫描 | GC 正确性 |
| §9 Consolidation 细节 | leaf/internal/root split 实现 | 树结构变更 |

**Tree-Local Flush 4 段 owner seam：**
1. `tree_sched.fold`: 对所有 sealed gens 做 per-key winner 裁决
2. `tree_read_domain[shard_idx].lookup.keys_to_leaf_groups`: 用 `manifest.leaf_order` 把 sorted keys 映射到 affected leaves（同一 `shard_partition_map` 作为 read / flush 共享决策源）
3. `tree_read_domain[shard_idx].worker.build_leaf_candidates`: 读 old leaf、merge、compact 出 candidate image（与 lookup 共享 read_domain 的 `node_cache`）
4. `tree_sched.plan_tree_delta_from_candidates`: 写 tree slots + NVMe FLUSH

**回收链：** CAT2 安装 → old CAT1 refs→0 → PRS1 refs→0 → fronts refs→0 → gen refs-- → loser_refs 可回收 → G0 refs→0 → retired dispatch (TRIM + value reclaim)

### [recovery_and_wal_reclaim.md](recovery_and_wal_reclaim.md) — Recovery 与 WAL 回收

| 章节 | 内容 | 实现时查阅场景 |
|------|------|---------------|
| §2 读 Superblock | CRC 选择、参数提取 | recovery 入口 |
| §3 Tree 遍历 + WAL 扫描 | shadow slot 选择、leaf record pool、WAL entry pool、不完整 batch | 数据收集 |
| §4-5 组装 + 重建 | 完整 batch 定义、per-key winner、live_value_refs | 数据合并 |
| §6-7 Flush WAL → Tree | 增量 CoW、zero-write 优化、superblock 更新、TRIM | recovery flush |
| §8-9 Allocator 重建 | tree allocator head/free、value allocator bump/class-pools | 状态重建 |
| §10-15 安装 Clean Runtime | CAT_clean、next_lsn、front 拓扑 | runtime 初始化 |
| §8 Tombstone 物理删除 | boot: data_ver ≤ recovery_safe_lsn 的可删 | GC 策略 |
| §9 recovery_safe_lsn | 定义、推进时机 | 安全屏障 |

**Recovery = Flush-like Merge：** read superblock → traverse tree → scan WAL → merge → CoW write → update superblock → rebuild allocators → install runtime

**recovery_safe_lsn 推进时机：** flush 完成 frontier_switch 安装时 / WAL segment 变为可回收时

---

## Layer 4 — 运行时基础设施

### [runtime_memory_and_cache.md](runtime_memory_and_cache.md) — 内存与帧管理

| 章节 | 内容 | 实现时查阅场景 |
|------|------|---------------|
| §2 对象分类（9 类） | 正确性 owner / cache / 放置元数据 | 确认对象生命周期 |
| §3 正确性 Owner 图 | pin 链: read_handle → cat → prs → {fronts[] → shared_ptr<memtable_gen> → kv_arena, guard → manifest} | 理解引用关系 |
| §5 Frame 抽象 | frame_id, frame_state, page_frame, value_page_frame | 实现 I/O buffer 管理 |
| §6-7 SPDK DMA Pool | 按 size+NUMA 分池、per-core cache、跨 shard 流程 | DMA 内存分配 |
| §9 Value 放置 + 状态耦合 | dirty_append/dirty_hole_fill/clean_readonly | value 写入策略 |
| §10 读路径 Zero-Copy | tree node pin → value page cache → CRC → copy-out | 读缓存集成 |
| §11 Frame 生命周期 | 回收条件、stale hit 防护、hole page recovery | 帧状态机 |

**帧状态转移：**
```
新页: none → dirty_append → writeback_inflight → clean_readonly
hole 复用: clean_allocatable → dirty_hole_fill → writeback_inflight → clean_readonly
逐出: clean_readonly (pin_count=0) → evicted
```

**DMA 池层次：** per-NUMA {4K, 8K, 16K, multi-LBA} → spdk_mempool (常驻帧) + spdk_iobuf (临时 buf)

### [runtime_state_machine.md](runtime_state_machine.md) — Tree Domain + 其余 Scheduler 状态机

| 章节 | Scheduler | 实现时查阅场景 |
|------|-----------|---------------|
| §2 coord_sched | 协调器：LSN 分配、publish_gate、durable_lsn 推进、seal 触发 | 写/读/seal 协调 |
| §3 front_sched | 前端：memtable、WAL stream、seal/lookup/collect | 前端操作实现 |
| §4 tree domain | `tree_sched` 单点 + K 个 `tree_read_domain<Cache>`（own lookup/worker + `node_cache` + `partitions` snapshot）；allocator、manifest、checkpoint_guard、shard_partition rebuild | tree 操作实现 |
| §5 wal_space_sched | WAL 空间：segment 分配/回收 | WAL 管理 |
| §6 value_alloc_sched | 值分配/读取：bump head、class pools、dirty pages、frame cache | value I/O 实现 |
| §7 nvme_sched | NVMe I/O：FUA write、FLUSH、TRIM | 底层 I/O |
| §8 对象生命周期 | catalog/read_set/value_handle/gen 引用计数 | GC 正确性 |
| §9 跨 Scheduler 时序 | 写/seal/flush 完整时间线 | 并发交互审查 |
| §10 并发安全论证 | same-key→same-front、coord 单线程、tree 域单线程 | 正确性论证 |

**Scheduler 一览：**

| Scheduler | 实例数 | 路由 | 核心职责 |
|-----------|--------|------|---------|
| coord_sched | 1 | 固定 | LSN、durable_lsn、seal、frontier switch |
| front_sched | N (分片) | key_hash % N | memtable、WAL stream |
| tree_sched | 1 | 固定 | tree allocator、tree-local flush、reclaim、shard_partition_map rebuild |
| tree_read_domain | M | `current_shard_partitions()->route(key)`（shard-partition binary search，INC-040 / step 030 §2.7） | own lookup + worker；持 `node_cache` shard、路由 snapshot |
| ↳ lookup | 与 read_domain 1:1 | 继承 | tree 查询、leaf mapping |
| ↳ worker | 与 read_domain 1:1 | 继承 | old leaf read、candidate build |
| wal_space_sched | 1 | 固定 | WAL segment 池 |
| value_alloc_sched | 1 | 固定 | value 分配/读取/缓存 |
| nvme_sched | K | 轮询/指定 | NVMe I/O |

---

## 快速定位表

| 我要实现... | 代码模块 | 先看 | 再看 | 验证 |
|------------|---------|------|------|------|
| 写 pipeline | pipeline/ | write_path §2,§6 | runtime_state §2(coord),§3(front) | cross_doc §5 写路径 |
| 读 pipeline | pipeline/ | read_api §4(GET),§5(Multi) | runtime_state §2(handle),§3(lookup) | cross_doc §4 数据源 |
| Value I/O | value/ | write_path §5 | runtime_state §6, runtime_memory §9 | on_disk §5 格式 |
| WAL 编码/解码 | front/ | on_disk §3 | write_path §6, runtime_state §3.9 | cross_doc §2 字段 |
| Tree 节点操作 | tree/ | on_disk §4 | flush §3,§9 | design_overview §10 |
| Seal 流程 | pipeline/ + coord/ | design_overview §9.1 | runtime_state §2.5,§3.6 | cross_doc §5 seal 路径 |
| Flush 流程 | pipeline/ + tree/ | flush 全文 | runtime_state §4 | cross_doc §5 flush 路径 |
| Recovery | recovery/ | recovery 全文 | on_disk §2-4 | cross_doc §5 recovery 路径 |
| 内存/DMA 管理 | memory/ | runtime_memory §5-7 | — | runtime_memory §14 不变量 |
| Scheduler handle | 对应 L2 模块 | runtime_state 对应章节 | cross_doc §1 签名 | cross_doc §3 Owner |
| 资源回收/GC | tree/ + value/ | flush §5,§7 | runtime_state §8 | recovery §8-9 tombstone |
| Superblock 读写 | format/ + recovery/ | on_disk §2 | recovery §2 | design_overview §12.3 |
| 域对象/共享类型 | core/ | design_overview §5 | cross_doc §2 字段 | — |
| NVMe 操作 | nvme/ | runtime_state §7 | code_modules | — |
| 模块归属判定 | — | code_modules 全文 | — | — |
