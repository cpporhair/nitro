# 007 — Value Cache Integration

> 实现第七步。把 step 6 的 `readonly_cache_`（无界 `std::map<paddr, vector<char>>`）替换为 step 4 的 `core::cache_concept`（clock / slru），同时把 value 模块所有 page image 从 `std::vector<char>` 全路径迁移到 `std::unique_ptr<char[]> + uint32_t image_size`，runtime 类型加双 Cache 模板参数。

## 文件结构

```
core/
├── page_cache.hh          — cache_concept 加 evict_one() → optional<evicted_entry>
├── clock_cache.hh         — 加 evict_one() 实现
└── slru_cache.hh          — 加 evict_one() 实现

value/
├── allocator.hh           — 删除 cls.open / install_open_page / close_open_page / value_open_page_meta；acquire_page 缩短为 whole_pool→bump；value_page_source 枚举去掉 open_frame，ready_page → writable
├── scheduler.hh           — 大改：拆 base/template + page_data char[] + readonly_cache 模板化 + 合并 open/ready → writable_pages_
└── sender.hh              — read_miss 加 admit_to_cache + buf 改 unique_ptr<char[]>

runtime/
├── builder.hh             — inconel_runtime_t<TreeCache, ValueCache>
└── start.hh               — start_options 双 cache 字段 + 4 种组合分支

test/
├── test_value.cc          — 适配 build_options 双 cache + 加 cache_eviction case
└── test_tree_value.cc     — 适配 build_options 双 cache + 加 cache_eviction case
```

## 设计决策

| # | 决策点 | 结果 | 取舍论证 |
|---|---|---|---|
| **D1** | cache 服务范围 | 只服务 1-LBA 页（sub-LBA + LBA-equal），multi-LBA bypass | cache 的核心价值是消除 hole-fill 的 read-modify-write，不是通用读优化。multi-LBA 大 value 流式访问命中率低；bypass 让 buf 池单一 lba_size 尺寸 |
| **D2** | tree/value Cache 模板 | 独立 — `runtime_t<TreeCache, ValueCache>` | tree/value 工作集和访问模式不同，未来可能 tree 用 clock、value 用 slru。配置拆开是对的 |
| **D3** | buf ownership | 直接 `make_unique<char[]>` + cache 接管 raw ptr，cache_concept 加 `evict_one()` | 跟 DMA pool 迁移路径完全一致，DMA 时只换 alloc/free 函数（3 处替换），无 owned_bufs_/free_bufs_ 中间层要拆 |
| **D4** | writable path 类型 | 全路径迁移 `unique_ptr<char[]>` + `image_size`，commit 零 copy | 类型统一，避免 vector / char[] 混用被后续代码模仿扩散（feedback_layered_complete_not_prototype）。memcpy 1.5% CPU 是可接受的，但分层稳固优先 |
| **D5** | decode helper 签名 | `std::span<const char>` | `format::decode_value_object` 已经接 `span<const char>`，无 wrap；边界检查内置；C++20 风格统一 |
| **D6** | writable 页结构合并 | 合并 `open_pages_` + `ready_pages_` → 单一 `writable_pages_[ci]`，allocator 去掉 active open page 协议 | step 6 的 1+N 二分（per-class 1 个 active open + N 个 ready 备用）只是因为 `value_allocator::cls.open` 字段强制 1 个 active 才被迫拆开。allocator 是纯 placement decision 模块，不该跟踪 in-flight image 状态。合并后 allocator 接口更窄、scheduler 数据结构更直接、handle_read 路径更短 |

**术语澄清**：本 plan 全文使用 **"writable path"** 描述 `writable_pages_[ci]` 这套"已 durable 在 NVMe，但 in-memory image 仍被 scheduler 持有、等待下次同 class 写入复用"的页集合。**不使用 "dirty"**——这些页的 NVMe 内容在两个 round 之间是已 durable 的，"dirty" 容易让人误解成"未持久化"。design_doc §5.3-5.4 用 `dirty_open_frame` 描述同一概念，那个是"可能被脏化的 open frame"，本 plan 为避免歧义统一用 "writable"。
真正"未持久化"的页只存在于 round 期间的 `round_page.image` 字段（`handle_persist` → `handle_finalize` 之间）。

## cache_concept 扩展

```cpp
template <typename C>
concept cache_concept = requires(C c, const C cc, paddr k, char* b) {
    { c.get(k) }       -> std::same_as<const char*>;
    { c.put(k, b) }    -> std::same_as<std::optional<evicted_entry>>;
    { cc.contains(k) } -> std::same_as<bool>;
    { cc.size() }      -> std::same_as<uint32_t>;
    { cc.capacity() }  -> std::same_as<uint32_t>;
    { c.evict_one() }  -> std::same_as<std::optional<evicted_entry>>;   // ★ 新增
};
```

`evict_one()` 用于 cache 持有 buf 的场景下，scheduler 析构时归还所有 buf：

```cpp
~scheduler() {
    while (auto e = readonly_cache_.evict_one()) delete[] e->buf;
}
```

**实现要点**：
- `clock_cache::evict_one`：扫一圈找第一个 occupied slot，移除该 slot + erase index_ + size_-- + 返回 evicted_entry
- `slru_cache::evict_one`：先 prob 尾部移除，没有再 prot 尾部；归还 free node list

不影响 tree_lookup —— hot path 仍然只用 get/put/contains。`evict_one` 只在 scheduler 析构时调用。

## scheduler 拆分（base/template）

参考 step 4 tree_lookup 的拆分模式：

```
value::scheduler_base                            ← 非模板，op/sender 持有
├── per_core::queue<_value_persist::req*>
├── per_core::queue<_value_finalize::req*>
├── per_core::queue<_value_read::req*>
├── per_core::queue<_value_fill::req*>
├── schedule_persist / schedule_finalize / schedule_read / schedule_fill
└── prepare_persist / finalize_persist / prepare_read / fill_and_decode 工厂

  ↓ public 单继承（无虚函数）

value::scheduler<Cache>                          ← 模板，所有 hot path
├── value_allocator alloc_
├── vector<vector<page_data>> writable_pages_    ← per class 的 writable 池（合并 open+ready）
├── Cache readonly_cache_                        ← 直接成员
├── map<round_id, unique_ptr<round>> inflight_rounds_
├── advance / handle_persist / handle_finalize
├── handle_read / handle_fill
├── persist_one_entry / acquire_round_page
├── commit_pages / rollback_pages
├── try_decode_value / serve_hit_or_fail (helpers)
└── ~scheduler() — evict_one 清 cache
```

**writable_pages_ 数据结构**（决策 D6）：

