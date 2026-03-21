# Sider 设计文档

## 1. 架构总览

```
                      TCP Clients (redis-cli, jedis, ...)
                            │ RESP2
        ┌───────────────────┼───────────────────┐
        │  Core 0           │  Core 1    ...  Core N-1
        │                   │                   │
        │  ┌─────────┐     │  ┌─────────┐     │  ┌─────────┐
        │  │TCP sched │     │  │TCP sched │     │  │TCP sched │
        │  │(accept+  │     │  │(session) │     │  │(session) │
        │  │ session) │     │  └────┬────┘     │  └────┬────┘
        │  └────┬────┘     │       │           │       │
        │       │           │       │           │       │
        │  ┌────▼────┐     │  ┌────▼────┐     │  ┌────▼────┐
        │  │  Store   │◄───┼──│  Store   │◄───┼──│  Store   │
        │  │scheduler │────┼─►│scheduler │────┼─►│scheduler │
        │  │(shard 0) │     │  │(shard 1) │     │  │(shard N) │
        │  └────┬────┘     │  └────┬────┘     │  └────┬────┘
        │       │           │       │           │       │
        └───────┼───────────┼───────┼───────────┼───────┘
                │           │       │           │
        ┌───────▼───────────▼───────▼───────────▼───────┐
        │           NVMe Schedulers (per core per disk)  │
        │  ┌──────┐ ┌──────┐ ┌──────┐     ┌──────┐     │
        │  │Disk 0│ │Disk 0│ │Disk 1│ ... │Disk M│     │
        │  │Core 0│ │Core 1│ │Core 0│     │Core N│     │
        │  └──────┘ └──────┘ └──────┘     └──────┘     │
        └───────────────────────────────────────────────┘
```

### 1.1 Scheduler 清单

| Scheduler | 类型 | 实例数 | 职责 |
|-----------|------|--------|------|
| TCP accept | 框架 | 1 (core 0) | 监听端口，accept 连接，分发到 session scheduler |
| TCP session | 框架 | N (每核1个) | 管理本核连接的 recv/send，SO_REUSEPORT 可选 |
| Store | **自建** | N (每核1个) | 哈希表、LRU、内存记账、淘汰决策、TTL 检查 |
| NVMe | 框架 | N×M (每核每盘1个) | SPDK 异步读写 |
| Task | 框架 | N (每核1个) | 通用任务（连接 handler 运行在此） |

### 1.2 每核布局

每个核心独立运行以下 advance 循环：

```
while (running) {
    tcp_session_sched->advance();
    store_sched->advance();       // 处理 GET/SET/DEL + 后台淘汰/过期
    nvme_sched[0]->advance();     // disk 0
    nvme_sched[1]->advance();     // disk 1 ...
    task_sched->advance();
}
```

### 1.3 连接分配

- Core 0 运行 accept scheduler，新连接按 round-robin 分配到各核心的 session scheduler
- 或：每核独立 listen 同一端口（SO_REUSEPORT），内核自动分配
- 连接一旦绑定到某核心，整个生命周期都在该核心

## 2. RESP2 协议层

### 2.1 自定义 Unpacker

框架的 `tcp_ring_buffer<unpacker_t>` 支持可插拔 unpacker。Sider 实现 `resp2_unpacker` 替代默认的 length-prefix：

```cpp
struct resp2_unpacker {
    using frame_type = net_frame;

    // 从 ring buffer 中提取一条完整的 RESP2 命令，返回原始字节
    // 不完整则返回空 frame（等待更多数据）
    static frame_type unpack(packet_buffer* buf);

    static bool empty(const frame_type& f) { return f.size() == 0; }

    // 发送响应不需要额外 framing（RESP2 自描述）
    static void prepare_send(send_req* req) {
        req->_send_vec[0] = {req->frame._data, req->frame._len};
        req->_send_cnt = 1;
    }
};
```

