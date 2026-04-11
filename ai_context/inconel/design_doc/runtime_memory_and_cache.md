# Inconel 详细设计：运行期对象与缓存模型

> 依据：`ai_context/inconel/design_overview.md`（唯一概要规范）
>
> 本文补足运行期对象的内存分类、生命周期、page/frame 运行时模型、cache 视图与 value placement 的耦合规则。它不重新定义 WAL / tree / value 的持久化语义，只统一“哪些对象属于 correctness owner，哪些属于 cache，哪些是待写回页”。

## 1. 目标与边界

本文解决四个问题：

1. 运行期里哪些对象不能被叫做 cache，为什么。
2. 读侧 page residency、写侧 dirty page、allocator free-pool 之间如何统一建模。
3. 为什么不能把所有内存对象塞进一个单层 LRU。
4. value hole reuse、resident page、writeback 状态如何反过来影响 placement priority。

本文不做的事：

1. 不重定义 `publish_catalog`、`read_handle`、WAL/tree/value 的提交与恢复语义。
2. 不替代 `runtime_state_machine.md` 对五类 scheduler 的请求流描述。
3. 不改变 `on_disk_formats.md` 中 `value_ref`、tree page、WAL entry 的盘格式。
4. 不把任何新的 runtime 元数据偷偷变成 recovery 输入。

## 2. 运行期对象总分类

运行期对象必须先按语义分层，不能先按“都在内存里”混成一类。

| 类别 | 典型对象 | Owner | 可否驱逐 | 是否参与 recovery 输入 | 主要作用 |
|------|---------|-------|---------|------------------------|---------|
| correctness owner | `publish_catalog`、`published_read_set`、`checkpoint_guard`、`tree_manifest` | `coord_sched` / `tree_sched` | 否 | 否 | 固定 reader 可见拓扑与生命周期边界 |
| correctness-carried hot data | `memtable_gen`、`memtable_entry`、`value_handle`、`hot_blob` | `front_sched` | 否（只要仍被 memtable 触达） | 否 | 承载前台 live 数据；memtable hit 必须直接返回 |
| 请求态临时对象 | `read_handle`、`batch_cache`、fan-out/fan-in 中间态 | 请求上下文 | 否（按请求结束释放） | 否 | 描述单次 API / 单次 batch 的局部状态 |
| page/frame 运行时对象 | tree node frame、value page frame、WAL tail frame | 各 scheduler 本地 shard | 视状态而定 | 否 | 表示 DMA-resident 的设备页像与其状态 |
| placement metadata | `whole_page_pool`、`hole_page_list`、`extent_free_pool`、`generic_free_spans`、allocator head | `value_alloc_sched` | 不适用 | 否 | 告诉 allocator 哪些空间可复用 |
| cache 视图 | read-only frame cache、可选 materialized value blob index | 按 domain 决定（tree_node 可多 shard；value_page 当前为单 owner） | 是（仅限非 pinned clean 对象） | 否 | 降低读放大、避免重复 device read、支持原地解析/快速 copy-out |
| dirty/writeback 集合 | `slab_page_buf`、hole-fill page、`tail_buf`、tree flush write buffers | 单一写 owner | 否（只能 flush） | 否 | 承载尚未落盘或正在落盘的页像 |

核心结论：

1. `publish_catalog / published_read_set / checkpoint_guard / tree_manifest` 不是 cache。
2. `hot_blob` 不是 page cache；它是 memtable 值语义的一部分。
3. `whole_page_pool / hole_page_list / extent_free_pool / generic_free_spans` 不是 cache；它们是 allocation metadata，归 `value_alloc_sched` 所有。
4. 真正可以按 cache 讨论的，只有 clean frame 的驻留策略和可选的 materialized view 索引。

## 3. Correctness Owner 图谱

读路径里真正 pin 住正确性的对象链如下：

```text
read_handle
  -> publish_catalog
     -> published_read_set
        -> front_read_set[]
           -> memtable_gen
              -> memtable_entry
                 -> value_handle
                    -> hot_blob
        -> checkpoint_guard
           -> tree_manifest
```

约束：

1. `read_handle` pin 的是 `publish_catalog`，不是裸 `durable_lsn`。
2. `tree_manifest` 固定 tree 结构定位；node page image 只是它的可驱逐载体。
3. `hot_blob` 由 memtable entry 持有；cache 只能额外持有引用，不能成为 correctness owner。
4. `batch_cache` 只存在于单次 batch 或未来的 read-your-own-writes 场景；它不跨请求共享。

因此，内存里的“热”对象至少分三层：

1. **不可驱逐的 correctness owner**：`publish_catalog`、`prs`、`guard`、`manifest`。
2. **不可被 cache eviction 破坏语义的 live data**：`memtable_entry`、`value_handle`、`hot_blob`。
3. **可以重读重建的 frame/page**：tree node/value page/WAL read page 的 clean residency。

## 4. 为什么不能做成一个单层统一 Cache

“统一缓存层”如果被理解成“一个大 hash + 一个大 LRU”，在 Inconel 里是不对的。原因有四个：

1. **语义不同**
   - `tree_manifest` 被驱逐会直接破坏 reader 的结构一致性。
   - tree node page 被驱逐只会导致下次重读。
   - `hot_blob` 被驱逐会破坏 memtable hit 语义。

2. **页状态不同**
   - 有的 frame 是 clean read-only。
   - 有的 frame 是 dirty append target。
   - 有的 frame 带 hole，可被 allocator 继续消费。

3. **owner 不同**
   - `value_alloc_sched` 独占 value append / hole-fill。
   - `front_sched` 独占 WAL tail。
   - `tree_sched` 独占 tree flush / reclaim。
   - 读 cache 是 local shard，不应该反向拥有写路径状态。

4. **策略不同**
   - read-only node page 适合按访问热度驱逐。
   - hole page 在 readonly_frame_cache 中的驻留价值不在”命中过去的读”，而在”避免下一次 hole reuse 的 read 部分”。
   - dirty page 不能驱逐，只能 flush。

因此，正确的统一方式不是“一个单层 cache”，而是：

1. **统一 page/frame 运行时抽象**。
2. **在这个抽象之上分出不同语义的 cache/pool/view**。
3. **保持 share-nothing owner，不引入全局锁式 cache 管理器**。

## 5. 统一的 Page/Frame 运行时抽象

### 5.1 基本原则

本文统一使用 `frame` 表示“一段当前驻留在内存中的设备页像”。

- 对 tree page，它通常对应一个 tree page image。
- 对 sub-LBA value class，它通常对应一个包含多个 slots 的 value page image。
- 对 WAL，它对应一个当前 tail page image。
- 对 multi-LBA value class，它可以是一个 span frame（覆盖连续多个 LBA）。

