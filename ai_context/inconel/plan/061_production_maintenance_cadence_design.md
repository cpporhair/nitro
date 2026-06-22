# 061 — Production Maintenance Cadence / Reclaim Driver

> 对应：060 完成后留下的相邻项：`rt::reclaim_once()` 已经是 first-class sender，但生产 runtime 还没有顶层 cadence 周期调用它。
>
> 本文是设计文档。后续实现 production 代码时仍按 `CLAUDE.md` 规则执行：实现阶段禁止读测试文件；测试维护阶段另行声明。

## 0. 一句话

新增一个 runtime-owned maintenance driver，在生产主循环中以 bounded cadence 启动一条显式 maintenance sender：

```text
runtime maintenance driver
  → tree::reclaim_once()
      → read_domain invalidate
      → tree TRIM
      → value::reclaim_values
      → wal::reclaim_check
  → if reclaim did work: value::drain_trim_once()
  → maintenance driver finish seam
```

它解决的问题不是 reclaim 业务语义本身，060 已经解决；061 解决的是“谁在真实 runtime 里持续驱动这条显式 pipeline”，并用静态门禁防止 hidden `submit(root_context)` 模式回流到 owner handler。

---

## 1. 背景

### 1.1 060 之后的状态

060 已经把 tree reclaim 从 hidden sub-pipeline 改成：

```cpp
rt::reclaim_once()
```

其内部是 first-class sender chain：

```text
tree owner acquire mutation gate
  → prepare reclaim plan
  → read_domain invalidate fan-out
  → finish invalidates
  → NVMe TRIM fan-out
  → finish trims / recycle tree ranges
  → value::reclaim_values(dead_value_refs)
  → wal::reclaim_check(flush_durable_frontier)
  → recompute recovery frontier
  → finish round
  → release mutation gate
```

`tree_sched::advance()` 现在只负责：

1. drain `reclaim_q` ingress，把 destructor post 进来的 task 搬入 owner-local pending 状态；
2. 处理 owner seam queue；
3. 不再自动启动 read_domain / NVMe / value / WAL I/O。

这符合 PUMP 的编排原则，但留下一个生产集成缺口：测试 harness 可以显式调用 `rt::reclaim_once()`，真实 runtime 不能靠“有空时 advance 一下 tree owner”自动消费完整 reclaim pipeline。

### 1.2 value TRIM 也是 maintenance

`tree::reclaim_once()` 尾部调用 `value::reclaim_values(...)`，但 value 的整页释放会进入 value owner 的 trim-pending 状态。当前显式 helper 是：

```cpp
value::drain_trim_pending()
```

它执行：

```text
value prepare_trim_batch
  → NVMe TRIM fan-out
  → value complete_trim_batch
```

所以 production maintenance cadence 不能只调用 tree reclaim。否则 tree old slot/range 与 dead value refs 会推进，但 value whole-page trim / 归池仍要靠外部手动补一轮。

061 的第一版 cadence 固定为：

```text
reclaim once, then value trim once iff reclaim was non-noop
```

这个 gate 是正确性边界，不只是优化：`value::prepare_trim_batch()` 会
temporarily withhold `global_free_extents`。如果 runtime 刚启动、还没有任何
reclaim work，就无条件 trim，可能把 bootstrap 的大块可分配空间整块放入
trim-inflight，使第一批前台 write 暂时看到 no allocatable extent。

后续如果要把 seal / flush trigger 也纳入同一个 driver，可以扩展同一 runtime-owned driver，但不在本步做。

### 1.3 当前 runtime 接入事实

`apps/inconel/runtime/start.hh` 当前调用的是：

```cpp
pump::env::runtime::start(...)
```

`apps/inconel/runtime/run.hh` 虽然定义了 Inconel 自己的 per-core `rt::run()`，但 production start path 没有包含或调用它。061 不应把 cadence 设计建立在一个未接线的 loop hook 上。

可选处理有两个：

1. 把 maintenance driver 做成普通 scheduler tuple entry，让 PUMP 现有 runtime loop 自然 advance 它；
2. 同步把 `runtime/run.hh` 接回 `start.hh`，并让新的 Inconel `rt::start()` 调用 `rt::run()`。