unpack 逻辑：
- 窥探首字节：`*` → multibulk，否则 → inline
- **Inline**：扫描 `\r\n`，找到则提取整行
- **Multibulk**：解析 `*N\r\n`，然后逐个检查 N 个 `$len\r\n..data..\r\n` 是否完整
- 所有情况下：数据不完整 → 返回空 frame，ring buffer 保留数据等下次 recv

每次 `tcp::recv(session)` 返回一条完整 RESP2 命令的原始字节。Pipelining 天然支持：一次 TCP read 可能填充多条命令到 ring buffer，unpacker 循环提取，frame_receiver 队列化，后续 recv 逐条取出。

### 2.2 Session Factory

```cpp
struct sider_factory {
    template<typename sched_t>
    using session_type = pump::scheduler::net::session_t<
        tcp::common::tcp_bind<sched_t>,
        tcp::common::tcp_ring_buffer<resp2_unpacker>,  // 自定义 RESP2 unpacker
        pump::scheduler::net::frame_receiver
    >;

    template<typename sched_t>
    static auto* create(int fd, sched_t* sche) {
        return new session_type<sched_t>(
            tcp::common::tcp_bind<sched_t>(fd, sche),
            tcp::common::tcp_ring_buffer<resp2_unpacker>(65536),  // 64KB recv buffer
            pump::scheduler::net::frame_receiver()
        );
    }
};
```

### 2.3 命令解析

recv 返回原始 RESP2 字节后，解析为命令结构：

```cpp
struct command {
    enum type_t : uint8_t { GET, SET, DEL, PING, QUIT, COMMAND_CMD, CLIENT_CMD, UNKNOWN };
    type_t type;
    // key + args 以 string_view 指向 frame 内存（零拷贝）
    std::string_view key;           // GET/SET/DEL 的 key
    std::string_view value;         // SET 的 value
    int64_t ttl_ms;                 // SET EX/PX 解析后的毫秒 TTL，-1 = 无
};
```

命令类型判断：对首个 bulk string 做大小写无关匹配（`GET`/`get`/`Get` 均合法）。

### 2.4 响应格式化

```cpp
// 预分配的常量响应（避免每次分配）
constexpr auto RESP_OK    = "+OK\r\n";
constexpr auto RESP_PONG  = "+PONG\r\n";
constexpr auto RESP_NIL   = "$-1\r\n";

// 动态响应
auto format_bulk_string(const char* data, uint32_t len) -> resp_buffer;  // $len\r\ndata\r\n
auto format_integer(int64_t n) -> resp_buffer;                           // :N\r\n
auto format_error(const char* msg) -> resp_buffer;                       // -ERR msg\r\n
```

## 3. Store Scheduler

核心组件，每核一个实例，持有该分片的全部状态。

### 3.1 Slab 内存页

内存按 4KB 页组织，每页是固定大小 slot 的 slab：

```
Size classes: 64B, 128B, 256B, 512B, 1KB, 2KB, 4KB

128B class 页 (4KB = 32 slots):
┌──────┬──────┬──────┬──────┬─── ... ──┐
│ 128B │ 128B │ 128B │ free │          │
│slot 0│slot 1│slot 2│      │          │
└──────┴──────┴──────┴──────┴─── ... ──┘
bitmap: [1][1][1][0][0]...    ← 1=占用, 0=空闲
```

- value 向上取整到最近的 size class（100B → 128B slot）
- 分配：选 size class → 找有空 slot 的页 → bitmap 取空位 → O(1)
- 释放：bitmap 置 0 → O(1)
- 同一页内 slot 大小相同，无外部碎片
- 内部浪费平均 ~25%（远优于 per-value 占整页的 97%）
- 页从 DMA 内存池分配（见 3.1.1），可直接 DMA 到 NVMe

### 3.1.1 DMA 内存池

所有 slab 页必须是 DMA 安全内存（SPDK NVMe DMA 要求）。

**为什么不能按需分配**：`spdk_dma_malloc` 每次调用需要 hugepage 映射 + IOMMU 注册，单次开销数十微秒，远高于一次 GET/SET 的 3μs。如果每分配一个 4KB 页都调一次，性能不可接受。