这里的统一点是“驻留页像 + pin/writeback/evict 规则”，不是“所有逻辑对象同用一个策略”。

### 5.2 `frame_id`

```cpp
struct frame_id {
    paddr base;                                     // 起始物理地址
    uint16_t span_lbas;                             // 覆盖多少个 LBA；sub-LBA/value page 通常为 1
    enum class domain : uint8_t {
        tree_node,
        value_page,
        wal_page,
        tree_writeback,
    } dom;
};
```

语义：

1. `frame_id` 描述“当前这段驻留页像对应的是哪类设备对象”。
2. `domain` 不能省略；同一物理地址在不同域下不能混用解析逻辑。
3. `span_lbas` 让 multi-LBA value extent 与单页对象使用同一套 frame 生命周期。
4. v1 的 cache key **不**携带 `reuse_epoch`；同一个 `{base, span_lbas, dom}` 能继续安全作为 lookup key，前提是 owner 在“同地址重写可见”或“地址重新进入可复用池”之前完成 mandatory invalidate barrier（见 §10.2）。

### 5.3 `frame_state`

```cpp
enum class frame_state : uint8_t {
    clean_readonly,         // 只读、可驱逐、cache miss 可重读
    clean_allocatable,      // resident 且包含可复用 hole；尚未被 writer 打开
    dirty_append,           // 当前顺序填充目标页
    dirty_hole_fill,        // 从 hole page 打开后继续填充
    writeback_inflight,     // 已提交写，等待完成
};
```

状态解释：

1. `clean_readonly`
   - tree node page、写满的 value page、seal 后只用于读取的页都属于这里。
   - 它们可以被驱逐，代价只是下次重读。

2. `clean_allocatable`
   - 只适用于 value page。
   - 说明该页当前 resident，而且 free-slot 信息已在内存里；把它交给 allocator 不需要再先读一页。
   - 它不是普通 read cache entry，而是“可消费的 placement 资源”。

3. `dirty_append`
   - 当前 open page，来自 fresh page / whole-page pool / fresh extent。
   - `slab_page_buf` 与 `tail_buf` 都属于这类的具体实例。

4. `dirty_hole_fill`
   - 说明 writer 已经拿到该 hole page 的独占可写视图，并正在往空洞里补写。
   - 它可能来自“先读再改”的 hole reuse，也可能来自此前已经 resident 的 `clean_allocatable` page。

5. `writeback_inflight`
   - 本轮内容已冻结，等待设备完成。
   - 不能再被 writer 修改，也不能按 clean cache 驱逐。

### 5.4 `page_frame`

```cpp
struct page_frame {
    frame_id id;
    frame_state st;

    void* dma_buf;                                  // DMA-resident page image
    uint32_t byte_len;

    uint32_t pin_count;                             // frame 级 pin，仅用于 residency 管理
    bool crc_valid;
};
```

**冻结的 canonical ownership 模型：cache-owning + pin token**

1. frame 由 cache / pool / dirty set 持有（raw pointer ownership）。
2. 消费者通过 `pin_count++` 获得 pin token（RAII），`pin_count--` 释放。
3. `pin_count > 0` 的 frame 不可驱逐。
4. `pin_count == 0` 且 `st == clean_readonly` 时可按 LRU 驱逐。
5. dirty frame 即使 `pin_count == 0` 也不能驱逐，只能 flush。
6. 不使用 `intrusive_ptr<page_frame>` 管理 frame 生命周期——frame 的生死由 cache/pool 决定，不由引用计数决定。
7. `crc_valid` 是 page image 可用性的本地判定；校验失败直接报错，不进入 cache。

```cpp
struct frame_pin {
    page_frame* frame;
    frame_pin(page_frame* f) : frame(f) { if (f) f->pin_count++; }
    ~frame_pin() { if (frame) frame->pin_count--; }
    frame_pin(frame_pin&& o) : frame(o.frame) { o.frame = nullptr; }
    // non-copyable
};
```

### 5.5 `value_page_frame`

```cpp
struct value_page_frame : page_frame {
    uint16_t class_idx;
    uint16_t slots_per_page;
    bitset<MAX_SLOTS_PER_PAGE> free_bitmap;
    uint16_t free_count;

    enum class open_mode : uint8_t {
        none,
        append,
        hole_fill,
    } mode;
};
```

`value_page_frame` 比普通 `page_frame` 多出的不是“缓存逻辑”，而是 allocator 关心的页内可分配状态。

规则：

1. `free_bitmap/free_count` 只在 value page 域存在。
2. `mode = append` 只用于 fresh/empty page 的顺序填充。
3. `mode = hole_fill` 说明该页当前既是 resident frame，又是 allocator 的活跃写入目标。
4. `free_count == 0` 的 value page 应退化为 `clean_readonly`，不再是 allocatable page。

### 5.6 冻结规则：所有 Frame/Page 使用 DMA 内存

本文进一步冻结：

1. 所有 `page_frame` / `value_page_frame` 的页像都来自 DMA 内存。
2. `page_frame` 本身的描述符可以在普通内存里；只有 `dma_buf` 指向的 payload 必须是 DMA-resident。
3. tree node frame、value page frame、`slab_page_buf`、hole-fill page、`tail_buf`、tree flush write buffers 都属于这条规则。
4. `publish_catalog`、`published_read_set`、`checkpoint_guard`、`tree_manifest`、memtable index、retire lists、`batch_cache` 不进入 DMA 池。
5. `hot_blob` 默认也不进入 DMA 池；它属于 hot data，不属于 frame/page。

这样做的收益只有一类：减少 page/frame 路径上的 staging copy。

1. 读路径：tree 为 `NVMe -> DMA frame -> 原地 parse`；value 为 `NVMe -> DMA frame -> 校验后 copy-out`，不需要额外 staging copy。
2. 写路径：dirty frame 直接作为写回源缓冲，不需要再做一次 `heap -> DMA` 搬运。
3. `value_alloc_sched` 的 `readonly_frame_cache` 若恰好持有 hole page 的 resident 页像，可以直接打开为 dirty_hole_fill，避免 hole reuse 前的再次读页。

### 5.7 基于 SPDK Env 的 DMA Pool 分层

既然系统绑定 SPDK，DMA 管理不应再抽象成一个与 SPDK 脱钩的通用 allocator。本文冻结：

1. frame/page 的 DMA payload 直接建立在 `spdk/env.h` 提供的 pinned memory 与 mempool API 上。
2. fixed-size frame class 优先使用 `spdk_mempool_create()` / `spdk_mempool_create_ctor()`。
3. 少量不规则的大 span、bootstrap 临时分配、或 pool 建立前的冷路径使用 `spdk_dma_malloc_socket()` / `spdk_dma_zmalloc_socket()`。
4. 只有在需要 **named** 且 **IOVA contiguous** 的长期区域时，才使用 `spdk_memzone_reserve_aligned()`。
5. 我们自己的 runtime 只管理 frame 状态、quota、owner 归属与 cache/view，不再自造一套底层 DMA 字节分配器。