```cpp
std::vector<std::vector<page_data>> writable_pages_;  // index 按 class_idx
```

每个 class 一个 page_data 队列，保存当前所有"已 durable 但未满"的页。`acquire_round_page` 优先从这里 pop_back（最近 install 的最先复用），耗尽后向 allocator 要 fresh 页。`handle_read` hit 路径线性扫这个队列（实测 N 通常很小）。

**对应 allocator 改造**：

`value_allocator` 不再持有 `cls.open` 字段，不再跟踪 active open page。allocator 只管:
- `whole_pool[ci]`：完全空闲的页池
- `bump_head`：fresh 页源
- 删除 `install_open_page` / `close_open_page` / `value_open_page_meta`
- `acquire_page` 决策树缩短为：whole_pool → bump → fail
- `value_page_source` 枚举去掉 `open_frame`，`ready_page` 改名 `writable`

op/sender 持 `scheduler_base*`，所有 cache 相关代码在派生类内，编译期完全内联。

## page_data / round_page 类型迁移

```cpp
// 之前
struct page_data {
    paddr             base;
    uint64_t          free_mask;
    std::vector<char> image;
};

// 之后
struct page_data {
    paddr                   base;
    uint64_t                free_mask;
    std::unique_ptr<char[]> image;
    uint32_t                image_size;     // ← 新增，因为 char[] 不带 size
};
```

`round_page` 同样改造（去掉 image 字段名重复声明）。

**writable path 内 image 访问适配**：
- `image.data()` → `image.get()`
- `image.size()` → `image_size`
- `image.empty()` → `!image`
- 构造：`vector<char>(N, 0)` → `make_unique<char[]>(N)`（value-init 全 0）

## 写路径改造

### `acquire_round_page`（合并 D6 后）

```cpp
round_page* acquire_round_page(round& rnd, uint16_t ci) {
    // 1. 从 writable_pages_[ci] 取（最近 install 的最先复用，pop_back LIFO）
    if (!writable_pages_[ci].empty()) {
        auto pd = std::move(writable_pages_[ci].back());
        writable_pages_[ci].pop_back();
        rnd.pages.push_back(round_page{
            .base = pd.base,
            .class_idx = ci,
            .span_lbas = alloc_.span_lbas(ci),
            .source = value_page_source::writable,
            .original_free_mask = pd.free_mask,
            .free_mask = pd.free_mask,
            .image = std::move(pd.image),
            .image_size = pd.image_size,
        });
        return &rnd.pages.back();
    }

    // 2. 找 allocator 要 fresh / whole_pool 页
    auto ar = alloc_.acquire_page(ci);
    if (!ar) return nullptr;

    uint32_t img_bytes = ar->span_lbas * alloc_.lba_size();
    auto image = std::make_unique<char[]>(img_bytes);   // value-init 全 0
    rnd.pages.push_back(round_page{
        .base = ar->page_base,
        .class_idx = ci,
        .span_lbas = ar->span_lbas,
        .source = ar->source,                           // whole_page 或 fresh_bump
        .original_free_mask = ar->free_mask,
        .free_mask = ar->free_mask,
        .image = std::move(image),
        .image_size = img_bytes,
    });
    return &rnd.pages.back();
}
```

跟 step 6 比少一个分支（原来是 open_pages_ → ready_pages_ → allocator 三步），allocator 接口也变窄（不再调 close_open_page）。

### `install_writable_page` 签名 + 简化实现

```cpp
void install_writable_page(uint16_t ci, paddr base, uint64_t free_mask,
                           std::unique_ptr<char[]>&& image, uint32_t image_size) {
    writable_pages_[ci].push_back(page_data{
        .base       = base,
        .free_mask  = free_mask,
        .image      = std::move(image),
        .image_size = image_size,
    });
    // 不再调 alloc_.install_open_page —— allocator 不知道 writable 页的存在
}
```

step 6 的版本要分支判断 open_pages_ 是否已占来决定进 open 还是 ready，合并后没这个区分。

### `commit_pages`（核心改动）

```cpp
void commit_pages(round& rnd) {
    for (auto& page : rnd.pages) {
        if (page.free_mask == 0) {
            // 满页 → release 给 cache，零 copy
            char* raw = page.image.release();
            if (auto evicted = readonly_cache_.put(page.base, raw)) {
                delete[] evicted->buf;     // 将来：spdk_mempool_put
            }
        } else {
            // 未满 → 装回 writable_pages_[ci]
            install_writable_page(page.class_idx, page.base, page.free_mask,
                                  std::move(page.image), page.image_size);
        }
    }
}
```

### `rollback_pages`

类型适配：`std::move(page.image)` 是 unique_ptr move，跟之前 vector move 同形态。

## 读路径改造

### `handle_read`（按 span_lbas 分两条路径）

```cpp
void handle_read(_value_read::req* item) {
    auto ci_opt = class_for_len(item->vr.len);
    if (!ci_opt) { item->fail(...); delete item; return; }
    uint16_t ci = *ci_opt;
    uint32_t span = alloc_.span_lbas(ci);
    bool admit = (span == 1);            // 决策 D1: 只 1-LBA 进 cache

    // 1. writable_pages_[ci] (已 durable 但未满的可写页池，决策 D6)
    for (auto& pd : writable_pages_[ci]) {
        if (pd.base == item->vr.base) {
            serve_hit_or_fail(item,
                std::span(pd.image.get(), pd.image_size),
                "writable");
            delete item; return;
        }
    }

    // 2. readonly_cache_ — 仅 1-LBA admit
    if (admit) {
        if (const char* p = readonly_cache_.get(item->vr.base)) {
            serve_hit_or_fail(item, std::span(p, alloc_.lba_size()), "readonly_cache");
            delete item; return;
        }
    }

    // 3. miss → 分配 buf，告诉 pipeline
    uint32_t img_bytes = span * alloc_.lba_size();
    auto buf = std::make_unique<char[]>(img_bytes);
    item->cb(read_prepare_result{
        read_miss{item->vr.base, span, std::move(buf), img_bytes, admit}
    });
    delete item;
}
```

### `handle_fill`（admit / bypass 分支）

```cpp
void handle_fill(_value_fill::req* item) {
    auto body_opt = try_decode_value(
        std::span(item->buf.get(), item->buf_size), item->vr);
    if (!body_opt) {
        item->fail(std::make_exception_ptr(std::runtime_error(
            "value::read: corrupt value object on disk (post-NVMe)")));
        delete item; return;
    }

    if (item->admit_to_cache) {
        // 1-LBA → release 给 cache
        char* raw = item->buf.release();
        if (auto evicted = readonly_cache_.put(item->vr.base, raw)) {
            delete[] evicted->buf;
        }
    }
    // multi-LBA bypass：item->buf 析构时 unique_ptr 自动 free

    item->cb(std::move(*body_opt));
    delete item;
}
```