**为什么不自建内存池**：可以用 `spdk_dma_malloc` 一次性分配大块内存，然后自己用 `mpmc::queue` + per-core 本地缓存管理 4KB 页的分配/释放。但 DPDK 的 `rte_mempool`（通过 `spdk_mempool` 封装）已经精确实现了这套机制——hugepage 预分配 + per-lcore 本地缓存 + lock-free 全局 ring。没必要重复造轮子。

**为什么能用 `spdk_mempool`**：`spdk_mempool` 的 per-core cache 依赖 DPDK 的 `rte_lcore_id()` 识别当前线程。如果用普通 `std::thread`，DPDK 不认识这些线程，per-core cache 不生效，每次 `get`/`put` 都走全局 CAS，失去本地缓存的性能优势。因此框架的 `share_nothing.hh` 已扩展支持注入线程启动方式——通过 DPDK launcher 启动 lcore 线程，使 `rte_lcore_id()` 正确返回核心编号，`spdk_mempool` 的 per-core cache 即可正常工作。

**架构**：

```
┌───────────────────────────────────┐
│  spdk_mempool("sider_pages")      │
│                                   │
│  ┌─────────────────────────────┐  │
│  │  全局 lock-free ring         │  │
│  │  (hugepage DMA 内存)         │  │
│  └──────────┬──────────────────┘  │
│        批量补充/归还                │
│  ┌──────┐ ┌──────┐ ┌──────┐     │
│  │ LC 0 │ │ LC 1 │ │ LC 2 │ ... │
│  │per   │ │per   │ │per   │     │
│  │lcore │ │lcore │ │lcore │     │
│  └──────┘ └──────┘ └──────┘     │
└───────────────────────────────────┘
```

```cpp
// 启动时创建
auto* pool = spdk_mempool_create("sider_pages",
    total_pages, PAGE_SIZE,
    SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,  // per-lcore 本地缓存（默认 256）
    SPDK_ENV_NUMA_ID_ANY);

// types.hh 替换两个函数，上层零改动
char* alloc_page() {
    return static_cast<char*>(spdk_mempool_get(pool));
}
void free_page(char* ptr) {
    spdk_mempool_put(pool, ptr);
}
```

| 项目 | 说明 |
|------|------|
| 内存来源 | `spdk_mempool_create` 从 hugepage 一次性预分配，运行期零系统调用 |
| per-lcore cache | DPDK `rte_mempool` 内置，`get`/`put` 优先走本地缓存，O(1) 无竞争 |
| 多核均衡 | 本地缓存空/满时自动批量从全局 ring 补充/归还，内存自然流向需求大的核心 |
| 线程要求 | 通过框架 `share_nothing.hh` 的 DPDK launcher 启动 lcore 线程 |
| 上层透明 | `slab_allocator` 只调 `alloc_page()`/`free_page()`，无需感知底层 |

### 3.2 Page Table

间接寻址层：index entry 存 `{page_id, slot_index}`，通过 page table 查实际地址。

```cpp
struct page_entry {
    enum state_t : uint8_t { FREE, IN_MEMORY, ON_NVME };
    state_t   state;
    uint8_t   size_class;      // 该页的 slot 大小类别
    uint16_t  live_count;      // 活跃 value 数
    uint32_t  hotness;         // 页热度（所有 slot 的 last_access 之和或最近访问计数）
    union {
        char* mem_ptr;         // IN_MEMORY: 指向 4KB 页
        struct { uint8_t disk_id; uint64_t lba; } nvme;  // ON_NVME
    };
    uint64_t  slot_bitmap;     // 哪些 slot 有活跃 value（最多 64 slot）
};
```

**关键优势**：淘汰时只改 page_table 中一条记录（IN_MEMORY → ON_NVME），**不需要更新任何 index entry**。

### 3.3 Index Entry

