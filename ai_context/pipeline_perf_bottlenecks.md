# Pipeline 热路径性能瓶颈分析

> 分析日期: 2026-03-11
> 分支: tcp-generic-unpacker
> 范围: 仅框架 pipeline 自身（src/pump/），不含 scheduler 业务逻辑

---

## 1. submit.hh: connect() 被调用两次

**位置**: `src/pump/sender/submit.hh:26-29`

**问题**: 为了推导 `op_tuple_t` 类型调了一次 `connect()`，为了构造值又调了一次。`connect()` 递归展开整条 sender 链，pipeline 越长浪费越多。

```cpp
// 当前代码
using op_tuple_t = __typ__(sender.template connect<context_t>().push_back(__fwd__(receiver)).take());
auto new_scope = core::scope_ptr(new core::root_scope<op_tuple_t>(
    sender.template connect<context_t>().push_back(__fwd__(receiver)).take()  // 第二次!
));
```

**修法**:
```cpp
auto op_data = sender.template connect<context_t>().push_back(__fwd__(receiver)).take();
auto new_scope = core::scope_ptr(new core::root_scope<decltype(op_data)>(__mov__(op_data)));
```

**难度**: 极低 | **收益**: 中（pipeline 越长收益越大）

---

## 2. for_each: 每个元素一次 CAS (acq_rel)

**位置**: `src/pump/sender/for_each.hh:82-134`

**问题**: `try_acquire_for_poll()` 和 `consume_and_continue()` 每处理一个流元素都走 CAS 循环，使用 `memory_order_acq_rel`。

**背景**: CAS 存在是为了处理 `concurrent(N) >> on(多个scheduler)` 场景下，多个回调线程同时 `poll_next` 的竞争。但在最常见场景（所有回调在同一线程）下，CAS 永远不会真正竞争，纯属开销。

```cpp
// try_acquire_for_poll — 每次 poll_next 调用
do {
    old_val = poll_next_flag.load(std::memory_order_acquire);
    // ... 计算 ...
} while (!poll_next_flag.compare_exchange_weak(old_val, new_val,
            std::memory_order_acq_rel));  // 每个元素一次

// consume_and_continue — 每次元素处理后
do {
    old_val = poll_next_flag.load(std::memory_order_acquire);
    // ... 计算 ...
} while (!poll_next_flag.compare_exchange_weak(old_val, new_val,
            std::memory_order_acq_rel));  // 又一次
```

**优化方向**:
- **批量 poll**: 获取 token 后处理 K 个元素再释放，摊薄 CAS（K 可以是编译期参数或自适应）
- **单线程快速路径**: 编译期或运行期检测是否单 scheduler，单线程时用普通 counter
- **降级 memory order**: 验证是否可用 `relaxed`（需确认跨线程可见性保证）

**难度**: 中 | **收益**: 高（流处理是最常见场景）

---

## 3. scope 堆分配: 每次 flat/for_each/concurrent 都 new

**位置**: `src/pump/core/scope.hh:120`

**问题**: `make_runtime_scope` 每次 `new` 一个 scope 对象，对应的 `delete` 在 `pop_pusher_scope_op`（flat 完成时）、`pop_to_loop_starter`（流元素完成时）、`concurrent_counter_wrapper::push_value`（并发元素完成时）。

**触发频率**:

| 场景 | new 次数 | delete 次数 |
|------|---------|------------|
| `flat()` 展开子 sender | 每次 flat 1 次 | flat 子 pipeline 完成时 |
| `for_each` 流处理 | 每个元素 1 次 | 元素处理完成时 |
| `concurrent` 并发 | 每个并发元素 1 次 | 并发元素回调时 |
| 嵌套场景 | 乘法关系 | — |

典型 `for_each(100k) >> concurrent(N) >> on(sched) >> then(f) >> reduce()` 至少 100k 次 new + 100k 次 delete。

**优化方向**:
- **per-thread 对象池**: scope 类型在编译期已知，可按类型做定长池（无锁，thread-local）
- **scope 复用**: stream 场景中每个元素的 scope 结构完全相同（类型、大小一致），处理完一个元素后复用内存给下一个
- **栈上 scope (SBO)**: 小 op_tuple 时走 small buffer optimization，避免堆分配
- **arena allocator**: per-pipeline 的 arena，pipeline 结束后统一释放

**难度**: 中 | **收益**: 高（高频流处理场景最显著）

---

## 4. concurrent: 多重 CAS 开销

