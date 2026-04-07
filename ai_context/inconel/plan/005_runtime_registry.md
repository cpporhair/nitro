# 005 — 全局运行期对象与 Scheduler 注册表

> 实现第五步。建立统一的 scheduler 访问机制，杜绝各 scheduler 各自约定如何找别的 scheduler。

## 文件结构

```
core/
└── registry.hh        — 非模板的全局 scheduler 注册表 + 路由 helpers

runtime/
├── builder.hh         — inconel_runtime_t<Cache> 别名 + build_runtime<Cache>() 工厂
└── start.hh           — start_runtime() 入口（按 cache_policy 字符串一次分支）

tree/
└── sender.hh          — lookup() 去掉显式 nvme 参数，改用 registry::local_nvme()

mock_nvme/
└── (不变)

test/
└── test_runtime.cc    — 注册表填充 + 多核 PUMP run() 端到端
```

## 设计目标

设计文档（CLAUDE.md `core/`）明确要求：

> 各类 scheduler 实例列表 | `by_core[]` 数组 + `list` vector，同 KV 的 `scheduler_objects.hh` 模式
> route_to_front(key_hash), route_to_tree_lookup(front_owner), local_nvme(), singleton 访问

核心目标：**统一 scheduler 访问方式**，未来加 scheduler 是往注册表加字段，不是发明新的访问机制。

## 选型

**用 PUMP `global_runtime_t<...>` 而非 KV 的裸全局变量风格。**

理由：
- PUMP 框架的 `share_nothing.hh` / `start()` / `run()` 是更新的多核 runtime 设施，sider 在用
- 直接复用 cpu affinity、advance loop、preemptive、shutdown 等基础设施
- 类型安全：编译期 tuple 而非运行时全局变量集合

但 PUMP runtime 是模板化的（`global_runtime_t<schedA, schedB, ...>`），而 `lookup_scheduler<Cache>` 又模板化在 Cache 上，会让"全局指针"变得棘手——全局变量没法存模板化指针。

## 双层注册架构

**两个并行的结构，各管各的：**

1. **PUMP `inconel_runtime_t<Cache>`** ──── 框架内部，跑 advance loop 用
   - 模板化在完整 scheduler 类型列表上
   - 知道 derived 类型，能调 `lookup_scheduler<Cache>::advance()`
   - 仅在 builder 内构造，仅传给 `pump::env::runtime::run/start`
   - **应用代码看不到这个类型**

2. **`core::registry`** ──── 应用注册表，非模板
   - 只存基类指针：`mock_nvme::scheduler*`、`tree::lookup_scheduler_base*`
   - 应用代码（pipeline、handler 等）只通过它访问 scheduler
   - 所有 scheduler kind 在这里集中声明，未来加 scheduler 改这里

`build_runtime<Cache>()` 构造 scheduler 时同时往两边写入，之后两边各管各的：

```text
build_runtime<Cache>(opts):
  pump_rt = new inconel_runtime_t<Cache>()
  for core in opts.cores:
    pump::core::this_core_id = core   // per_core::queue 必须知道
    nvme    = new mock_nvme::scheduler(opts.device)
    tlookup = new lookup_scheduler<Cache>(Cache(opts.cache_capacity))
    pump_rt->add_core_schedulers(core, nvme, tlookup)         // PUMP
    registry::nvme_scheds.list.push_back(nvme)                // 应用注册表
    registry::nvme_scheds.by_core[core] = nvme
    registry::tree_lookup_scheds.list.push_back(tlookup)      //（base 上转）
    registry::tree_lookup_scheds.by_core[core] = tlookup
  return pump_rt
```

## core/registry.hh 细节