因此，DMA 池的分层应按 **NUMA + buffer size / span** 建在 SPDK mempool 之上，而不是按逻辑 domain 分层。

```cpp
struct dma_pool_class {
    uint32_t bytes;                                 // 4K / 8K / 16K / ...
    int numa_id;

    spdk_mempool* mp;                               // 固定尺寸快路径

    uint64_t hard_limit_bytes;
    uint64_t low_watermark_bytes;
};

struct dma_pool {
    small_vector<dma_pool_class, 8> classes;
};
```

规则：

1. 4K class 可以被 tree node page、单页 value page、WAL page 共用；逻辑 domain 由 `frame_id.dom` 区分，不由 DMA pool 区分。
2. multi-LBA frame 使用更大的 DMA class；v1 可以按 `span_lbas * lba_size` 的常见尺寸建 class。
3. `spdk_mempool` 已提供 thread-safe pool 以及 per-core cache；我们的 wrapper 不再重复维护另一层底层字节 freelist。
4. DMA pool 只管理“原始 DMA bytes”；frame 的状态、域、pin、free bitmap、cache 归属都挂在 `page_frame` 描述符上。
5. 对外暴露的是“申请一个多少字节的 DMA span”，不是“申请一个 tree page”或“申请一个 value page”。

### 5.8 SPDK API 的职责划分

这三类 SPDK API 的职责应明确区分：

1. `spdk_mempool_*`
   - 用于固定尺寸 frame class 的主路径池化
   - 适合 4K / 8K / 16K / 常见 multi-LBA span
   - 依赖其 thread-safe pool 和 per-core cache 作为快路径

2. `spdk_dma_malloc_socket()` / `spdk_dma_zmalloc_socket()`
   - 用于按 NUMA 分配 pinned DMA buffer
   - 用于 pool bootstrap、非常规大对象、或 mempool 不适用的冷路径
   - 不应成为 steady-state 每次 I/O 的快路径分配方式

3. `spdk_memzone_reserve_aligned()`
   - 用于 named、长期存在、需要显式对齐或 IOVA contiguous 的共享区域
   - 默认不用于普通 page/frame 的热路径缓存
   - 更适合启动期 arena、跨进程共享、或特殊设备约束场景

### 5.9 DMA 内存的来源与初始化

boot 时，系统应按设备与 NUMA 归属初始化 DMA pool：

1. 通过 `spdk_env_get_numa_id()` / runtime 配置确定每个 owner 的 NUMA 归属。
2. 对每个常见 frame size 建立对应的 `spdk_mempool`。
3. `cache_size` 使用 SPDK 的 per-core cache 能力；不要求我们额外复制一套底层 local byte freelist。
4. 若某类大 span 不适合放进常驻 mempool，则在冷路径上使用 `spdk_dma_malloc_socket()` 单独申请，并纳入单独 quota。
5. 若需要命名且长期存在的启动期 DMA arena，则用 `spdk_memzone_reserve_aligned()` 建立。

这样做的目的：

1. 快路径复用 SPDK 已有的池化与 per-core cache。
2. NUMA 归属直接落在 SPDK env 的 socket-aware 分配上。
3. 我们自己只保留上层的 frame 生命周期与 quota 管理。

### 5.10 分配、归还与跨 Shard 管理

#### 分配快路径

```text
alloc_dma_span(bytes, numa_id):
1. 定位到对应 dma_pool_class
2. 先从 spdk_mempool_get(mp) 或 spdk_mempool_get_bulk(mp, ...) 获取
3. 若 mempool 不足：
   - 先触发 clean frame eviction / dirty flush
4. 若该 size class 不是常驻 pool：
   - 走 spdk_dma_malloc_socket(bytes, align, NULL, numa_id) 冷路径
5. 若仍无法获得 DMA span：
   - 进入既有 backpressure / 空间不足路径
```

#### 归还路径

```text
free_dma_span(span, pool_class):
1. 仅当 frame 已不在任何 cache/view 中、没有 pin、不是 dirty、也不在 inflight writeback 中时，才允许归还
2. 若 span 来自 spdk_mempool：
   - 用 spdk_mempool_put() / put_bulk() 归还
3. 若 span 来自 spdk_dma_malloc_socket()：
   - 用 spdk_dma_free() 归还
```

跨 shard 规则：

1. shard 之间不直接共享 dirty frame 的可写 ownership。
2. clean frame 可以跨 shard 流转，但其底层 DMA span 仍然只通过 SPDK pool API 归还。
3. dirty frame 的 ownership 不迁移；需要迁移时，先结束 writeback 生命周期，再以 clean frame 身份重新归属。

### 5.11 `spdk_mempool` 与 `spdk_iobuf` 的分工

`spdk_mempool` 和 `spdk_iobuf` 都能提供 DMA buffer，但语义不同，不能混用。

#### `spdk_mempool`

适合：

1. 长生命周期 resident frame
2. 需要显式 quota 的 cache / dirty page
3. 需要精确 owner 管理的可写 frame
4. 固定尺寸对象

原因：

1. 生命周期由我们自己控制，不依赖 waiter queue。
2. 很适合 `page_frame` 这种“拿到之后可能被 pin 很久”的对象。
3. 容易按 frame class 记账和做 hard reservation。

#### `spdk_iobuf`

适合：

1. 请求级、临时、可等待的 DMA buffer
2. 短命 scratch / encode / decode / gather-scatter 临时拼装
3. 可以接受“当前没 buffer 就排队，等 callback 继续”的流程

不适合：

1. `readonly_frame_cache`
2. open dirty page
3. WAL tail frame

原因：

1. `spdk_iobuf_get()` 在无 buffer 时会把请求排队，等 buffer 可用后回调继续；这很适合请求态 buffer，不适合长期持有的 cache frame。
2. `spdk_iobuf_put()` 归还时可能直接把 buffer 转交给等待者；这说明它更像共享供给池，而不是长期驻留对象仓库。
3. 若把 resident cache frame 直接压进 `spdk_iobuf`，会把“缓存占用”和“请求等待 buffer”耦合在一起，污染 backpressure 语义。

#### API 落点

1. `spdk_mempool_create()` / `spdk_mempool_create_ctor()`
   - 建固定尺寸 frame pool
   - `cache_size` 直接用于底层 per-core cache

2. `spdk_mempool_get()` / `spdk_mempool_get_bulk()`
   - resident frame / dirty frame 的主路径获取

3. `spdk_mempool_put()` / `spdk_mempool_put_bulk()`
   - resident frame 生命周期结束后的归还

4. `spdk_iobuf_set_opts()` + `spdk_iobuf_initialize()`
   - 进程启动时设置 global iobuf pool 参数并初始化