### helpers（span 签名）

```cpp
std::optional<std::string>
try_decode_value(std::span<const char> image, value_ref vr) const {
    if (vr.byte_offset >= image.size()) return std::nullopt;
    auto slot = image.subspan(vr.byte_offset);
    auto d = format::decode_value_object(slot, vr.len);
    if (!d.ok()) return std::nullopt;
    return std::string(d.body.data(), d.body.size());
}

void serve_hit_or_fail(_value_read::req* item,
                       std::span<const char> image,
                       const char* source_label) {
    auto body = try_decode_value(image, item->vr);
    if (!body) {
        std::string msg = "value::read: corrupt value object in ";
        msg += source_label;
        item->fail(std::make_exception_ptr(std::runtime_error(msg)));
        return;
    }
    item->cb(read_prepare_result{ read_hit{ std::move(*body) } });
}
```

## req / variant 类型变化

```cpp
struct read_miss {
    paddr                   base;
    uint32_t                span_lbas;
    std::unique_ptr<char[]> buf;
    uint32_t                buf_size;
    bool                    admit_to_cache;     // ★ 新增
};

namespace _value_fill {
    struct req {
        value_ref               vr;
        std::unique_ptr<char[]> buf;            // 之前 shared_ptr<vector<char>>
        uint32_t                buf_size;       // ★ 新增
        bool                    admit_to_cache; // ★ 新增
        std::move_only_function<void(std::string&&)>      cb;
        std::move_only_function<void(std::exception_ptr)> fail;
    };
}
```

`fill_and_decode` 工厂签名同步：

```cpp
auto fill_and_decode(value_ref vr, std::unique_ptr<char[]> buf,
                     uint32_t buf_size, bool admit_to_cache) { ... }
```

## sender.hh 适配

`on_read_miss` 用 `with_context` 模式（已经在用，保留），buf 类型从 shared_ptr 换 unique_ptr，访问点改 `.get()`：

```cpp
inline auto
on_read_miss(scheduler_base* sched, value_ref vr, read_miss&& alt) {
    return just()
    >> with_context(__fwd__(alt), vr)([sched]() {
        return get_context<read_miss>()
            >> flat_map([](const read_miss &rm) {
                return core::registry::local_nvme()->read(
                    rm.base.lba, rm.buf.get(), rm.span_lbas);
            })
            >> false_to_exception(std::runtime_error(
                "value::read_value: NVMe read failed"))
            >> get_context<read_miss, value_ref>()
            >> flat_map([sched](read_miss &rm, const value_ref &vr, bool) mutable {
                return sched->fill_and_decode(vr, std::move(rm.buf),
                                              rm.buf_size, rm.admit_to_cache);
            });
    });
}
```

注意 sender 持 `scheduler_base*`，跟拆 base 后保持一致。

## runtime 改造

### `inconel_runtime_t` 双模板参数

```cpp
template <core::cache_concept TreeCache, core::cache_concept ValueCache>
using inconel_runtime_t = pump::env::runtime::global_runtime_t<
    mock_nvme::scheduler,
    tree::lookup_scheduler<TreeCache>,
    value::scheduler<ValueCache>
>;
```

### `build_options`

```cpp
struct build_options {
    std::span<const uint32_t> cores;
    mock_nvme::mock_device*   device;

    // tree cache
    uint32_t                  tree_cache_capacity = 32;

    // value cache + scheduler config
    std::span<const uint32_t> value_class_sizes;
    uint32_t                  lba_size = 4096;
    format::paddr             value_data_area_base = {0, 0};
    format::paddr             value_data_area_end  = {0, 0};
    uint32_t                  value_cache_capacity = 32;
};
```

旧字段 `cache_capacity` 改名 `tree_cache_capacity`，加 `value_cache_capacity`。

### `build_runtime` 模板参数

```cpp
template <core::cache_concept TreeCache, core::cache_concept ValueCache>
inline inconel_runtime_t<TreeCache, ValueCache>*
build_runtime(const build_options& opts) { ... }

template <core::cache_concept TreeCache, core::cache_concept ValueCache>
inline void
destroy_runtime(inconel_runtime_t<TreeCache, ValueCache>* rt) { ... }
```

构造 value_sched 时传 `ValueCache(opts.value_cache_capacity)`：

```cpp
value_sched = new value::scheduler<ValueCache>(
    opts.value_class_sizes, opts.lba_size,
    opts.value_data_area_base, opts.value_data_area_end,
    ValueCache(opts.value_cache_capacity));
```

### `start_options` + `start_runtime`

```cpp
struct start_options {
    std::string_view tree_cache_policy;     // "clock" | "slru"
    std::string_view value_cache_policy;    // "clock" | "slru"
    uint32_t         tree_cache_capacity = 32;
    uint32_t         value_cache_capacity = 32;
    std::span<const uint32_t> cores;
    uint32_t         main_core = 0;
    mock_nvme::mock_device*   device;
    // value scheduler 配置
    std::span<const uint32_t> value_class_sizes;
    uint32_t                  lba_size = 4096;
    format::paddr             value_data_area_base;
    format::paddr             value_data_area_end;
};

template <core::cache_concept TreeCache, core::cache_concept ValueCache>
inline void run_with(const start_options& opts) {
    build_options bopts{ ... };  // 把 opts 字段拷过去
    auto* rt = build_runtime<TreeCache, ValueCache>(bopts);
    pump::env::runtime::start(rt, opts.cores, opts.main_core, ...);
    destroy_runtime<TreeCache, ValueCache>(rt);
}

template <core::cache_concept TreeCache>
inline void start_with_tree(const start_options& opts) {
    if      (opts.value_cache_policy == "clock") run_with<TreeCache, core::clock_cache>(opts);
    else if (opts.value_cache_policy == "slru")  run_with<TreeCache, core::slru_cache>(opts);
    else throw std::invalid_argument("unknown value_cache_policy");
}

inline void start_runtime(const start_options& opts) {
    if      (opts.tree_cache_policy == "clock") start_with_tree<core::clock_cache>(opts);
    else if (opts.tree_cache_policy == "slru")  start_with_tree<core::slru_cache>(opts);
    else throw std::invalid_argument("unknown tree_cache_policy");
}
```

两层模板嵌套展开 4 种组合，比 4 个 if/else 平铺更清晰。

## 测试改造

### `test_value.cc`

