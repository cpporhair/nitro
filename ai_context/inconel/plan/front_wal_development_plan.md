# Inconel Front/WAL/Coord 分支迁移与重构计划

> 本文是把旧 `inconel` 分支前端实现迁移到当前 `inconel.new` 架构的总计划。
> 它不是单个 step 的设计文档，也不占用 step 编号。
>
> `039_front_wal_phase_a_carrier_inc055_design.md` 是本文第一个迁移步
> `M01` 的详细设计文档。另一个 agent 应在 039 落定后，实现 M01 代码。
>
> 目标：按旧 `inconel` 分支已经验证过的 FRONT/WAL/coord/write/read 演进步骤，
> 一步一步迁移并重构到当前分支。本文范围实现完成后，应能启动 live
> read/write 端到端测试：`write_batch(PUT/DEL)` 完成 value durable、WAL
> durable、memtable visible、publish，然后 `point_get` 通过 coord snapshot +
> front memtable + value read 返回结果。

## 1. 为什么按分支迁移

当前分支 `inconel.new` 已经重写了 TREE/VALUE 和真实 NVMe frame 路径，但
`coord/`、`front/`、`wal/`、`pipeline/`、`recovery/` 仍基本为空。旧
`inconel` 分支的问题主要出在 TREE/VALUE 后续路径；前端的 batch carrier、
coord publish、front memtable、WAL stream、write_batch pipeline、runtime
topology 和 read-after-write 测试梯度仍然是正确参考。

因此这里不再写一份从零发明的模块计划，而是采用迁移/重构策略：

1. 逐步对照旧 `inconel` 的 step 设计、测试和实现。
2. 每个迁移步先写当前分支详细设计文档，再实现。
3. 保留旧 step 冻结的语义和测试意图。
4. 重构掉旧分支不适合当前架构的形态。
5. 每步完成后都能累积到最终 live read/write e2e。

## 2. 迁移总规则

1. **代码质量是唯一最高原则**  
   迁移过程以代码质量、语义完整性、可验证性和长期可维护性为最高优先级。
   “实现简单”、“先做最小版本”、“后续再实现”、“先让测试过”不能作为降低设计或
   实现质量的理由。若某个更小的切分、临时约束或延后项确实能提升整体质量，必须
   在详细设计中说明原因、风险和替代方案，并申请人工判断。

2. **每步迁移必须是完整语义切片**  
   step 可以小，但不能是假子集。每个迁移步都必须交付该步声明的完整生产语义、
   错误语义、owner/lifetime 边界和测试证据；不得留下以通用名字伪装的
   unsupported stub，也不得把已知必需语义藏到“后续补”。

3. **参考旧分支语义，不照搬旧代码目录**  
   旧分支在 `apps/inconel/runtime/*` 下组织 front/coord/wal；当前分支应落到
   `apps/inconel/{coord,front,wal,pipeline,...}` 和当前 runtime builder/registry。

4. **不迁移旧 TREE/VALUE 实现**  
   旧分支 TREE/VALUE 代码只作为接口历史背景。当前分支使用已有
   `tree::tree_sched`、`tree_read_domain`、`value::value_alloc_sched`、
   segmented frame 和真实 NVMe adapter。

5. **当前分支现状不是自动权威**  
   当前 `inconel.new` 里如果已经有 FRONT/WAL/coord/pipeline 相关语义，它只能作为
   候选实现证据。由于这些模块还不是当前分支的核心落地点，某些代码可能只是为了
   临时测试通过而加入，不能默认视为正确设计。

6. **迁移设计必须做三方对照**  
   每个迁移 step 的详细设计必须对照：
   - 旧 `inconel` 分支已验证的实现和测试
   - 当前 `inconel.new` 分支已有代码
   - `ai_context/inconel/design_doc/*` 中的正式设计

   若三者冲突，详细设计文档必须显式记录冲突、取舍理由和最终决议。不能因为当前
   分支已有某个 header/struct/helper 就默认沿用。

