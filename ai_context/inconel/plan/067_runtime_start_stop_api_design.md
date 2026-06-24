# 067: Inconel Runtime Start/Stop Pipeline API Design

## 状态

062 把 `inconel_ycsb` 落成第一个真实 NVMe 执行入口，065 把入口参数收敛成
JSON/CLI effective config，066 固化了一致性验证体系。当前入口已经能驱动真实
NVMe、recovery continuation 和 YCSB oracle，但系统生命周期边界仍然不对：

- `apps/inconel/ycsb/main.cc` 同时负责设备打开、format/recovery、
  `build_runtime`、`rt::start`、root submit、stop run flags、maintenance stats、
  YCSB workload、oracle 和 stats 输出。
- `apps/inconel/ycsb/format_device.hh` 实际是 destructive bootstrap format helper，
  属于 runtime/recovery 边界，不应放在 benchmark 目录。
- `apps/inconel/runtime/start.hh` 已有一个低层 `start_runtime()`，但它不是
  `start_db(...)(user_sender)` 形态，也没有承载 app root submit、recovery boot、
  完整 topology 和 YCSB 当前实际使用的 `build_options`。

本设计定义 067 的目标形态：新增 Inconel 自己的 `runtime::start_db` /
`runtime::stop_db` pipeline API。YCSB 继续作为当前版本的应用入口，但不再拥有系统
启动外壳。

**实现进度（2026-06-24）**：Phase 1/2 已落地到代码。新增
`runtime/config.hh`、`runtime/session.hh`、`runtime/start_db.hh`、
`runtime/stop_db.hh`、`runtime/format_device.hh`、`runtime/stats.hh` 与
`ycsb/runtime_bridge.hh`；`ycsb/main.cc` 已迁出 device/recovery/build/start/root
submit/destroy；`ycsb/format_device.hh` 保留 forwarding header。当前实现采用
`start_db` 包装 app sender 并自动追加 terminal `stop_db()`，应用入口只返回 workload
sender，不直接拥有停机策略。

## 背景

KV/Sider 参考形态是：

```cpp
start_db(argc, argv)([] {
    return load(...)
        >> updt(...)
        >> read(...)
        >> stop_db();
});
```

这一形态的关键不是函数名，而是依赖方向：

- system/runtime 层拥有配置、设备、scheduler 初始化、run loop 和 stop。
- app/YCSB 层只返回一条 user pipeline。
- stop 也是 sender；KV 里通常由 user pipeline 显式追加。Inconel 067 实现把
  terminal `stop_db()` 包装在 `start_db` 的 app-root 边界，避免 app 忘记停机。
- runtime 层把 user pipeline 提交到唯一 app root，并负责异常停机和 teardown。

Inconel 当前已有正确的一半：`runtime/operations.hh` 已经给 app 层暴露
`rt::write_batch`、`rt::point_get`、`rt::seal_once`、`rt::flush_once`、
`rt::maintenance_once`。067 要补的是同等级的 lifecycle API。

## 目标

1. 新增 `runtime::start_db(options)(user_sender_factory)`，以 KV `start_db` 方式组织
   system lifecycle。
2. 新增 `runtime::stop_db()` sender，让 app pipeline 通过 context 请求 runtime 停止，
   不再直接写 `is_running_by_core`。
3. 把设备打开、bootstrap format、recovery boot、`build_options` 装配、
   `build_runtime`、`rt::start`、`destroy_runtime` 和 maintenance stats 收进
   `runtime/`。
4. YCSB 只保留 workload/oracle/stats/CLI 输出，并通过 bridge 把现有 `ycsb::config`
   映射成 `runtime::db_options`。
5. cache policy dispatch 只在 runtime lifecycle 层出现一次，避免 `ycsb/main.cc`
   与 `runtime/start.hh` 两条 bring-up 路径继续发散。
6. root submit allowlist 从 YCSB 迁移到 `runtime/start_db.hh`：YCSB 不再出现
   `make_root_context()` 或 `submit()`。
7. 保持现有 CLI、JSON schema、target 名、stats 输出和 real-NVMe smoke 命令兼容。

## 非目标

1. 不改 `write_path/`、`pipeline/`、`coord/`、`front/`、`tree/`、`wal/`、`value/`
   的生产数据路径语义。
2. 不在本步实现完整 production `format_disk()` 命令。067 只把当前 bootstrap
   format helper 移到正确模块边界；INC-035 的完整格式化流程仍是相邻后续项。