5. `spdk_iobuf_channel_init(name, small_cache_size, large_cache_size)`
   - 为每个 scheduler / thread 建立请求态 DMA scratch channel

6. `spdk_iobuf_get(ch, len, entry, cb_fn)` / `spdk_iobuf_put(ch, buf, len)`
   - 请求态 scratch 的获取与归还
   - 若当前无 buffer，`get` 可以排队等待，`put` 会把 buffer 交给等待者

7. `spdk_iobuf_entry_abort()`
   - 请求取消或超时时，把 waiter 从 iobuf 等待队列摘掉

#### 冻结的使用边界

1. **resident frame 一律来自 `spdk_mempool` 或冷路径 `spdk_dma_malloc_socket()`**
   - tree node frame
   - value page frame
   - hole page（若 readonly_frame_cache 命中）
   - WAL tail / in-flight page
   - tree flush writeback frame

2. **`spdk_iobuf` 只用于 request-scope DMA scratch**
   - 大 scan 的临时拼装页
   - 临时 encode/decode buffer
   - 某些不进入 cache、用完即还的 read/write 辅助页

3. **任何一旦进入 `readonly_frame_cache` / `dirty_frame_set` 的对象，都不能继续以 `spdk_iobuf` waiter 语义管理**

#### 推荐映射

| 对象 | 推荐底层机制 | 原因 |
|------|-------------|------|
| tree node frame | `spdk_mempool` | 固定尺寸、长期 resident、可被 pin |
| full value page frame | `spdk_mempool` | 固定尺寸、可能进入只读缓存 |
| resident hole page（value_alloc_sched cache hit） | `spdk_mempool` | 若 readonly_frame_cache 命中 hole page，可直接打开为 dirty_hole_fill |
| WAL tail frame | `spdk_mempool` + reserved quota | 必须有保底，不接受被临时请求 buffer 挤占 |
| tree flush writeback frame | `spdk_mempool` | owner 明确、burst 可预算 |
| 临时 scratch / 拼装页 | `spdk_iobuf` | 请求级、短命、适合等待可用 buffer |
| bootstrap / 非规则大 span | `spdk_dma_malloc_socket()` | 冷路径，不适合常驻池化 |

#### 实现建议

1. 把 `page_frame` payload 池和 `spdk_iobuf` 明确拆开。
2. `spdk_iobuf_channel` 可以按 scheduler / thread 建立，用于请求态 scratch。
3. `spdk_iobuf` 失败等待语义只能出现在 pipeline 可以自然 suspend/resume 的位置，不能出现在持有 dirty frame 独占权的关键区。
4. 若某个读请求先用 `spdk_iobuf` 做临时接收，再决定把页晋升为 resident frame，则必须复制/迁移到 `spdk_mempool` 持有的 frame payload，不能直接把 iobuf waiter buffer 变成长生命周期 cache entry。

### 5.12 DMA Budget 的分账方式

底层 DMA bytes 池按 size class 共享，但逻辑预算必须分账，避免 read cache 把 dirty/writeback 所需 DMA 全吃光。

建议至少拆四类 quota：

1. `readonly_frame_quota`
   - tree node cache、full value page cache

2. `readonly_value_page_quota`
   - `clean_readonly` / `clean_allocatable` value pages in readonly_frame_cache

3. `dirty_frame_quota`
   - open append pages、hole-fill pages、tree flush write buffers

4. `wal_reserved_quota`
   - WAL tail / WAL in-flight 保留池，不能被普通读 cache 挤占

约束：

1. `dirty_frame_quota + wal_reserved_quota` 必须有 hard floor。
2. clean cache 只能消费剩余 DMA 预算。
3. DMA 压力到来时，回收顺序必须是：
   - 先驱逐 `clean_readonly`
   - 继续驱逐 clean value page frames（hole 元数据由 value_alloc_sched 维护，不受影响）
   - 再提前 flush dirty open pages
   - 最后才进入写侧反压

### 5.13 `page_frame` 与 DMA Pool 的关系

因此，`page_frame` 的正确理解应当是：

```cpp
struct page_frame {
    frame_id id;
    frame_state st;

    void* dma_buf;                                  // 来自 dma_pool
    uint32_t byte_len;

    uint32_t pin_count;
    bool crc_valid;
};
```

这里的 `dma_buf` 不是一次性临时申请的 staging buffer，而是：

1. 读 miss 填充后的 resident frame payload
2. dirty open page 的可写 payload
3. writeback 的直接数据源
4. tree page 原地 parse 与 value copy-out 前校验所依赖的底层内存

## 6. Cache 视图与 Pool 视图

统一 frame 抽象之上，必须拆成不同视图。v1 至少有四类。

### 6.1 Read-Only Frame Cache

用途：缓存可重读的 clean page image。

典型对象：

1. tree node page
2. 写满的 value page
3. seal 之后仅用于恢复/诊断读取的 WAL page（若实现需要）

概念结构：

```cpp
struct readonly_frame_cache {
    // key = frame_id, value = cache-owned raw page_frame*
    // 消费者通过 frame_pin 保活，不通过 intrusive_ptr
    lru_or_clock<frame_id, page_frame*> entries;
};
```

约束：

1. 只收纳 `clean_readonly` frame。
2. `pin_count > 0` 时不可驱逐。
3. 驱逐后只丢 page image，不改变 allocator / reclaim / correctness 状态。
4. tree node cache 是它的一个逻辑视图，不需要单独发明另一套生命周期语义。

### 6.2 Hole Page 的分层管理（value_alloc_sched 集中）

> **模型变更**：value 分配与持久化完全集中在 `value_alloc_sched` 上（leader-follower 模式，见 `write_path_and_pipeline.md` §5 和 `runtime_state_machine.md` §6）。`front_sched` 不再参与 value 分配、DMA 填充或 hole reuse。

**1. value_alloc_sched 持有 hole 元数据**

`value_alloc_sched` 通过 `hole_page_list` 跟踪所有可复用的 partially-free page：

```cpp
// value_alloc_sched 内部
struct hole_page_list {
    // class_idx -> non-resident page descriptors
    flat_hash_map<uint16_t, intrusive_list<hole_page_descriptor>> by_class;
};
```

这是 placement metadata，不是 cache。`value_alloc_sched` 知道哪些页有空洞、空洞分布如何。

**2. value_alloc_sched 拥有 open frames 并管理 DMA 页像**

`value_alloc_sched` 持有 per-class `open_frames`（见 `runtime_state_machine.md` §6.3）。当 `persist_put_values` 内部需要打开一个 hole page 时：

