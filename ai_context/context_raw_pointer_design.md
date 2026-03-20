# Context 裸指针化设计文档

> 目标：将 `pushed_context` 从 `std::shared_ptr` 改为裸指针 + slab 分配器，消除 scheduler 边界的原子引用计数开销。

## 1. 问题

每次 scheduler 回调捕获 context：

```cpp
// tasks_scheduler.hh:38, nvme/put_page.hh:68, tcp/send_sender.hh:37 等（15+ 处）
[context = context, scope = scope]() mutable {
    core::op_pusher<pos + 1, scope_t>::push_value(context, scope);
}
```

- `scope = scope`：`scope_ptr`（裸指针），拷贝零开销
- `context = context`：`shared_ptr`，拷贝 = atomic refcount++，销毁 = atomic refcount--
- **每次 scheduler 边界 = 2 次原子操作**

对于多 scheduler hop 的 pipeline（如 AiSAQ：task → NVMe → index → task → ...），这是不必要的开销。

此外，每次 `push_context` 调用 `std::make_shared`（堆分配 + atomic 初始化）。

## 2. 当前架构

### 2.1 类型层次

```
context.hh:
├── root_context<content_t...>          — 根节点，find_base=false，pop_able=false
├── pushed_context<id, base_t, content_t...>  — 栈帧节点，find_base=true，pop_able=true
└── when_all_context<base_t, content_t...>    — 已定义但未使用（死代码）
```

### 2.2 管理方式

| 操作 | 当前实现 |
|------|---------|
| `make_root_context(...)` | `std::make_shared<root_context<...>>(...)` |
| `push_context(value)` | `std::make_shared<pushed_context<id, ctx_t, ...>>(context, tuple)` |
| `pop_context()` | 传递 `context->base_context` 到下游（不 delete，靠 refcount） |
| `get_context<T>()` | `_get<T, 0>(context)` 递归搜索 `context->datas` 和 `context->base_context` |
| scheduler 回调 | `[context = context]` 拷贝 shared_ptr |

### 2.3 生命周期保证（编译期）

**push/pop 配对在编译期保证**，通过 `op_tuple_builder.hh` 的 `compute_matching_context_compile_id`：

- 编译期括号匹配算法：counter=0 开始，遇 pop → counter+1，遇 push && counter==0 → 匹配，遇 push && counter!=0 → counter-1
- `static_assert(!has_id<push_compile_id>())` 在 push_context 中防止双重 push
- `static_assert(has_id<context_compile_id>())` 在 pop_context 中确保匹配的 push 已执行
- `__COUNTER__` 宏保证每个 push/pop 对有唯一 compile_id

**异常路径**同样安全：
- push_context 的 push_exception：`static_assert(!has_id<...>())` → 异常路径**不创建**节点，直接透传
- pop_context 的 push_exception：`if constexpr (has_id<...>())` → 有则 pop（传 base_context），无则透传

### 2.4 context 不能放入 scope 的原因

- **生命周期不同**：when_all reducer 中 `delete scope.get()` 删除分支 scope，但 parent context 必须存活（在 collector 的 `parent_pusher_status.context` 中）
- **变化时机不同**：scope 在 flat/for_each/when_all 变化，context 在 push/pop 变化。合并导致每次 push_context 都需分配新 scope

## 3. 设计方案

### 3.1 核心思路

| 类型 | 改动 |
|------|------|
| `root_context` | **保留 shared_ptr**（一次性分配，不在热路径） |
| `pushed_context` | **改为裸指针 + slab 分配器**（热路径，循环内频繁 push/pop） |
| `when_all_context` | **删除**（死代码，未被引用） |

### 3.2 新类型：`context_ptr<T>`

仿照 `scope_ptr<T>`（scope.hh:82-99），创建 trivially copyable 的裸指针包装：

```cpp
template<typename T>
struct context_ptr {
    using element_type = T;
    T* ptr_ = nullptr;

    context_ptr() = default;
    explicit context_ptr(T* p) : ptr_(p) {}
    context_ptr(const context_ptr&) = default;
    context_ptr& operator=(const context_ptr&) = default;

    T* operator->() const { return ptr_; }
    T& operator*() const { return *ptr_; }
    T* get() const { return ptr_; }
};
```

### 3.3 pushed_context 改造

