# 063: Automatic Seal / Flush Maintenance Cadence Design

## 1. 背景

061 已经把 reclaim + value trim 接到 production runtime 的唯一 maintenance cadence 上：

```text
maintenance_sched::launch_round()
  -> rt::maintenance_once()
  -> maintenance driver finish/fail seam
```

但 061 明确没有做自动 seal/flush。062 的 YCSB 入口因此仍需要在 load 后显式提交：

```text
seal_once -> flush_once
```

这说明读链路已经能从 memtable/tree/value 取值，但写入进入 memtable 后，production runtime 还缺一个把 active memtable 切成 sealed gen、再把 durable sealed gen flush 到 tree 的自动触发机制。

063 的目标是把 seal/flush 接到同一条 maintenance cadence，避免再次出现 helper 内部创建隐藏 root pipeline 的异步碎片化问题。

## 2. 目标

1. 扩展 `rt::maintenance_once()`：

```text
pressure policy
  -> maybe seal_round
  -> maybe flush_round
  -> tree reclaim_once
  -> value drain_trim_once
```

2. 复用 061 的唯一 root submit allowlist：
   - `apps/inconel/runtime/maintenance_scheduler.hh::maintenance_sched::launch_round()`
   - 不新增 scheduler handler 内部 submit。
3. 给 `front_sched` 暴露 production 可读的 pressure counters：
   - active memtable approximate bytes
   - sealed memtable approximate bytes
   - sealed gen count
   - active 是否有数据
4. 使用 `wal_space_sched::used_segment_count()` / `segment_count()` 参与 seal trigger。
5. maintenance result/reporting 能区分：
   - seal 是否执行
   - flush 是否执行、是否 no-op
   - reclaim/trim 是否执行
6. 保持 flush 的 correctness contract：
   - 只 flush `gen.max_lsn <= durable_lsn` 的 full gen
   - flush 不是 commit point
   - flush 后才允许 WAL reclaim 进入后续 reclaim round

## 3. 非目标

1. 不在本步实现完整 RSM §2.7 的 pre-LSN backpressure pending raw batch 队列。
   - 这需要改写 `coord_sched::handle_assign_lsn` 的入队、恢复和客户端等待语义。
   - 063 只把 `max_sealed_gens_per_front` 作为 policy trigger / 观测阈值，不把前台写阻塞在 post-LSN 阶段。
2. 不把 seal trigger 放回 owner handler 内部启动子 pipeline。
   - RSM §2.6 早期文字写的是 coord inline trigger；在 PUMP 当前约束下，跨 owner / NVMe 的异步流程必须是 L3 sender pipeline。
   - 063 选择 runtime maintenance cadence 作为唯一触发边界。
3. 不新增独立 monitor thread。
4. 不让 YCSB 或其他 app helper 私自维护第二套 background cadence。
5. 不改变 write ACK 语义。ACK 仍由 value durable + WAL durable + memtable insert + publish 决定。

## 4. 触发策略

新增 maintenance policy，默认开启 auto seal/flush：

```cpp
struct maintenance_policy {
    bool     auto_seal_flush = true;
    uint64_t seal_active_memtable_bytes = 256 * 1024 * 1024;
    uint64_t total_memtable_limit_bytes = 1024 * 1024 * 1024;
    float    wal_seal_used_ratio = 0.70f;
    uint32_t max_sealed_gens_per_front = 4;
};
```

一次 maintenance round 开头采集 pressure snapshot：

```text
active_bytes = sum(front.active_memtable_bytes)
sealed_bytes = sum(front.sealed_memtable_bytes)
total_bytes  = active_bytes + sealed_bytes
max_sealed   = max(front.sealed_gen_count)
wal_ratio    = wal.used_segment_count / wal.segment_count
active_has_data = any(front.active_memtable_has_entries)
```

Seal 条件：

```text
auto_seal_flush &&
active_has_data &&
(
  active_bytes > seal_active_memtable_bytes ||
  total_bytes  > total_memtable_limit_bytes ||
  wal_ratio    > wal_seal_used_ratio ||
  max_sealed   >= max_sealed_gens_per_front
)
```

`active_has_data` 是必要保护：否则空闲 maintenance round 会不断 seal 空 active gen，制造无意义 imms。

Flush 条件：

```text
auto_seal_flush &&
(
  seal just ran ||
  max_sealed > 0 ||
  total_bytes > total_memtable_limit_bytes ||
  wal_ratio > wal_seal_used_ratio
)
```

