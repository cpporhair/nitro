# 068: INC-057 WAL Group Commit Closeout / Concurrent Verification Design

## 状态

`INC-057` 的主体设计见
[`054_wal_append_group_commit_design.md`](054_wal_append_group_commit_design.md)。
当前生产代码已经按 054 方向落入 entry group commit：

- `wal_append_plan` 带 `participants`，`wal_prepare_result` 拆成
  `issue_plan / committed / needs_segment`。
- front owner 在 idle 时从 pending prepare FIFO 合并 followers，仍保持单个
  physical WAL plan 在飞。
- follower completion 只采用 leader FUA 后的 durable cursor，不发 NVMe I/O。
- `prepare_wal_fragment_core` 已覆盖 drain / build / begin_pending 的异常扇出。
- `wake_wal_group_committed` / `fail_wal_group` 已按 `plan_id` 匹配 group。
- `inconel_test_m14_wal_group_commit` 已覆盖 054 §16.1 六类场景。

本设计不是重写 WAL group commit，而是定义 068 的收口目标：用并发工作流把
`INC-057` 从 `known_issues.md` 的 `urgent` 状态推进到“已实现且经验证，剩余为
二阶性能项”。

## 目标

1. 对照 054 / WP / RSM / cross-doc 合同，确认当前实现满足 `INC-057` 的
   correctness 和 owner-boundary 要求。
2. 跑通 deterministic 覆盖，至少包括 M14 group commit 和 M06/M08/M09/M11
   相关回归。
3. 明确 `known_issues.md` 中 `INC-057` 的状态迁移条件，并在证据满足后更新。
4. 把剩余工作限制为二阶优化或性能评估，不让“多 pending plan”重新混入本轮。
5. 采用并发推进：代码审计、测试执行、文档状态收敛可以同时做，但不得并发编辑同一文件。

## 非目标

1. 不实现 disjoint-LBA 多 pending WAL plan。
2. 不实现普通 write + NVMe FLUSH barrier。
3. 不扩大 header / trailer / segment allocation 的 group participant 语义。
4. 不改变 WAL on-disk format 或 recovery parser。
5. 不用测试反推 production spec；测试只用于验证 054 和设计文档已经规定的行为。
6. 不并发运行多个 real-NVMe 测试进程抢同一个 BDF。

## 当前实现证据

| 领域 | 代码点 | 结论 |
|---|---|---|
| WAL result surface | `apps/inconel/front/wal_append.hh` | `wal_prepare_issue_plan` / `wal_prepare_committed` / `wal_prepare_needs_segment` 已存在 |
| 单 physical plan | `wal_stream_state::begin_pending` / `commit_pending` / `abort_pending` | stream 仍只允许一个 pending plan，并校验 `plan_id` |
| FIFO coalesce | `front_sched::drain_followers_into_wal_group` | budget / rotation 边界保留 FIFO 头，bad follower 单独失败 |
| group build | `front_sched::build_wal_entry_group_plan` | plan 只生成一组 full-page FUA writes |
| 异常扇出 | `front_sched::prepare_wal_fragment_core` | 已 pop 进 `waiters` 的 follower 会收到同一 exception |
| plan_id guard | `wake_wal_group_committed` / `fail_wal_group` | commit / abort 只唤醒当前 plan 的 group |
| L3 follower 语义 | `write_path::write_wal_fragment_step` | follower 不碰 NVMe、不 commit，只更新 cursor / done |

## 收口不变量

068 完成时必须能同时说明以下不变量：

1. **同 LBA 覆写安全**：任何时刻同一个 front WAL stream 只有一个 physical plan
   pending；group commit 降低 FUA 次数，但不引入多 plan 同 LBA 乱序覆写。
2. **value-before-WAL 不变**：group commit 不改变 value durable 在 WAL durable 之前的顺序。
3. **all-WAL barrier 不变**：同一 batch 所有 front fragments 都 durable 后才进入 memtable phase。
4. **follower 无设备 I/O**：follower 只收到 committed completion，不发 WAL FUA，不 commit stream。
5. **失败在 memtable 前释放**：leader FUA false/exception 会 abort physical plan，并让全部
   participants 走 WAL failure release；不得进入 memtable。
6. **FIFO / backpressure 清晰**：`pending_prepare_capacity` 仍是 front owner 排队容量，不是
   NVMe I/O 并发度；budget / rotation 不破坏 FIFO。
7. **recovery 合同不漂移**：recovery 仍按 `lsn + entry_count` 重组 batch，不要求同 batch
   entries 在 segment 内连续。

## 并发推进方式

### Workstream A: 代码审计

负责人检查生产代码，不读取测试作为规格来源。

范围：

1. `apps/inconel/front/wal_append.hh`
2. `apps/inconel/front/scheduler.hh`
3. `apps/inconel/write_path/sender.hh`
4. `apps/inconel/design_doc/write_path_and_pipeline.md`
5. `apps/inconel/design_doc/runtime_state_machine.md`
6. `apps/inconel/design_doc/cross_doc_contracts.md`

验收：