```cpp
struct entry {
    uint32_t key_hash;         // 完整 hash（rehash + 快速比较）
    uint16_t key_len;
    uint16_t value_len;
    uint32_t page_id;          // 值所在页（通过 page_table 间接寻址）
    uint8_t  slot_index;       // 页内 slot 编号
    uint8_t  type;             // STRING (Phase 0 only)
    uint32_t last_access;      // LRU 时钟
    uint32_t version;          // 版本号，每次修改递增（解决冷读竞态）
    int64_t  expire_at;        // 过期时间戳 ms，-1 = 不过期
    char*    key_data;         // 堆分配的 key 字节
};
```

**值访问**：
```
entry = hash_table.lookup(key)
page  = page_table[entry.page_id]
if page.state == IN_MEMORY:
    value_ptr = page.mem_ptr + slot_offset(page.size_class, entry.slot_index)
    → 直接读取，两次内存访问（page_table 大概率在 L1/L2 cache）
if page.state == ON_NVME:
    → 冷读路径：NVMe 读页 → 提取 value → 分配新热 slot → 更新 entry
```

### 3.4 哈希表

- 开放寻址，power-of-2 容量
- Robin Hood hashing（减少探测方差，保持高装载因子）
- 装载因子 > 0.75 时 2× 扩容
- 每核独立，无同步
- 查找：`hash(key) % capacity` → 线性探测，比较 `key_hash` + `key_len` + `memcmp(key_data)`

### 3.5 Store Scheduler 接口

store_scheduler 作为自建 scheduler，提供以下 sender：

```cpp
// 查找 key，返回 variant
//   hot_result{value_ptr, value_len}     — 热命中，可直接构建响应
//   cold_result{page_id, slot_index, size_class, version} — 冷命中，需 NVMe 读
//   nil_result{}                         — key 不存在或已过期
store->lookup(key_data, key_len)

// 写入/更新 key（值写入内存 slab 页）
//   返回 ok_result{} 或 err_result{}
store->put(key_data, key_len, value_data, value_len, ttl_ms)

// 删除 key，返回 deleted_count (0 or 1)
store->del(key_data, key_len)

// 冷 GET 后提升为热（NVMe 读完回调）
//   分配新热 slot，拷贝 value，更新 entry
//   version 不匹配则静默忽略
store->promote(key_data, key_len, version, value_data, value_len)
```

每个 sender 遵循 PUMP 自建 scheduler 六组件模式（req/op/sender/scheduler/op_pusher 特化/compute_sender_type 特化）。

### 3.6 advance() 逻辑

```
advance():
  1. drain req_queue → 处理 lookup/put/del/promote 请求
  2. 如果 memory_used > evict_urgent → 积极淘汰（每轮淘汰多个页）
  3. 如果 memory_used > evict_begin 且无 pending 请求 → 惰性淘汰（淘汰 1 页）
  4. 周期性扫描过期 key（每 N 轮 advance 扫描一批）
  return progress
```

## 4. 命令执行 Pipeline

### 4.1 Per-Connection Handler

```
tcp::join(session_sched, session)
  >> flat_map([session, ...](...) {
      return just()
        >> forever()
        >> flat_map([session](...) { return tcp::recv(session); })
        >> flat_map([...](net_frame&& frame) {
            auto cmd = parse_command(frame);
            return dispatch(cmd, session, ...);
        })
        >> reduce();
  })
  >> any_exception([session](...) { /* 清理 session */ return just(); })
```

连接内命令**顺序处理**（与 Redis 一致）。Pipelining 的性能收益来自批量网络 IO，不是并行执行。

### 4.2 命令路由 dispatch()