1. 先检查 `value_alloc_sched` 本地的 `readonly_frame_cache` 是否命中该 page_base。
2. 若命中：直接从 cache 中取出 frame，转为 `dirty_hole_fill`，免去 device read。
3. 若未命中：通过本核 `nvme_sched` 读入完整页像，构造 `value_page_frame`，转为 `dirty_hole_fill`。
4. 该 frame 成为 `open_frames[class_idx]`，后续同 class 的 PUT 继续消费它。

收益：

1. 页像的 read-only residency 由 `value_alloc_sched` 本地的 `readonly_frame_cache` 管理。
2. hole 元数据和 DMA 写入 ownership 都在同一个单线程 scheduler 上，无跨 sched 协调。
3. hole page cache 命中时免去 device read，和旧模型收益相同。

### 6.3 Dirty/Writeback Set

用途：保存”当前不能被当作 cache 驱逐”的可写页像。

典型对象与归属：

| 对象 | Owner |
|------|-------|
| value open page（dirty_append / dirty_hole_fill） | `value_alloc_sched`（per-class open_frames） |
| WAL tail frame | `front_sched`（per-stream） |
| tree flush writeback frame | `tree_sched` |

概念结构（每个 owner scheduler 各自持有一份）：

```cpp
struct dirty_frame_set {
    intrusive_list<page_frame> open_append;
    intrusive_list<page_frame> open_hole_fill;
    intrusive_list<page_frame> inflight_writeback;
};
```

约束：

1. dirty frame 不能按 LRU 驱逐。
2. dirty frame 的 owner 必须唯一；不得让读 cache shard 共享其可写视图。
3. dirty frame 只能通过 writeback completion 转成 clean frame，或在写失败时触发既有错误路径。

### 6.4 Non-Resident Placement Metadata

不是所有可复用空间都需要保留 resident frame。

这类对象属于 allocation metadata，而不是 cache：

1. `whole_page_pool`：完整空页的 paddr 队列
2. `hole_page_list`：partially-free 页的 **page-level** 描述符队列
3. `extent_free_pool`：multi-LBA dead extent 的 paddr 队列
4. `generic_free_spans`：whole-free 但暂时无法唯一归入某个 class pool 的 owner-local free region 列表

```cpp
// sub-LBA 空洞的 non-resident 元数据：按页为单位，不按 slot
struct hole_page_descriptor {
    paddr page_base;
    uint16_t class_idx;
    bitset<MAX_SLOTS_PER_PAGE> free_mask;            // 哪些 slot 是空闲的
};

// whole-free region 的 owner-local 元数据：必要时由 recovery install 种入
struct free_span_descriptor {
    paddr base;
    uint32_t span_lbas;
    uint16_t class_idx_or_invalid;                  // UINT16_MAX = 暂无 class hint
};
```

语义：

1. 它们说明”哪些位置值得复用”。
2. 它们本身不带页像。
3. 从 `hole_page_list` 取到一个 descriptor 后，如果对应页不 resident，就需要先把该页读成 `value_page_frame`（用 `free_mask` 初始化 `free_bitmap`），然后转入 `dirty_hole_fill`。
4. **page-level claim**：取出一个 descriptor 就 claim 了整页的所有 free slots。不会有同一页的 sibling slot 散落在其他 scheduler 的队列中。
5. `generic_free_spans` 是 allocator 内部的缓冲层：当某块 whole-free region 语义上确定 free，但 class 暂时无法唯一判定时，先保存在这里，后续再归桶或切分；它不是 recovery 对外暴露的独立 owner。

**owner 规则**：所有 placement metadata（`whole_page_pool`、`hole_page_list`、`extent_free_pool`、`generic_free_spans`）和 open frames 统一归 `value_alloc_sched` 所有（单一 owner）。`front_sched` 不参与 value 分配或持久化。

因此，value 写侧的两套状态都在 `value_alloc_sched` 上：

1. **non-resident free-space metadata**：告诉 allocator 位置和页内空洞分布。
2. **resident frame state**（open_frames + 本地 readonly_frame_cache）：告诉写路径当前代价和后续写法。
3. boot recovery 只把 `occupied` truth 交给 owner；这些 placement metadata 都由 `value_alloc_sched.install_recovered_state()` 内部重建。

## 7. Scheduler Ownership 与 Sharding

统一模型不等于做一个全局可变 hash 表。所有可变状态仍然必须有单一 owner。

### 7.1 归属规则

1. `value_alloc_sched`（value 读写唯一 owner）
   - 拥有所有 value placement metadata：`whole_page_pool`、`hole_page_list`、`extent_free_pool`、`generic_free_spans`、allocator head
   - 拥有 per-class `open_frames`（dirty_append / dirty_hole_fill DMA frames）
   - 拥有**全局唯一**的 `readonly_frame_cache`（value_page domain）：同时服务写侧 hole reuse 和读侧 tree-path value read
   - 通过本核 `nvme_sched` 提交 value FUA 写入和 value page 读取
   - 提供 `read_value(value_ref)` 与 `read_page_values(value_read_group)` 接口：前者用于 Point GET，后者用于 MultiGet / Scan 的按页批读
   - 最佳实践是把它部署在独占核心上，但这属于部署建议，不是 correctness 前提

2. `front_sched(owner)`
   - 拥有本 owner 的 WAL tail frame
   - **不**拥有 value_page cache（tree-path value read 统一由 `value_alloc_sched` 服务）
   - **不**拥有 value open pages 或 placement metadata

3. `tree_read_domain`
   - 拥有本 shard 的 `readonly_frame_cache`（tree_node domain）
   - 由同 shard 的 `tree_lookup_sched` / `tree_worker_sched` 共享
   - 服务普通读路径的 tree traversal，以及 flush 中的 old-leaf read

4. `tree_lookup_sched`
   - 执行普通读路径的 tree traversal，以及 flush 中的 `keys_to_leaf_groups()`
   - 访问所属 `tree_read_domain` 的 `readonly_frame_cache`
   - **不**拥有 value_page cache（tree hit value 后路由到 `value_alloc_sched` 读取）

5. `tree_worker_sched`
   - 执行 flush 中的 old leaf read / decode / candidate materialization
   - 访问所属 `tree_read_domain` 的 `readonly_frame_cache`
   - **不**拥有 allocator、manifest mutation 或 retire list

6. `tree_sched`
   - 拥有 tree flush write buffers
   - 拥有 `checkpoint_guard` retire lists
   - 决定 tree slot / old tree-visible value 何时真正回收
   - **不**拥有 tree-node `readonly_frame_cache`

7. read cache local shard（各 scheduler 内部）
   - 只拥有 clean frame 的 local residency 索引
   - 不拥有 allocator 元数据
   - 不拥有 dirty 可写页

### 7.2 为什么 hole 元数据不能挂在读 cache 下

hole 的 free-slot 分布、页是否已被 writer 打开、写满后需从可分配集合摘除——这些都是写侧 allocator 状态，必须有单一 owner。