```cpp
namespace apps::inconel::core::registry {

    // ── Per-instance scheduler lists ──
    struct nvme_list {
        std::vector<mock_nvme::scheduler*> list;     // 实际实例
        std::vector<mock_nvme::scheduler*> by_core;  // core_id 索引，未配置 = nullptr
    };
    inline nvme_list nvme_scheds;

    struct tree_lookup_list {
        std::vector<tree::lookup_scheduler_base*> list;
        std::vector<tree::lookup_scheduler_base*> by_core;
    };
    inline tree_lookup_list tree_lookup_scheds;

    // ── 未来 scheduler slot（注释占位）──
    // inline coord::scheduler*       coord_sched       = nullptr;
    // inline tree::scheduler*        tree_sched        = nullptr;
    // inline wal::scheduler*         wal_space_sched   = nullptr;
    // inline value::scheduler*       value_alloc_sched = nullptr;
    // struct front_list { ... };  inline front_list front_scheds;

    // ── helpers ──
    inline mock_nvme::scheduler*           local_nvme();        // 当前核
    inline tree::lookup_scheduler_base*    local_tree_lookup(); // 当前核
    inline mock_nvme::scheduler*           nvme_for_core(uint32_t);
    inline tree::lookup_scheduler_base*    tree_lookup_for_core(uint32_t);
    inline tree::lookup_scheduler_base*    tree_lookup_at(uint32_t idx);
    inline uint32_t tree_lookup_count();
    inline uint32_t nvme_count();

    // ── 未来路由 helper（注释占位）──
    // inline front::scheduler* route_to_front(uint64_t key_hash);
    // inline tree::lookup_scheduler_base* home_tree_lookup_for_front(uint32_t owner);

    inline void init_capacity(uint32_t max_cores);
    inline void clear();
}
```

`local_nvme()` 用 `pump::core::this_core_id`（worker 线程在 PUMP run() 入口设置）索引 by_core，每个 worker 线程访问到的是它自己核心的 nvme，零开销。

## runtime/builder.hh

```cpp
template <core::cache_concept Cache>
using inconel_runtime_t = pump::env::runtime::global_runtime_t<
    mock_nvme::scheduler,
    tree::lookup_scheduler<Cache>
    // 未来：coord::scheduler, front::scheduler, tree::scheduler, ...
>;

struct build_options {
    std::span<const uint32_t> cores;
    mock_nvme::mock_device*   device;
    uint32_t                  cache_capacity = 32;
};

template <core::cache_concept Cache>
inline inconel_runtime_t<Cache>* build_runtime(const build_options& opts);

template <core::cache_concept Cache>
inline void destroy_runtime(inconel_runtime_t<Cache>* rt);
```

## runtime/start.hh

```cpp
struct start_options {
    std::string_view          cache_policy;     // "clock" | "slru"
    uint32_t                  cache_capacity = 32;
    std::span<const uint32_t> cores;
    uint32_t                  main_core = 0;
    mock_nvme::mock_device*   device;
};

inline void start_runtime(const start_options& opts);

template <core::cache_concept Cache>
inline void run_with(const start_options& opts);
```

`start_runtime` 内部一次分支选 Cache 类型，调 `run_with<Cache>` → `build_runtime<Cache>` → `pump::env::runtime::start()`。**Cache 模板参数仅在这条分支链上出现**，应用代码完全看不到。

## sender 改造：local_nvme 替代显式参数

之前 `tree::lookup()` 的签名：

```cpp
template<typename nvme_sched_t, typename key_range_t>
inline auto lookup(lookup_scheduler_base* tree_sched,
                   nvme_sched_t* nvme,            // 显式传入
                   key_range_t&& keys, ...);
```

**问题**：调用方在**提交线程**捕获 nvme 指针。当 `tree_sched->process()` 的 callback 在 home core（比如 core Y）上 fire 时，pipeline 继续在 Y 上跑，但用的是提交线程那一核的 nvme 指针——跨核了，违反 share-nothing。

**改造后**：

```cpp
template<typename key_range_t>
inline auto lookup(lookup_scheduler_base* tree_sched,
                   key_range_t&& keys,
                   const core::tree_manifest* manifest);

inline auto on_decision_need_read(lookup_scheduler_base* tree_sched, decision_need_read&& dec) {
    // ...
    >> flat_map([](decision_need_read& ctx, size_t i) {
        auto* nvme = core::registry::local_nvme();   // ← 在执行核解析
        return nvme->read(...);
    })
```