```cpp
template <uint64_t id, typename base_t, typename ...content_t>
struct pushed_context {
    // ... 现有字段不变 ...
    base_t base_context;
    std::tuple<content_t...> datas;

    // 新增：slab 分配
    static void* operator new(size_t) {
        constexpr auto sc = scope_size_class(sizeof(pushed_context));
        if constexpr (sc > 0) return scope_slab<sc>::alloc();
        else return ::operator new(sizeof(pushed_context));
    }
    static void operator delete(void* p) {
        constexpr auto sc = scope_size_class(sizeof(pushed_context));
        if constexpr (sc > 0) scope_slab<sc>::dealloc(p);
        else ::operator delete(p);
    }
};
```

### 3.4 类型变化链

改动前：
```
shared_ptr<pushed_context<id, shared_ptr<pushed_context<id2, shared_ptr<root_context<>>>>, T>>
```

改动后：
```
context_ptr<pushed_context<id, context_ptr<pushed_context<id2, shared_ptr<root_context<>>>>, T>>
```

scheduler 回调捕获 `[context = context]`：
- 改动前：拷贝 shared_ptr → atomic++
- 改动后：拷贝 context_ptr → 裸指针拷贝，零开销

### 3.5 pop_context 中显式释放

当前 `pop_context` 的 op_pusher 只传递 `base_context`，不做释放。改为显式 delete：

```cpp
// pop_context.hh op_pusher — push_value 路径
template<typename context_t, typename ...value_t>
static inline void
push_value(context_t& context, scope_t& scope, value_t&& ...v) {
    static_assert(context_t::element_type::template has_id<context_compile_id>());
    static_assert(context_t::element_type::pop_able, "context is root");
    auto base = context->base_context;
    delete context.get();  // 新增：显式释放 pushed_context
    op_pusher<pos + 1, __typ__(scope)>::push_value(base, scope, __fwd__(v)...);
}

// push_exception 路径
template<typename context_t>
static inline void
push_exception(context_t& context, scope_t& scope, std::exception_ptr e) {
    if constexpr (!context_t::element_type::template has_id<context_compile_id>()) {
        op_pusher<pos + 1, __typ__(scope)>::push_exception(context, scope, e);
    } else {
        static_assert(context_t::element_type::pop_able, "context is root");
        auto base = context->base_context;
        delete context.get();  // 新增
        op_pusher<pos + 1, __typ__(scope)>::push_exception(base, scope, e);
    }
}

// push_skip、push_done 路径同理
```

## 4. 安全性分析

### 4.1 为什么裸指针安全

核心不变量：**pushed_context 的生命周期严格包含在 push/pop 配对之间，且配对是编译期保证的。**

| 场景 | 分析 |
|------|------|
| 线性 pipeline | push 创建，pop 删除。中间所有 scheduler 回调只是借用（裸指针拷贝） |
| when_all | parent context 存活于 `collector_wrapper` 的 `parent_pusher_status` 中。分支借用 parent context 但不拥有。所有分支完成 → collector fire → parent 继续。pushed_context 在外层 pop 才删除 |
| concurrent + reduce | context 被所有迭代共享。reduce 完成后才执行 pop_context → context 在 pop 前天然存活 |
| scheduler 回调 | 回调是 continuation 本身。pushed_context 的 pop 在回调之后的 pipeline 中。回调执行时 context 天然存活 |
| 异常路径 | push_context 的 push_exception 不创建节点（`static_assert(!has_id)`）。pop_context 的 push_exception 正确 pop 并可 delete |

### 4.2 不安全的唯一情况

用户手动写了 `push_context >> ... >> submit(ctx)` 但没有对应的 `pop_context`。这在改动前就是 BUG（context 栈不平衡），编译期 `compute_matching_context_compile_id` 会报错，无法编译通过。

## 5. 特殊处理：await_sender

`await_sender.hh:41` 有：
```cpp
std::shared_ptr<void> context_holder;  // 类型擦除持有 context
```

`await_sender.hh:285`：
```cpp
state.context_holder = context;  // 保持 context 在协程挂起期间存活
```

**分析**：
- `await_sender(sender)` 无参版本：内部创建 `make_root_context()`，是 shared_ptr → 赋值给 `shared_ptr<void>` 正常工作
- `await_sender(sender, context)` 有参版本：用户传入 context
  - 如果传入 root_context（shared_ptr）→ 正常
  - 如果传入 pushed_context（context_ptr）→ 不能赋值给 `shared_ptr<void>`

**解决方案**：`context_holder` 的目的是防止 context 在协程挂起期间被释放。对于 pushed_context（裸指针），其生命周期由 push/pop 配对管理，pipeline 活着（`scope_holder` 已保证）则 context 活着。因此：
- 当 context 是 shared_ptr 时：`context_holder = context`（不变）
- 当 context 是 context_ptr 时：不需要存 context_holder（scope_holder 已保证生命周期）

