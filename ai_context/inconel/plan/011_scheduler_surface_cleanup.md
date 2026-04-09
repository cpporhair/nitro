# 011 — Scheduler Surface Cleanup

> 实现第十一步。收敛当前 scheduler / runtime 表面的命名、作用域和启动失败语义：把 tree/value scheduler 类型名对齐到 spec，删掉 tree lookup 的 `first_call` dead code，明确 `tree_manifest` 继续留在 `core/` 的归属理由，并把 runtime init 失败统一收敛成 panic。

## 本 step 覆盖的 issue

| Issue | 说明 |
|---|---|
| `INC-014` | tree/value scheduler 裸命名与 spec 不一致 |
| `INC-013` | tree lookup 的 `first_call` 是 dead code |
| `INC-007` | `tree_manifest` 模块归属需要拍板 |
| `INC-033` | runtime init 失败不应默默 leak 后继续抛异常 |

## 文件结构

```text
core/
├── registry.hh                       — 更新 base type 名称与注释
└── tree_manifest.hh                  — 明确模块归属理由（继续留在 core/）

tree/
├── scheduler.hh                      — `lookup_scheduler*` 重命名 + 删除 first_call
└── sender.hh                         — 跟着 base type 名称同步

value/
├── scheduler.hh                      — `scheduler*` 重命名
└── sender.hh                         — 跟着 base type 名称同步

runtime/
├── builder.hh                        — 跟着新类型名同步
└── start.hh                          — init failure 统一 panic

test/
└── apps/inconel/test/*.cc            — 仅同步新类型名与新断言文本
```

## 设计目标

1. 让 public / semi-public type 名称和 spec 对齐，降低跨模块阅读与 AI 误抄成本。
2. 删除 tree lookup 明显无效的控制流分支，减少 scheduler hot path 的无意义状态。
3. 对 `tree_manifest` 的模块归属给出明确决定，而不是继续悬空。
4. 让 runtime 初始化失败在进程级 fail-fast，而不是 leak 后继续依赖异常传播。

## 设计决策

| # | 决策点 | 结果 | 说明 |
|---|---|---|---|
| `D1` | rename 范围 | **改类型名，不改文件路径** | 控制 diff 噪音，保留现有 include 路径 |
| `D2` | compatibility alias | **不保留旧类型别名** | 这一步就是为了真正收掉裸命名；保留 alias 只会拖长过渡期 |
| `D3` | `tree_manifest` 归属 | **继续放在 `core/`** | 它是 immutable runtime snapshot，供多个 scheduler/shared runtime 结构使用，不是 tree writer-local helper |
| `D4` | runtime init 失败语义 | **catch 后直接 panic** | 不做半初始化 cleanup 设计；失败后进程终止，由 OS 回收资源 |

## 详细设计

### 类型名对齐：`INC-014`

统一改成 spec 名字：

| 旧名 | 新名 |
|---|---|
| `tree::lookup_scheduler_base` | `tree::tree_lookup_sched_base` |
| `tree::lookup_scheduler<Cache>` | `tree::tree_lookup_sched<Cache>` |
| `value::scheduler_base` | `value::value_alloc_sched_base` |
| `value::scheduler<Cache>` | `value::value_alloc_sched<Cache>` |

同步范围：

- `tree/scheduler.hh`
- `tree/sender.hh`
- `value/scheduler.hh`
- `value/sender.hh`
- `core/registry.hh`
- `runtime/builder.hh`
- `apps/inconel/test/*` 的类型引用

不改：

- 文件名 `scheduler.hh`
- `core::registry::value_alloc_sched` 这个变量名（它已经与 spec 一致）

### 删除 `first_call`：`INC-013`

当前 `make_lookup_state()` 已经在空 manifest 时：

- `all_done = true`
- 所有 entry 直接标 `resolved = true`

所以 `tree/scheduler.hh::handle()` 里的这段：

```cpp
if (s.first_call) {
    ...
}
```

是 dead code。

本步直接删除：

- `lookup_state::first_call`
- `handle()` 里的整个 `if (s.first_call)` 块

这样 `handle()` 一进入就走：

1. `process_entries(s)`
2. `s.all_done ? done : need_read / wait`

### `tree_manifest` 归属拍板：`INC-007`

本步明确决定：`tree_manifest` 继续留在 `core/`，不迁文件。

原因：

1. 它不是 page-format helper
2. 它不是 tree 写侧内部状态对象
3. 它是 reader-visible 的 immutable snapshot
4. 它已经被 tree lookup、runtime registry、future checkpoint/reclaim 语义共同引用

落地动作：

- 在 `core/tree_manifest.hh` 文件头加一段短注释，说明“为何在 core/ 而不在 tree/”
- 不做路径迁移，不改 include 路径

### runtime init 失败统一 panic：`INC-033`

`runtime/start.hh::run_with()` 包一层统一 catch：

```cpp
try {
    ...
} catch (const std::exception& e) {
    core::panic_inconsistency("runtime::run_with", "init failed: %s", e.what());
} catch (...) {
    core::panic_inconsistency("runtime::run_with", "init failed: unknown exception");
}
```

覆盖来源：

- `build_runtime(...)` 内部任意构造异常
- `pump::env::runtime::start(...)` 异常

本步的关键点：

- 不继续向上抛
- 不试图做“部分初始化 cleanup”
- 直接把 init failure 定义成 process-fatal

这与当前 Inconel 的整体语义一致：如果 runtime 连 scheduler 拓扑都没建起来，就没有可恢复运行的意义。

## 实施顺序

1. tree/value 两侧类型名重命名。
2. sender / registry / builder / tests 跟着同步。
3. 删除 tree lookup 的 `first_call`。
4. `core/tree_manifest.hh` 增加模块归属说明。
5. `runtime/start.hh` 加统一 catch + panic。

## 验证

实现本 step 时至少回归：

- `inconel_test_tree_lookup`
- `inconel_test_tree_lookup_multicore`
- `inconel_test_value`
- `inconel_test_tree_value`
- `inconel_test_runtime`

重点观察：

- rename 后所有 sender / registry / runtime 绑定仍正常
- tree lookup 空 manifest 路径不回归
- runtime init failure 走 panic，而不是漏到外层异常