`local_nvme()` 在 worker 线程执行 `on_decision_need_read` 内 lambda 时调用，那时 `pump::core::this_core_id` 已经是 worker 自己核的 id，所以拿到的是本地 nvme。

## 关键不变量

1. **应用代码只看基类**：`local_nvme()`、`local_tree_lookup()` 返回基类指针，pipeline 永远不知道 Cache 类型
2. **统一访问入口**：所有 scheduler 通过 `core::registry::xxx` 访问，禁止应用代码自行存 scheduler 指针绕过注册表
3. **模板隔离在 builder**：`<Cache>` 只在 `build_runtime<Cache>` 和 `run_with<Cache>` 这两个函数模板里出现
4. **PUMP runtime 类型纯内部**：`inconel_runtime_t<Cache>` 只在 builder 内构造和传给 PUMP，应用代码看不到
5. **零运行时开销**：基类指针调用是非虚函数（详见 step 4 文档），thread_local 是 fs/gs 段访问，全部内联

## Per-core placement 支持

PUMP `add_core_schedulers(core, sched1, sched2, ...)` 接受 nullptr 占位。advance loop 跳过 nullptr：

```cpp
// share_nothing.hh
std::apply([](auto*... sche) {
    if (!(... | (sche ? advance_one(sche) : false)))
        std::this_thread::yield();
}, runtime->schedulers_by_core[core]);
```

这意味着：**编译期类型集合 ⊇ 运行期实际配置**，每个 core 只跑它有的 scheduler，未来的拓扑（例如 ingest core 跑 front+nvme+tree_lookup，background core 跑 tree+value+nvme，单例 core 跑 coord）天然支持。当前 step 5 的 builder 硬编码"每个 core 都有 nvme + tree_lookup"，配置侧的灵活性留待未来加配置文件时再展开。

## 未来加 scheduler 的固定流程

每加一个新 scheduler kind，三处改动：

1. `core/registry.hh` 加全局指针（singleton）或 list（per-instance）字段，加访问 helper
2. `runtime/builder.hh` 加构造代码 + 双向注册（PUMP runtime + registry）
3. `inconel_runtime_t<Cache>` 模板参数列表加新类型

不需要发明新的访问方式，不需要修改其他 scheduler 的代码。

## 验证

`test_runtime.cc`（4 个测试）：

1. **registry population (clock + slru)**：`build_runtime<Cache>` 后验证：
   - `nvme_count()` / `tree_lookup_count()` 等于配置的 core 数
   - `by_core[c]` 对配置 core 非空，对未配置 core 为 nullptr
   - PUMP runtime tuple 中的实例与 registry 中的相同（向上转换后比较）
   - clean shutdown 通过 `destroy_runtime`

2. **e2e multi-core via runtime (clock + slru)**：
   - `build_runtime<Cache>(cores={2,4})` 构造完整 runtime
   - 用 `pump::env::runtime::run(rt, core, init)` 在 jthread 里跑 worker（PUMP 标准入口）
   - 主线程发起 400 个独立 lookup，按 shard 分发到 tree_lookup_at(i % shards)
   - 验证 400/400 结果正确
   - 清通过 `is_running_by_core[core].store(false)` 关 worker

### 全部回归

- `inconel_test_page_cache` — 11/11 通过
- `inconel_test_tree_lookup` — 7 cases 通过（test_env / test_cache_eviction / test_empty_tree 都注册了 registry）
- `inconel_test_tree_lookup_multicore` — 400 + 100 通过（注册两个 core 的 nvme/tree_lookup）
- `inconel_test_runtime` — 4 cases 通过

## 已知限制 / 后续工作

- `core::registry` 在 `core/`，但 `tree::sender` 反向依赖它，模块层级倒挂。后续可以拆出 `core/scheduler_handles.hh` 之类的零依赖 helper 文件让 sender 引用，避免 sender 直接依赖整个 registry
- `start_runtime` 的 `start_options` 还没有正式 config 文件，由调用方手工填
- placement 配置（哪个 core 跑哪些 scheduler）目前硬编码"每核都有 nvme+tree_lookup"，等更多 scheduler 实现后再补 config 文件解析