- `build_options` 适配新字段
- 加 **case_7_cache_evict**：
  - 用极小 `value_cache_capacity = 2`
  - 写 5 个 distinct sub-LBA value，落到 5 个 distinct paddr
  - 再读这 5 个 vr，验证最早的 3 个走 NVMe（read_count 涨），最近的 2 个 cache 命中

### `test_tree_value.cc`

- `build_options` 适配
- 加 **case_5_cache_eviction_isolation**：
  - 极小 `value_cache_capacity = 2`
  - 通过 tree lookup → read_value 读 5 个不同 vr
  - 验证 NVMe read count 反映出 cache miss 模式
  - 析构 test_env，验证 scheduler::~scheduler 通过 evict_one 把所有 cache buf 都 delete[] 掉（无 leak —— ASAN 因为 share_nothing 问题不跑，靠 valgrind 单次手动验证）

### `test_page_cache.cc`

- 加 **evict_one 单元测试**（clock + slru 各跑）：
  - put N 个 → 反复 evict_one() 直到返回 nullopt → 验证返回 N 次 + 最后一次 size==0

## 验证

### 全套回归（Release + Debug）

| target | 期望 | 实测 (Debug + Release) |
|---|---|---|
| inconel_tests | step_01 7/7 通过 | 7/7 ✓ |
| inconel_test_page_cache | 原有全过 + evict_one 单元测试 (clock + slru) | 11 + 4 = 15/15 ✓ |
| inconel_test_runtime | clock+slru e2e 通过 | 400+400 ✓ |
| inconel_test_tree_lookup | 单 + 缓存压测全过 | 745/654 NVMe reads ✓ |
| inconel_test_tree_lookup_multicore | 400+100 通过 | 400+100 ✓ |
| inconel_test_value | 原 6 + cache_evict case | 7/7 ✓（case_7: cap=2 5 reads → 3 NVMe = 2 hits + 3 misses）|
| inconel_test_tree_value | 原 4 + cache_eviction_isolation case | 5/5 ✓（case_5: round1=5 reads, round2=+5 misses, cap=2 forces ≥3）|

cache 路径回归数字与 step 6 完全一致(case_2 2 reads / case_3 1 read / case_4 0 reads / case_3_corrupt 1→2 reads / case_4_nvme_fail 0 reads),证明 step 7 双 Cache 模板拆分对运行时行为零影响。

ASAN 路径见 §实测结果 — `build_asan/inconel_test_page_cache` 和 `build_asan/inconel_test_tree_value` 完全 clean,`build_asan/inconel_test_value` 因 share_nothing 不 drain in-flight pipeline 残留 72 个 leak,但全部源于 worker 中断时 sender pipeline 未释放,与 cache 实现无关(双对照实验证明)。

## 跳过功能（v7 不实现 — 留到后续）

| 功能 | 跳过原因 |
|---|---|
| DMA pool / spdk_mempool | 等 SPDK 真接入时统一改 alloc/free 函数（3 处 + cache_evict_one 的 callback），形态一致 |
| `value_page_frame { free_bitmap, mode }` 完整 frame 模型 | design_doc §5.5 的高级 frame 抽象，v7 用 page_data 简化版 |
| `frame.pin_count` | scheduler 是 cache 唯一 owner，v7 不需要跨 shard pin |
| multi-LBA cache | 决策 D1：bypass，命中率低不值得占容量 |
| cross-shard cache invalidate | design_doc 明确 value_alloc_sched 是 cache 唯一 owner，v1 设计就不需要 |
| `out_vr` 主动失效（rollback 后） | step 6 已知缺口 #1，跟本 step cache 无关 |

## 已知缺口

1. **DMA pool 未接入**：buf 仍是 heap 分配（`make_unique<char[]>` / `delete[]`）。DMA 阶段的迁移是 3 处函数名替换 + 加一个 `dma_pool*` 字段
2. **multi-LBA 每次 miss 重新 alloc**：因为 bypass cache，每次 multi-LBA read 都 alloc 一份 unique_ptr<char[]>。multi-LBA 罕见 → acceptable
3. **writable page 不进 cache**：design_doc §6.5 说 read 路径"先查 dirty open frames" —— v7 已经满足（handle_read 第 1 步线性扫 `writable_pages_[ci]`，所有已 durable 但未满的页都在这一层覆盖）
4. **NVMe write 失败的端到端测试**：跟 step 6 一样未覆盖（mock_nvme 没注入 IO 失败接口）。需要 step 8+ 加注入接口
5. **cache_eviction 的真实 leak 检测**：valgrind 在 Garuda Linux 上不可用(stripped ld-linux.so 缺 memcmp redirection,需要 `glibc-debug` debuginfo 包,Arch 系不预装)。改用 `build_asan` 的 ASAN+LSAN 等价覆盖,实测结论:cache 实现本身在两个测试运行时 + 析构都零 leak;`inconel_test_value` 残留的 72 个 leak 全部源于 share_nothing 硬停中断 in-flight sender pipeline,通过 `inconel_test_tree_value`(manual `advance_until` loop)双对照实验证明跟 cache 路径无关。详见 §实测结果。
6. **start.hh 双模板参数**：4 种组合 → 二进制大小变 2 倍。可接受
7. **inconel_runtime_t 模板参数变化**：`<Cache>` → `<TreeCache, ValueCache>`，所有引用点（builder/start/test_runtime/test_value/test_tree_value）都要适配

## 实施顺序（增量验证）

按 feedback_incremental_refactor 的要求，分步走、每一步都跑测试：

1. **page_cache evict_one** — 加 cache_concept 接口 + clock/slru 实现 + 单元测试 → 跑 inconel_test_page_cache
2. **合并 open_pages_/ready_pages_ → writable_pages_** — 简化 allocator（删 cls.open / install_open_page / close_open_page / value_open_page_meta）+ scheduler 字段合并 + acquire_round_page/install_writable_page/handle_read 适配 + value_page_source 枚举改名（**先合并再改类型，保持功能等价不动 cache**）→ 跑 inconel_test_value (验证 6 个 case 仍全过)
3. **page_data 类型迁移** — page_data/round_page 改 unique_ptr<char[]> + image_size，写路径全部适配（暂不接 cache，readonly_cache_ 仍是 std::map<paddr, vector<char>>，commit_pages 临时 vector→unique_ptr 桥接）→ 跑 inconel_test_value
4. **decode helpers 改 span 签名** → 跑 inconel_test_value + tree_value
5. **value scheduler 拆 base/template** — 加 Cache 模板参数，readonly_cache_ 类型变 Cache，但用 trivial cache adapter 包 std::map（先让它 build 通过）→ 跑回归
6. **真接入 clock/slru cache** — 把 trivial cache adapter 换成 readonly_cache 真用 Cache 字段；handle_read/handle_fill 改 admit/bypass 分支；析构 evict_one → 跑回归
7. **runtime 双 Cache 模板** — inconel_runtime_t/build_options/start_options 全部适配 → 跑回归
8. **加测试 case** — case_7_cache_evict + case_5_cache_eviction_isolation + page_cache evict_one 单测 → 跑回归
9. **valgrind 手动验证** — 跑 inconel_test_value + test_tree_value 一次 valgrind，验证 cache 析构无 leak
10. **文档** — 更新 plan/007 实测结果 + commit