```cpp
auto dispatch(command& cmd, session_t* session, ...) {
    switch (cmd.type) {
        case PING:
            return tcp::send(session, RESP_PONG, 7);  // 本地处理，不跨核

        case GET: {
            // Stage 1: store 就是本核唯一实例
            // Stage 2: auto core = key_hash % N; stores[core]->...
            return store->lookup(cmd.key, cmd.key_len)  // → store_scheduler
              >> visit()                                        // variant 展开
              >> then([...](auto&& result) {
                  if constexpr (is<hot_result>(result))
                      return just(format_bulk_string(result));
                  else if constexpr (is<nil_result>(result))
                      return just(RESP_NIL);
                  else  // cold_result
                      return cold_get_pipeline(result, ...);
              }) >> flat()
              >> flat_map([session](auto&& resp) {
                  return tcp::send(session, resp.release(), resp.size());
              });
        }

        case SET: {
            return store->put(cmd.key, cmd.key_len,
                              cmd.value, cmd.value_len, cmd.ttl_ms)
              >> then([](auto&&) { return RESP_OK; })       // put 总是成功
              >> flat_map([session](auto&& resp) {
                  return tcp::send(session, resp, 5);
              });
        }

        case DEL: { /* similar to GET but simpler */ }

        default:
            return tcp::send(session, format_error("unknown command"), ...);
    }
}
```

### 4.3 冷 GET Pipeline

```
store->lookup(key)  →  cold_result{page_id, slot_index, size_class, version}
  │
  │  从 page_table[page_id] 获取 NVMe 位置
  ▼ NVMe scheduler
nvme_sched->get(page)   // 读整个 4KB 页
  │
  │  从读回的页中提取 slot_index 位置的 value
  ▼ Store scheduler (回到同一个 store)
store->promote(key, version, value_data, value_len)
  │  分配新热 slot → 拷贝 value → 更新 entry 的 page_id/slot_index
  │  旧 NVMe 页 live_count--，归零则释放
  ▼
format_bulk_string(data, len)  →  tcp::send
```

**冷读竞态处理**：
- lookup 返回 `cold_result` 时携带当时的 `version`
- NVMe 读期间，如果同一 key 被 SET/DEL → entry.version 递增
- promote 时对比 version：匹配 → 分配新热 slot，更新 entry；不匹配 → 丢弃
- GET 返回的是 NVMe 上读到的值（查询时刻的快照），promote 只是缓存优化

### 4.4 跨域数据流（完整 GET 冷路径）

```
[Core X: TCP session_sched]   recv → parse RESP2 → GET key
[Core X: Task sched]          dispatch → hash(key) = Core Y
                               ↓ per_core::queue
[Core Y: Store sched]         lookup(key) → COLD → cb(cold_result)
[Core X: pipeline continues]  构造 NVMe 读请求
                               ↓ per_core::queue
[Core Y: NVMe sched]          SPDK 读 → cb(data)
[Core Y: pipeline continues]  准备 promote
                               ↓ per_core::queue（如果 store 在不同核心，但这里 store 和 NVMe 同核）
[Core Y: Store sched]         promote(key, ver, data) → 版本匹配则更新 entry
[Core Y: pipeline continues]  format_bulk_string
                               ↓ tcp::send routes through session_sched queue
[Core X: TCP session_sched]   writev 响应到客户端
```

## 5. 冷存储（NVMe）

### 5.1 核心思路：内存页 = NVMe 页

内存 slab 页和 NVMe 页都是 4KB，1:1 对应。淘汰 = 把内存页 DMA 写到 NVMe；冷读 = 把 NVMe 页读回来提取 value。无格式转换，无打包/拆包。

```
淘汰（内存 → NVMe）:
┌─ Memory Page (4KB) ─┐        ┌─ NVMe LBA (4KB) ─┐
│ [slot0][slot1][slot2]│  DMA→  │ [slot0][slot1][slot2]│
└─────────────────────┘        └──────────────────────┘
                                page_table 状态: IN_MEMORY → ON_NVME

冷读（NVMe → 内存）:
NVMe read 4KB → 提取 slot N 的 value → 拷贝到新热页 slot → 更新 entry
```

### 5.2 空间分区

每块 NVMe 盘按核心均分：

```
Disk 总容量 = C pages (每 page 4KB)

Core 0: LBA [0, C/N)
Core 1: LBA [C/N, 2C/N)
...
Core N-1: LBA [(N-1)C/N, C)
```

每核独立管理自己的分区，无跨核竞争。

### 5.3 NVMe 空间分配