3. 不引入 virtual / override / runtime plugin registry。
4. 不为 YCSB 增加 MultiGet、scan、RMW、Zipfian 或网络协议。
5. 不新增第二套 background cadence。maintenance 仍只走 061/063 的 runtime
   maintenance scheduler。
6. 不把 YCSB expected-state oracle、benchmark stats 或 workload mix 放进 runtime。
7. 不改变 recovery 的 no-Value-Area-scan 约束。

## 当前耦合点

`apps/inconel/ycsb/main.cc` 中应下沉到 runtime 的内容：

| 位置 | 当前内容 | 目标归属 |
|---|---|---|
| `submit_app_root` | 创建 root context、submit `run_workload`、terminal stop | `runtime/start_db.hh` |
| `collect_maintenance_stats` | 遍历 runtime tuple 聚合 maintenance stats | `runtime/stats.hh` 或 `runtime/start_db.hh` |
| `nvme::real_device` 构造 | SPDK env / qpair / device 生命周期 | `runtime/start_db.hh` |
| `force_format_device` | destructive bootstrap format | `runtime/format_device.hh` |
| `recover_empty_clean_boot` | recovery boot 决策 | `runtime/start_db.hh` |
| `maintenance_options` 装配 | runtime policy mapping | `runtime/config.hh` bridge |
| `build_options` 装配 | topology/cache/recovered state mapping | `runtime/start_db.hh` |
| `build_runtime` / `rt::start` / `destroy_runtime` | system lifecycle | `runtime/start_db.hh` |
| `stop_runtime` | 清 runtime run flags | `runtime::stop_db()` sender |

应留在 YCSB 的内容：

- `workload_kind`、records/operations/value_size/batch/inflight/seed/key_prefix。
- key/value/op generator。
- load/run/verify/expected-verify phase pipeline。
- `expected_state` oracle 与 expect file 读写。
- benchmark phase stats、stats 输出、error counter 判定。
- CLI help、dry-run、effective config 打印。

## 目标 API

### `runtime::db_options`

新增 `apps/inconel/runtime/config.hh`，定义 owning 配置值对象。它不保存 span，不保存
YCSB workload 字段。

建议分组：

```cpp
namespace apps::inconel::runtime {

struct db_device_options {
    std::string pci_addr;
    std::string spdk_core_mask;
    std::string spdk_name = "inconel";
    uint32_t qpair_depth = 128;
    uint32_t device_id = 0;
};

enum class db_boot_mode {
    recover,
    force_format,
};

struct db_topology_options {
    std::vector<uint32_t> cores;
    std::vector<uint32_t> read_domain_cores;
    std::vector<uint32_t> front_cores;
    uint32_t main_core = 0;
    int32_t value_core = -1;
    int32_t owner_core = -1;
    int32_t coord_core = -1;
    int32_t wal_space_core = -1;
};

struct db_cache_options {
    std::string tree_policy = "clock";
    std::string value_policy = "clock";
    uint32_t tree_capacity = 1024;
    uint32_t value_capacity = 4096;
};

struct db_options {
    db_device_options device;
    db_boot_mode boot = db_boot_mode::recover;
    db_topology_options topology;
    db_cache_options cache;
    maintenance_options maintenance = {};

    // Build/runtime knobs not exposed by YCSB today still get defaults here,
    // so start_db can build a complete runtime::build_options without reaching
    // back into YCSB.
    std::size_t front_queue_depth = 1024;
    std::size_t coord_queue_depth = 1024;
    std::size_t coord_ready_window = 65536;
    std::size_t tree_queue_depth = 2048;
    std::size_t value_queue_depth = 2048;
    std::size_t nvme_queue_depth = 2048;
    std::size_t nvme_local_depth = 128;
    uint64_t nvme_dma_pool_pages_per_core = 4096;
    uint32_t nvme_dma_alignment = 4096;
    int nvme_numa_id = SPDK_ENV_NUMA_ID_ANY;
};

}  // namespace apps::inconel::runtime
```

`db_options` 必须覆盖当前 `runtime::build_options` 和 `nvme::real_device_options`
所需的全部输入。实现时不得因为 YCSB 当前没有 CLI 字段就丢掉 builder 默认项。

### `runtime::db_session`

新增 `apps/inconel/runtime/session.hh`，承载一次 DB run 的 system-owned 生命周期：