本设计选择两者都做：

- maintenance cadence 的正确性依赖普通 scheduler entry，不依赖特殊 loop hook；
- 既然本步要改 runtime start 边界，就顺手把现有 `runtime/run.hh` 接线或清理掉，避免“注释说替换 PUMP loop，实际没替换”的漂移继续存在。

---

## 2. 目标与非目标

### 2.1 本步目标

1. 新增生产 runtime maintenance cadence：
   - 周期性启动 `rt::maintenance_once()`；
   - 一次最多一个 maintenance pipeline inflight；
   - 每轮工作量 bounded；
   - idle 时 backoff，避免无活时持续跨 owner no-op。

2. 把 reclaim 和 value trim 放进同一条显式 top-level pipeline：
   - `tree::reclaim_once()` 先跑；
   - 仅当 reclaim 非 no-op 时，`value::drain_trim_once()` 后跑；
   - WAL reclaim 仍只由 `tree::reclaim_once()` 内部按 `flush_durable_frontier` 调用，不新增第二条 WAL maintenance 路径。

3. 新增 runtime-owned driver scheduler：
   - driver 只拥有 cadence / inflight / stats；
   - 不拥有 tree/value/WAL 业务状态；
   - 完成/失败必须通过 driver owner seam 回来，不能在最后一个业务 owner 上直接改 driver state。

4. 明确 root `submit()` 的允许边界：
   - runtime driver 可以在 `launch_round()` 作为顶层入口启动 first-class pipeline；
   - L2 owner handler 和 L3 business pipeline helper 仍禁止 hidden root submit；
   - 加静态扫描门禁。

5. 补齐 shutdown 约束：
   - 生产 stop 必须先 disable maintenance driver；
   - 等 inflight pipeline 回到 driver finish seam 后，再清各 core `is_running_by_core`；
   - 禁止在 maintenance pipeline inflight 时直接销毁 runtime scheduler。

### 2.2 本步不做

1. 不实现 seal / flush 自动触发策略。
   - seal/flush trigger 需要阈值、front memory/WAL pressure、write backpressure 策略一起设计；
   - 061 只接 060 后续的 reclaim / trim cadence。

2. 不做 INC-053 `tree_allocator.free_ranges` coalescing。
   - 061 只保证 reclaim 被生产 runtime 驱动；
   - allocator free-range 数据结构改造另起 step。

3. 不做 INC-054 alloc floor / ENOSPC backpressure。
   - 该项仍按 2026-06-16 裁决 blocked。

4. 不把 reclaim 并入 flush。
   - flush 产出 retired；
   - reclaim 消费 guard/gen 释放后 post 的 task；
   - 二者必须继续通过生命周期边界隔离。

5. 不引入 production `virtual` / `override`。

---

## 3. 设计原则

### 3.1 Top-level Driver 可以启动 Root Pipeline，Owner Handler 不可以

060 禁止的是这种形态：

```text
tree owner handler
  → submit(root_context)
  → hidden read_domain / NVMe sub-pipeline
  → completion queue 回 owner
```

061 允许的是这种形态：

```text
runtime maintenance driver
  → submit(root_context)  # top-level runtime boundary
  → rt::maintenance_once()
  → maintenance driver finish seam
```

差异有三条：

1. 启动点是 runtime 顶层 driver，不是业务 owner handler。
2. 业务子流程完整出现在 first-class sender chain 里。
3. 完成回 driver 只表示 root pipeline 生命周期结束，不承载 read_domain/NVMe/value/WAL 的业务 fan-in。

### 3.2 Driver 是 Runtime State Owner，不是 Business Owner

driver 可以拥有：

```text
inflight flag
cooldown/backoff counters
round stats
stop/disabled state
```

driver 不可以拥有：

```text
retired object lists
tree allocator state
value free-space metadata
WAL segment pool state
read_domain cache state
```

业务状态仍归各 L2 owner；driver 只决定“什么时候启动下一轮顶层 maintenance pipeline”。