新模型下，这些元数据、DMA 写入 ownership 和 value_page 读缓存统一归 `value_alloc_sched`（单线程，天然互斥）。最佳实践是把它部署在独占核心上。`value_alloc_sched` 的 `readonly_frame_cache` 同时服务写侧 hole reuse（免去 device read）和读侧 tree-path value read（`read_value` / `read_page_values` 接口）。`front_sched`、`tree_lookup_sched` 和 `tree_worker_sched` 都不持有 value_page cache。

### 7.3 允许重复 residency，但不允许重复可写 ownership

**tree_node domain**：同一个 tree node page 可以在多个 `tree_read_domain` shards 的 `readonly_frame_cache` 中各自驻留。这只是空间换性能。同一个 shard 内由 `tree_lookup_sched` / `tree_worker_sched` 共享该 cache。

**value_page domain**：`value_alloc_sched` 是 value_page cache 的**唯一 owner**。没有跨 shard 重复 residency，因此不存在 value_page 的 stale hit 问题。`value_alloc_sched` 内部 hole-fill writeback 后直接更新本地 cache，不需要跨 shard invalidate。

通用约束：

1. dirty frame 的可写 ownership 只能有一份，且只在 `value_alloc_sched` 上（value pages）或 `front_sched` 上（WAL tail）。
2. tree 地址只有在完成跨全部 `tree_read_domain` shards 的 `tree_node` invalidate barrier 后，才允许进入 `free_ranges` 等可复用池（见 §10.2）。
3. value 地址的复用安全由 `value_alloc_sched` 单 owner 天然保证：旧 clean frame 在 writeback / 回收时由同一线程直接更新或删除，不需要跨 shard 协调。

## 8. Value Placement 与 Cache/Page State 的耦合

### 8.1 设计原则

value placement 不能只看”哪些地址空着”，还必须看”这些地址当前在内存里是什么状态”。

因为两者代价不同：

1. fresh page：不需要读旧内容，但消耗新空间。
2. whole-page pool：复用已完全死亡的页，也不需要读旧内容。
3. hole page（cache miss）：需要先读整页，再做 page-based 写回。
4. hole page（cache hit）：`value_alloc_sched` 本地 `readonly_frame_cache` 命中，不需要 hole reuse 前的那次 read，只需要后续 writeback。
5. 当前已打开的 dirty page（open_frames）：代价最低，应优先继续填充。

### 8.2 Policy Mode

`hole_reuse_watermark` 只控制”何时进入 partial-hole reuse 模式”。`value_alloc_sched` 根据该阈值在 `persist_put_values` 内部决定分配策略，全部在单线程内完成。

```cpp
struct value_placement_config {
    float hole_reuse_watermark = 0.80f;
};
```

模式定义：

1. `fresh_first`
   - `used_ratio < hole_reuse_watermark`
   - 不主动消费 partial hole；优先使用 open page、whole-page pool、fresh space。

2. `hole_first`
   - `used_ratio >= hole_reuse_watermark`
   - partial hole 进入优先级体系。

### 8.3 Candidate Priority

所有优先级判断和分配都在 `value_alloc_sched` 内部完成（单线程，见 `runtime_state_machine.md` §6.4）。

#### `fresh_first` 模式

```text
1. 当前 open_frames[class_idx]（只要仍有 free slot）      ← value_alloc_sched 本地
2. whole_page_pool / extent_free_pool                      ← value_alloc_sched 本地
3. fresh bump page                                         ← value_alloc_sched 本地
4. hole page（仅作最后回退）                                 ← value_alloc_sched 本地
5. space exhausted
```

#### `hole_first` 模式

```text
1. 当前 open_frames[class_idx]（只要仍有 free slot）      ← value_alloc_sched 本地
2. whole_page_pool / extent_free_pool                      ← value_alloc_sched 本地
3. hole page                                               ← value_alloc_sched 本地
4. fresh bump page                                         ← value_alloc_sched 本地
5. space exhausted
```

解释：

1. `whole_page_pool` 与 partial hole 不是一回事；它复用的是完整空页，成本仍接近 fresh page。
2. hole page 分配后，`value_alloc_sched` 检查本地 `readonly_frame_cache` 是否命中——命中则免去 device read，未命中则通过本核 `nvme_sched` 读入。这是 cache 的自然行为，不影响优先级决策。
3. hole page 是否先于 fresh page，由 `hole_reuse_watermark` 控制的模式决定，而不是由硬件类型直接决定。
4. future 若底层写方法变细粒度，改变的是”某类 candidate 的写代价”，不是 placement policy 的框架。

### 8.4 打开一个 Non-Resident Hole Page

以下流程完全在 `value_alloc_sched` 内部执行（`persist_put_values` → `alloc_page` → `prepare_frame`）：

```text
1. alloc_page(class_idx) 从 hole_page_list 取到一个 hole_page_descriptor
2. 检查 value_alloc_sched 本地 readonly_frame_cache 是否命中该 page_base
3. 若命中：直接取出 frame；若未命中：通过本核 nvme_sched 读入完整页像
4. 构造 value_page_frame，用 descriptor 的 free_mask 初始化 free_bitmap
5. frame.st = dirty_hole_fill
6. 设为 open_frames[class_idx]
7. 后续同 class PUT 继续消费该 frame
8. 若写后仍有 free slot：保留为 open_frames[class_idx] 到下一轮 persist_put_values
9. 若写满：writeback（FUA）后转成 clean_readonly 进入本地 readonly_frame_cache
```

关键点：

1. 一旦某个 hole page 已经成为 `open_frames[class_idx]`，后续同 class 的分配优先继续消费它。
2. 这正是在 `readonly_frame_cache` 中保留 hole page 页像的意义：当 open frame 写满后变成 clean_readonly 留在 cache，下次该页有新空洞被回收并再次分配时，直接 cache hit 免去 device read。

### 8.5 `slab_page_buf` 的统一解释

旧文档中的 `slab_page_buf` 可以被看成 `value_alloc_sched` 的 open_frames 中的一个 frame：

1. `frame.dom = value_page`
2. `frame.st = dirty_append`
3. `value_page_frame.mode = append`

也就是说，它不是一个孤立的特殊 buffer，而是 `value_alloc_sched` 持有的 value page runtime 的一种具体状态。

### 8.6 实现注记 — 当前 `writable_pages_` 与 spec 概念的对应

当前 `apps/inconel/value/scheduler.hh` 把 §6.3 的 `open_frames[class_idx]` 与 §6.4 的 `hole_page_list` partial-hole resident continuation 这两层概念，工程化合并成同一个 per-class 队列 `writable_pages_[ci]`。它的语义是：