```cpp
template <core::cache_concept TreeCache, core::cache_concept ValueCache>
struct db_session {
    db_options opts;
    std::optional<nvme::real_device> device;
    std::optional<recovery::recovered_boot_state> recovered_boot;
    inconel_runtime_t<TreeCache, ValueCache>* rt = nullptr;
    maintenance_stats_snapshot maintenance_stats = {};
    std::exception_ptr app_error;
    bool runtime_built = false;
};
```

具体实现可拆成 RAII helper，但 ownership 必须满足：

- `real_device` outlive `build_runtime` 和所有 scheduler。
- `recovered_boot` outlive `build_runtime`，因为 `build_options` 只传 profile/state 指针。
- `rt` 在 `rt::start` 返回后 collect stats，然后 destroy。
- app 层只能拿 view，不拥有 `rt` 或 device。

### `runtime::db_session_view`

新增 type-erased view 放入 app root context，供 `stop_db()` 和未来 app-visible runtime
terminal helpers 使用。不用 virtual：

```cpp
struct db_session_view {
    void* runtime = nullptr;
    std::span<const uint32_t> cores;
    void (*request_stop)(void*, std::span<const uint32_t>) = nullptr;

    void stop() const {
        request_stop(runtime, cores);
    }
};
```

`make_db_session_view(rt, cores)` 用函数指针 thunk 绑定 concrete runtime type：

```cpp
template <typename Runtime>
db_session_view make_db_session_view(Runtime* rt, std::span<const uint32_t> cores) {
    return db_session_view{
        .runtime = rt,
        .cores = cores,
        .request_stop = +[](void* p, std::span<const uint32_t> cs) {
            auto* typed = static_cast<Runtime*>(p);
            for (uint32_t core : cs) {
                typed->is_running_by_core[core].store(false, std::memory_order_release);
            }
        },
    };
}
```

该 view 是 app-root-scoped context，不是 result owner。YCSB 的 `run_state` 仍由 YCSB
外层 `shared_ptr` 持有，用于 workload stats/oracle/error handoff。

### `runtime::stop_db()`

新增 `apps/inconel/runtime/stop_db.hh`：

```cpp
[[nodiscard]] inline auto stop_db() {
    return pump::sender::just()
        >> pump::sender::get_context<db_session_view>()
        >> pump::sender::then([](db_session_view& db) {
            db.stop();
        });
}
```

语义：

- `start_db` 的 app-root wrapper 在 finite app pipeline 正常结束后统一追加
  terminal `stop_db()`。
- `stop_db()` 只请求 runtime 停止；`rt::run()` 仍负责 disable maintenance 并等待
  inflight maintenance root pipeline quiesce。
- 多次 stop 必须幂等。
- app/YCSB 不再直接写 `runtime->is_running_by_core`。

### `runtime::start_db(opts)(user_sender_factory)`

新增 `apps/inconel/runtime/start_db.hh`。形态：

```cpp
runtime::start_db(opts)([state] {
    return ycsb::run_workload(state);
});
```

`user_sender_factory` 是模板 callable，返回 sender。不要用 `std::function` 保存它。

生命周期分两层：

1. Host lifecycle pipeline：一次性冷路径，负责 device/boot/build/start/destroy。
2. App root pipeline：运行在 Inconel runtime 的 `main_core`，只负责 user workload 和
   terminal stop。

伪代码：

```cpp
template <typename AppFactory>
auto start_db(db_options opts) {
    return [opts = std::move(opts)](AppFactory&& app_factory) mutable {
        return dispatch_cache_policy(opts.cache, [&](auto tree_tag, auto value_tag) {
            using TreeCache = typename decltype(tree_tag)::type;
            using ValueCache = typename decltype(value_tag)::type;

            db_session<TreeCache, ValueCache> session(std::move(opts));
            session.open_device();
            session.format_or_recover();
            session.build_runtime();

            auto cores = std::span<const uint32_t>(
                session.opts.topology.cores.data(),
                session.opts.topology.cores.size());

            rt::start(session.rt, cores, session.opts.topology.main_core,
                [&](auto* rt_ptr, uint32_t core) {
                    if (core != session.opts.topology.main_core) {
                        return;
                    }
                    auto view = make_db_session_view(rt_ptr, cores);
                    auto ctx = pump::core::make_root_context(view);
                    app_factory()
                        >> pump::sender::ignore_results()
                        >> ::apps::inconel::runtime::stop_db()
                        >> pump::sender::any_exception([&session](std::exception_ptr ep) {
                            session.app_error = std::move(ep);
                            return ::apps::inconel::runtime::stop_db_sender();
                        })
                        >> pump::sender::submit(ctx);
                });

            session.collect_maintenance_stats();
            session.destroy_runtime();
            if (session.app_error) {
                std::rethrow_exception(session.app_error);
            }
            return session.make_result();
        });
    };
}
```