7. **最终决策依据是详细设计文档**  
   迁移实现以本迁移 step 的详细设计文档为准。若详细设计无法从已有设计文档中推导
   出唯一正确答案，或者会改变持久化格式、recovery 输入、owner 边界、read
   visibility 等核心语义，必须停下来申请人工裁决。

8. **WAL format 需要专项裁决，不预设当前文件正确**  
   旧分支的 WAL helper 名称、decode API、segment buffer 形态不能直接照搬；当前
   分支的 `format/wal.hh` 也不能无条件视为最终格式。迁移 WAL 相关 step 必须把
   `format/wal.hh`、旧分支 `format/wal.hh`、正式 WAL 设计文档逐项对照后，在详细
   设计中裁决最终 byte layout 和 helper surface。

9. **INC-055 是强制重构点**  
   旧分支 `memtable_entry` 使用 `hot_blob`。当前迁移必须改成 memtable 只保存
   durable `value_ref`；memtable hit 之后由 `value_alloc_sched.read_value()` 读
   value body。

10. **owner/sender 边界必须按当前 PUMP 规则重写**  
   旧分支 first-class owner 方向是对的，但当前实现应使用当前分支 scheduler
   模式、registry、builder 和 nvme/value/tree sender surface。不得退化成
   `on(task) + 调裸 state method`。

11. **bounded concurrency**  
   旧分支和当前 value path 都有 unbounded `concurrent()` 的历史问题。迁移时
   value/WAL/NVMe fan-out 必须带显式 budget。

12. **每步有回归证据**  
   旧分支每个 step 都有 design/test/review。迁移时可以合并小步，但不能丢掉对应
   不变量；测试名可以更新，测试意图必须保留。

## 3. 迁移步概览

| 迁移步 | 对应旧 `inconel` step | 当前分支目标 | 完成后能力 |
|---|---|---|---|
| M01 / 039 | Step 7, Step 10, Step 26I, Step 26J/K | shared carrier + INC-055 memtable shape | batch/fragment/memtable value_ref 形态冻结 |
| M02 | Step 8, Step 11 | read_handle / PRS / memtable lookup carrier | snapshot-based memtable lookup 可测 |
| M03 | Step 9, Step 16 | coord_sched assign/publish/release/read_handle | gap-free durable_lsn 和 read_handle |
| M04 | Step 18 | wal_space_sched + WAL stream state | segment alloc/rotation/reclaim shape |
| M05 | Step 17 | front_sched memtable owner | insert/lookup/scan/seal/collect/release |
| M06 | Step 18, Step 26P | WAL append prepare + bounded FUA issue | WAL bytes 可 decode，失败不进 memtable |
| M07 | Step 19, Step 26M/N | value persist/read adapter | write_batch 可用当前 value module |
| M08 | Step 22, Step 23 | write_batch baseline + inflight gap handling | 单/多 batch 写路径语义闭环 |
| M09 | Step 24A, Step 24B | production write_batch sender pipeline | all-WAL/all-memtable barrier + release failure |
| M10 | Step 25 | point_get live memtable read | write 后可读回 value body |
| M11 | Step 25A, Step 26B-E | runtime topology + operation surface | 正式 runtime API/registry/builder |
| M12 | Step 26 | seal/CAT1/front generation boundary | batch 不跨代，flush 可接真实 gens |
| M13 | Step 26L/O/Q | mock/real e2e test harness | live read/write e2e 和多核测试 |

旧 Step 27 之后是 flush/tree/recovery 主线，不在本文迁移范围内。本文只保留
front-facing bridge：M12 产出的 sealed gens 必须能喂给当前分支已有 tree-local
flush。

## 4. 详细迁移计划

### M01 / Step 039：Carrier + INC-055 Memtable Shape

对应旧 step：