1. 队列中每个 entry 代表一页 “已经 durable on NVMe、但仍然有 free slot、由 scheduler 在本地保留页像以便下一轮 persist 继续填充”。
2. 取页时按 LIFO（`pop_back`）走，保证最热的 partial page 最先被复用。
3. round 提交后，若页仍有 free slot，commit 路径把它放回 `writable_pages_[ci]`；若 round 失败，rollback 路径把原始 `free_mask` 一起恢复回 `writable_pages_[ci]`。
4. round 内 `value_page_source::writable` 这个枚举值的命名与 `writable_pages_` 一一对应：从这里取出去的页，rollback 也只能回到这里。

它**不是**完整意义上的：

- spec `hole_page_list`：那是 non-resident placement metadata（带 `free_mask` 的 `hole_page_descriptor`），由 allocator 在内存中追踪所有 partially-free pages 的 free slot 分布；当前实现里没有独立的 `hole_page_list`，因为 v1 没有跨 round 的 hole 回收路径。
- spec per-class 单一 `open_frame`：那要求每个 class 同时只有一帧在被 fill，所有 `dirty_append` / `dirty_hole_fill` 状态切换都集中在那一帧上；当前实现允许同 class 多个 partial page 在 `writable_pages_[ci]` 里排队，是 spec 的一个超集，等到引入完整 frame state machine 时再收紧。

未来引入 frame state machine（`dirty_append` / `dirty_hole_fill` / `clean_readonly` 的显式状态、`hole_page_list` 的非 resident 描述符、每 class 单一 open frame）时，`writable_pages_` 会被拆成两套结构：`hole_page_list` 负责非 resident metadata，per-class `open_frames` 负责正在 fill 的 DMA frame。本步只补这层语义说明，不改实现。

## 9. 读路径与 Zero-Copy 视图

### 9.1 Tree Lookup

tree lookup 的固定语义不变：

1. `read_handle` pin `publish_catalog -> prs -> guard -> manifest`
2. manifest 解析 exact slot
3. 请求被路由到某个 `tree_lookup_sched`
4. 该 scheduler 在自己的 tree-node `readonly_frame_cache` 上命中/回填
5. 解析在 frame 上原地进行

因此，tree 侧真正需要的不是“另一套独立对象模型”，而是：

1. `tree_manifest` 负责结构正确性。
2. tree node frame 负责 residency 和 zero-copy page parse。

### 9.2 Value Read（`value_alloc_sched.read_value`）

对 tree hit value，读路径不再把 `value_page` frame 生命周期暴露给调用方，而是统一路由到 `value_alloc_sched`：

1. Point GET：直接调用 `read_value(value_ref)`。
2. MultiGet / Scan：先在调用方按 `frame_id` 把同页 refs 分组，再调用 `read_page_values(value_read_group)`。
3. owner 内部先查 `open_frames`，再查本地 `readonly_frame_cache`。
4. 若仍 miss，则通过本核 `nvme_sched` 读整页并回填本地 cache。
5. 在同一个 resident frame 上解析多个 `value_object_header`，校验 magic/body_len/body_crc。
6. copy body bytes，向调用方返回 owning bytes。

语义：

1. `value_page` frame 的 residency 生命周期完全封闭在 `value_alloc_sched` 内部。
2. 上层不会拿到 `frame_pin` 或 `value_view`；因此不需要跨 scheduler 管理 DMA frame 生命周期。
3. 对 sub-LBA value，多个 value objects 仍共享同一 resident frame；批量路径下同页 refs 只做一次 page lookup / page miss 判定。
4. 对 multi-LBA value，仍可在 owner 内部扩成 span frame；不改变 `read_value(value_ref) -> owning bytes` / `read_page_values(group) -> owning bytes[]` 抽象。

### 9.3 `hot_blob` 仍然独立

memtable hit 的语义不变：

1. 命中 memtable value 时，直接返回 `hot_blob`。
2. 不允许退化成对 `value_ref` 的 SSD 读。
3. `hot_blob` 的存在不依赖 value page cache 是否命中。

因此，`hot_blob` 不是 unified page cache 的一部分。它属于 correctness-carried hot data。

### 9.4 对旧文档里 `value_cache` 的解释

若其他详细设计里仍出现“`value_cache = value_ref -> hot_blob`”这种简写，应理解为两层：

1. **底层**：page/frame cache，负责 resident page image 与 `read_value()` / `read_page_values()` 的 cache hit / copy-out。
2. **上层可选**：materialized value bytes/blob 的复用索引。

v1 的基础缓存层应以前者为主；后者只是接口适配优化，不是必须的 correctness 路径。

## 10. 生命周期、回收与失效

### 10.1 Correctness Owner 与 Frame 的释放条件不同

1. `publish_catalog / prs / guard / manifest`
   - 由 reader pin 控制释放。
   - 与 frame cache 命中率无关。

2. `hot_blob`
   - 由 memtable entry 与额外引用控制释放。
   - 与 page eviction 无关。

3. clean frame
   - `pin_count == 0` 时可驱逐。
   - 驱逐不改变 durable state。

4. dirty frame
   - 只能在 writeback completion 后离开 dirty 集合。
   - 不允许因内存压力直接丢弃。

### 10.2 地址复用与 Stale Hit

任何 page/frame cache 都不能忽略“物理地址会被复用”这个事实。

基础约束只有一条：

**同一物理地址被回收到可重用状态后，旧页像不能再以 cache hit 身份冒充新对象。**

v1 在这里**不采用 `reuse_epoch`**。冻结协议如下：

1. `readonly_frame_cache` 继续用裸 `frame_id = { base, span_lbas, domain }` 做 key。
2. 但在地址重新进入可复用池之前，owner 必须先完成跨 shards 的 invalidate barrier。
3. invalidate barrier 的语义是：
   - 向所有可能持有该 domain `readonly_frame_cache` shard 的 scheduler 广播 invalidate；
   - 每个 shard 删除命中的 clean frame；若此时仍发现 pinned frame，说明 retire / reclaim 生命周期判断有 bug；
   - 只有全部 ack 返回后，该地址才允许被重新发布给读路径或重新进入 free pool。

分域冻结：

1. `tree_node`（需要跨 shard invalidate barrier）
   - 普通 old slot 不会单独重写复用；只有 enclosing range 进入 `free_ranges` 前才需要 barrier。
   - 因此 tree 侧 barrier 的粒度是”整个 old range 的所有 slot 地址”。
   - 广播目标包括所有 `tree_read_domain` 的 tree-node cache shards；`tree_sched` 不在广播目标内，因为它不再持有 tree-node read cache。
2. `value_page`（**无需跨 shard invalidate barrier**）
   - `value_alloc_sched` 是 value_page cache 的唯一 owner。hole-fill writeback 后直接更新本地 cache；whole page / extent 回收时直接从本地 cache 删除。
   - 因为不存在其它 shard 持有 value_page 的 clean 副本，不需要广播 invalidate。
   - 这是 value_page cache 集中到 `value_alloc_sched` 的核心简化收益。