实际代码需要注意两点：

- `any_exception` 的 failure path 必须先保存 `exception_ptr`，再请求 stop；正常
  finite job 由 `start_db` wrapper 追加 terminal stop。
- 如果实现里需要捕获 `session`，必须证明 app root 完成前 `session` 一直存活。
  `rt::start` 阻塞直到 stop 后返回，因此 host stack `session` 可以覆盖 app root
  生命周期。不能把 `session` 放进 root context 当唯一 owner。

### `runtime::db_run_result`

`start_db` 返回 system-level 结果，YCSB 用它打印 maintenance stats：

```cpp
struct db_run_result {
    maintenance_stats_snapshot maintenance;
};
```

YCSB phase stats、expected-state 文件和 benchmark error counter 仍从 YCSB
`run_state` 读取，不进入 `db_run_result`。

## Pipeline 形状

### Host lifecycle

```text
start_db(opts)(user_factory)
  -> validate/normalize db_options
  -> open real_device
  -> [force_format] bootstrap_format_device
     [recover]      recover_empty_clean_boot
  -> build_runtime<TreeCache, ValueCache>(complete build_options)
  -> rt::start(... on main_core submit app root ...)
  -> collect maintenance stats
  -> destroy_runtime
  -> rethrow app error if any
  -> db_run_result
```

这是 cold path。允许同步打开设备、同步 bootstrap format、同步 recovery boot 和阻塞
`rt::start`。这些不在 per-request 热路径上。

### App root

```text
make_root_context(db_session_view)
  -> user_factory()
  -> runtime::stop_db()
  -> submit(ctx)

exception:
  -> capture exception_ptr in db_session
  -> runtime::stop_db()
```

`runtime::stop_db()` 通过 context 找到 `db_session_view`，清 run flags。随后现有
`rt::run()` 进入 maintenance disable/quiesce 路径，再返回 `rt::start`。

## 文件落点

新增：

```text
apps/inconel/runtime/config.hh
apps/inconel/runtime/session.hh
apps/inconel/runtime/start_db.hh
apps/inconel/runtime/stop_db.hh
apps/inconel/runtime/format_device.hh
apps/inconel/runtime/stats.hh
apps/inconel/ycsb/runtime_bridge.hh
```

调整：

```text
apps/inconel/ycsb/main.cc
apps/inconel/ycsb/runner.hh
apps/inconel/ycsb/format_device.hh
apps/inconel/runtime/start.hh
ai_context/inconel/design_doc/code_quality_standard.md
```

`apps/inconel/ycsb/format_device.hh` 第一阶段保留为 forwarding header，避免
out-of-tree include 立即断裂。等仓库内外引用清干净后再删除。

`runtime/start.hh` 不应继续作为独立发散的 top-level path。实现时二选一：

- 改成 include/forward 到 `start_db.hh` 的低层 compatibility wrapper。
- 或在文档和代码注释中明确它是 legacy lower-level helper，不再作为 app entry
  推荐路径。

不能继续保留一个缺 recovery/topology/app-root 的“主入口”。

## 配置拆分

067 的实现不要一次性改 JSON schema。保持 065 的外部形态不变：

```json
{
  "device": {},
  "runtime": {},
  "maintenance": {},
  "workload": {},
  "verification": {},
  "output": {}
}
```

新增 `apps/inconel/ycsb/runtime_bridge.hh` 做显式映射：

```cpp
runtime::db_options make_db_options(const ycsb::config& cfg);
```

映射规则：

- `device.*` -> `db_options.device`。
- `runtime.*` -> `db_options.topology` / `db_options.cache`。
- `maintenance.*` -> `db_options.maintenance.policy`。
- `force_format` -> `db_options.boot = force_format ? force_format : recover`。
- 当前兼容规则保留：`cfg.flush_after_load == true` 时，
  `db_options.maintenance.policy.auto_seal_flush = false`，避免 deterministic manual
  barrier 与 automatic maintenance 同时争用 coord frontier。

后续配置清理再把 `ycsb::config` 拆成：