```cpp
struct nvme_allocator {
    uint64_t region_start;     // 本核起始 LBA
    uint64_t region_end;       // 本核结束 LBA
    uint64_t used_pages;       // 已用页数

    // 空闲页管理：简单 free page 栈（每页 4KB，1:1 映射，不需要连续分配）
    std::vector<uint64_t> free_pages;

    uint64_t allocate();       // pop 一个空闲页
    void free(uint64_t lba);   // push 回空闲页
};
```

与 per-key 方案不同：**不需要连续页分配**。每个 slab 页是独立的 4KB，一对一映射到一个 NVMe LBA。分配/释放都是 O(1) 的栈操作。

### 5.4 NVMe 页类型

```cpp
struct sider_page {
    uint64_t  lba;        // NVMe LBA 位置
    char*     payload;    // 直接指向内存 slab 页（DMA 安全内存）

    uint64_t get_pos() const { return lba; }
    char* get_payload() { return payload; }
    uint32_t get_size() const { return 4096; }  // 始终 4KB
    uint32_t get_io_flags() const { return 0; }
};
```

payload 直接指向要淘汰的内存 slab 页，零拷贝 DMA。

### 5.5 多盘支持

- page_table 的 `nvme` 字段记录 `{disk_id, lba}`
- 淘汰时选盘策略：轮询各盘，优先选剩余空间多的
- 读取时按 `disk_id` 定位到对应 NVMe scheduler
- 每个盘独立分区、独立分配

## 6. 淘汰

### 6.1 水位线检查

在 store_scheduler 的 advance() 中：

```
if memory_used < evict_begin:
    无操作

if evict_begin ≤ memory_used < evict_urgent:
    仅在 advance 空闲（无 pending 请求）时淘汰 1 页

if evict_urgent ≤ memory_used < evict_max:
    每轮 advance 淘汰多页（与超出比例正相关）

if memory_used ≥ evict_max:
    前台 SET 同步淘汰（保底，正常运行不应到达）
```

### 6.2 页级 LRU 选择

淘汰单位是**整页**，不是单个 key：

```
1. 从所有 IN_MEMORY 页中采样 S 页
2. 选 hotness 最低的页作为 victim
3. 淘汰该页
```

页的 `hotness` 可以是：
- 所有活跃 slot 的 `last_access` 之和
- 或最近 N 轮 advance 中被访问的 slot 次数
- 每次 GET/SET 命中某 slot 时，同时更新所在页的 hotness

### 6.3 淘汰执行

淘汰一个内存页：

```
1. 选中 victim 页（hotness 最低）
2. 分配 NVMe 页 → 得到 lba
3. 构造 sider_page{lba, mem_ptr} → nvme_sched->put(page) → 异步 DMA 写
4. 写完回调（回到 store_scheduler）→ 更新 page_table:
     state = ON_NVME
     nvme = {disk_id, lba}
     释放内存页（mem_ptr 归还 DMA 内存池）
     memory_used -= 4KB
```

**淘汰期间（DMA 写未完成）的并发访问**：

页在 DMA 写入期间仍在内存中（DMA 读取源内存，不修改它），状态标记为 EVICTING：

- **GET 命中该页上的 slot** → 直接读内存，返回热结果（页还在内存里）
- **SET 命中该页上的 slot** → 正常更新 value（如果 size class 不变，原地写；如果变了，迁移到新页）。页的 live_count 和 hotness 更新。NVMe 写完回调时检查页是否仍该淘汰（如果 hotness 明显上升，可以取消）
- **DEL 命中该页上的 slot** → 正常删除，live_count--。如果 live_count 归零，取消 NVMe 写（或写完后直接释放 NVMe 页）

### 6.4 NVMe 空间满

所有盘的本核分区都满 → 无法淘汰到 NVMe → 直接丢弃最冷页上的所有 key（内存释放），等同于 Redis 内存满时的 eviction。

## 7. TTL / 过期

### 7.1 惰性过期

每次 lookup/put/del 时检查 `entry.expire_at`：