- 列出 054 §2 / §13 / §14 对应到代码的证据。
- 若发现代码和设计文档漂移，先记录具体行与风险，不直接改生产代码。
- 明确 `known_issues.md` 里 “review 后两处加固” 是否已被代码满足。

### Workstream B: deterministic 验证

负责人以测试维护者角色执行现有 deterministic targets。

首轮命令：

```bash
cmake --build build --target inconel_test_m14_wal_group_commit
./build/inconel_test_m14_wal_group_commit
cmake --build build --target inconel_test_m06_front_wal_append_prepare
./build/inconel_test_m06_front_wal_append_prepare
cmake --build build --target inconel_test_m08_write_baseline_inflight
./build/inconel_test_m08_write_baseline_inflight
cmake --build build --target inconel_test_m09_production_write_batch
./build/inconel_test_m09_production_write_batch
cmake --build build --target inconel_test_m11_runtime_topology_operations
./build/inconel_test_m11_runtime_topology_operations
```

ASAN 复跑：

```bash
cmake --build build_asan --target inconel_test_m14_wal_group_commit
./build_asan/inconel_test_m14_wal_group_commit
```

验收：

- M14 六个场景全通过。
- M06/M08/M09/M11 不因 group commit 回归。
- 若失败，先归类为编译失败、测试假设漂移、生产行为回归或环境问题。

### Workstream C: 文档 / issue 状态

负责人只在 A/B 证据齐备后编辑状态文档。

候选修改：

1. `known_issues.md` 的 `INC-057` 从 `urgent` 改为已完成 / verified。
2. 保留 `INC-057` 背景和否决方案，避免后续重复选择“简单多 pending plan”。
3. 把剩余项单列为二阶性能评估：
   - disjoint-LBA 多 pending plan。
   - 普通 write + FLUSH barrier。
   - 真盘小 batch microbench。

验收：

- 状态不夸大：如果只跑 deterministic，不写“real NVMe 性能已验证”。
- 文档写明已验证日期、验证命令和剩余非阻塞项。

### Workstream D: 可选性能设计

本流不阻塞 `INC-057` closeout。只有 A/B/C 完成后再启动。

目标是定义 microbench，不直接以临时命令数字作为设计结论：

1. 固定 workload：同 front 小 batch PUT/DEL，控制 batch size、client inflight、value size。
2. 固定指标：group participants 分布、WAL FUA writes / logical batch、tail snapshot 次数、
   write latency p50/p99。
3. 固定环境：real NVMe BDF、SPDK status、独占进程、build type。

## 串行化点

1. `known_issues.md` 只能在 A/B 都完成后更新。
2. 同一 build dir 内不要同时 `cmake --build` 同一 target；不同 build dir 可以并行。
3. real NVMe 测试必须串行，并先确认 scratch BDF；068 首轮不要求 real NVMe。
4. 如果 A 发现 production correctness 缺口，立即停止 C，先讨论修复设计。

## 状态迁移标准

`INC-057` 可从 `urgent` 收口为已完成，当且仅当：

1. A 确认当前代码满足 054 的核心不变量。
2. B 的 deterministic 首轮和 ASAN M14 通过。
3. C 更新 `known_issues.md`，把剩余工作降为二阶性能项。

不要求：

1. 真盘 microbench 已完成。
2. disjoint-LBA 多 pending plan 已实现。
3. 普通 write + FLUSH barrier 已设计或实现。

## 执行记录

- 2026-06-24: 创建 068 closeout 设计。当前判断是 production 实现已包含 054
  主体和 review hardening，下一步采用 A/B/C 并发推进。
- 2026-06-24: A/B/C 收口完成。代码审计确认 production 已满足 054 核心不变量：
  front WAL 仍单 physical pending plan，followers 通过 `wal_prepare_committed`
  完成，异常和 abort 统一 fan-out，commit/abort 以 `plan_id` guard group。
- 2026-06-24: deterministic 验证通过：
  `inconel_test_m01_batch_carrier_memtable`、
  `inconel_test_m14_wal_group_commit`、`inconel_test_m06_front_wal_append_prepare`、
  `inconel_test_m08_write_baseline_inflight`、
  `inconel_test_m09_production_write_batch`、
  `inconel_test_m11_runtime_topology_operations` 均 exit 0。
  ASAN 复跑 `inconel_test_m01_batch_carrier_memtable` 与
  `inconel_test_m14_wal_group_commit` 均 exit 0；本地环境需
  `ASAN_OPTIONS=detect_leaks=0`，因为 LeakSanitizer 在当前 ptrace-like
  运行环境下无法启用 leak pass。
- 2026-06-24: 验证中顺手修复两处相邻生产/测试基建问题：
  `batch_carrier` 的 rvalue build path 不再保留指向 moved-from input buffer
  的 `string_view`，并把 canonical position map 换成确定性 `btree_map`；
  runtime front topology 支持注入 WAL DMA allocator，使 fake topology 能使用
  heap DMA allocator 而不依赖 SPDK DMA。
- 2026-06-24: `INC-057` 可迁移为已完成 / deterministic verified。未做也不阻塞
  closeout 的剩余项：真盘小 batch microbench、disjoint-LBA 多 pending plan、
  普通 write + FLUSH barrier。
