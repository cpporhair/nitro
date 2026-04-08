# Runtime 模块 Phase 0 审计

> Spec-only audit。**未读** test 文件 / plan 文件。

## 1. 范围

| 文件 | 行数 |
|------|------|
| `runtime/builder.hh` | 152（tree audit 部分读过，本轮重读） |
| `runtime/start.hh` | 107 |

总 ~260 行。

Spec 依据：`design_overview.md` §3.3 / §4.1（格式化参数 + 运行时部署），`code_modules.md` runtime/ 段。

## 2. 已被现有 INC 覆盖的部分

- `lookup_scheduler` 用 `std::make_unique<char[]>` 非 DMA buffer → INC-002（runtime/builder.hh 是这个 buffer 的最终消费者，但 root cause 在 lookup_scheduler）
- `lookup_scheduler` / `value::scheduler` 命名问题 → INC-014

## 3. 本轮新发现

### R1 — `build_runtime` + `start_runtime` 没有 RAII，部分 init 失败时 leak

**位置**：`builder.hh::build_runtime` + `start.hh::run_with`

```cpp
// builder.hh
auto* rt = new inconel_runtime_t<TreeCache, ValueCache>();
// 多个 new 构造 schedulers
auto* nvme = new mock_nvme::scheduler(opts.device);
auto* tlookup = new tree::lookup_scheduler<TreeCache>(...);
// ... 任何一个 new 抛 → 之前的全 leak
```

```cpp
// start.hh
auto* rt = build_runtime<TreeCache, ValueCache>(bopts);
pump::env::runtime::start(rt, opts.cores, opts.main_core, [](auto*, uint32_t){...});
// start() 抛 → rt 永远不会被 destroy_runtime 释放
destroy_runtime<TreeCache, ValueCache>(rt);
```

**问题**：

1. `build_runtime` 内部一连串 `new`，任何一个抛 `std::bad_alloc` 或构造异常 → 已分配的 scheduler / nvme / tlookup / value_sched 全部 leak（包括内部已注册到 core::registry 的）
2. `start.hh::run_with` 调 `pump::env::runtime::start` 后才调 `destroy_runtime`，start 抛了就 leak rt + 所有 scheduler

**Tier 1**：silent leak on init failure。dev 期间 mock_nvme 不会失败，但 v2 真 nvme 上线时 init 失败是常见路径。

**修复方向**：用 `std::unique_ptr` + 自定义 deleter，或者 build_runtime 内部 try/catch + cleanup。

### R2 — `start_options.value_class_sizes` 为空时**静默禁用** value scheduler

**位置**：`builder.hh::build_runtime` 注释 + 实现：

```cpp
value_sched_t* value_sched = nullptr;
if (first && !opts.value_class_sizes.empty()) {
    value_sched = new value_sched_t(...);
    core::registry::value_alloc_sched = value_sched;
}
```

`start.hh::start_options` 注释也说："Empty value_class_sizes disables the value scheduler (the runtime still builds, just without it)."

**问题**：

- 用户传空 `value_class_sizes`（可能是配置错误 / 默认值忘填 / 单元测试默认）→ runtime 静默不带 value scheduler
- 后续任何 `value_alloc_sched` 调用走 `core::registry::value_sched()`（assert 非 null）→ 触发 assert
- 也就是 silent disable + 延后 assert，而不是 build 期间 fail-fast

**Tier 2**：silent feature toggle。比 INC-004/005 那种 corruption 弱，但是同性质的"静默不当行为"。AI 抄这种"empty disables 子系统" pattern 会扩散。

**修复方向**：要么 build_runtime 在 empty 时直接 throw（fail-fast 不允许"半 runtime"），要么改成 `optional<...> value_class_sizes` 让"禁用"显式表达。

### R3 — `start_options.device` 是 `mock_nvme::mock_device*`，runtime 跟 mock_nvme 硬耦合

**位置**：`start.hh::start_options::device`

```cpp
mock_nvme::mock_device*   device;
```

整个 runtime 只接受 mock_nvme 的 device。等 nvme/ 落地时，runtime 必须改 abstraction。

**Tier 2 weak**：跟 INC-002 (DMA buffer mock_nvme only) 同组——runtime 也是"mock_nvme only"形态，没声明这个限制。

**修复方向**：跟 INC-002 一起做——nvme/ 落地时引入 device 抽象（virtual base 或 template 参数），runtime 接抽象不接 mock_nvme。

