# Scope 裸指针重写方案

## 1. 背景与动机

当前 scope 使用 `std::shared_ptr`（`std::make_shared`），每次 scheduler callback 捕获 scope 都产生原子引用计数操作。在高并发场景下（`concurrent(10000) >> on(scheduler)`），原子操作成为性能瓶颈。

**核心洞察**：scope 的生命周期由 pipeline 拓扑结构决定，不需要引用计数。每个 scope 都有确定的创建者和确定的删除时机。中间所有操作只需要传递裸指针。

## 2. scope_ptr 定义

替换 `std::shared_ptr`，改为无引用计数的轻量指针包装：

```cpp
template<typename T>
struct scope_ptr {
    using element_type = T;
    T* ptr_ = nullptr;

    scope_ptr() = default;
    explicit scope_ptr(T* p) : ptr_(p) {}
    scope_ptr(const scope_ptr&) = default;
    scope_ptr& operator=(const scope_ptr&) = default;

    T* operator->() const { return ptr_; }
    T& operator*() const { return *ptr_; }
    T* get() const { return ptr_; }
};
```

特性：trivially copyable，无原子操作，无析构行为。所有 op、scheduler callback 都可以自由复制。

## 3. make_runtime_scope 改动

```cpp
template <runtime_scope_type scope_type, typename base_t, typename op_tuple_t>
auto make_runtime_scope(base_t& scope, op_tuple_t&& opt) {
    using scope_t = runtime_scope<scope_type, op_tuple_t, std::decay_t<base_t>>;
    return scope_ptr<scope_t>(new scope_t(__fwd__(opt), scope));
}
```

`runtime_scope` 构造函数中 `base_scope = scope` 直接拷贝 scope_ptr（trivial copy）。

## 4. 删除点总览

| scope 类型 | 创建者 | 删除者 | 删除时机 | 备注 |
|-----------|--------|--------|---------|------|
| root scope | submit | null_receiver | push_value / push_exception / push_skip 时 | 管道终点 |
| flat 子 scope | flat op_pusher | pop_pusher_scope_op | 获取 base_scope 后 delete child | 确定性一对一 |
| any_exception 恢复 scope | any_exception op_pusher | pop_pusher_scope_op | 同上 | 同 flat |
| when_skipped 恢复 scope | when_skipped op_pusher | pop_pusher_scope_op | 同上 | 同 flat |
| stream scope | for_each starter | for_each starter 析构 | root scope 被 delete 时级联 | 存裸指针字段 |
| concurrent 元素 scope | concurrent op_pusher | concurrent_counter | 元素完成后 delete | 见 §5.5 |
| when_all 分支 scope | when_all_starter | collector_wrapper 或 reducer | 分支完成时 delete | 见 §5.6 |
| when_any 分支 scope | when_any_starter | race_wrapper | 分支完成时 delete | 见 §5.7 |
| sequential buffer scope | drain_single | drain_single 内部管理 | 每次 drain 创建，drain 结束后 delete | 见 §5.8 |
| await root scope | start_sender | scope_holder | co_await 结束后 delete | 保持现有 scope_holder 模式 |

## 5. 各模块详细改动

### 5.1 submit.hh — root scope

```cpp
// 改前：
auto new_scope = std::make_shared<core::root_scope<...>>(...);
core::op_pusher<0, __typ__(new_scope)>::push_value(context, new_scope);

// 改后：
auto new_scope = core::scope_ptr(new core::root_scope<...>(...));
core::op_pusher<0, __typ__(new_scope)>::push_value(context, new_scope);
```

scope_ptr 是 trivially copyable，op_pusher 按引用传递，scheduler callback 按值捕获（拷贝裸指针）。

### 5.2 null_receiver.hh — root scope 删除点

```cpp
// 改前：空函数体
static void push_value(context_t& context, scope_t& scope, value_t&& ...v) {
    static_assert(context_t::element_type::root_flag, "...");
}

// 改后：
static void push_value(context_t& context, scope_t& scope, value_t&& ...v) {
    static_assert(context_t::element_type::root_flag, "...");
    delete scope.get();
}
```

push_exception、push_skip 同理。push_done 在 root scope 层不应发生（static_assert 保护）。

### 5.3 op_pusher.hh — pop_pusher_scope_op 删除 flat/any_exception/when_skipped 子 scope