```
if expire_at != -1 && now_ms() >= expire_at:
    delete entry
    return nil / treat as non-existent
```

### 7.2 主动过期

store_scheduler advance() 中周期性扫描：

```
每 100 轮 advance（可配置）:
    随机采样 20 个 key
    删除其中已过期的
    如果过期比例 > 25% → 再来一轮（快速清理大量过期 key）
```

与 Redis 的 activeExpireCycle 策略一致。

### 7.3 冷 key 过期

冷 key 过期时：
- 惰性：lookup 发现过期 → 删除 entry + 释放 NVMe 空间
- 主动：扫描到过期冷 key → 直接删除 entry + 释放 NVMe 空间（不需要 NVMe 读）

## 8. 内存记账

### 8.1 记账项

每核独立跟踪：

| 项目 | 计算方式 |
|------|---------|
| Slab 页 | `in_memory_page_count × 4KB`（精确，页为单位） |
| Index 开销 | `entry_count × sizeof(entry) + hash_table_capacity × sizeof(slot)` |
| Page table | `page_table_capacity × sizeof(page_entry)` |
| Key 存储 | `Σ entry.key_len`（堆分配） |
| **总计** | 以上之和，与 `evict_begin/urgent/max` 比较 |

主要开销是 slab 页。淘汰以页为单位，每淘汰一页精确释放 4KB。

### 8.2 预算分配

```
每核内存预算 = --memory / --cores
每核淘汰阈值 = 预算 × evict_begin% / evict_urgent%
```

## 9. 开发阶段

### Stage 1：单核完整功能

在单核上实现全部逻辑，充分测试后再扩展多核。

| 模块 | 范围 |
|------|------|
| 协议 | RESP2 解析（inline + multibulk）+ 响应格式化 |
| 命令 | GET, SET (含 EX/PX), DEL, PING, QUIT, COMMAND |
| 数据类型 | String only |
| 存储 | 哈希表 + 热 value 内存存储 |
| NVMe | 冷存储：淘汰写入 + 冷读提升 + 空间管理 |
| 淘汰 | 水位线模型 + 近似 LRU + 异步写 NVMe |
| TTL | 惰性过期 + 主动过期扫描 |
| 多核 | **不实现**（单核 N=1） |

单核下所有 scheduler 在同一线程 advance()，无跨核通信，便于调试：
- store_scheduler 的请求直接在本核队列
- NVMe 回调在同一 advance 循环中执行
- 所有状态可用 printf/gdb 直接查看

**验收标准**：
- redis-cli 连接正常，所有 Phase 0 命令正确
- redis-benchmark 单核对比 Redis 单线程，吞吐接近或持平
- 数据量超内存时，冷热淘汰正常工作，冷 GET 正确返回
- TTL 过期行为正确
- 长时间压测无内存泄漏、无崩溃

### Stage 2：多核扩展

单核验证通过后，增加多核支持：

| 新增模块 | 说明 |
|---------|------|
| Key 分片路由 | `hash(key) % N` → 目标核心的 store_scheduler |
| 跨核通信 | per_core::queue 路由请求，回调自动回送 |
| 连接分配 | accept 分发到各核 session scheduler |
| NVMe 空间分区 | 每核独立管理自己的 NVMe 区域 |

核心逻辑（store_scheduler、哈希表、淘汰、TTL）**零修改**。

**验收标准**：
- N 核吞吐 ≈ N × 单核吞吐（线性扩展）
- 跨核 GET/SET 结果正确
- redis-benchmark 多核 vs Redis 单线程，吞吐显著领先

### Stage 3：完善

| 模块 | 说明 |
|------|------|
| Phase 1 命令 | MGET/MSET、Hash、List、Set、Sorted Set |
| 多 key 跨核 | DEL k1 k2 k3 拆分分发 + 汇聚 |
| 冷读优化 | waiter 机制：重复冷读合并为一次 NVMe IO |
| 小 value 内联 | 极小 value（<16B）内联到 index entry，不走 slab |
