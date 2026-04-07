# 004 — Page Cache 模块（Clock + SLRU）

> 实现第四步。把 `lookup_scheduler` 的裸 `unordered_map` 缓存改造为有容量限制、可配置淘汰策略的页缓存模块。

## 文件结构

```
core/
├── clock_cache.hh    — Clock（second-chance）淘汰策略
├── slru_cache.hh     — Segmented LRU 淘汰策略
└── page_cache.hh     — cache_concept 定义 + 实现注册

tree/
├── scheduler.hh      — lookup_scheduler 模板化在 Cache 上 + 拆基类
└── sender.hh         — lookup_scheduler* → lookup_scheduler_base*

mock_nvme/
└── device.hh         — 加 read/write 计数器（测试观测点）

test/
└── test_page_cache.cc — Cache 单元测试 + 改造缓存淘汰回归
```

## 设计选型讨论结论

| 维度 | Clock | SLRU | LRU |
|------|-------|------|-----|
| Hit path 开销 | 1 byte ref bit 写 | 链表 move-to-head（4-6 指针写） | 同 SLRU |
| 内存布局 | 连续数组 | 索引式侵入链表（连续 node 数组） | 指针式链表（离散） |
| Scan 抗性 | 中等 | 优秀（probation/protected 双段） | 差 |
| 适用场景 | 纯点查 | 混合点查 + scan |

**结论**：两种都实现，启动时配置选择。点查为主用 clock，有 range scan 用 slru。

## cache_concept

```cpp
template <typename C>
concept cache_concept = requires(C c, const C cc, paddr k, char* b) {
    { c.get(k) }       -> std::same_as<const char*>;        // miss → nullptr
    { c.put(k, b) }    -> std::same_as<std::optional<evicted_entry>>;
    { cc.contains(k) } -> std::same_as<bool>;
    { cc.size() }      -> std::same_as<uint32_t>;
    { cc.capacity() }  -> std::same_as<uint32_t>;
};

struct evicted_entry {
    paddr key;
    char* buf;          // 淘汰时返回 buf 给调用方复用
};
```

`clock_cache` 和 `slru_cache` 都满足这个 concept，编译期 `static_assert` 验证。

## clock_cache

```cpp
struct clock_cache {
    struct slot {
        paddr key;
        char* buf;
        bool occupied;
        bool ref;
    };
    std::vector<slot> slots_;                    // 连续数组
    std::unordered_map<paddr, uint32_t> index_;  // key → slot index
    uint32_t hand_;                              // clock 游标
};
```

- **get**: hash lookup → `slots_[idx].ref = true` → 返回 buf（hot path 仅 1 cache line 写）
- **put**: 有空 slot 直接占；满则推进 hand，遇 ref=true 清零跳过，遇 ref=false 淘汰替换
- 全部连续内存，clock sweep 对 cache line 友好

## slru_cache

```cpp
struct slru_cache {
    struct node {
        paddr key;
        char* buf;
        uint32_t prev, next;     // 索引式链表，非指针
        bool in_protected;
        bool occupied;
    };
    std::vector<node> nodes_;                    // 连续 node 数组
    std::unordered_map<paddr, uint32_t> index_;
    uint32_t prot_head_, prot_tail_, prob_head_, prob_tail_;
    uint32_t prot_size_, prot_cap_, prob_size_, prob_cap_;
    uint32_t free_head_;                         // 空闲 node 链表
};
```

- protected 占 80%，probation 占 20%
- **get**: 在 probation 命中 → 升入 protected 头部（protected 满则尾部降回 probation 头部）；在 protected 命中 → 移到 protected 头部
- **put**: 新页插 probation 头部；probation 满则淘汰 probation 尾部
- 链表用 `uint32_t` 索引而非裸指针，prev/next 节点都在同一个 vector 内，cache line 友好

## 模板化 + 基类拆分

`lookup_scheduler` 模板化在 Cache 上之后，op/sender 不能再持有它（否则 PUMP 框架特化和 sender.hh 全部要传染模板参数）。解决：拆出非模板基类作为模板防火墙。