**位置**: `src/pump/sender/concurrent.hh`

**问题**: concurrent 有三个独立的原子变量，每个并发元素都要操作：

| 原子变量 | 行号 | 用途 | 每元素操作 |
|---------|------|------|-----------|
| `concurrent_counter::counter` | :19 | wait/done 打包计数 | CAS (relaxed) — 元素完成时 `add_done()` |
| `pending_status` | :120 | 限流控制（有限并发时） | CAS (relaxed) — 元素完成时 `sub_now_pending_and_check()`，启动时 `add_now_pending_and_check()` |
| `count_of_values` | :118 | 元素计数 | `++`（原子递增） |

### count_of_values 可能不需要是 atomic

`count_of_values` 在 `push_value:478` 做原子递增，但在 `push_done:503` 作为普通值传给 `set_source_done(op.count_of_values)`。

`push_value` 和 `push_done` 都由 for_each 的 `poll_next` 驱动，而 `poll_next` 有令牌保护（同一时刻只有一个线程执行 poll 循环）。所以 `count_of_values` 的写入天然是串行的，可以改成普通 `uint32_t`。

### concurrent_counter 的 CAS

`add_wait` 和 `add_done` 的 CAS 循环是 `relaxed`，性能尚可。但打包 wait/done 到 uint64_t 的设计增加了计算开销（位移、掩码、比较），每次 CAS 失败重试时都要重算。

**优化方向**:
- `count_of_values` 改为非原子 `uint32_t`
- 考虑拆分 wait/done 为两个独立 atomic（消除打包计算），但会变成两次原子操作

**难度**: 低-中 | **收益**: 中

---

## 5. context 的 shared_ptr 原子引用计数

**位置**: `src/pump/core/context.hh:131-138`

**问题**: `make_root_context` 用 `std::make_shared`，引用计数是原子的。context 在以下操作时触发原子操作：

| 操作 | 原子操作 |
|------|---------|
| `push_context` | shared_ptr 拷贝 → `fetch_add(1)` |
| `pop_context` | 拷贝 base context → `fetch_add(1)`，原 context 析构 → `fetch_sub(1)` |
| 多层 push/pop | 每层各一组 |

在深度 pipeline（多层 push_context / pop_context 嵌套）中，每次 context 层级切换都有原子操作。

**优化方向**:
- 使用非原子引用计数的 shared_ptr（boost::local_shared_ptr 或自制）
- 重新设计 context 生命周期：context 通常在单线程内创建和销毁，跨线程时通过 scheduler 回调传递（回调本身已保证了 happens-before），不需要原子引用计数
- 极端方案：context 改为 move-only，但会影响 when_all 等需要 fork context 的场景

**难度**: 高 | **收益**: 中

---

## 6. 其他次要开销

### 6.1 concurrent_copy 的 tuple 拷贝

`concurrent.hh:469` 每个并发元素调 `core::concurrent_copy(op.stream_op_tuple)` 拷贝整个 stream op tuple。如果 op 中有大的 lambda 捕获（如 `std::string`），拷贝成本不低。

### 6.2 tuple_to_tie 的模板实例化

`for_each.hh:473` 的 `tuple_to_tie(op.stream_op_tuple)` 将 tuple 转为引用 tuple。本身运行时成本为零（编译期完成），但增加模板实例化数量和编译时间。

### 6.3 pop_to_loop_starter 的链式 delete

`scope.hh:182-195` 在流元素完成时从当前 scope 向上走到 stream_starter，沿途 delete 中间 scope。嵌套深度大时是 O(depth) 的指针追踪 + delete 序列。

### 6.4 flat 的 std::visit

`flat.hh:115-122` 处理 `variant<sender...>` 时用 `std::visit`，有虚表/跳转表开销。`visit()` sender 分支后的 `flat()` 每次都走这条路径。

---

## 优先级总结

| 序号 | 项目 | 难度 | 收益 | 建议 |
|------|------|------|------|------|
| 1 | submit double connect | 极低 | 中 | 立刻修 |
| 2 | count_of_values 改非原子 | 低 | 低 | 顺手修 |
| 3 | scope 对象池/复用 | 中 | **高** | 最值得做 |
| 4 | for_each CAS 批量化 | 中 | **高** | 值得做 |
| 5 | concurrent CAS 简化 | 中 | 中 | 可做 |
| 6 | context 非原子 shared_ptr | 高 | 中 | 长期改进 |