1. Step 7：`memtable_gen` / `memtable_entry` / `hot_blob`
2. Step 10：canonicalization + routing + `batch_ctx`
3. Step 26I：memtable arena foundation
4. Step 26J/K：network-style batch ingress / view-based batch plan

参考文件：

1. `inconel:apps/inconel/runtime/batch_ctx.hh`
2. `inconel:apps/inconel/runtime/front/memtable.hh`
3. `inconel:apps/inconel/test/step_07_memtable_contract_test.cc`
4. `inconel:apps/inconel/test/step_26i_memtable_arena_contract_test.cc`

当前分支落点：

1. `apps/inconel/core/`：batch input、canonical entry、front fragment、
   batch pipeline state、memtable lookup result。
2. `apps/inconel/core/memtable.hh`：移除新路径对 `value_handle.hot` 的依赖。
3. 后续 front/wal/coord sender 都引用这些 carrier。

必须重构：

1. 旧 `hot_blob` 语义不得迁移为 production contract。
2. memtable arena 只保存 key bytes，不保存 value body。
3. fragment 可以引用 batch-owned entries，但必须证明 batch context 活到 WAL 和
   memtable phase 完成；优先使用 stable index 或 context-owned vector。

完成测试：

1. same-key last-writer-wins canonicalization。
2. PUT/DEL route by `key_hash % front_count`。
3. fragment 不悬挂 caller stack。
4. memtable entry value hit carrier 只含 `value_ref`。
5. duplicate key insert 不重复 materialize key storage。

M01 的详细设计文档就是 `039_front_wal_phase_a_carrier_inc055_design.md`。
039 不设计 WAL append、coord ready bitmap 或 write_batch pipeline。

039 还必须包含一张“语义来源对照表”：对每个新增 carrier / memtable 字段 /
lookup result，列出旧分支实现、当前分支现状、正式设计文档依据和最终决议。若
当前分支现状只是兼容旧测试的临时形态，必须明确标出，不能让后续实现误当成正式
语义。

### M02：Read Handle / PRS / Memtable Lookup Carrier

对应旧 step：

1. Step 8：跨 gen `lookup_memtable` / `scan_memtable`
2. Step 11：`publish_catalog` / `published_read_set` / `read_handle`

参考文件：

1. `inconel:apps/inconel/runtime/front/read_set.hh`
2. `inconel:apps/inconel/runtime/front/state.hh`
3. `inconel:apps/inconel/runtime/coord/catalog.hh`
4. `inconel:apps/inconel/test/step_08_front_read_set_contract_test.cc`

当前分支落点：

1. `apps/inconel/core/`：PRS/CAT/read_handle carrier，如当前已有类型不足则补齐。
2. `apps/inconel/front/` 后续 lookup handle 复用这些 carrier。
3. `apps/inconel/coord/` 后续 `acquire_read_handle` 发布这些 carrier。

必须重构：

1. lookup 返回 `value_ref`，不是 `hot_blob`。
2. lookup 搜索传入的 PRS snapshot，不能读 front 当前 active/imms。
3. old read_handle pin 链必须与当前 `std::shared_ptr<memtable_gen>` 语义一致。

完成测试：

1. active/imms winner 选择，`data_ver <= read_lsn` 最大版本胜出。
2. tombstone 命中返回 not_found marker。
3. 旧 read_handle 捕获后，新 CAT/front 切换不影响旧 lookup。

### M03：Coord Scheduler

对应旧 step：

1. Step 9：`ready_bitmap` + `publish_gate`
2. Step 16：`coord_sched`

参考文件：

1. `inconel:apps/inconel/runtime/coord/gate.hh`
2. `inconel:apps/inconel/runtime/coord/state.hh`
3. `inconel:apps/inconel/runtime/coord/owner_impl.hh`

当前分支落点：

1. `apps/inconel/coord/scheduler.hh`
2. `apps/inconel/coord/sender.hh`
3. `apps/inconel/core/registry.hh`

必须重构：