```cpp
// 改前：
static void push_value(context_t& context, scope_t& scope, value_t&& ...v) {
    auto base_scope = scope->base_scope;
    op_pusher<new_pos, __typ__(base_scope)>::push_value(context, base_scope, __fwd__(v)...);
}

// 改后：
static void push_value(context_t& context, scope_t& scope, value_t&& ...v) {
    auto base_scope = scope->base_scope;
    delete scope.get();  // 删除子 scope
    op_pusher<new_pos, __typ__(base_scope)>::push_value(context, base_scope, __fwd__(v)...);
}
```

push_exception、push_skip、push_done 同理。

**安全性**：`base_scope` 是 scope_ptr（trivial copy），在 delete 前已拷贝完毕。子 scope 的 op_tuple 在 delete 后不再访问。

### 5.4 for_each.hh — stream scope

**创建**：for_each starter 的 push_value 中创建 stream scope，把裸指针存在 starter 的字段里。

```cpp
// starter 增加字段：
struct starter {
    // ... 现有字段 ...
    void* stream_scope_raw = nullptr;  // 存储 stream scope 裸指针

    ~starter() {
        // 由具体的 op_pusher 在创建时注册 deleter
        // 或者用 scope_holder 模式
    }
};
```

考虑到 stream scope 类型是模板参数，starter 析构时不知道具体类型。两种方案：

**方案 A**：用 `std::unique_ptr<scope_holder_base>` 类型擦除（await_sender 已有此模式）。

```cpp
struct scope_holder_base { virtual ~scope_holder_base() = default; };
template<typename T>
struct scope_holder_impl : scope_holder_base {
    T* ptr;
    scope_holder_impl(T* p) : ptr(p) {}
    ~scope_holder_impl() { delete ptr; }
};

// starter 字段：
std::unique_ptr<scope_holder_base> stream_scope_holder;
```

**方案 B**：用 `std::move_only_function<void()>` 存 deleter。

推荐方案 A，与 await_sender 保持一致。

**stream scope 的删除时机**：root scope 被 null_receiver delete 时，级联析构 for_each starter → starter 析构函数 delete stream scope。

### 5.5 concurrent.hh — 元素子 scope

concurrent 为每个元素创建子 scope。元素完成后需要删除子 scope。

当前流程：
1. `concurrent_starter::push_value` → `make_new_scope` → 创建子 scope
2. 子 scope 经过 stream_op_tuple 处理
3. 完成后到达 `concurrent_counter_wrapper::push_value` → `op.counter.on_set_value()`
4. counter 追踪完成数，最后一个完成时 `counter_push_done`

**改动**：在 concurrent_counter_wrapper 的 push_value/push_exception/push_skip 中，先获取所需数据，然后 delete 子 scope，再操作 counter。

```cpp
// concurrent_counter_wrapper op_pusher push_value:
static void push_value(context_t& context, scope_t& scope, value_t&&... v) {
    auto& op = std::get<pos>(scope->get_op_tuple());
    // 保存需要的数据
    auto value_copy = variant_value_type(std::in_place_index<2>, __fwd__(v)...);
    auto& counter = op.counter;
    // 获取父 scope 链上需要的信息（starter ref、stream scope ref）
    auto& starter_scope = find_stream_starter(scope);  // 这是 scope_ptr 的引用
    // delete 子 scope
    delete scope.get();
    // 用已保存的数据操作 counter
    counter.on_set_value(context, starter_scope, __mov__(value_copy));
}
```

注意：这里需要仔细看 counter 的回调是否需要子 scope 的数据。如果 counter 只需要 starter_scope（stream scope），那 delete 子 scope 后用 starter_scope 继续推进即可。

### 5.6 when_all.hh — 分支 scope

when_all 为每个分支创建子 scope。分支完成后到达 reducer op，reducer 将结果写入 collector。

**当前流程**：
1. `submit_senders<index>` → `make_runtime_scope<other>` → 每分支一个子 scope
2. 分支完成 → 到达 `reducer<index, collector_t>`
3. reducer 写入 collector → `done_count.fetch_sub(1)` → 最后一个到达时 push 结果到父 scope

**改动**：reducer 的 push_value/push_exception/push_done 中，先保存结果，delete 分支 scope，再操作 collector。