### 3.3 Completion 必须回到 Driver Owner Seam

错误形态：

```cpp
rt::maintenance_once()
  >> then([driver] {
       driver->inflight = false;  // 可能运行在 tree/value/wal owner core
     })
```

正确形态：

```text
rt::maintenance_once()
  → runtime::maintenance_finish(driver, result)
```

也就是说，pipeline 尾部必须显式 enqueue 一个 driver-owned finish/fail request，由 `maintenance_sched::advance()` 在 driver 所在 core 上更新 `inflight`、backoff 和 stats。

### 3.4 No-op 也要有 Cadence

reclaim/trim 都可能 no-op。no-op 不能 busy-loop：

```text
noop round
  → idle_backoff = min(idle_backoff * 2, max_idle_backoff)
```

有工作时不能一直睡很久：

```text
work round
  → idle_backoff = initial_idle_backoff
  → next_launch_after = active_gap_ticks
```

这样能同时满足：

1. 长时间无 reclaim task 时不打扰前台；
2. 大 flush 释放出很多 retired task 时能连续小步 drain；
3. 单轮 work 已由 060 bounded plan 保证，不需要 driver 自己一次吃光 backlog。

---

## 4. 新增 API 与类型

### 4.1 `value_trim_round_result`

当前 `value::drain_trim_pending()` 返回 `void`，driver 无法判断 trim 是否做了实际工作。061 应新增结果类型：

```cpp
namespace apps::inconel::value {

struct value_trim_round_result {
    bool     noop = true;
    uint32_t trimmed_ranges = 0;
    uint64_t trimmed_lbas = 0;
};

}
```

并把 public helper 收敛为单轮语义：

```cpp
value::drain_trim_once() -> value_trim_round_result
```

实现上可保留旧名 `drain_trim_pending()` 作为 thin wrapper，或直接改名；但设计语义必须是“一次 bounded trim batch”，不是“drain 到空”。

`trim_idle` 分支返回：

```cpp
{ .noop = true }
```

`trim_batch` 分支在 NVMe TRIM 全部成功且 `complete_trim_batch` 成功后返回：

```cpp
{
    .noop = false,
    .trimmed_ranges = alt.trims.size(),
    .trimmed_lbas = sum(alt.trims[].num_lbas),
}
```

TRIM 失败继续按当前语义传播异常。

### 4.2 `maintenance_round_result`

```cpp
namespace apps::inconel::runtime {

struct maintenance_round_result {
    tree::reclaim_round_result reclaim;
    value::value_trim_round_result trim;

    [[nodiscard]] bool did_work() const noexcept {
        return !reclaim.noop || !trim.noop;
    }
};

}
```

结果只用于 driver policy / stats，不作为业务正确性输入。

### 4.3 `rt::maintenance_once()`

`runtime/operations.hh` 新增：

```cpp
[[nodiscard]] inline auto maintenance_once();
```

建议实现放到 `apps/inconel/pipeline/maintenance_round.hh`，`operations.hh` 只做 facade。

sender 拓扑：

```text
tree::reclaim_once(*tree_sched)
  → if reclaim.noop: trim result = noop
  → else: value::drain_trim_once()
  → return maintenance_round_result
```

概念伪码（真实 C++ sender 里用 `std::variant` + `visit()` 做同型分支，不能让
`flat_map` 的两个 branch 直接返回不同 sender 类型）：

```cpp
return tree::reclaim_once(*core::registry::tree_sched_singleton())
    >> then(branch_on_reclaim_noop)
    >> visit()
    >> flat_map(skip_trim_or_run_value_trim_once);
```

`reclaim` result 是小 POD，允许按值捕获。

---

## 5. `maintenance_sched`

### 5.1 归属

新增 runtime 模块内部 scheduler：

```text
apps/inconel/runtime/maintenance_scheduler.hh
namespace apps::inconel::runtime
```

它属于 L3 runtime，不是新的 L2 business owner。它的存在是 runtime cadence control，不改变 coord/front/tree/value/wal 的 owner 边界。

### 5.2 Runtime Tuple