1. owner 请求队列使用当前 PUMP owner/sender 模式。
2. `assign_batch_lsn` 消费 M01 batch carrier。
3. `publish_batch` / `release_batch` 只推进 ready slot，不产生可见数据。
4. `acquire_read_handle` 使用 M02 CAT/PRS carrier。

完成测试：

1. assign LSN 递增唯一。
2. publish 乱序时 durable_lsn 不跳洞。
3. release 失败 batch 后可解锁后续成功 batch。
4. gate closed 期间 pending advance 正确累积，open 后应用。

### M04：WAL Space Scheduler

对应旧 step：

1. Step 18：WAL Stream + `wal_space_sched`

参考文件：

1. `inconel:apps/inconel/runtime/wal/space.hh`
2. `inconel:apps/inconel/test/step_18_wal_space_sched_contract_test.cc`

当前分支落点：

1. `apps/inconel/wal/scheduler.hh`
2. `apps/inconel/wal/sender.hh`

必须重构：

1. `wal_space_sched` 只管理 segment metadata，不做 NVMe I/O。
2. segment header/trailer bytes 由 front WAL stream owner 写。
3. alloc 耗尽必须 backpressure 或 fail-fast，不返回假成功。
4. segment generation 必须进入 token，防止旧 segment 误用。

完成测试：

1. alloc head / free pool / generation bump。
2. sealed segment reclaim 后复用。
3. 多 front stream allocation 串行化。
4. alloc empty 行为明确。

### M05：Front Scheduler Memtable Surface

对应旧 step：

1. Step 17：`front_sched` 的 memtable 面

参考文件：

1. `inconel:apps/inconel/runtime/front/state.hh`
2. `inconel:apps/inconel/runtime/front/owner_impl.hh`
3. `inconel:apps/inconel/test/step_17_*`

当前分支落点：

1. `apps/inconel/front/scheduler.hh`
2. `apps/inconel/front/sender.hh`

必须重构：

1. `insert_memtable_fragment` 消费 M01 fragment carrier。
2. PUT entry 只插入 durable `value_ref`。
3. `lookup_memtable` / `batch_lookup` / `scan_memtable` 使用 M02 PRS。
4. `seal_active` / `collect_eligible_gens` / `release_gens` 保留旧分支语义。

完成测试：

1. insert 后 lookup/scan 正确。
2. seal 后 active/imms 切换。
3. PRS snapshot 搜索不是当前 active 搜索。
4. collect 只返回 `gen.max_lsn <= durable_lsn` 的 sealed gens。

### M06：WAL Append Prepare + Bounded FUA Issue

M06 的详细设计文档是 `044_wal_append_prepare_bounded_fua_design.md`。

对应旧 step：

1. Step 18：WAL append / segment rotation
2. Step 26P：NVMe I/O 回到 pipeline 层

参考文件：

1. `inconel:apps/inconel/runtime/wal/space.hh`
2. `inconel:apps/inconel/runtime/front/owner_impl.hh`
3. `inconel:apps/inconel/test/step_04_wal_contract_test.cc`
4. `inconel:apps/inconel/test/write_batch_device_verification_test.cc`

当前分支落点：

1. `apps/inconel/front/scheduler.hh`：WAL stream cursor 与 prepare。
2. `apps/inconel/front/sender.hh`：`write_wal_fragment` sender。
3. `apps/inconel/nvme/` current frame I/O surface。

必须重构：

1. WAL byte layout 和 helper surface 先由本步详细设计裁决；不得默认当前
   `format/wal.hh` 或旧分支 `format/wal.hh` 其中之一正确。
2. 支持 entry 跨 LBA/page。
3. segment full 时写 trailer、FUA、向 M04 申请新 segment、写新 header。
4. WAL writes 使用 bounded concurrency。
5. WAL failure 只使该 batch 走 release，不进入 memtable。

完成测试：