每一步必须 build pass + Release/Debug 全套测试通过才能进入下一步。

## 实测结果

### 环境

- Garuda Linux (Arch-based, rolling)
- glibc 2.43,**ld-linux-x86-64.so.2 stripped**(无 debuginfo)
- valgrind 3.25.1 — **启动失败**:
  ```
  A must-be-redirected function whose name matches the pattern: memcmp
  in an object with soname matching: ld-linux-x86-64.so.2 was not found
  ```
  需要 sudo 装 `glibc-debug`/debuginfod 镜像,本机未安装。
- 替代方案: `build_asan` 已存在 cmake target,`-fsanitize=address,undefined` + LeakSanitizer。LSAN 跟 valgrind memcheck 的 leak detection 都是 mark-and-sweep,语义等价覆盖 cache 析构和 evict_one drain 路径。

### ASAN+LSAN 三测试结果

| target | leak | ASAN error | UBSAN warning |
|---|---|---|---|
| `build_asan/inconel_test_page_cache` | **0** | 0 | 0 |
| `build_asan/inconel_test_tree_value` | **0** | 0 | 1(`pump/sender/reduce.hh:278` 一处 `bool` load,framework 内部,与 step 7 cache 无关) |
| `build_asan/inconel_test_value` | 72 allocations / 9984 bytes | 0 | 0 |

`inconel_test_value` 用 `LSAN_OPTIONS=max_leaks=0:report_objects=1` 拿到完整 report(默认 LSAN 折叠会 truncate)。

### `inconel_test_value` leak 完整分类

按 immediate alloc site (`#1` 帧) 分组:

| 来源 | unique stacks | allocations | bytes |
|---|---|---|---|
| `make_unique<char[]>` ← `scheduler<clock_cache>::handle_read` (scheduler.hh:775) | **1** | 1 | 4096 |
| `pump::core::scope_slab<64>::alloc()` (PUMP scope 框架,64-byte slab) | 32 | 53 | 3392 |
| `pump::core::scope_slab<128>::alloc()` (PUMP scope 框架,128-byte slab) | 13 | 18 | 2496 |
| **合计** | 46 | 72 | 9984 |

校验: `4096 + 3392 + 2496 = 9984` ✓,`1 + 53 + 18 = 72` ✓,与 LSAN summary 数字一致。

### Bug 1 — `cache.put(existing_key)` 静默丢旧 buf(已修复)

**初次 step 9 ASAN 报告里有 1 个 4096 B `Direct leak`**,stack `#1` 是 `make_unique<char[]>` ← `handle_read` at `scheduler.hh:775`。我最初误判为"share_nothing 中断 in-flight pipeline"的间接后果(写了一长段双对照实验论证),但 user 复查 `clock_cache::put` / `slru_cache::put` 的 update-existing 路径后指出问题:

```cpp
// clock_cache.hh (pre-fix)
if (auto it = index_.find(key); it != index_.end()) {
    auto& s = slots_[it->second];
    s.buf = buf;          // ← 旧 buf 被覆盖,nullopt 返回,调用方无从释放
    s.ref = true;
    return std::nullopt;
}
```

**触发链**(case_2_read_miss 完美 trigger):
1. `vr1` 和 `vr2` 同 `base == VALUE_LBA`
2. `value::scheduler::advance()` 顺序: `read_q_.drain()` 先,`fill_q_.drain()` 后
3. `read_q_` drain 时两个 read req 都进 `handle_read`,各自 `make_unique<char[]>` 分配独立 buf,分别走 sender pipeline → NVMe → `fill_q_`
4. `fill_q_` drain 时两个 fill req 都到 `handle_fill`,顺序执行:
   - `put(VALUE_LBA, buf1)` → 新插入,nullopt
   - `put(VALUE_LBA, buf2)` → update existing,**buf1 被静默覆盖,缺少返回值,调用方没法 free → leak**
5. case_2 实测"2 device reads"(两个 read 都 miss + fill)正是这个并发窗口

User 用最小 harness 独立验证:`put(k, b1); put(k, b2); evict_one();` → `allocs=2, frees=1`。

**修复**(`clock_cache.hh` / `slru_cache.hh` 同模式):
```cpp
if (auto it = index_.find(key); it != index_.end()) {
    auto& s = slots_[it->second];
    evicted_entry old{ key, s.buf };   // ← 把旧 buf 作为 evicted 返回
    s.buf = buf;
    s.ref = true;
    return old;
}
```

调用方 `commit_pages` / `handle_fill` 已经处理 evicted return path(`if (auto evicted = ...put(...)) delete[] evicted->buf;`),所以无需改调用点,只是修正 cache 实现里 update-existing 的 ownership 语义。

`page_cache.hh` 的 `cache_concept` 文档加了 ownership 规则的 normative 段落,明确 update / cap-evict / drain 三种返回 `evicted_entry` 的语义。

**回归测试**:
- `test_page_cache.cc` 的 `test_update_existing` 改成验证 `e2.has_value() && e2->buf == B(1)`(之前 hardcoded 期望 `!e.has_value()`,正好把 bug 行为当 contract)
- 新增 `test_put_same_key_drain_no_leak`: 同 key 连续 put N 次 + evict_one drain,验证 N 个 buf 全部各自被回收一次(user 的 harness 的程序化版)

**ASAN 验证(post-fix)**:

| 指标 | Pre-fix | Post update-existing fix | 差 |
|---|---|---|---|
| Allocations | 72 | 71 | **-1** |
| Bytes | 9984 | 5888 | **-4096** |
| 来自 `make_unique<char[]>` (cache char[]) | 1 | **0** | **-1** |
| 来自 `pump::core::scope_slab<64/128>::alloc()` | 71 | 71 | 0 |

差正好等于消失的那 1 个 4096 B leak。

### Bug 2 — `commit_pages` 把 multi-LBA full page 也塞进 cache,违反 D1(已修复)