`inconel_runtime_t` 增加一个 scheduler 类型：

```cpp
using inconel_runtime_t = pump::env::runtime::global_runtime_t<
    nvme::runtime_scheduler,
    core::tree_read_domain<TreeCache>,
    value::value_alloc_sched<ValueCache>,
    tree::tree_sched,
    coord::coord_sched,
    front::front_sched,
    wal::wal_space_sched,
    runtime::maintenance_sched
>;
```

builder 只创建一个 `maintenance_sched` 实例，默认挂在 `owner_core`。原因：

1. reclaim 的第一跳和 gate 在 tree owner；
2. owner core 已经承载 tree maintenance，不把 driver 放到 front hot path 上；
3. driver 自身 per-tick 成本极低，不需要单独核心。

可配置项：

```cpp
struct maintenance_options {
    bool enabled = true;
    int32_t core = -1;  // -1 => owner_core

    uint32_t active_gap_ticks = 1;
    uint32_t idle_initial_backoff_ticks = 256;
    uint32_t idle_max_backoff_ticks = 1u << 20;
    uint32_t completion_queue_depth = 4;
};
```

`start_options` / `build_options` 可以带这个结构。默认 enabled，但测试 fixture 可以关闭，继续手动调用 `rt::maintenance_once()` 做确定性验证。

### 5.3 State

```cpp
struct maintenance_sched {
    enum class mode : uint8_t {
        running,
        stopping,
        disabled,
    };

    mode st = mode::running;
    bool inflight = false;

    uint32_t cooldown_ticks = 0;
    uint32_t idle_backoff_ticks = idle_initial_backoff_ticks;

    uint64_t launched_rounds = 0;
    uint64_t completed_rounds = 0;
    uint64_t failed_rounds = 0;
    uint64_t work_rounds = 0;
    uint64_t noop_rounds = 0;

    per_core::queue<finish_req*> finish_q;
    per_core::queue<fail_req*> fail_q;
};
```

这里的 finish/fail queue 不是 060 那类业务 completion queue：

1. producer 只有当前 inflight root pipeline；
2. 同时最多一个 finish 或 fail；
3. queue 不承载 read_domain/NVMe fan-in；
4. 它只把 root lifecycle 结果送回 driver owner core。

如果 `try_enqueue` 失败，直接 `panic_inconsistency`，因为这表示“一次一个 inflight”的 driver 不变量坏了。

### 5.4 `advance()`

执行顺序：

```text
1. drain at most one finish/fail request
2. if disabled/stopping: do not launch
3. if inflight: return progress
4. if cooldown_ticks > 0: --cooldown_ticks; return progress_from_step_1
5. launch one root maintenance pipeline
6. inflight = true; ++launched_rounds; return true
```

冷却计数递减本身不应返回 progress，否则系统完全 idle 时会阻止 runtime loop yield。

### 5.5 Launch

driver launch 是本设计唯一允许的 production root submit 边界：

```cpp
void maintenance_sched::launch_round() {
    auto ctx = pump::core::make_root_context();
    rt::maintenance_once()
        >> flat_map([this](maintenance_round_result&& r) {
            return this->submit_finish_round(std::move(r));
        })
        >> any_exception([this](std::exception_ptr ep) {
            return this->submit_fail_round(std::move(ep));
        })
        >> pump::sender::submit(ctx);
}
```

实现注意：

1. `submit_finish_round` / `submit_fail_round` 是 maintenance owner sender，不是裸 lambda 改字段。
2. `this` 捕获是稳定 carrier：driver 由 runtime builder 创建，outlives started runtime loop；shutdown 必须先等 inflight=false 再 destroy。
3. `any_exception` 覆盖整条 business pipeline，失败后回 driver owner 记录 failure。failure 是否 panic 由 driver policy 决定；第一版建议 panic，因为 reclaim/trim 失败通常表示 device/runtime consistency 问题。

### 5.6 Finish / Fail Policy

finish:

```text
inflight = false
++completed_rounds
if result.did_work():
    ++work_rounds
    idle_backoff_ticks = idle_initial_backoff_ticks
    cooldown_ticks = active_gap_ticks
else:
    ++noop_rounds
    cooldown_ticks = idle_backoff_ticks
    idle_backoff_ticks = min(idle_backoff_ticks * 2, idle_max_backoff_ticks)
```

fail:

```text
inflight = false
++failed_rounds
panic_inconsistency("runtime::maintenance_sched", "maintenance round failed")
```

第一版不做“吞掉 reclaim/trim 错误继续跑”。原因：

1. tree TRIM / value TRIM failure 已经被各 owner 定义为 runtime/device failure；
2. WAL reclaim / recovery frontier 错误不能静默跳过；
3. 自动后台化不应降低错误可见性。

---

## 6. Pipeline Order 与 Fairness

### 6.1 Tuple 顺序

把 `maintenance_sched` 放在 runtime tuple 末尾。这样同一个 core 上每轮 advance 的大致顺序仍是：

```text
nvme
tree_read_domain
value
tree owner
coord
front
wal
maintenance driver
```

对默认 `maintenance_core = owner_core` 来说，tree owner 会先 drain reclaim ingress / owner seam queue，然后 driver 决定是否启动下一轮 `maintenance_once()`。这避免了“destructor 刚 post 到 `reclaim_q`，driver 先启动却看到 no-op”的常见空转。

### 6.2 Reclaim 与 Flush 的互斥

061 不新增互斥机制，继续使用 060 的 tree mutation gate：

```text
flush tree-local round
reclaim round
```

二者通过 tree owner 的 FIFO mutation gate 串行。driver 一次只发一个 `reclaim_once()`，不会排出一串 acquire 请求压住 flush。

如果 flush 已经持 gate，maintenance pipeline 会挂在 acquire seam 上；如果 reclaim 持 gate，flush 等本轮 bounded reclaim 完成。060 已经保证单轮 reclaim 有 bounded plan，不会长期占 gate。

### 6.3 Value Trim 与 Foreground Value I/O

`value::drain_trim_once()` 使用 value owner 的 `prepare_trim_batch / complete_trim_batch` seam 和 NVMe bounded TRIM fan-out。它和前台 `persist_put_values` / `read_value` 一样通过 value owner queue 排序。

061 driver 只在本轮 reclaim 非 no-op 后调用 trim。空闲 no-op reclaim 后不发
trim，避免启动时把 bootstrap free extent withhold 到 trim-inflight 并短暂饿死
前台 allocation。

061 不允许 trim driver 绕过 value owner 直接访问 `value_space_manager` 或 NVMe backend。

### 6.4 Idle Backoff 不等于 Correctness Boundary

idle backoff 只影响回收延迟，不影响正确性：

1. retired tree slots/ranges 在 reclaim 前仍被 old guard 生命周期保护；
2. dead value refs 在 reclaim 前只是空间暂未归还；
3. WAL segment 在 reclaim 前只是不可复用；
4. value trim pending 在 drain 前只是物理 TRIM/归池延后。

因此 backoff 可以保守；真正的容量压力策略以后应由 write backpressure / flush/reclaim trigger 一起设计，不在 061 内偷做。

---

## 7. Shutdown Contract

自动 maintenance driver 引入了一个新事实：即使没有前台请求，runtime 也可能有一条后台 pipeline inflight。

生产 shutdown 不能再只做：

```cpp
rt->is_running_by_core[core].store(false);
destroy_runtime(rt);
```

安全顺序必须是：

```text
1. maintenance_sched.disable()
2. pump runtime 继续 advance，直到 maintenance_sched.inflight == false
3. 停止接收新的前台 root pipeline
4. 等前台 pipeline quiesced
5. 清 is_running_by_core flags
6. start() 返回
7. destroy_runtime()
```

061 implementation 至少要提供一个 runtime helper：

```cpp
runtime::disable_maintenance_and_wait(rt)
```

或者在生产 stop API 中内联同等逻辑。若当前 `start_runtime()` 仍不暴露 runtime handle，则不能把“外部直接清 flags”作为 production stop contract；文档注释也要同步更新。