1. PUT/DELETE WAL bytes decode 正确。
2. global `entry_count` 写入每条 WAL entry。
3. cross-page entry 正确。
4. segment rotation 正确。
5. FUA failure 后 memtable 未写。

### M07：Value Persist / Read Adapter

M07 的详细设计文档是 `046_value_persist_read_adapter_design.md`。

对应旧 step：

1. Step 19：`value_alloc_sched`
2. Step 26M/N：leader-follower merge / async persist reset

参考文件：

1. `inconel:apps/inconel/runtime/value_alloc/*`
2. `inconel:apps/inconel/runtime/operations/write_batch.hh`

当前分支落点：

1. 当前 `apps/inconel/value/sender.hh`
2. 当前 `apps/inconel/value/*`
3. write/read pipeline 对 value module 的适配层

必须重构：

1. 不迁移旧 value allocator 实现。
2. 使用当前 value module 的 `persist_put_values` / `read_value`。
3. 修正或规避当前 `INC-048` unbounded concurrent 风险。
4. 保证 value persist completion 先于 WAL append。

完成测试：

1. PUT value durable 后返回 `value_ref`。
2. `read_value(value_ref)` 返回原始 value body。
3. DELETE-only batch 不触发 value writes。
4. 多 PUT batch bounded I/O。

### M08：Write Baseline + Inflight Semantics

M08 的详细设计文档是 `047_write_baseline_inflight_design.md`。

对应旧 step：

1. Step 22：单 batch baseline 写路径
2. Step 23：多 batch 并发与 durable_lsn 推进

参考文件：

1. `inconel:apps/inconel/test/legacy_runtime/write_path_baseline.hh`
2. `inconel:apps/inconel/test/legacy_runtime/write_path_inflight.hh`
3. `inconel:apps/inconel/test/step_22_*`
4. `inconel:apps/inconel/test/step_23_*`

当前分支落点：

1. `apps/inconel/write_path/write_batch_state.hh`（047 裁决：按
   `code_modules.md` 关键约束，写请求组合层在 `write_path/` 而非
   `pipeline/`；phase senders 落 `write_path/sender.hh`）
2. whitebox pipeline tests（`inconel_test_m08_write_baseline_inflight`）

必须重构：

1. 不重建 legacy runtime path。
2. baseline 只能作为语义测试梯度，不作为最终 public surface。
3. inflight state 不得长期复制第二份完整 batch owner。

完成测试：

1. 单 batch PUT/DEL 完成 value/WAL/memtable/publish。
2. park/inflight 后不提前 publish。
3. 乱序 batch resolve 下 durable_lsn gap-free。

### M09：Production Write Batch Sender Pipeline

M09 的详细设计文档是 `048_production_write_batch_pipeline_design.md`。

对应旧 step：

1. Step 24A：写路径 scheduler / pipeline success-path 化
2. Step 24B：写路径异常语义接入 pipeline

参考文件：

1. `inconel:apps/inconel/runtime/operations/write_batch.hh`
2. `inconel:apps/inconel/runtime/operations/write_batch_state.hh`
3. `inconel:apps/inconel/test/step_24a_*`
4. `inconel:apps/inconel/test/step_24b_*`

当前分支落点：

1. `apps/inconel/write_path/write_batch.hh`（048 裁决：与 M08 同因，
   写请求组合层在 `write_path/`）
2. public operation surface 留 M11（048 §15.2：write_batch 保持底层
   显式拓扑签名，facade 包装届时另做）

必须重构：

1. 顶层 sender 清楚显示 owner 边界：
   `coord -> value -> front WAL fan-out -> front memtable fan-out -> coord`。
2. all-WAL barrier 和 all-memtable barrier 必须分开。
3. value/WAL phase 失败必须 `release_batch`。
4. memtable phase 失败必须 fatal。
5. submit 前不得产生 owner side effect。

完成测试：

1. success path sender 语义等价 M08。
2. value failure release 且 invisible。
3. WAL failure release 且 memtable invisible。
4. earlier failed batch release unblocks later success。

