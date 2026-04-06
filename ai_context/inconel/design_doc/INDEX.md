# Inconel 设计文档索引

> 分层组织，实现时按需查阅。每层自顶向下：先看概览定位，再进具体文档找细节。

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
| §1 系统定位与边界 | 7 种 scheduler、实例数、路由策略 | 新建 scheduler 或确认职责分工 |
| §2 冻结结论 | 持久化白名单（superblock/WAL/tree/value 四类）、单盘约束 | 任何涉及"要不要落盘"的决策 |
| §6 序号与前沿 | batch_lsn, data_ver, durable_lsn, read_lsn, recovery_safe_lsn 语义 | 任何 LSN 相关逻辑 |
| §7 写路径提交语义 | value FUA → WAL FUA → memtable → durable_lsn 三阶段 | 写路径持久化顺序 |
| §8 读路径可见性 | read_handle + PRS 快照 + data_ver 比较 | 读路径正确性论证 |
| §9 Seal/Flush/Switch | 三阶段 flush、frontier switch、retire 链 | flush 流程设计 |
| §10 Tree 规范 | shadow CoW、slot_map、consolidation 触发 | tree 节点操作 |
| §11 WAL 段池 | segment 分配/回收/rotation | WAL 空间管理 |
| §12 Recovery | 启动恢复 = flush-like merge | recovery 实现 |
| §14 典型 Pipeline | 写/读/seal/flush/recovery 的 PUMP sender 链 | pipeline 编排参考 |

### [cross_doc_contracts.md](cross_doc_contracts.md) — 跨文档一致性检查表

**不是实现参考，是验证工具。** 改完代码后对照此表检查是否违反约束。

| 内容 | 用途 |
|------|------|
| 24 个 scheduler handle 签名 | 确认接口参数不漂移 |
| 16 个关键 struct 字段映射 | 确认字段在定义/引用处一致 |
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
| §4 Tree Page | slot header、internal/leaf 节点、shadow range、空 slot 检测 | magic=0x54524545, 典型 16KiB |
| §5 Value Object | 布局、size class、sub-LBA 策略 | magic=0x56414C55, ≤16 个 size class |
| §6 参数关系 | 对齐约束、容量估算 | leaf ~268 条 (32B key)，internal ~371 children |

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
| §9 Page/Frame Cache 模型 | tree-node cache、value read、hot_blob | 缓存集成 |
| §11 可见性判定总结 | 单条 winner 规则、永不回退 | 正确性审查 |

**Point GET 关键路径：**
```
coord: acquire_read_handle → (cat, read_lsn)
 → front[owner]: lookup_memtable (PRS 快照的 active+imms)
 → miss → tree_lookup_sched: tree_lookup (manifest 快照)
 → tree hit → value_alloc: read_value → copy-out
```

**可见性规则：** memtable 内 max(data_ver) ≤ read_lsn 胜出；memtable vs tree 取 max(data_ver)；tombstone = 不存在

---

## Layer 3 — 后台流程（Flush / Recovery / WAL 回收）

### [flush_and_frontier_switch.md](flush_and_frontier_switch.md) — Flush 与前沿切换

| 章节 | 内容 | 实现时查阅场景 |
|------|------|---------------|
| §2 Phase 1: 选 gen | 资格规则 (gen.max_lsn ≤ durable_lsn)、选择策略 | flush 触发判定 |
| §3 Phase 2: Fold + Write Tree | fold 算法（4步）、loser 处理、shadow slot 选择、consolidation | 核心 flush 逻辑 |
| §4 Phase 3: Frontier Switch | new guard 构造、PRS2、CAT2 安装、front 通知 | 前沿切换实现 |
| §5 Retire List Hooks | 旧 tree value、旧 slot/range、memtable loser、orphan、双 hook 防护 | 资源回收入口 |
| §6 Root-Change vs Root-Stable | 条件判定、superblock 更新时机 | root 变更处理 |
| §7 Old CAT/Guard Reclaim Chain | 引用计数归零链、value 回收判定、延迟扫描 | GC 正确性 |
| §9 Consolidation 细节 | leaf/internal/root split 实现 | 树结构变更 |

**Fold 算法 4 步：**
1. 收集所有 memtable key，排序
2. 通过 manifest + internal 定位受影响 leaf
3. 并发 NVMe 读所有受影响 leaf
4. 内存 merge（per-key winner: tree vs WAL）

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
| §3 正确性 Owner 图 | pin 链: read_handle → cat → prs → {fronts[], guard} → manifest, hot_blob | 理解引用关系 |
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

### [runtime_state_machine.md](runtime_state_machine.md) — 7 个 Scheduler 状态机

| 章节 | Scheduler | 实现时查阅场景 |
|------|-----------|---------------|
| §2 coord_sched | 协调器：LSN 分配、publish_gate、durable_lsn 推进、seal 触发 | 写/读/seal 协调 |
| §3 front_sched | 前端：memtable、WAL stream、seal/lookup/collect | 前端操作实现 |
| §4 tree_sched | 树管理：allocator、manifest、checkpoint_guard、fold | tree 操作实现 |
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
| tree_sched | 1 | 固定 | tree allocator、manifest、fold |
| tree_lookup_sched | M | key_hash % M | 无状态 tree 查询 |
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
| NVMe 操作 | nvme/ or mock_nvme/ | runtime_state §7 | code_modules | — |
| 模块归属判定 | — | code_modules 全文 | — | — |