实现：
```cpp
if constexpr (requires { std::shared_ptr<void>(context); }) {
    state.context_holder = context;
}
// context_ptr 不进入此分支，不需要额外持有
```

## 6. 涉及文件清单

### 6.1 框架核心（必改）

| 文件 | 改动 |
|------|------|
| `src/pump/core/context.hh` | 新增 `context_ptr<T>`。`pushed_context` 加 slab `operator new/delete`。删除 `when_all_context`（死代码）。`pushed_context` 构造函数改为接收 `base_t` 而非引用（值语义，裸指针可直接拷贝） |
| `src/pump/sender/push_context.hh` | `get_new_context()` 改为 `return context_ptr(new pushed_context<...>(...))` 替代 `make_shared`。`compute_context_type` 特化改为 `context_ptr<pushed_context<...>>` |
| `src/pump/sender/pop_context.hh` | op_pusher 的 push_value/push_exception/push_skip/push_done 中加 `delete context.get()`（在 `has_id` 匹配的分支中） |
| `src/pump/sender/await_sender.hh` | `context_holder` 赋值加 `if constexpr` 判断 |

### 6.2 框架核心（需检查，可能无需改动）

| 文件 | 说明 |
|------|------|
| `src/pump/sender/get_context.hh` | `_get()` 通过 `context->datas` 和 `context->base_context` 访问。`operator->` 行为不变（context_ptr 和 shared_ptr 都有），**无需改动** |
| `src/pump/sender/submit.hh` | 接收 `context_t context` 参数传给 `push_value`。context_t 是 shared_ptr（root_context），**无需改动** |
| `src/pump/sender/when_all.hh` | `parent_pusher_status` 存 context（值拷贝）。context_ptr 可直接拷贝，**无需改动** |
| `src/pump/core/scope.hh` | `scope_slab` 被复用，**无需改动** |
| `src/pump/core/compute_sender_type.hh` | `compute_context_type` 基础模板，**无需改动** |
| `src/env/scheduler/*/` | 所有 `[context = context, scope = scope]` 捕获。context_ptr 可直接拷贝，**无需改动** |

### 6.3 应用层

**零改动。** 应用代码只通过以下 API 接触 context：
- `make_root_context()` → 不变（仍返回 shared_ptr）
- `push_context()` / `pop_context()` / `with_context()` → sender 组合语法不变
- `get_context<T>()` → 返回 `T&` 引用，不变
- `submit(ctx)` → 不变
- 自建 scheduler 的 `[context = context]` → 语法不变，只是底层类型从 shared_ptr 变为 context_ptr

## 7. `_get()` 函数适配

`_get()` 通过 `context->` 和 `context->base_context` 遍历 context 链。需要确认两种指针类型都支持统一的 `->` 和 `element_type`：

| 成员 | `shared_ptr<T>` | `context_ptr<T>` |
|------|-----------------|------------------|
| `operator->` | 有 | 有（设计中已包含） |
| `element_type` | `T`（标准定义） | `T`（context_ptr 中定义） |
| `context->datas` | OK | OK |
| `context->base_context` | OK | OK |

`_get()` 使用 `context_t::element_type::data_size` 等编译期常量 — `context_ptr::element_type` 提供相同的类型接口。**无需改动 `_get()`。**

## 8. 实施步骤

1. **context.hh**：新增 `context_ptr<T>`，给 `pushed_context` 加 slab allocator，删除 `when_all_context`
2. **push_context.hh**：`get_new_context()` 改用 `context_ptr(new ...)`，更新 `compute_context_type` 特化
3. **pop_context.hh**：四个路径（push_value/push_exception/push_skip/push_done）加 `delete context.get()`
4. **await_sender.hh**：`context_holder` 赋值加条件编译
5. **编译验证**：全量编译（所有 apps + tests）
6. **测试**：运行 `scope_stress_test`、`sequential_test`、`concurrent_loop_test`、`when_any_test` 等现有测试
7. **性能验证**：AiSAQ search benchmark 对比前后

## 9. 预期收益

| 项目 | 改动前 | 改动后 |
|------|--------|--------|
| scheduler 回调 context 拷贝 | atomic refcount++ | 裸指针拷贝 |
| scheduler 回调 context 销毁 | atomic refcount-- | 无 |
| push_context 分配 | `make_shared`（堆分配 + atomic 初始化） | slab 分配（thread_local free list，多数情况 pool hit） |
| pop_context 释放 | refcount-- → 可能触发 dealloc | slab 释放（归还 free list） |
| 每次 scheduler hop 原子操作数 | 2（increment + decrement） | 0 |