### R4 — 4 cache policy 组合产生 4 份模板实例化

**位置**：`start.hh::start_runtime` → `start_with_tree<T>` → `run_with<T, V>`

策略组合 = clock×clock / clock×slru / slru×clock / slru×slru = 4 份。整个 inconel_runtime_t<T, V> 模板（含 build/destroy + 所有 scheduler 类型）展开 4 次。

**评估**：

- 编译时间：每组合一份完整 PUMP scheduler tree 实例化，编译时间 ~4×
- 二进制大小：相应 ~4× scheduler 代码
- 运行时：单进程只跑一组，所以 runtime cost 还是 1×

**Tier 3 quality**——不是 bug，但 v2 加更多 cache policy（hot/cold split / arc 等）时组合会爆。

**修复方向**：v2 用 type erasure 或 cache_concept 的 virtual interface 替代模板分发。v1 接受现状。

### R5 — value::scheduler 硬编码绑 `cores[0]`

**位置**：`builder.hh::build_runtime`

```cpp
if (first && !opts.value_class_sizes.empty()) {
    value_sched = new value_sched_t(...);
}
```

`first` 是循环里的第一次 iteration，对应 `cores[0]`。注释说 "value::scheduler is a singleton, pinned to cores[0]. Future per-core placement... when configuration plumbing exists"。

**Tier 3**——已经有 placement 注释，是已知 v1 限制。

### R6 — Cache policy 用字符串 dispatch ("clock"/"slru")

**位置**：`start.hh::start_runtime` + `start_with_tree`

`std::string_view` 比较 `"clock"` / `"slru"`，未识别 throw `std::invalid_argument`。

**Tier 3**：plumbing OK，加新 policy 要改 3 处函数。但增长率低（policy 不会频繁加），可接受。

### R7 — Format/init 流程**完全缺失**

**Spec**：ODF §7 定义"格式化流程"——计算各区域边界 → TRIM 整盘 → 写 superblock A/B → 完成。

**现状**：runtime/ 没有 `format_disk()` 路径。`build_runtime` 假设 device 已经就绪——mock_nvme 是空白 buffer 直接当"已格式化"使，真 nvme 上来就需要先 format 否则没有 superblock 也没有 data area 边界。

**Tier 2**：缺 functional 入口。但跟 INC-031 (`superblock` POD 缺失) + INC-020 (`install_recovered_state` 缺失) 是一组——format/init/recovery 整套都没。

**Phase 1 影响**：用 mock_nvme 跑测试不需要 format（mock device 是 zero buffer），所以 dev 期不需要。production 必需。

### R8 — Per-core init hook 是空 placeholder

**位置**：`start.hh::run_with`

```cpp
pump::env::runtime::start(rt, opts.cores, opts.main_core,
    [](auto*, uint32_t /*core*/) {
        // Per-core init hook. PUMP has already set this_core_id by
        // the time this runs. Future per-core init (e.g. NUMA-local
        // allocator warm-up) goes here.
    });
```

注释解释了用途，hook 是空的。NUMA warm-up 等 future 工作要往这里塞。

**Tier 3**——已知占位，不算 finding。

## 4. 跟现有 INC 的关系

| Finding | 处置建议 |
|---|---|
| R1 (RAII / leak on init fail) | **新 INC**，Tier 1 priority urgent（dev 期不触发，但 nvme/ 落地时会触发） |
| R2 (silent disable value sched) | **新 INC**，Tier 2 priority urgent（fail-fast 应当现在就改）|
| R3 (mock_nvme 硬耦合) | **扩 INC-002**（INC-002 是同组，nvme/ 落地时一起改 device abstraction） |
| R4 (4 模板实例化) | **不建条目**（v1 quality，acceptable） |
| R5 (value sched 硬编码 cores[0]) | **不建条目**（已有注释声明，known v1 limit） |
| R6 (字符串 dispatch) | **不建条目**（plumbing OK） |
| R7 (format/init 缺失) | **新 INC**，Tier 2 priority blocked（依赖 INC-031 superblock POD） |
| R8 (空 hook) | **不建条目**（占位） |

预计 3-4 条新 INC。

## 5. 审计纪律

| 项目 | 状态 |
|------|------|
| 打开 test 文件 | 否 |
| 打开 plan 文件 | 否 |
| 想读测试的冲动 | 0 次 |