### M10：Point GET Live Read

M10 的详细设计文档是 `049_point_get_live_read_design.md`。

对应旧 step：

1. Step 25：Point GET 的 memtable-only 路径
2. Step 33 的目标读形态中与 memtable hit/value read 相关部分

参考文件：

1. `inconel:apps/inconel/runtime/operations/point_get_memtable_only.hh`
2. `inconel:apps/inconel/test/step_25_*`
3. `inconel:apps/inconel/test/write_batch_multicore_test.cc`

当前分支落点：

1. `apps/inconel/pipeline/point_get.hh`
2. `apps/inconel/front/sender.hh`
3. value read sender adapter

必须重构：

1. 旧分支 memtable hit 返回 `hot_blob`；当前必须返回 `value_ref` 后读 value。
2. point_get 第一跳必须是 coord `acquire_read_handle`。
3. front lookup 必须使用 read_handle PRS。
4. 本文的 live read/write e2e 只验收 recently-written memtable-hit 读回，不声明
   tree-miss 完整读语义。若 M10 需要对 memtable miss 给出对外完成语义，必须在
   详细设计中裁决是接入当前 tree path、返回显式 miss，还是扩大本步范围；不得用
   “后续接入”掩盖用户可见语义缺口。
   （049 §4.1 裁决：接入当前 tree path，point_get 落 OV §8.1 完整语义，
   对外只有 found / not_found；tree-hit 分支由 node-cache 预热测试在
   m10 target 内驱动，未留未测 production 分支。）

完成测试：

1. write PUT 后 point_get 返回 value body。
2. overwrite 后 point_get 返回最新版本。
3. DELETE 后 point_get 返回 not_found。
4. missing key 不伪装成 found。
5. cross-front point_get。

### M11：Runtime Topology + Operation Surface

M11 的详细设计文档是 `050_runtime_topology_operation_surface_design.md`。

对应旧 step：

1. Step 25A：runtime owner registry / topology
2. Step 26B：runtime-level operation 入口与 context carrier 收口
3. Step 26C/D/E：legacy quarantine / root alias retirement / operation split

参考文件：

1. `inconel:apps/inconel/runtime/inconel_runtime.hh`
2. `inconel:apps/inconel/runtime/runtime_operations.hh`
3. `inconel:apps/inconel/runtime/operations/*`
4. `inconel:ai_context/inconel/design_doc/runtime_owner_topology.md`
5. `inconel:ai_context/inconel/design_doc/runtime_operation_entry.md`

当前分支落点：

1. `apps/inconel/core/registry.hh`
2. `apps/inconel/runtime/builder.hh`
3. current operation headers

必须重构：

1. 不迁移旧 global runtime object 的具体目录形状，除非它匹配当前 builder。
2. front owner 选择必须 semantic-first：same key -> same front。
3. front -> tree_read_domain 映射使用当前 `shard_partition_map` / route 规则。
4. 不重新引入 root alias header 或 giant public header。

完成测试：

1. coord/front/wal/value/tree scheduler 全部注册。
2. write_batch/point_get 可从正式 operation surface 调用。
3. front list/by_core 稳定。
4. runtime header 自包含。

### M12：Seal / CAT1 / Front Generation Boundary

M12 的详细设计文档是 `051_seal_round_design.md`。

对应旧 step：

1. Step 26：Seal / CAT1 / batch 不跨代

参考文件：

1. `inconel:apps/inconel/runtime/operations/seal_once.hh`
2. `inconel:apps/inconel/test/step_26_*`

当前分支落点：

1. `apps/inconel/pipeline/seal_round.hh` 或等价 operation
2. `coord` gate handles
3. `front` seal/collect/release handles

必须重构：

1. seal 由 coord 发起，front 不能自行决定。
2. close_gate 只阻止 publish 越过 seal 边界，不阻止已投递 front work 完成。
3. 同一 batch 不能一部分进旧 active，一部分进新 active。
4. M12 只建立 front sealed gen bridge，不提前实现完整 recovery。

