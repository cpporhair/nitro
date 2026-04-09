# 013 — Tree Lookup Single-Flight

> 实现第十三步。把 tree lookup 当前的 `loading_pages_ + waiters_head_` 简化模型收敛成真正按页聚合的 single-flight inflight 结构，避免继续固化“全局 set + 全局 waiter 链”的临时实现。

## 本 step 覆盖的 issue

| Issue | 说明 |
|---|---|
| `INC-039` | tree lookup inflight 仍是全局 set + waiter 链，没对齐 spec 的 single-flight 方向 |

## 文件结构

```text
tree/
└── scheduler.hh                      — inflight read bookkeeping 从 set/list 改成 per-page single-flight map
```

## 设计目标

1. 让同一 tree page 的并发 miss 真正合并成单次 NVMe read。
2. 不再在 cache completion 时扫描整条全局 waiter 链。
3. 保持 tree lookup sender API 不变，变化只发生在 scheduler 内部。

## 设计决策

| # | 决策点 | 结果 | 说明 |
|---|---|---|---|
| `D1` | inflight key | **按当前 cache key `paddr` 聚合** | 当前 tree lookup 读的就是精确 slot paddr，本步不引入新 key type |
| `D2` | waiter 失效处理 | **用 wait generation 消解旧注册** | 请求可能在多个 page 上挂过等待；旧挂载不做即时删除，靠 generation 过滤 |
| `D3` | duplicate wakeup 防护 | **req 增加 `wake_enqueued` 标记** | 同一个 req 被多个 page 同轮完成时，只入队一次 |
| `D4` | buffer ownership | **继续复用 `owned_bufs_` / `free_bufs_`** | 本步只改 inflight bookkeeping，不改 buffer 生命周期 |

## 详细设计

### 替换的数据结构

删除：

```cpp
absl::flat_hash_set<paddr> loading_pages_;
_tree_lookup::req* waiters_head_;
```

新增：

```cpp
struct wait_token {
    _tree_lookup::req* req;
    uint32_t           wait_gen;
};

struct pending_read {
    std::vector<wait_token> waiters;
};

absl::flat_hash_map<paddr, pending_read> inflight_reads_;
```

`_tree_lookup::req` 增加两个字段：

```cpp
uint32_t wait_gen = 0;
bool     wake_enqueued = false;
```

并删除旧的 intrusive `next`。

### 停车（park）逻辑

当 `handle()` 发现：

- `process_entries()` 之后还有 unresolved entry
- `prepare_reads()` 没有生成新的 `read_descs`

说明所需 page 已全部在 inflight 中。

这时不再挂到全局 `waiters_head_`，而是：

1. `r->wait_gen += 1`
2. `r->wake_enqueued = false`
3. 对当前所有 unresolved entry 收集唯一的 `next_page`
4. 对每个 page：
   - 若已在 cache 中，跳过
   - 否则必须已存在于 `inflight_reads_`
   - 向其 `waiters` 追加 `{r, r->wait_gen}`

如果收集出的等待页集合为空，说明状态机出现不一致，应直接 `assert` / panic，而不是悄悄丢请求。

### 发起 read 逻辑

`prepare_reads()` 改成按 page 查 `inflight_reads_`：

- page 已在 cache 中：跳过
- page 已在 `inflight_reads_` 中：不再重复分配 buf / 不再重复发 read
- page 不在 inflight 中：
  - 从 `free_bufs_` 或 `owned_bufs_` 取 buf
  - `inflight_reads_.emplace(page, pending_read{})`
  - 生成 `read_desc` 与 `page_map`

这样“同一页只有第一个 miss 负责发 read”，后来的 miss 只登记等待。

### completion 唤醒逻辑

cache completion 处理某个 `_cache_pages::req` 时：

1. 对 `page_map` 中每个 `(addr, buf)`：
   - `page_cache_.put(addr, buf)`
   - 取出并 erase `inflight_reads_[addr]`
   - 遍历其 `waiters`
2. 对每个 waiter token：
   - 若 `token.wait_gen != token.req->wait_gen`，跳过（旧注册）
   - 若 `token.req->wake_enqueued` 已为真，跳过（本轮已被其它 page 唤醒）
   - 否则：
     - `token.req->wake_enqueued = true`
     - `lookup_queue_.try_enqueue(token.req)`

这样：

- 同一个 req 等待多个 page，只要其中一个 page 完成，就会被唤醒一次
- 同一 req 的旧 wait 注册不需要即时从其它 page 上删除，等那些 page 完成时会被 generation 自动过滤

### `handle()` 入口

在真正开始处理 req 前，先把：

```cpp
r->wake_enqueued = false;
```

清掉，表示这次入队已经被消费。  
如果本轮处理后仍需等待新的 inflight 页，req 会用新的 `wait_gen` 重新注册。

## 实施顺序

1. `req` 结构加 `wait_gen` / `wake_enqueued`，删 `next`。
2. `loading_pages_` / `waiters_head_` 替换成 `inflight_reads_`。
3. `prepare_reads()` 改成 single-flight 发 read。
4. `handle()` 的 park 逻辑改成 per-page wait registration。
5. cache completion 逻辑改成按 `pending_read.waiters` 唤醒。

## 验证

实现本 step 时至少回归：

- `inconel_test_tree_lookup`
- `inconel_test_tree_lookup_multicore`
- `inconel_test_runtime`

重点观察：

- 同一 page 的并发 miss 只发一次 NVMe read
- 多个 req 等同一页完成时能全部被唤醒
- req 等多个 page 时不会重复入队或丢失唤醒