User 第二次复查发现:`scheduler.hh:659` 的 `commit_pages` 对所有 `free_mask == 0` 的 page 一律 `readonly_cache_.put()`,**包括 multi-LBA**(class 4 `span_lbas == 4`)。但 `handle_read` 在 `scheduler.hh:733` 已经按 D1 实现了 `admit = (span == 1)`,multi-LBA 永远跳过 `cache.get()`。

**结果**:multi-LBA full page 写入 cache 后**永远不会被 hit**,只会:
- 占用 bounded cache 容量
- 驱逐真正有用的 1-LBA entry
- 析构时被 `evict_one` drain 释放(所以 ASAN 看不到 leak,但 cache 容量语义被污染)

`case_1_write_path` / `case_6_cross_class` 都写 16000 字节 multi-LBA,正好在 trigger 这个污染。

**修复**(`scheduler.hh:659` `commit_pages`):
```cpp
if (page.free_mask == 0) {
    if (page.span_lbas == 1) {       // ← D1: 只 1-LBA 进 cache
        char* raw = page.image.release();
        if (auto evicted = readonly_cache_.put(page.base, raw)) {
            delete[] evicted->buf;
        }
    }
    // span > 1: page.image's unique_ptr destructor frees the buf at end
    // of round.
}
```

这跟 `handle_read` 的 D1 检查对称。multi-LBA 永远不会进 `writable_pages_`(因为 multi-LBA class `slots_per_page == 1`,1 个 entry 即 `free_mask == 0` 直接走 commit full path),所以 D1 现在三处都一致:**read path bypass**(`handle_read`)/ **write path bypass**(`commit_pages`)/ **writable pool 自然不持有 multi-LBA**。

**新增 case** `case_8_multi_lba_bypass` — 4 个独立 falsifiable 断言:

| label | 断言 | 否证目标 |
|---|---|---|
| **W1** | `!sched_typed->readonly_cache_.contains(vr.base)` (write 后) | `commit_pages` 的 D1 (`page.span_lbas == 1` 检查) 被回归删除 |
| **R1** | `reads_after_1 == 1`(第一次 read 走 NVMe) | NVMe pipeline 未 short-circuit |
| **R2** | `reads_after_2 == 2`(第二次 read 仍走 NVMe) | NVMe pipeline 未 short-circuit |
| **W2** | `!sched_typed->readonly_cache_.contains(vr.base)` (read 后) | `handle_fill` 的 admit 路径在 `admit_to_cache=false` 时漏 admit |

**白盒 cast** 必须的设计:
- `case_8` 早期版本只用 `reads_after_2 == 2` 间接观察,user 指出这无法 falsify W1 — 即使 `commit_pages` 把 multi-LBA admit 进 cache,read 路径(`handle_read` line 725)在 `span > 1` 时会跳过 `cache.get()`,reads count 仍然 2,test 仍然通过(`code_quality_standard.md:213` 的"白盒断言某路径不发生"标准)
- 修复:test_env 公开 `using value_scheduler_t = value::scheduler<value_cache_t>`(单一 type source),`case_8` 用 `static_cast<test_env::value_scheduler_t*>(sched_base)` 拿 derived 指针,直接 `sched_typed->readonly_cache_.contains(vr.base)` 白盒探测 cache state
- `scheduler<Cache>` 是 `struct`,继承默认 public,`readonly_cache_` 是 public field;`scheduler_base` 无 virtual function,`static_cast` 安全(test_env hardcoded `clock_cache`,所以 actual type 已知)

**Falsification 验证**(real 验证而不是推理):
- 临时把 `commit_pages` 改回不带 D1 的版本(直接 `release()` + `put()`)
- 重 build + run `inconel_test_value`
- 输出: `CHECK failed: !sched_typed->readonly_cache_.contains(vr.base) at test_value.cc:615`
- W1 在 `commit_pages` D1 删除时立刻 abort,证明 W1 是真实 falsifier
- 恢复 D1 fix → W1 重新通过

这个 falsification 实测在 plan §"修正记录" 的"教训"列表中也作为案例 — **白盒 invariant 测试必须通过故意破坏被测代码并观察 test 反应来证明它真的能 catch 回归**,只看 test pass 不能证明 test 有效。

8 个 case 全 PASS:`case_8_multi_lba_bypass: OK (cache.contains(vr_multi)=false before+after 1→2 reads)`。

### Bug 3 — 新暴露的 cache 配置面无下界,小 capacity 直接 abort(已修复)

User 在第三次复查后发现 step 7 把 `tree_cache_capacity` / `value_cache_capacity` 公开成无约束 `uint32_t`,但 cache 实现实际依赖更强的不变量,边界值会直接 abort:

| 调用 | 失效路径 |
|---|---|
| `clock_cache(0).put(...)` | `slots_(0)` 是空 vector;`size_=0 < capacity_=0` 是 false → 落入 "cache full" 分支 → for 循环 `slots_[hand_=0]` deref 空 vector |
| `slru_cache(0).put(...)` | `prob_size_=0 ≥ prob_cap_=0` → evict prob_tail_=NIL → `unlink_probation(NIL)` |
| `slru_cache(1).get(...)` | `prot_cap_ = floor(1*0.8) = 0` → first promote 路径 `prot_size_=0 ≥ prot_cap_=0` → demote prot_tail_=NIL |

User 用最小 harness 独立验证:`clock_cache(0).put(...)`、`slru_cache(0).put(...)`、`slru_cache(1).get(...)` 都会直接 abort。

实际可达性: step 7 之前 runtime 单 Cache,builder 内部用 hardcoded `Cache(opts.cache_capacity)`,容量字段虽然也无校验但没暴露给配置面。step 7 把它拆成 `tree_cache_capacity` + `value_cache_capacity`,暴露给 `build_options` 和 `start_options`,user 只要写 `value_cache_capacity = 1` 就能让 `slru_cache` 在第一次 read 时崩溃。是 step 7 引入新的可达路径。

**修复**(`clock_cache.hh` ctor + `slru_cache.hh` ctor):

```cpp
// clock_cache
explicit clock_cache(uint32_t capacity)
    : slots_(capacity), capacity_(capacity) {
    if (capacity == 0) {
        throw std::invalid_argument(
            "clock_cache: capacity must be >= 1");
    }
    index_.reserve(capacity);
}

// slru_cache
explicit slru_cache(uint32_t capacity, float prot_ratio = 0.8f)
    : nodes_(capacity)
    , prot_cap_(static_cast<uint32_t>(capacity * prot_ratio))
    , prob_cap_(capacity - static_cast<uint32_t>(capacity * prot_ratio))
{
    if (capacity < 2) {
        throw std::invalid_argument("slru_cache: capacity must be >= 2");
    }
    if (prot_cap_ == 0 || prob_cap_ == 0) {
        throw std::invalid_argument(
            "slru_cache: prot_ratio must yield prot_cap >= 1 and prob_cap >= 1");
    }
    index_.reserve(capacity);
    // build free list...
}
```