```
lookup_scheduler_base                  ← 非模板，op/sender 持有它
├── pump::core::per_core::queue<req*>  ← cache 无关
├── schedule_lookup / schedule_cache   ← 仅 enqueue
└── process / submit_cache             ← 构造 sender

  ↓ public 单继承（无虚函数，无 vtable）

lookup_scheduler<Cache>                ← 模板，所有 cache 相关逻辑
├── Cache page_cache_                  ← 直接成员
├── loading_pages_, owned_bufs_, free_bufs_, waiters_head_
├── advance / handle / process_entries / prepare_reads
```

**性能特点**：
- Cache hot path（process_entries → page_cache_.get/contains）完全在派生类内执行，编译器看到完整 Cache 类型，全部内联
- 通过基类指针调用的只有 `schedule_lookup`/`schedule_cache`（非虚函数 + 函数体一行 enqueue），编译器静态解析后内联
- 单继承无虚函数，对象布局与单一类相同，`derived*` → `base*` 是 identity cast（零指令）
- 等价于把所有代码写在一个类里的机器码

## scheduler.hh 改造细节

- `unordered_map<paddr, const char*>` + `vector<unique_ptr<char[]>>` → `Cache page_cache_` + `vector<unique_ptr<char[]>> owned_bufs_` + `vector<char*> free_bufs_`
- `prepare_reads()`：从 `free_bufs_` 复用被淘汰的 buffer，没有则新分配
- `advance()` cache drain：`page_cache_.put()` 返回的 `evicted_entry` 的 buf 进 `free_bufs_`
- 构造函数：`lookup_scheduler<Cache>(Cache cache, size_t depth = 2048)`，cache 在外部构造好传入

## 启动期配置选择

```cpp
// 配置文件读 cache_policy 字段
if (config.cache_policy == "clock")
    run<clock_cache>(config);
else if (config.cache_policy == "slru")
    run<slru_cache>(config);
```

入口处一次分支决定实例化哪个特化，之后全程零开销。

## 缓存零拷贝 + DMA 兼容性

- Cache 节点只存 `(paddr, char*)`，不持有页内容
- 页数据被 NVMe **直接写入** buffer 一次，之后所有访问都是裸指针解引用
- 淘汰返回 `evicted_entry { key, buf }`，buffer 进 `free_bufs_` 复用池
- Buffer 内存来源完全由 scheduler 控制（当前 `make_unique<char[]>`，未来可换 SPDK DMA pool），cache 模块零改动

## 验证

### page cache 单元测试（11 case）

通用功能（两种策略各跑）：
- 基础 put/get/contains
- 容量满后淘汰返回正确 evicted_entry
- 更新已存在 key 不触发淘汰
- 淘汰返回的 buf 指针正确

策略专属：
- **clock**: 热页（反复 get）经过多轮 sweep 仍未被淘汰
- **slru**: probation 中只访问一次的页被优先淘汰，访问过的页升入 protected
- **slru**: scan 抗性 — 100 次冷页插入零热页驱逐

### tree lookup 缓存淘汰回归

`test_cache_eviction(Cache, label)`：
- 用 `cache_capacity = 4` 极小 cache（13 个唯一页装不下）
- 把 400 个 key **乱序**遍历，强制每次 lookup 跨 leaf 跳跃
- 验证 400/400 结果正确（淘汰后重读路径无误）
- 验证 NVMe read count > 13 × 5（说明发生大量重读，不只一次性加载）
- mock_device 读计数器作为观测点

实测结果：
- clock: 400/400 正确，745 次 NVMe 读（平均每个唯一页被重读 ~57 次）
- slru: 400/400 正确，654 次 NVMe 读（比 clock 少 ~12%，热页保护更好）

### 全部回归

- `inconel_test_page_cache` — 11/11 通过
- `inconel_test_tree_lookup` — 7 cases（5 原有 + 2 淘汰场景）通过
- `inconel_test_tree_lookup_multicore` — 400 并发 + 100 miss 通过