```cpp
// reducer op_pusher push_value:
static void push_value(context_t& context, scope_t& scope, value_t&&... v) {
    auto& op = std::get<pos>(scope->get_op_tuple());
    auto* collector = op.collector;
    // 保存 parent scope 供 collector 最后使用
    auto parent = scope->base_scope;  // copy scope_ptr (trivial)
    // delete 分支 scope
    delete scope.get();
    // collector 操作（可能触发 parent push_value 和 delete this）
    collector->template on_set<index>(context, parent, __fwd__(v)...);
}
```

collector 的 `on_set` 在最后一个分支完成时：
- 调用 `op_pusher<pos+1>::push_value(context, parent_scope, result)`
- `delete this`（collector 自身删除，保持现有模式）

### 5.7 when_any.hh — 分支 scope

与 when_all 类似。分支完成后到达 reducer，reducer 操作 race_wrapper。

**改动**：reducer 中先获取 parent scope，delete 分支 scope，再操作 race_wrapper。

```cpp
// reducer push_value:
auto parent = scope->base_scope;
delete scope.get();
wrapper->template on_set<index>(context, parent, __fwd__(v)...);
```

race_wrapper 的 `on_set`（赢家）→ push_value 到 parent；`release_ref`（所有分支完成后）→ `delete this`。

### 5.8 sequential.hh — buffer scope

sequential 的 `make_new_scope` 创建一个 "other" 类型的 scope 用于 drain。

**当前流程**：
1. `drain_single` 调用 `make_new_scope(scope)` 创建 scope
2. `push_from_storage` 通过这个 scope 推送值到下游
3. 下游可能包含 flat（创建子 scope）→ scheduler（async）→ pop_pusher（delete flat 子 scope，回到 sequential scope）→ reduce
4. reduce 完成一个值后 poll_next → 再次 drain_single

**问题**：drain_single 创建的 scope 被 flat `__mov__` 走了（在旧方案中）。

**裸指针方案**：flat 不 move scope，只是把裸指针存到子 scope 的 base_scope。pop_pusher delete flat 的子 scope 后，sequential 的 scope 仍然有效。drain_single 可以在整个 drain 过程中复用同一个 scope。

```cpp
static void drain_single(context_t& context, scope_t& scope) {
    auto& op = std::get<pos>(scope->get_op_tuple());
    if (op.is_draining) { op.drain_requested = true; return; }

    auto new_scope = make_new_scope(scope);  // 创建一次

    while (true) {
        // ... dequeue and push via new_scope ...
        // flat 不再 move new_scope，只拷贝裸指针
        // pop_pusher delete flat 子 scope 后，new_scope 仍有效
        // 可以继续下一次 drain

        if (done) {
            // push_done 通过 new_scope 推送
            // push_done 完成后 delete new_scope
            delete new_scope.get();
            return;
        }
    }
}
```

**关键变化**：flat 不再 move scope，sequential 的 scope 在整个 drain 生命周期内有效。drain 结束时（函数 return 前或 push_done 后）由 drain_single 自己 delete。

### 5.9 await_sender.hh — 协程 root scope

保持 `scope_holder_base / scope_holder_impl` 模式，scope_holder 析构时 delete：

```cpp
void start_sender() {
    auto ops = sender.template connect<context_t>()
        .push_back(receiver_t{&state}).take();
    using scope_element_t = core::root_scope<__typ__(ops)>;
    auto* raw = new scope_element_t(__mov__(ops));
    state.context_holder = context;
    state.scope_holder = std::make_unique<core::scope_holder_impl<scope_element_t>>(raw);
    auto scope = core::scope_ptr<scope_element_t>(raw);
    core::op_pusher<0, __typ__(scope)>::push_value(context, scope);
}
```

scope_holder 在 co_await 结束后析构 → delete scope。

注意：await_sender 的 root scope 不应由 null_receiver delete（它用的是 await_receiver 不是 null_receiver），而是由 scope_holder 管理。

### 5.10 scope.hh — pop_to_loop_starter

```cpp
// 改前：
template<typename pop_scope_t>
auto pop_to_loop_starter(pop_scope_t& scope) {
    if constexpr (stream_starter) {
        auto base = scope->base_scope;  // 拷贝 scope_ptr
        return base;
    } else {
        auto& base = scope->base_scope;
        return pop_to_loop_starter(base);
    }
}
```

裸指针方案下不需要改动逻辑。`scope->base_scope` 是 scope_ptr（trivial copy）。