```text
ycsb::workload_options
ycsb::verification_options
ycsb::output_options
runtime::db_options
```

但这不是 067 第一阶段必须完成的内容。

## Root Submit Allowlist

当前 quality gate 只允许 runtime maintenance scheduler 创建 hidden/root pipeline。
067 后 production root submit allowlist 应调整为：

1. `apps/inconel/runtime/maintenance_scheduler.hh`
   - 后台 maintenance cadence。
2. `apps/inconel/runtime/start_db.hh`
   - app root boundary，由 `start_db` 在 main core 的 on-init hook 中提交。

`apps/inconel/ycsb/**` 不允许再命中：

```bash
rg -n 'pump::sender::submit|make_root_context|the_null_receiver' \
  apps/inconel -g'*.hh' -g'*.cc' -g'!apps/inconel/test/**'
```

实现 067 时必须同步更新 `code_quality_standard.md` 的 allowlist，否则 gate 会与新设计冲突。

## Ownership / Lifetime

### System-owned

- `db_options`: host lifecycle owning config。
- `real_device`: `db_session` 成员，outlive runtime。
- `recovered_boot`: `db_session` 成员，outlive `build_runtime`。
- `rt`: `db_session` raw pointer，构造后由 session destroy，不能泄漏到 app owner。
- `db_session_view`: root context view，不拥有 runtime。
- `maintenance_stats`: session 收集后放入 `db_run_result`。

### App-owned

- `ycsb::run_state`: YCSB owner，继续用 `shared_ptr` 保证 app root callbacks 与
  `rt::start` 返回后的 stats/oracle 读取共享同一对象。
- `phase_stats` 和 `expected_state`: YCSB benchmark/oracle 状态，不进入 runtime。

### Context use

- `db_session_view` 放在 app root context，供 `stop_db()` 获取。
- 不把完整 `db_session` 或 YCSB `run_state` move 进 context 当唯一 owner。
- request/round-local runtime state 继续按现有 write/read/flush pipeline 用 context。

## Failure / Shutdown

正常路径：

```text
user workload
  -> start_db app-root wrapper terminal runtime::stop_db()
  -> run flags cleared
  -> rt::run disables maintenance and waits quiesce
  -> rt::start returns
  -> collect stats
  -> destroy runtime
```

异常路径：

```text
user workload throws
  -> start_db app-root wrapper captures exception_ptr
  -> runtime::stop_db()
  -> rt::start returns
  -> destroy runtime
  -> rethrow on host thread
```

Build/boot failure：

- If device open, format or recovery fails before runtime is built, propagate exception; RAII
  destroys `real_device`.
- If `build_runtime` succeeds and later `rt::start` or app root fails, call
  `destroy_runtime` before rethrow.
- Init failure remains process/application fatal from caller perspective; 067 does not add
  partial-recovery semantics.

## Hot Path Cost

067 affects startup/shutdown and app root setup only.

- Write hot path: 0 new per-request allocation, 0 new copy, 0 new queue hop.
- Point GET hot path: 0 new per-request allocation, 0 new copy, 0 new queue hop.
- `stop_db()` terminal path: one context lookup + one loop over configured cores.
- App root setup: one `make_root_context(db_session_view)` per DB run.
- `db_options` owning vectors/strings are startup-only.

Any implementation that adds per-KV `std::function`, virtual dispatch, per-request config copy,
or per-request owning topology copy violates this design.

## Migration Plan

### Phase 0: Document and freeze API

This document is Phase 0.

Implementation preparation:

- Record `runtime/start.hh` as stale/incomplete relative to current YCSB bring-up.
- Keep `inconel_ycsb` behavior unchanged until Phase 2.

### Phase 1: Add runtime lifecycle API

Add runtime files:

- `runtime/config.hh`
- `runtime/session.hh`
- `runtime/format_device.hh`
- `runtime/stats.hh`
- `runtime/stop_db.hh`
- `runtime/start_db.hh`

Do not migrate YCSB yet. Compile-only usage can be introduced through a small non-YCSB smoke or
temporary internal caller, but do not duplicate production behavior long-term.

### Phase 2: Migrate YCSB

Add:

- `ycsb/runtime_bridge.hh`

Change `ycsb/main.cc`:

- Keep parse/dry-run/effective config output.
- Build `runtime::db_options` through bridge.
- Build YCSB `run_state` / expected oracle as today.
- Call:

```cpp
auto result = runtime::start_db(db_opts)([state] {
    return ycsb::run_workload(state);
});
```

- Print YCSB stats from `state`.
- Print maintenance stats from `result.maintenance`.
- Write expected file from `state`.

Change `ycsb/runner.hh`:

- Remove `stop_runtime`.
- Continue using `rt::write_batch`, `rt::point_get`, `rt::seal_once`, `rt::flush_once`.
- Do not include builder/run/device/recovery headers.

### Phase 3: Clean compatibility remnants

- Delete or shrink `ycsb/format_device.hh` forwarding header after references are gone.
- Update `runtime/start.hh` so there is one canonical lifecycle path.
- Optionally split `ycsb::config` into runtime/workload/verification/output sub-objects.
- Add explicit config field for `maintenance.auto_seal_flush` if operators need to decouple it
  from `workload.flush_after_load`.

## Static Gates

After Phase 2:

```bash
rg -n 'build_runtime<|destroy_runtime<|rt::start\(|real_device|recover_empty_clean_boot|force_format_device' \
  apps/inconel/ycsb
```

Expected:

- No hits in `apps/inconel/ycsb/main.cc`, `runner.hh`, `workload.hh`, `stats.hh`,
  `expected_state.hh`.
- A temporary forwarding `ycsb/format_device.hh` may mention `format_device` only until Phase 3.

```bash
rg -n 'make_root_context\(|pump::sender::submit|the_null_receiver' apps/inconel/ycsb
```

Expected: no hits.

```bash
rg -n 'stop_runtime' apps/inconel
```

Expected: no hits.

Virtual dispatch gate remains unchanged:

```bash
rg -n '\bvirtual\b|\boverride\b' apps/inconel --glob '!**/test*' --glob '!**/*test*'
```

Expected: no new hits.

## Build and Behavior Gates

Minimum build gate:

```bash
cmake --build build_real --target \
  inconel_ycsb \
  inconel_real_nvme_compile_check \
  inconel_test_m11_runtime_topology_operations \
  inconel_test_recovery_boot_ram_device \
  -j2
```

Dry-run compatibility:

```bash
build_real/inconel_ycsb \
  --config apps/inconel/ycsb/config.sample.json \
  --dry-run --dump-config --no-print-config
```

Expected: canonical JSON equivalent to pre-067 output for the same config.

Real NVMe safety preflight:

```bash
sudo -n /home/null/work/kv/spdk/scripts/setup.sh status
```

Expected:

- Scratch device is `0000:04:00.0`.
- System disk `0000:03:00.0` is not used.

Behavior smoke:

```bash
apps/inconel/scripts/ycsb_consistency.sh a0
```

Stronger regression set:

```bash
apps/inconel/scripts/ycsb_consistency.sh c1
apps/inconel/scripts/ycsb_consistency.sh c7
apps/inconel/scripts/ycsb_consistency.sh c8
```

These cover basic real-NVMe operation, concurrency, and recovery continuation without claiming
new crash-point coverage beyond 066D.

## Review Checklist

Implementation review must answer:

1. Does `apps/inconel/ycsb` still include any builder/run/device/recovery header?
2. Is there exactly one app root submit boundary, in `runtime/start_db.hh`?
3. Does app-root exception handling always request stop before `rt::start` waits forever?
4. Does `destroy_runtime` run on every path after runtime is built?
5. Does `recovered_boot` outlive `build_runtime` inputs?
6. Does `db_session_view` remain a view, not an owner?
7. Are cache policy branches still compile-time template dispatch, not virtual?
8. Did the change add any per-request allocation/copy/queue hop? Expected answer: no.
9. Does `flush_after_load` compatibility still disable automatic seal/flush exactly as before?
10. Did `inconel_ycsb` CLI/JSON/output remain stable?

## Adjacent Work

Best done with or immediately after 067:

- Move current bootstrap format helper out of YCSB. This is a boundary correction and directly
  relates to INC-035.
- Update `code_quality_standard.md` root submit allowlist for `runtime/start_db.hh`.
- Mark `runtime/start.hh` as compatibility/legacy or rewrite it to delegate to `start_db`.

Should not be mixed into 067:

- INC-057 / WAL small batch group commit.
- RSM §2.7 pre-LSN backpressure.
- Full production `format_disk()` beyond moving the current bootstrap helper.
- New KV API surfaces such as MultiGet/Scan.
- YCSB distribution expansion or performance benchmark methodology changes.