完成测试：

1. batch 整体进旧 gen 或整体进新 gen。
2. seal 期间 publish pending，open 后推进。
3. collect eligible gens 可喂当前 tree-local flush。
4. release_gens 不破坏 old read_handle pin 的 gen。

### M13：E2E Test Harness / Device Verification / Multicore

对应旧 step：

1. Step 26L：mock NVMe owner / scheduler boundary reset
2. Step 26O：mock NVMe device 分层 + write_batch 设备验证
3. Step 26Q：多核测试基础设施 + 热路径优化

参考文件：

1. `inconel:apps/inconel/test/write_batch_device_verification_test.cc`
2. `inconel:apps/inconel/test/write_batch_multicore_test.cc`
3. `inconel:apps/inconel/runtime/mock_nvme/*`

当前分支落点：

1. current test target under `apps/inconel/test/`
2. current `nvme::runtime_scheduler` mock/real test backend
3. existing real NVMe smoke flow from `038_pump_nvme_lba_page_adapter.md`

必须重构：

1. 不把旧 `memory_block_device` 变成 production boundary。
2. 测试可以有同步 inspection helper，但 production path 必须走 owner/sender/NVMe。
3. 多核测试推进 owner `advance()` / runtime scheduler，而不是直调 state。

完成测试矩阵：

1. single PUT value on device。
2. multi-key fan-out to both fronts。
3. same-key canonicalization。
4. PUT then DELETE。
5. DELETE-only no value write。
6. WAL entry decode and value_ref match。
7. sequential LSN advance。
8. memtable visible after write。
9. point_get after write。
10. overwrite read latest。
11. delete read not_found。
12. concurrent write batches from multiple cores。
13. front seal -> collect gens -> current tree-local flush input bridge。

## 5. 不迁移或延后迁移的旧 step

1. 旧 Step 20/21/27-32A 的 TREE/flush 实现不迁移。当前分支已有新的 tree/flush
   路径，应只做前端 bridge。
2. 旧 Step 29-33 的 tree-backed GET 只迁移读 API 的目标语义，不迁移旧 tree code。
3. 旧 Step 36-40 recovery/reclaim 暂不迁移。本文只保证 WAL bytes 可被未来 recovery
   扫描。
4. 旧 Step 41/42 real NVMe/stability 只迁移测试意图。当前分支已有 SPDK adapter
   和 real-NVMe smoke 经验，但迁移 step 是否直接沿用现有 adapter surface，仍由
   对应详细设计对照后裁决。

## 6. 迁移完成定义

本文范围完成时必须同时满足：

1. M01-M13 均有对应当前分支设计/实现/测试证据。
2. 每个迁移步都满足“完整语义切片”要求；没有以“简单/最小/后续再做”为理由留下
   已知语义缺口。若存在人工批准的例外，文档中必须能追溯裁决记录。
3. 039 只冻结 M01，不承担后续 WAL/coord/write pipeline 设计。
4. `write_batch` 完成 value durable -> WAL durable -> memtable insert -> publish。
5. `point_get` 通过 read_handle PRS、front memtable 和 value read 返回
   recently-written value。
6. coord `publish_batch` / `release_batch` 保证 gap-free durable_lsn。
7. WAL bytes 使用本迁移设计裁决后的 WAL format/helper 可 decode；若该决议要求
   修改当前 `format/wal.hh`，必须先改设计再改代码。
8. memtable 不保存 hot value，不暴露 value body carrier。
9. value/WAL I/O fan-out bounded。
10. mock e2e 覆盖 PUT、DEL、overwrite、multi-front、failure release、multicore、
   point_get-after-write。
11. seal/collect/release 能把真实 front sealed gens 接给当前 tree-local flush。

未满足以上任一项时，不能把前端迁移闭环标为完成。