测试或 benchmark 若使用 lower-level `build_runtime()`，也必须在销毁前显式 disable/wait，或构造时关闭 maintenance driver。

---

## 8. Static Guard

### 8.1 禁止模式

生产 Inconel 代码里，除明确 allowlist 外，以下命中都应 fail review：

```text
pump::sender::submit
make_root_context
the_null_receiver
```

尤其禁止出现在：

```text
apps/inconel/coord/
apps/inconel/front/
apps/inconel/tree/
apps/inconel/value/
apps/inconel/wal/
apps/inconel/write_path/
apps/inconel/pipeline/
```

### 8.2 Allowlist

允许：

1. `apps/inconel/runtime/maintenance_scheduler.hh` 的 `launch_round()`；
2. 未来真正的 application/main/request boundary；
3. `apps/inconel/test/`。

每个 allowlist 命中必须在同文件附近有注释说明：

```text
top-level runtime root submit boundary; not an owner handler hidden sub-pipeline
```

### 8.3 Review 命令

建议加入脚本或 CI gate；手工命令：

```bash
rg -n 'pump::sender::submit|make_root_context|the_null_receiver' apps/inconel \
  --glob '!**/test*' \
  --glob '!**/*test*'
```

预期只命中 allowlist。任何新增 allowlist 必须写进对应 step 文档，不允许口头例外。

同时继续保留现有 no-virtual gate：

```bash
rg -n '\bvirtual\b|\boverride\b' apps/inconel \
  --glob '!**/test*' \
  --glob '!**/*test*'
```

---

## 9. 实现分解

### Phase A — value trim result

1. 新增 `value_trim_round_result`。
2. 把 `value::drain_trim_pending()` 收敛为 single-round result helper，或新增 `value::drain_trim_once()` 并迁移 runtime caller。
3. 更新 `cross_doc_contracts.md` 中 `drain_trim_pending` 的签名描述。

验收：

```text
trim_idle → noop=true
trim_batch success → noop=false + ranges/lbas 非零
trim failure → exception path 不被吞
```

### Phase B — `rt::maintenance_once()`

1. 新增 `pipeline/maintenance_round.hh`。
2. `runtime/operations.hh` 暴露 `rt::maintenance_once()`。
3. sender 顺序固定为 reclaim then conditional trim。

验收：

```text
reclaim work + trim idle
reclaim noop + trim idle
reclaim failure / trim failure
```

### Phase C — maintenance scheduler

1. 新增 `runtime/maintenance_scheduler.hh`。
2. 添加 finish/fail owner seam。
3. 实现 `advance()` cadence/backoff。
4. 添加到 runtime tuple 和 builder。
5. `build_options` / `start_options` 添加 `maintenance_options`。

验收：

```text
一次最多一个 inflight
finish/fail 在 driver owner core 更新状态
idle backoff 生效
有 work 时 active gap 生效
disable 后不再 launch
```

### Phase D — runtime start / stop

1. 接线或清理 `runtime/run.hh`。
   - 若接线：新增 `rt::start()`，`runtime::run_with()` 调用 Inconel start wrapper。
   - 若不接线：删除或改注释，不能继续声称 Inconel 替换了 PUMP loop。
2. 添加 production-safe maintenance disable/wait helper。
3. 更新 `runtime/start.hh` shutdown 注释。

验收：

```text
start path 实际使用的 loop 与文档一致
destroy_runtime 前 maintenance inflight=false
```

### Phase E — static guard

1. 加脚本或在 review checklist 明确命令。
2. allowlist 只包含 runtime boundary。
3. 更新 `code_quality_standard.md` §3.10，补上 runtime boundary 例外和扫描命令。

验收：

```text
owner handler 内无 root submit
pipeline helper 内无 root submit
runtime allowlist 有注释
```

---

## 10. 验证计划

实现完成后至少跑：

```bash
git diff --check
rg -n 'pump::sender::submit|make_root_context|the_null_receiver' apps/inconel --glob '!**/test*' --glob '!**/*test*'
rg -n '\bvirtual\b|\boverride\b' apps/inconel --glob '!**/test*' --glob '!**/*test*'
```