设计要点:
- 用 `std::invalid_argument` throw 而不是 `assert`,因为 Release `-DNDEBUG` 会吞 assert,这种 input 校验必须无条件触发(`feedback_release_assert_sideeffect`)
- `slru_cache` 加双校验:`capacity < 2` 拦截 0/1,`prot_cap_/prob_cap_ == 0` 同时拦截极端 `prot_ratio`(如 0.0 / 1.0),两条覆盖所有边界
- `runtime/builder.hh` 的 `tree_cache_capacity` / `value_cache_capacity` 加 doc comment 说明最小 capacity 规则,`runtime/start.hh` 的 `start_options` 同步注释。Builder 不重复校验 — Cache ctor throw 会自然 propagate 到 `build_runtime` 调用方,fail-fast 集中在一处

**新增单元测试**(`test_page_cache.cc`):

| 测试 | 期望 |
|---|---|
| `test_clock_capacity_zero_throws` | `clock_cache(0)` ctor throw |
| `test_slru_capacity_zero_throws` | `slru_cache(0)` ctor throw |
| `test_slru_capacity_one_throws` | `slru_cache(1)` ctor throw(prot_cap=0) |
| `test_slru_extreme_ratio_throws` | `slru_cache(8, 0.0f)` 和 `slru_cache(8, 1.0f)` 都 throw |
| `test_clock_min_capacity_works` | `clock_cache(1)` 完整 put/get/evict/drain 周期 |
| `test_slru_min_capacity_works` | `slru_cache(2)` (prot_cap=1, prob_cap=1) 完整 put/promote/evict 周期 |

**验证**: 所有 6 个 boundary case PASS。`test_clock_min_capacity_works` 严格验证 cap=1 仍能 put 1 个 entry → get → put 第二个 → evict 第一个返回旧 buf → drain。`test_slru_min_capacity_works` 验证 cap=2 时 P1 promote 到 protected (cap=1)、P2 进 probation (cap=1)、P3 evict P2 而 P1 (protected) 存活。

**ASAN 回归** post-fix3: 80 allocations / 6720 bytes,**全部** PUMP scope_slab(33 × 64-byte slab + 21 × 128-byte slab),零 cache 路径 leak。boundary check 在 ctor 中 throw 路径未引入额外 alloc。

### 最终 ASAN leak 分类(两个 fix 后)

| 来源 | unique stacks | allocations | bytes |
|---|---|---|---|
| `make_unique<char[]>` cache buf | **0** | 0 | 0 |
| `pump::core::scope_slab<64>::alloc()` | 33(行号 alloc 计 33 unique stacks) | 56 | 3584 |
| `pump::core::scope_slab<128>::alloc()` | 21 | 24 | 3136 |
| **合计** | 54 | 80 | 6720 |

数字增长(80 vs 71 allocations)是因为 `case_8_multi_lba_bypass` 新增了 in-flight pipeline,share_nothing teardown 残留对应增加。**所有 leak `#1` 帧都是 PUMP scope_slab,零 cache 路径 leak**。

### share_nothing 残留 leak 的根因(plan-level constraint,非 step 7 引入)

来自 `pump::core::scope_slab<64/128>::alloc()` 的对象,是 PUMP 的 sender pipeline 中间状态(`with_context` / `flat` / `for_each` / `concurrent` / `reduce` 创建的 `runtime_scope`)。每个被 in-flight pipeline 通过 `scope_ptr`(shared_ptr)持有,worker 退出时 ref count > 0,析构链不触发。这正是 `feedback_share_nothing_no_drain` 描述的 PUMP-level 模式,跟 step 7 cache 设计无关。

**双对照实验**(确认 cache 实现本身在 fix 后零 leak):

| 测试 | driver | cache 操作 | leak (post-fix) |
|---|---|---|---|
| `inconel_test_tree_value` | manual `advance_until`(同步 drain) | 5 cases(case_5 包含 5 distinct paddr × 2 轮 read) | **0** |
| `inconel_test_page_cache` | 单元测试,无 scheduler | clock + slru put/get/evict_one + same-key 验证 | **0** |
| `inconel_test_value` | `share_nothing.run` (worker + hard stop) | 8 cases(case_2 trigger 同 key fill,case_7 cap eviction,case_8 multi-LBA) | 80 PUMP scope leak,**0 cache leak** |

### 71 个 PUMP scope 路径 leak

来自 `pump::core::scope_slab<64>::alloc()` 和 `<128>::alloc()` 的对象,是 PUMP 的 sender pipeline 中间状态(`with_context` / `flat` / `for_each` / `concurrent` / `reduce` 创建的 `runtime_scope`)。每个被 in-flight pipeline 通过 `scope_ptr`(shared_ptr)持有,worker 退出时 ref count > 0,析构链不触发。这正是 `feedback_share_nothing_no_drain` 描述的 PUMP-level 模式,跟 step 7 cache 设计无关。

### 步骤一致性验证(step 1 → step 8 数字)

cache 实现的可观察行为在每个 step 完成时都对照 Debug + Release 两套 build,数字完全一致:

| case | 数字 | 验证 step |
|---|---|---|
| case_2_read_miss | 2 device reads | step 2-8 |
| case_3_cache_hit | 1 device read across 2 reads | step 2-8 |
| case_4_write_then_read | 0 device reads | step 2-8 |
| case_3_corrupt_value_page | nvme reads 1→2, cache clean | step 2-8 |
| case_4_nvme_read_failure | device read 0, cache clean across 2 attempts | step 2-8 |
| `tree_lookup eviction stress` | clock 745 / slru 654 NVMe reads | step 1-8 |
| case_7_cache_evict (新加,step 8) | cap=2, 5 reads → 3 NVMe = 2 hits + 3 misses | step 8 严格 == 3 |
| case_5_cache_eviction_isolation (新加,step 8) | round1=5 reads, round2=+5(>=3 lower bound) | step 8 |
| case_8_multi_lba_bypass (新加,step 9 bug-fix) | multi-LBA write + 2 reads → NVMe 1 → 2 | step 9 严格 == 2 |

step 5 → step 6 的 `commit_pages` 从 vector 桥接换成 `release()` + `cache.put()`,`handle_fill` 从 vector 拷贝换成 `release()`(admit) / 自动析构(bypass)— 两种实现的可观察行为(NVMe 读次数、cache hit/miss 模式)完全一致,因为 D1 admission policy 跟之前 unbounded 的 std::map 在 1-LBA case 上语义等价。