**但谁 delete stream scope？** pop_to_loop_starter 本身不应该 delete，因为 stream scope 的所有权在 for_each starter 的 scope_holder 里。root scope 被 null_receiver delete 时，级联析构 for_each starter → scope_holder → delete stream scope。

**时序安全**：
1. reduce push_done → pop_to_loop_starter → 获取 parent（root scope）
2. push_value 到 root scope 的下一个 op → ... → null_receiver
3. null_receiver delete root scope → 析构 for_each starter → scope_holder delete stream scope
4. stream scope 在步骤 2-3 之间不会被再次访问（已经 pop 出去了）

### 5.11 tasks_scheduler.hh — scheduler callback

```cpp
// 不需要改动语义，只是 scope 从 shared_ptr 变成 scope_ptr（trivially copyable）
[context = context, scope = scope]() mutable {
    core::op_pusher<pos + 1, scope_t>::push_value(context, scope);
}
```

scope 拷贝零开销（裸指针拷贝 vs shared_ptr 原子操作）。

## 6. 安全性论证

### 6.1 为什么裸指针不会悬空？

每个 scope 的删除点都在该 scope 最后一次被访问之后：

- **pop_pusher**：先拷贝 base_scope，再 delete child，再用 base_scope 继续。child 不再被访问。
- **null_receiver**：管道终点，delete 后不再有代码运行。
- **for_each scope_holder**：root scope 被 delete 时级联，此时 stream scope 已经 pop 出去不再使用。
- **concurrent counter**：元素完成、数据保存后再 delete 子 scope。
- **when_all/when_any**：结果保存到 collector/wrapper 后再 delete 分支 scope。

### 6.2 when_any 输者分支不会泄漏

赢家分支：on_set 推送结果 → delete 分支 scope → release_ref。
输家分支：检测到已完成 → delete 分支 scope → release_ref → 最后一个 release_ref 触发 `delete wrapper`。

每个分支都负责 delete 自己的 scope，collector/wrapper 的 ref count 只管理 collector/wrapper 自身生命周期。

### 6.3 scheduler callback 不会访问已删除的 scope

scheduler callback 捕获的是直接包含自己的 scope（通常是 flat 子 scope 或 concurrent 子 scope）。这个 scope 的 delete 发生在 pop_pusher 或 counter 中——都是 callback 执行链的下游。callback 执行完毕前，scope 不会被 delete。

## 7. 性能收益

| 操作 | shared_ptr | scope_ptr (裸指针) |
|------|-----------|-------------------|
| 创建 scope | make_shared（一次 heap alloc + 初始化 control block） | new（一次 heap alloc） |
| 传递给 op_pusher | 按引用，零开销 | 按引用，零开销 |
| scheduler callback 捕获 | 拷贝 shared_ptr（原子 fetch_add） | 拷贝裸指针（零开销） |
| scheduler callback 销毁 | 析构 shared_ptr（原子 fetch_sub） | 无析构行为 |
| pop_pusher 恢复父 scope | 拷贝 shared_ptr（原子 fetch_add） | 拷贝裸指针（零开销） |
| scope 销毁 | ref count 归零时 delete | 确定性 delete |

在 `for_each(10000) >> concurrent(10000) >> on(scheduler)` 场景下，消除 20000+ 次原子操作。

## 8. 实现顺序

1. **scope.hh**：定义 scope_ptr，scope_holder_base/impl，改 make_runtime_scope
2. **op_pusher.hh**：pop_pusher_scope_op 添加 `delete scope.get()`
3. **null_receiver.hh**：添加 `delete scope.get()`
4. **submit.hh**：scope_ptr 替代 make_shared
5. **flat.hh**：无代码改动（scope 传递语义不变）
6. **for_each.hh**：starter 添加 scope_holder 字段存储 stream scope
7. **concurrent.hh**：counter 完成时 delete 元素子 scope
8. **when_all.hh**：reducer 中 delete 分支 scope
9. **when_any.hh**：reducer 中 delete 分支 scope
10. **any_exception.hh**：无改动（pop_pusher 已处理）
11. **when_skipped.hh**：无改动（pop_pusher 已处理）
12. **sequential.hh**：drain_single 管理 buffer scope 生命周期
13. **await_sender.hh**：scope_holder 管理 root scope
14. **reduce.hh**：无改动（pop_to_loop_starter 语义不变）
15. **编译验证 + 运行所有测试**