`flush_round_once()` 自己仍按 durable_lsn 过滤 eligible gen。即使本轮没有 eligible gen，也只能返回 no-op，不能绕过 durable frontier。

## 5. Pipeline 形状

`pipeline/maintenance_round.hh` 扩展成两个层级：

```text
maintenance_round_once(coord, fronts, wal, tree, policy, value_nvme)
  -> pressure snapshot
  -> policy branch
  -> maybe seal_round(coord, fronts)
  -> maybe flush_round_once(coord, fronts, tree)
  -> reclaim_trim_round_once(tree, value_nvme)
  -> maintenance_round_result
```

保留旧的 `maintenance_round_once(tree, value_nvme)` overload，作为 reclaim/trim-only sender，避免已有 deterministic harness 必须同时承担 seal/flush。

`rt::maintenance_once()` 使用新的 full overload：

```text
coord + fronts + wal_space + tree + policy
```

`maintenance_sched::launch_round()` 只从：

```text
rt::maintenance_once()
```

变为：

```text
rt::maintenance_once(policy)
```

它仍是唯一 root pipeline submit 点。

## 6. Pressure Counters

`core::memtable_gen` 增加 owner-local approximate memory helper：

```text
gen_arena reserved bytes
+ version_count * sizeof(memtable_entry)
+ table.size() * small key/index estimate
```

这是 trigger/tuning signal，不是 correctness boundary。估算偏大只会更早 seal；估算偏小只会更晚 seal。

`front_sched` 维护 relaxed atomic counters：

```cpp
std::atomic<uint64_t> active_memtable_bytes;
std::atomic<uint64_t> sealed_memtable_bytes;
std::atomic<uint32_t> sealed_gen_count;
std::atomic<bool>     active_memtable_has_entries;
```

更新点：

1. constructor 初始化 active counter。
2. `apply_insert_validated()` 后更新 active counter。
3. `seal_active_now()` 后更新 active + sealed counters。
4. `release_gens_now()` 后更新 sealed counters。

跨线程 reader 只做 relaxed load；它不读取 front 内部容器。

## 7. Failure / Shutdown

1. seal/flush/reclaim/trim 任一 sender failure 仍走 061 的 `submit_fail_round()`。
2. maintenance driver panic 策略不变：production maintenance round failure 是 fatal inconsistency。
3. shutdown disable/wait 语义不变：
   - disable 后不再 launch 新 round
   - 已 inflight 的 full maintenance round 继续走完 finish/fail seam

## 8. 与 RSM 的偏差和后续项

RSM §2.6 原文说 coord 在 `handle_assign_lsn` 内联触发 seal。该方案在当前代码质量约束下会诱导 owner handler 内部提交跨 owner pipeline，因此 063 选择 061 的 runtime maintenance cadence 作为实现边界。

RSM §2.7 的 pre-LSN backpressure 仍然是后续项。建议作为 064 单独做：

```text
coord pending raw batch queue
  -> max_sealed pressure blocks assign_lsn
  -> flush release wakes pending assign
```

这一步和 063 相邻，但不适合混做；否则既改后台 cadence，又改前台 LSN 分配和客户端等待语义，回归面过大。

## 9. 验收

1. production 代码中仍只有 061 allowlist 的 maintenance root submit，以及 app root submit。
2. `rt::maintenance_once(policy)` 在低阈值下能自动执行 seal + non-noop flush。
3. 空闲 maintenance round 不制造空 sealed gen。
4. reclaim/trim 仍在 seal/flush 后运行。
5. full build 通过。
6. 真实 NVMe E2E 至少覆盖一组 auto seal/flush，而不只靠 YCSB load 后手动 flush barrier。

YCSB 实盘验证可以通过以下参数把默认阈值压低：

```text
--maintenance-seal-active-bytes N
--maintenance-total-memtable-bytes N
--maintenance-wal-seal-percent N
--maintenance-max-sealed-gens-per-front N
```

063 后 YCSB 默认不再执行 load 后手动 `seal_once -> flush_once` barrier；它使用 production auto maintenance。若显式传 `--flush-after-load`，YCSB 会进入 deterministic/manual barrier 模式并关闭 auto seal/flush，避免两条 root pipeline 同时抢 `coord_sched.catalog_update_in_progress`。