### 结论

| 论断 | 证据 |
|---|---|
| `~scheduler()` 的 `evict_one()` drain 路径正确 | `inconel_test_tree_value` 5 cases 全 clean,`inconel_test_page_cache` evict_one 单测全 clean |
| `commit_pages` 的 `release()` + `cache.put()` + evicted `delete[]` 路径正确(且对 multi-LBA bypass) | `case_7_cache_evict` 写 5 个 1-LBA page 触发 eviction;`case_8_multi_lba_bypass` 写 multi-LBA 验证不进 cache;两者 ASAN 都零 cache leak |
| `handle_fill` 的 admit `release()` / bypass auto-free 路径正确 | `case_5_cache_eviction_isolation` 2 轮 read × 5 paddr 触发 admit/evict 路径,无 leak |
| `cache.put(existing_key)` 正确返回 old buf | `test_update_existing` + `test_put_same_key_drain_no_leak` 单测验证;`case_2_read_miss` 的 ASAN 4096 B leak 在 fix 后归零 |
| D1 完整生效(read + write 两路一致 bypass) | `case_8_multi_lba_bypass` 严格 NVMe `1 → 2`,multi-LBA 写后立即读两次都走 NVMe |
| step 7 双 Cache 模板拆分零行为影响 | 所有 case 的可观察数字与 step 6 完全一致 |
| share_nothing 不 drain in-flight pipeline 是 plan-level 已知缺口的实测确认 | `inconel_test_value` 的 80 个 PUMP scope leak 全部源于 worker 中断时 sender pipeline 未释放,manual advance loop 测试零 leak |
| 用 valgrind 等价的 leak 验证已完成(用 ASAN+LSAN 替代) | 三个 ASAN 测试完整 report 已 capture,分析见上 |

### 修正记录

User 在 step 9 完成后连续做了三次复查,发现三个 bug,我之前的 step 9 ASAN 分析全部漏掉了。每次都需要修正之前的结论:

**Bug 1 (`cache.put(existing_key)` 静默丢旧 buf)**: 我第一次跑 ASAN 后看 LSAN 默认 truncate 后的 7 条 leak headers,**误判**那 1 个 4096 B `Direct leak`(stack trace 显示 `handle_read` at scheduler.hh:775)是"share_nothing 中断 in-flight pipeline 的间接后果",写了一段双对照实验论证。User 复查后指出这其实是 `cache.put(existing_key)` 路径的 silent overwrite,并独立验证了最小 harness(`put k,b1; put k,b2; evict_one()` → `allocs=2 frees=1`)。修正后 fix update-existing → 那 1 个 leak 消失,差正好 4096 B / 1 alloc。教训:**LSAN 默认 truncate 时不要相信局部样本论证**,必须 `LSAN_OPTIONS=max_leaks=0` 拿全量。

**Bug 2 (`commit_pages` 没按 D1 bypass multi-LBA)**: User 第二次复查后指出 `scheduler.hh:659` 的 `commit_pages` 对所有 `free_mask == 0` 的 page 一律 `cache.put()`,违反了我自己 plan §"设计决策" D1 写的"只服务 1-LBA 页,multi-LBA bypass"。`handle_read` 在 step 6 已经按 D1 实现 read 路径 bypass,但 `commit_pages` 在写路径还在 admit。这条 ASAN 看不到(因为 `~scheduler()` evict_one drain 会释放),但污染 cache 容量。我在 plan §"已知缺口" 第 3 条原本写"writable page 不进 cache → v7 已经满足",但只覆盖 read 路径没复查 write 路径。fix + 加 `case_8_multi_lba_bypass` 验证 read after write 的 NVMe count 严格 `1 → 2`。

**Bug 3 (cache 配置面无下界)**: User 第三次复查后指出 step 7 把 `tree_cache_capacity` / `value_cache_capacity` 公开到 `build_options` / `start_options`,但 `clock_cache(0)` / `slru_cache(0)` / `slru_cache(1)` 都会在第一次 `put`/`get` abort,且没有任何校验。这是 step 7 引入的可达路径 — step 6 之前 cache 容量没暴露给配置面,user 不可能传 0。我在 step 7 加了 `tree_cache_capacity = 32` / `value_cache_capacity = 32` 默认值就以为够了,完全没考虑 user 显式传值的边界。User 用最小 harness 复现三个 abort 后 fix:cache ctor throw `std::invalid_argument`,加 6 个 boundary 单测。

**总结教训**:
1. ASAN/LSAN 默认 truncate 输出,分析 leak 必须用 `max_leaks=0` 拿全量
2. 实现 D1 / D2 类决策时要在所有可能 admit 的路径检查,不止 read 路径
3. 公开新配置字段时,必须沿着 callee 一路查 invariant 边界,不能假设默认值就够防御
4. ASAN 没报的 bug 不等于没有 bug — Bug 2 (multi-LBA admit) 不 leak 但污染 cache 容量;Bug 3 (capacity=0) 在测试用 32 时永远不触发但是真实可达 abort。LSAN 只覆盖 leak,功能性 / 边界 bug 必须靠 review + 边界单测
5. **测试通过 ≠ 测试有效** — `case_8` 第一版用 `reads_after_2 == 2` 间接观察,看似在测 D1,但实际只能 falsify read-side 路径。user 指出后改成白盒 `cache.contains(vr.base)` + 通过临时回退 `commit_pages` D1 实测 test 真的 abort,才证明 falsifier 有效。**白盒断言必须可被故意破坏的回归捕获,要主动 try-then-revert 验证一次**(`code_quality_standard.md §4` 的"白盒测试断言某路径不发生某类行为"标准)

### 后续工作(本 step 范围之外)

- 装 `glibc-debug`(或在 valgrind 环境跑)做一次"金标准" valgrind 复测,作为 ASAN 结果的二次确认
- share_nothing teardown 的 drain 修复是 PUMP 框架 task,跨多个 step,不在本 step 范围
- DMA pool 接入时,把 `delete[]` / `make_unique<char[]>` 替换成 spdk_iobuf alloc/free(3 处),evict_one drain 的 callback 也跟着改
- inconel-level read coalescing: `case_2` 的 NVMe read 数 = 2 但 `vr1`/`vr2` 同 paddr,可以在 `handle_read` 用 `loading_pages_` (类似 tree scheduler) dedup,让多个同 paddr 的 read miss 共享一次 NVMe read。这是性能优化,不是 correctness fix。当前的 cache update-existing fix 已经保证 leak-safe。