### 10.3 Hole Page 相关 Frame 的回收

`value_alloc_sched` 的 `readonly_frame_cache` 中可能存在 partially-free value page 的 clean 页像。这类 frame 的回收遵循与普通 clean frame 相同的规则：

1. `pin_count == 0` 时可按 LRU 驱逐。
2. 驱逐只丢页像，不影响 `value_alloc_sched` 持有的 hole 元数据。
3. 若页已经完全死亡并被回收到 `whole_page_pool`，`value_alloc_sched` 直接从本地 cache 删除对应 frame（单 owner，无需跨 shard invalidate）。
4. `dirty_hole_fill` 状态的 frame 不能驱逐，只能在 writeback completion 后退出 dirty set。writeback 完成后 `value_alloc_sched` 直接更新本地 cache，新 `value_ref` 即可对外可见。

### 10.4 Crash 语义

所有 frame/cache/page runtime 状态都属于 runtime 对象：

1. crash 后全部丢失。
2. recovery 不尝试恢复“哪一页当时 resident”。
3. recovery 只恢复逻辑 live 数据与 allocator/free-pool 的 clean runtime 状态。
4. `value_alloc_sched` 的 placement metadata、read-only frame cache、dirty open frame 集合都在 boot 后重新建立。

## 11. 内存预算与反压

内存预算不能只看“cache 占了多少”，而要按类别分账。

建议至少区分四个 bucket：

1. correctness-pinned objects
   - `publish_catalog`、`prs`、`guard`、`manifest`
   - 不能通过 eviction 缩减，只能观测与告警

2. hot data
   - memtable entries、`hot_blob`
   - 主要受写入速率、seal 频率、长读影响

3. clean frames
   - read-only frame cache（含可能命中 hole page 的 clean 页像）

4. dirty/writeback frames
   - open append pages、hole-fill pages、WAL tail、tree write buffers

压力处理顺序应为：

```text
1. 先驱逐 unpinned clean_readonly frames
2. 继续驱逐 clean frames（含 value page 页像；hole 元数据由 value_alloc_sched 独立维护，不受 cache eviction 影响）
3. 再提前 flush 一部分 dirty open pages
4. 若仍不足，触发既有写侧 backpressure / long-read 反压策略
```

注意：

1. hole page 的 clean 页像被 cache 驱逐后，`value_alloc_sched` 的 `hole_page_list` 元数据不受影响；下次分配该 hole 时只是需要重新读页。
2. 不能为了省内存而把 `hot_blob` 从 live memtable entry 上剥离。
3. 不能为了省内存而驱逐 `tree_manifest` 或已被 reader pin 的 `checkpoint_guard`。

### 11.1 Hole Page 写完后的归还

当 `value_alloc_sched` 完成 hole page 的 writeback（FUA completion）后，若该页仍有 free slots：

```text
return_partial_page(frame):
    assert frame->st was dirty_hole_fill, now writeback completed

    // 1. 从 free_bitmap 生成一个 page-level hole descriptor
    desc = hole_page_descriptor {
        page_base = frame->id.base,
        class_idx = frame->class_idx,
        free_mask = frame->free_bitmap,      // 剩余 free slot 信息
    }

    // 2. 内部调用 handle_return_page（value_alloc_sched 本地操作）
    handle_return_page(desc.page_base, desc.class_idx, desc.free_mask)

    // 3. frame 转为 clean_readonly，进入 value_alloc_sched 本地 readonly_frame_cache
    //    无需跨 shard invalidate（value_alloc_sched 是 value_page cache 唯一 owner）
    frame->st = clean_readonly
    readonly_frame_cache.insert(frame)
```

若该页已写满（`free_count == 0`），直接转为 `clean_readonly` 进入 cache，无需调用 `return_page`。

**page-level 不变量**：同一个 partially-free page 在任一时刻只存在于以下之一：
- `value_alloc_sched` 的 `hole_page_list` 中的一个 `hole_page_descriptor`（free_mask 在 descriptor 上）
- `value_alloc_sched` 的 `open_frames[class_idx]`（dirty_hole_fill，正在被写入）

`readonly_frame_cache` 中的 clean 页像只是 residency 缓存，不参与 allocation 决策。

## 12. 与现有术语的对应关系

本文不是要另起炉灶，而是把现有散落术语收敛到统一模型。

| 旧术语 | 在本文中的统一解释 |
|--------|------------------|
| `node_cache` | `readonly_frame_cache` 在 tree domain 的逻辑视图；运行时由 `tree_read_domain` 持有，供 `tree_lookup_sched` / `tree_worker_sched` 共享 |
| `value_cache` | 底层应理解为 value frame residency；上层可再叠加 materialized view/blob 索引 |
| `slab_page_buf` | `value_page_frame { st = dirty_append, mode = append }` |
| hole reuse RMW 临时页 | `value_page_frame { st = dirty_hole_fill, mode = hole_fill }` |
| `tail_buf` | `page_frame { dom = wal_page, st = dirty_append }` |
| `whole_page_pool` | non-resident placement metadata，归 `value_alloc_sched` 所有，不是 cache |
| `hole_page_list` | non-resident placement metadata，归 `value_alloc_sched` 所有，per-page descriptor |
| `generic_free_spans` | owner-local non-resident placement metadata，用于承接暂时无法归桶的 whole-free region |
| `slot_free_list`（旧） | 已改为 `hole_page_list`：per-page descriptor，不是 per-slot 条目 |
| `resident_hole_pool`（旧） | 已废除；hole page 的 residency 由 `value_alloc_sched` 的 `readonly_frame_cache` 自然覆盖 |
| `hot_blob` | correctness-carried hot data，不属于 page cache |
| `publish_catalog / prs / guard / manifest` | correctness owner，不属于 cache |

## 13. 应冻结的不变量

1. 不是所有运行期对象都叫 cache；correctness owner、hot data、frame residency、placement metadata 必须分层。
2. 统一的是 frame/page 运行时抽象，不是一个全局锁式单层 LRU。
3. dirty page 与 allocatable hole page 都是 placement state，不能按普通 read cache 对待。
4. hole page 的 residency 由 `value_alloc_sched` 本地的 `readonly_frame_cache` 覆盖；`value_alloc_sched` 集中管理 hole 元数据和 open frames。
5. `hot_blob` 独立于 page cache；memtable hit 绝不能退化成 SSD 读。
6. page/frame cache 必须处理物理地址复用，不能只靠裸 `paddr` 命中。
7. crash 后所有 cache/frame/runtime residency 都丢弃；recovery 不依赖它们。
8. future 若底层写粒度改变，应替换的是 write method 的代价模型，不是 correctness owner、placement policy 框架或 frame 分类本身。