编译目标：

```text
inconel_real_nvme_compile_check
inconel_test_runtime_topology
inconel_test_m11_runtime_topology_operations
```

功能验证应覆盖两层：

1. Deterministic unit / integration：
   - maintenance disabled 时，旧手动 `rt::maintenance_once()` 路径可用；
   - maintenance enabled 时，driver 能自动 drain reclaim + value trim；
   - disable/wait 后不再 launch。

2. Real NVMe E2E：
   - 060 已跑过的 steady / value placement / concurrent / backpressure / seal race / multishard split 应重新跑一轮；
   - 新版本 harness 不应再手动在每个 quiesce loop 里强制 reclaim，至少要有一组覆盖“production auto maintenance cadence”。

真实盘使用规则仍沿用 060：不要碰系统盘；只用用户明确允许的 scratch NVMe。

---

## 11. 风险与裁决

### 11.1 Root Submit 回归风险

风险：别人看到 maintenance driver 里有 `submit(root_context)`，误以为 owner handler 里也可以这么做。

裁决：root submit 只允许在 runtime/application 顶层入口。owner handler / business pipeline helper 的 hidden root submit 仍是禁止项，静态门禁必须 fail。

### 11.2 Driver Completion Queue 命名风险

风险：`finish_q` / `fail_q` 被误解成 060 删除的 completion queue 又回来了。

裁决：driver finish/fail queue 是 root lifecycle seam，容量和 producer 都由“一次一个 inflight”约束；它不承载业务 fan-in。代码命名建议用：

```text
round_finish_q
round_fail_q
```

不要叫：

```text
reclaim_done_q
trim_done_q
completion_q
```

### 11.3 Foreground Interference

风险：maintenance 在写入压力下抢占太多 owner/NVMe 时间。

裁决：第一版用三层限制：

1. 一次一个 inflight；
2. 060 reclaim plan 和 value trim batch 已 bounded；
3. active/idle gap 可配置。

如果后续 workload 显示仍有干扰，再加 pressure-aware policy；061 不引入复杂 heuristics。

### 11.4 Stop 时 Inflight Pipeline

风险：生产 stop 直接清 runtime running flags，后台 pipeline 没机会回 finish seam，随后 destroy scheduler。

裁决：061 implementation 必须新增 disable/wait stop helper 或等价机制。没有这个机制，不允许默认启用 autonomous maintenance driver。

---

## 12. 与相邻工作的关系

### 12.1 与 060

060 解决“reclaim 业务 pipeline 必须显式”；061 解决“生产 runtime 必须显式驱动这条 pipeline”。

061 不改变 060 的 owner seam 和业务顺序。

### 12.2 与 INC-053

INC-053 是 tree allocator free-range 数据结构问题。061 只让 reclaim 按 cadence 发生，不改善 free_ranges 的 coalescing / bump-adjacent 回吞能力。

因此 061 之后，下一步仍建议做 INC-053；但 INC-053 的收益要建立在 reclaim 已被生产驱动的前提上，顺序上 061 先做更合理。

### 12.3 与 seal / flush trigger

061 不做自动 seal/flush。未来可以扩展 maintenance driver：

```text
seal/flush policy
  → seal_once()
  → flush_once()
  → reclaim/trim
```

但这需要单独设计 trigger 条件、front memory/WAL pressure、write backpressure 和 shutdown quiesce，不应混进 061。

---

## 13. 最终验收标准

061 实现完成后，应满足：

1. 生产 runtime 默认能自动推进 reclaim + value trim。
2. `tree_sched::advance()` 仍不启动 read_domain/NVMe/value/WAL hidden sub-pipeline。
3. `apps/inconel` 生产代码中 root submit 只出现在 runtime/application allowlist。
4. maintenance driver 一次最多一个 inflight round。
5. maintenance result/failure 通过 driver owner seam 更新状态。
6. shutdown 在 destroy runtime 前保证 maintenance inflight=false。
7. 060 真实 NVMe E2E 重新跑通，并至少一组验证 auto maintenance cadence，而不是只靠 harness 手动 reclaim。
