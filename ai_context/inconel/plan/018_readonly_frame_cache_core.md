# 018 — Read-Only Frame Cache Core

> 实现第十八步。先把 `INC-036` 最核心、最通用的那半收掉：引入 `page_frame` / `frame_pin` / `frame_id`，把 clock/slru 和 tree/value 两侧的 readonly cache 从 raw `char*` 提升成真正的 frame cache，并顺手把 teardown-only 的 `evict_one` 改名成 `drain_one`。

## 本 step 覆盖的 issue

| Issue | 说明 |
|---|---|
| `INC-036`（phase A） | `cache_concept` 的 value 类型是 raw `char*`，缺 `pin_count` / clean-readonly frame carrier |
| `INC-038` | `evict_one` 名字与语义漂移，实际是 teardown drain |

## 文件结构

```text
memory/
└── frame.hh                          — `frame_id` / `frame_state` / `page_frame` / `frame_pin`

core/
├── page_cache.hh                     — frame-oriented readonly cache concept
├── clock_cache.hh                    — value 改成 `page_frame*`，支持 pin-aware eviction
└── slru_cache.hh                     — 同步迁移 + `drain_one`

tree/
└── scheduler.hh                      — tree node cache 改存 `page_frame*`

value/
└── scheduler.hh                      — readonly cache 改存 `page_frame*`，`writable_pages_` 暂不拆

test/
├── apps/inconel/test/test_page_cache.cc
├── apps/inconel/test/test_tree_lookup*.cc
└── apps/inconel/test/test_value.cc
```

## 设计目标

1. 把 readonly cache 的语义从“缓存一块裸字节并让 caller 自己记住生命周期”提升到“缓存一个有状态的 frame”。
2. 为 tree/value 读路径补上最基本的 pin 语义，避免 cache hit 返回未 pin 的裸指针。
3. 在不触碰 DMA abstraction（`INC-016`/`INC-002`）的前提下，先把 frame carrier 和 cache contract 立住。

## 设计决策

| # | 决策点 | 结果 | 说明 |
|---|---|---|---|
| `D1` | 新类型归属 | **`memory/frame.hh`** | `page_frame` 是 runtime 基础设施，不继续塞在 `core/` |
| `D2` | 本 step 的 backing storage | **继续允许 heap-backed page bytes** | 本 step 解决的是 carrier/state，不是 DMA backend；DMA 替换仍留给 `INC-016` |
| `D3` | cache API | **`pin(id)` / `put(frame*)` / `drain_one()`** | 不再暴露 `get()->const char*` 这种未 pin 裸视图 |
| `D4` | scope 边界 | **只处理 `clean_readonly` frame cache** | value 的 `writable/open/hole-fill` resident 状态留到下一步再拆 |

## 详细设计

### `memory/frame.hh`

新增：

```cpp
struct frame_id {
    paddr base;
    uint16_t span_lbas;
    enum class domain : uint8_t {
        tree_node,
        value_page,
        wal_page,
        tree_writeback,
    } dom;
};

enum class frame_state : uint8_t {
    clean_readonly,
    clean_allocatable,
    dirty_append,
    dirty_hole_fill,
    writeback_inflight,
};

struct page_frame {
    frame_id    id;
    frame_state st;
    char*       buf;
    uint32_t    byte_len;
    uint32_t    pin_count;
    bool        crc_valid;
};

struct frame_pin { ... };
```

说明：

1. `buf` 先保持 heap-backed，避免和 DMA/buffer-pool 改造绑在一起。
2. `frame_pin` 是 owner-local RAII；释放时只减 `pin_count`，不 delete frame。
3. `page_frame` 本身由 cache / owner 状态结构持有，调用方只拿 pin，不拿 owning pointer。

### `core/page_cache.hh`

把概念收敛成 readonly frame cache：

```cpp
template <typename C>
concept cache_concept = requires(C c, const C cc, frame_id id, page_frame* f) {
    { c.pin(id) }        -> std::same_as<frame_pin>;
    { c.put(f) }         -> std::same_as<std::optional<page_frame*>>;
    { cc.contains(id) }  -> std::same_as<bool>;
    { cc.size() }        -> std::same_as<uint32_t>;
    { cc.capacity() }    -> std::same_as<uint32_t>;
    { c.drain_one() }    -> std::same_as<std::optional<page_frame*>>;
};
```

contract：

1. `put(frame*)` 只接受 `st == clean_readonly`
2. `pin(id)` 命中时必须先 `pin_count++` 再返回
3. 逐出只能发生在 `pin_count == 0`
4. `drain_one()` 是 teardown-only API，不承诺 policy eviction

### `clock_cache.hh` / `slru_cache.hh`

同步改成：

- key：`frame_id`
- value：`page_frame*`
- runtime eviction：跳过 `pin_count > 0`
- teardown：`drain_one()`

这里一起完成 `INC-038` 的 rename，避免“新 API 还是旧误导名字”。

### `tree/scheduler.hh`

tree lookup 的 page cache 改成 `frame_id{page, span_lbas = tree_page_size / lba_size, dom = tree_node}`。

变化点：

1. cache hit 不再拿 `const char*`，改成 `frame_pin pin = page_cache_.pin(id)`
2. parser / corruption check 用 `pin.frame->buf`
3. miss completion 时把 read 回来的 page bytes 包成 `page_frame{st = clean_readonly}`
4. 旧的 `free_bufs_ / owned_bufs_` 也从“裸 buffer 池”改成“frame descriptor + backing bytes 池”

### `value/scheduler.hh`

本 step 只处理 readonly cache：

1. `readonly_cache_` 改成缓存 `page_frame*`
2. `handle_read()` 命中 readonly cache 时走 `pin(id)`，decode 从 `pin.frame->buf` 读取
3. `handle_fill()` / `commit_pages()` 在把 1-LBA 页放入 readonly cache 前，先构造 `page_frame{st = clean_readonly}`
4. 当前 `writable_pages_`、`page_data`、`value_page_source::writable` 暂不拆；它们在下一步处理

## 与 `INC-016` 的边界

本 step 明确**不**做：

- DMA pool / SPDK buffer
- 跨 owner invalidate
- tree/value buffer ownership 大统一

也就是：

- `page_frame` 先承载“状态 + pin + stable owner-local descriptor”
- backing bytes 仍可继续用 `new char[]`

这样 `INC-036` 的“frame 语义”与 `INC-016` 的“DMA/buffer backend”分开落地。

## 实施顺序

1. 新建 `memory/frame.hh`。
2. `core/page_cache.hh` / `clock_cache.hh` / `slru_cache.hh` 改 frame cache concept。
3. 同一批完成 `evict_one -> drain_one` rename。
4. 迁 tree readonly cache。
5. 迁 value readonly cache。
6. 补单测与 integration test。

## 验证

至少回归：

- `inconel_test_page_cache`
- `inconel_test_tree_lookup`
- `inconel_test_tree_lookup_multicore`
- `inconel_test_value`
- `inconel_test_runtime`

`test_page_cache` 需要新增断言：

- `pin()` 命中会提升 `pin_count`
- pinned frame 不会被 runtime eviction 选中
- `put()` 拒绝非 `clean_readonly` frame
- `drain_one()` 只是 teardown pop，不承诺 policy victim
