# Session Composition 设计文档

## 1. 核心思想

Session 由多个 **layer** 组合而成，每个 layer 是一个普通 struct，通过 **tag dispatch + `requires` 编译期链式分发** 协作。

```cpp
session_t<layer_A, layer_B, layer_C, ...>
```

- `session_t` 本身只是一个 tuple + invoke 分发器，没有业务逻辑
- 所有能力（fd 管理、缓冲区、解包、请求匹配、应用状态）全部来自 layers
- 每个 layer 只响应自己认识的 tag，不认识的自动 fallthrough 到下一个 layer
- layer 可以通过 owner（session 引用）回调其他 layer 的 tag，实现层间协作

## 2. session_t 骨架

```cpp
template<typename ...layer_t>
struct session_t {
    std::tuple<layer_t...> _impl;

    // --- invoke: first-match 分发（数据路由、操作请求、工厂查询） ---

    template<uint32_t index_v, typename tag_t, typename ...args_t>
    auto _invoke(const tag_t& tag, args_t&&... args) {
        static_assert(index_v < sizeof...(layer_t), "unhandled tag");
        if constexpr (requires { std::get<index_v>(_impl).invoke(tag, *this, __fwd__(args)...); })
            return std::get<index_v>(_impl).invoke(tag, *this, __fwd__(args)...);
        else
            return _invoke<index_v + 1>(tag, __fwd__(args)...);
    }

    template<typename tag_t, typename ...args_t>
    auto invoke(const tag_t& tag, args_t&&... args) {
        return _invoke<0>(tag, __fwd__(args)...);
    }

    // --- broadcast: all-match 广播（生命周期事件，所有匹配 layer 都会被调用） ---

    template<uint32_t index_v = 0, typename tag_t, typename ...args_t>
    void broadcast(const tag_t& tag, args_t&&... args) {
        if constexpr (index_v < sizeof...(layer_t)) {
            if constexpr (requires { std::get<index_v>(_impl).invoke(tag, *this, args...); })
                std::get<index_v>(_impl).invoke(tag, *this, args...);
            broadcast<index_v + 1>(tag, __fwd__(args)...);  // 不停，继续遍历所有 layer
        }
    }
};
```

**两种分发语义**：

| 方法 | 语义 | 适用场景 |
|------|------|---------|
| `invoke(tag, args...)` | **first-match**：找到第一个匹配的 layer 执行并返回 | 数据路由（`on_frame`）、操作请求（`do_recv`）、工厂查询（`get_recv_sender`）|
| `broadcast(tag, args...)` | **all-match**：遍历所有匹配的 layer 依次执行，无返回值 | 生命周期事件（`on_error`、`do_close`）|

**要点**：
- `invoke` / `broadcast` 自动注入 `*this`，调用者不需要手动传 session 引用
- layer 的 invoke 签名统一为 `invoke(tag, owner_t& owner, args...)`
- owner 是 `session_t<...>&`，layer 通过 `owner.invoke(other_tag, ...)` 或 `owner.broadcast(other_tag, ...)` 调用其他 layer
- `broadcast` 无返回值（`void`）——生命周期事件不需要返回结果

## 3. Tag 体系

Tag 是空 struct + constexpr 实例，用于编译期分发。按职责分为两类：

### 3.1 事件 tag（scheduler → session）

scheduler 的 advance() 在 IO 事件发生时调用 session。数据从底层向上流动。

Tag 都是空 struct，数据作为 invoke 的额外参数传递（如 `s->invoke(on_recv{}, bytes)`）。

| Tag | 参数 | 含义 | 调用方 |
|-----|------|------|--------|
| `on_recv` | `int bytes` | TCP 字节到达 | session_scheduler |
| `on_kcp_recv` | `net_frame&&` | KCP 完整消息到达 | kcp scheduler |
| `on_frame` | `net_frame&&` | **收敛点**：完整帧到达 | ring_buffer / kcp_bind |
| `on_error` | `exception_ptr` | IO 错误（**用 broadcast 调用**） | scheduler / bind layer |

### 3.2 操作 tag（sender op → session）

pipeline 的 sender op 在 `start()` 中调用 session，注册回调或发起操作。

| Tag | 含义 | 响应 layer | 调用方 |
|-----|------|-----------|--------|
| `do_recv` | 注册 recv 回调到队列 | frame_receiver | recv sender op |
| `do_send` | 提交发送请求 | tcp_bind / kcp_bind | send sender op |
| `do_join` | 注册 session 到 scheduler IO 循环 | tcp_bind / kcp_bind | join sender op |
| `do_stop` | 从 scheduler 解除注册 | tcp_bind / kcp_bind | stop sender op |
| `do_wait_response` | 注册 RPC 响应等待 | rpc_trigger_layer | trigger sender op |

### 3.3 sender 工厂 tag（pipeline API → session）

返回 sender 对象，供 pipeline 组合使用。这是 session "半个 scheduler" 特性的体现。

**每个 sender 工厂 tag 由持有相关队列/状态的 layer 直接响应**，不需要 bind layer 转发。

| Tag | 返回值 | 响应 layer | 用途 |
|-----|--------|-----------|------|
| `get_recv_sender` | `recv_sender<session_t>` | frame_receiver | `tcp::recv(session)` |
| `get_wait_response_sender` | `wait_response_sender<session_t>` | rpc_trigger_layer | RPC 客户端等待响应 |

**注意**：`tcp::send()` 不需要 sender 工厂 tag。send_sender 由 `tcp::send()` 自由函数直接构造，内部通过 `invoke(prepare_send{})` + `invoke(do_send{})` 完成。

### 3.4 数据查询 tag（scheduler → session）

scheduler 需要从 session 获取底层数据。

| Tag | 返回值 | 响应 layer | 调用方 |
|-----|--------|-----------|--------|
| `get_fd{}` | `int` | tcp_bind | scheduler（io_uring/epoll 注册 fd） |
| `get_status{}` | `session_status` | tcp_bind | scheduler（检查 session 状态） |
| `get_read_iov{}` | `pair<iovec*, int>` | tcp_ring_buffer | scheduler（提交 readv） |

### 3.5 join/stop 是 scheduler 操作

join/stop 是把 session 注册到/移除出 scheduler 的 IO 循环，不属于 session layer 的能力。但由于 bind layer 持有 scheduler 引用，API 不需要额外传 scheduler：

```cpp
tcp::join(session);    // 内部：session->invoke(do_join{}, req) → bind layer → scheduler->schedule_join(req)
tcp::stop(session);    // 内部：session->invoke(do_stop{}, req) → bind layer → scheduler->schedule_stop(req)
```

join/stop 的 tag（`do_join`, `do_stop`）由 bind layer 响应，bind layer 转发给 scheduler。这和 `do_send` 的模式一致。

## 4. Layer 设计

### 4.1 tcp_bind layer

持有 scheduler 引用、fd、状态。**只处理自己独有的数据操作**，不转发其他 layer 能直接响应的 tag。

设计原则：tag dispatch 会自动路由到能处理它的 layer，tcp_bind 不需要做中间人。例如 `get_read_iov` 由 ring_buffer 直接响应，`get_recv_sender` 由 frame_receiver 直接响应。

```cpp
template<typename scheduler_t>
struct tcp_bind {
    scheduler_t* scheduler;
    int fd;
    std::atomic<session_status> status{session_status::normal};

    // 提供 fd（scheduler 用于 io_uring/epoll 注册）
    template<typename owner_t>
    auto invoke(const get_fd&, owner_t& owner) { return fd; }

    // 提供 status
    template<typename owner_t>
    auto invoke(const get_status&, owner_t& owner) {
        return status.load(std::memory_order_relaxed);
    }

    // IO 提交：统一通过 scheduler 调度
    template<typename owner_t>
    auto invoke(const do_join&, owner_t& owner, auto* req) {
        scheduler->schedule_join(req);
    }

    template<typename owner_t>
    auto invoke(const do_stop&, owner_t& owner, auto* req) {
        scheduler->schedule_stop(req);
    }

    template<typename owner_t>
    auto invoke(const do_send&, owner_t& owner, send_req* req) {
        scheduler->schedule_send(req);
    }

    // 关闭：关 fd + 更新状态 + 广播通知所有 layer 清理资源
    template<typename owner_t>
    auto invoke(const do_close&, owner_t& owner) {
        if (fd >= 0) { ::close(fd); fd = -1; }
        status.store(session_status::closed);
        owner.broadcast(on_error{}, std::make_exception_ptr(session_closed_error()));
    }
};
```

### 4.2 tcp_ring_buffer layer

持有 packet_buffer，处理字节流接收和缓冲区管理。

```cpp
struct tcp_ring_buffer {
    packet_buffer buf;

    explicit tcp_ring_buffer(uint32_t size) : buf(size) {}

    // scheduler 调用：字节到达
    template<typename owner_t>
    auto invoke(const on_recv&, owner_t& owner, int bytes) {
        buf.forward_tail(bytes);
        // 循环解包，每解出一帧就通知上层
        while (true) {
            auto frame = owner.invoke(unpack{}, buf);
            if (!frame) break;
            owner.invoke(on_frame{}, std::move(*frame));
        }
    }

    // 提供读缓冲区 iovec
    template<typename owner_t>
    auto invoke(const get_read_iov&, owner_t& owner) -> std::pair<iovec*, int> {
        int cnt = buf.make_iovec();
        return {buf.iov(), cnt};
    }
};
```

### 4.3 tcp_length_prefix_unpacker layer

从 ring buffer 中解析长度前缀帧。

```cpp
struct tcp_length_prefix_unpacker {
    // 尝试从 buffer 解出一帧
    template<typename owner_t>
    auto invoke(const unpack&, owner_t& owner, packet_buffer& buf)
        -> std::optional<net_frame>
    {
        // 读 4 字节长度头，检查是否有完整帧，拷贝出来
        return copy_out_frame(&buf);
    }

    // send 路径：填充长度前缀
    template<typename owner_t>
    auto invoke(const prepare_send&, owner_t& owner, send_req* req) {
        auto len = static_cast<uint32_t>(req->frame._len + sizeof(uint32_t));
        memcpy(req->_hdr, &len, sizeof(uint32_t));
        req->_send_vec[0] = {req->_hdr, sizeof(uint32_t)};
        req->_send_vec[1] = {req->frame._data, req->frame._len};
        req->_send_cnt = 2;
    }
};
```

### 4.4 frame_receiver layer

通用帧接收层：管理 recv_q / ready_q，处理帧到达和 recv 请求匹配。

```cpp
template<typename frame_type = net_frame>
struct frame_receiver {
    core::local::queue<recv_req*> recv_q;
    core::local::queue<frame_type*> ready_q;

    // 被动：帧到达
    template<typename owner_t>
    auto invoke(const on_frame&, owner_t& owner, frame_type&& frame) {
        if (auto opt = recv_q.try_dequeue()) {
            opt.value()->cb(std::move(frame));
            delete opt.value();
        } else {
            ready_q.try_enqueue(new frame_type(std::move(frame)));
        }
    }

    // 主动：pipeline 请求接收（由 recv sender op 调用）
    template<typename owner_t>
    auto invoke(const do_recv&, owner_t& owner, recv_req* req) {
        if (auto opt = ready_q.try_dequeue()) {
            req->cb(std::move(*opt.value()));
            delete opt.value();
            delete req;
        } else {
            recv_q.try_enqueue(req);
        }
    }

    // 错误：通知所有等待者
    template<typename owner_t>
    auto invoke(const on_error&, owner_t& owner, std::exception_ptr ex) {
        while (auto opt = recv_q.try_dequeue()) {
            opt.value()->cb_err(ex);
            delete opt.value();
        }
        while (auto opt = ready_q.try_dequeue()) {
            delete opt.value();
        }
    }

    // sender 工厂
    template<typename owner_t>
    auto invoke(const get_recv_sender&, owner_t& owner) {
        return recv_sender<std::decay_t<owner_t>>{&owner};
    }
};
```

### 4.5 rpc_trigger_layer

RPC 客户端的乱序响应匹配。per-session 独立 map，不再需要 thread_local。

```cpp
struct rpc_trigger_layer {
    pending_requests_map map{2048};

    // 被动：帧到达，按 request_id 匹配
    template<typename owner_t>
    auto invoke(const on_frame&, owner_t& owner, net_frame&& frame) {
        auto* header = reinterpret_cast<rpc_frame*>(frame._data);
        auto rid = header->request_id;
        if (auto res = map.on_frame(rid, std::move(frame)))
            res.value().cb(completion_result(std::move(res.value().frame)));
    }

    // 主动：注册等待回调（由 trigger sender op 调用）
    template<typename owner_t>
    auto invoke(const do_wait_response&, owner_t& owner, uint64_t rid, completion_callback&& cb) {
        if (auto res = map.on_callback(rid, std::move(cb)))
            res.value().cb(completion_result(std::move(res.value().frame)));
    }

    // 连接失败：通知所有 pending
    template<typename owner_t>
    auto invoke(const on_error&, owner_t& owner, std::exception_ptr ex) {
        map.fail_all(ex);
    }

    // sender 工厂
    template<typename owner_t>
    auto invoke(const get_wait_response_sender&, owner_t& owner, uint64_t rid) {
        return wait_response_sender<std::decay_t<owner_t>>{&owner, rid};
    }
};
```

**注意**：`rpc_trigger_layer` 和 `frame_receiver` 都响应 `on_frame`。区分方式：
- **RPC 客户端 session**：只用 `rpc_trigger_layer`，不用 `frame_receiver`。trigger 按 request_id 匹配。
- **RPC 服务端 session**：只用 `frame_receiver`。serv_proc 顺序处理，不需要 trigger。
- 或者：RPC 层不直接处理 `on_frame`，而是定义自己的 tag（如 `on_rpc_frame`），由一个 rpc_frame_parser layer 从 `on_frame` 解析 RPC header 后发出。

### 4.6 kcp_bind layer

KCP 版本的 bind layer，持有 kcp scheduler 引用和 conv_id。同样只处理自己独有的数据。

```cpp
template<typename scheduler_t>
struct kcp_bind {
    scheduler_t* scheduler;
    kcp::common::conv_id_t conv_id;
    std::atomic<session_status> status{session_status::normal};

    // KCP recv 直接是完整消息，跳过 ring_buffer + unpacker
    // scheduler 调用 on_kcp_recv，kcp_bind 转换为 on_frame
    template<typename owner_t>
    auto invoke(const on_kcp_recv&, owner_t& owner, net_frame&& frame) {
        owner.invoke(on_frame{}, std::move(frame));
    }

    // IO 提交
    template<typename owner_t>
    auto invoke(const do_send&, owner_t& owner, send_req* req) {
        scheduler->schedule_send(req);
    }

    // 数据查询
    template<typename owner_t>
    auto invoke(const get_conv_id&, owner_t& owner) { return conv_id; }

    template<typename owner_t>
    auto invoke(const get_status&, owner_t& owner) {
        return status.load(std::memory_order_relaxed);
    }

    // 关闭：广播通知所有 layer 清理资源
    template<typename owner_t>
    auto invoke(const do_close&, owner_t& owner) {
        status.store(session_status::closed);
        owner.broadcast(on_error{}, std::make_exception_ptr(session_closed_error()));
    }
};
```

## 5. 具体 Session 类型组合

### 5.1 TCP + 通用帧接收（echo 等简单场景）

```cpp
using tcp_session = session_t<
    tcp_bind<io_uring::session_scheduler_t>,
    tcp_ring_buffer,
    tcp_length_prefix_unpacker,
    frame_receiver<>
>;

// 构造：每个 layer 按顺序传入构造参数
auto* s = new tcp_session{
    tcp_bind{scheduler, fd},
    tcp_ring_buffer{4096},
    tcp_length_prefix_unpacker{},
    frame_receiver<>{}
};
```

数据流：
```
on_recv(bytes)
  → tcp_ring_buffer: forward_tail, 循环 unpack
    → tcp_length_prefix_unpacker: 解析长度前缀
      → on_frame(net_frame)
        → frame_receiver: 匹配 recv_q 或缓存到 ready_q
```

### 5.2 TCP + RPC 服务端

```cpp
using rpc_server_session = session_t<
    tcp_bind<session_scheduler_t>,
    tcp_ring_buffer,
    tcp_length_prefix_unpacker,
    frame_receiver<>                // 服务端顺序处理，不需要 trigger
>;
```

数据流同 5.1。`rpc::serv_proc` 通过 `tcp::recv(session)` 从 frame_receiver 取帧。

### 5.3 TCP + RPC 客户端

```cpp
using rpc_client_session = session_t<
    tcp_bind<session_scheduler_t>,
    tcp_ring_buffer,
    tcp_length_prefix_unpacker,
    rpc_trigger_layer               // 客户端需要 request_id 乱序匹配
>;
```

数据流：
```
on_recv(bytes) → ring_buffer → unpacker → on_frame
  → rpc_trigger_layer: 按 request_id 查 map，匹配 callback 或缓存
```

### 5.4 KCP + RPC 客户端

```cpp
using kcp_rpc_client_session = session_t<
    kcp_bind<kcp_scheduler_t>,
    rpc_trigger_layer               // 和 TCP 共用同一个 trigger layer
>;
```

数据流：
```
on_kcp_recv(net_frame)
  → kcp_bind: 转为 on_frame
    → rpc_trigger_layer: 和 TCP 完全相同的匹配逻辑
```

**不需要 ring_buffer 和 unpacker**——KCP 的 ikcp_recv 已经保证消息完整性。

### 5.5 TCP + Raft（未来）

```cpp
using raft_session = session_t<
    tcp_bind<session_scheduler_t>,
    tcp_ring_buffer,
    tcp_length_prefix_unpacker,
    rpc_trigger_layer,
    raft_state_layer                // term, voted_for, role, ...
>;
```

## 6. Sender 机制

每个 layer 可以通过 sender 工厂 tag 返回 sender。sender 的 op::start() 调用 session->invoke(操作 tag) 来执行实际工作。

### 6.1 公共 API

session 持有 scheduler 引用后，公共 API 不再需要 scheduler 参数：

```cpp
namespace pump::scheduler::tcp {
    // 现在（需要 scheduler 参数）
    tcp::recv(scheduler, session);
    tcp::send(scheduler, session, data, len);
    tcp::join(scheduler, session);
    tcp::stop(scheduler, session);

    // 变为（bind layer 持有 scheduler，不需要额外传）
    tcp::recv(session);
    tcp::send(session, data, len);
    tcp::join(session);
    tcp::stop(session);
}
```

实现：

```cpp
// recv: 通过 sender 工厂 tag，frame_receiver 响应
template<typename session_t>
inline auto recv(session_t* session) {
    return session->invoke(get_recv_sender{});
}

// send: 直接构造 sender，不需要工厂 tag
template<typename session_t>
inline auto send(session_t* session, void* data, uint32_t len) {
    return send_sender<session_t>{session, data, len};
}

// join/stop: 通过操作 tag，bind layer 响应
template<typename session_t>
inline auto join(session_t* session) {
    return join_sender<session_t>{session};
    // op::start() → session->invoke(do_join{}, req) → bind layer → scheduler
}
```

### 6.2 Sender 内部结构

以 recv_sender 为例：

```cpp
template<typename session_t>
struct recv_sender {
    session_t* session;

    auto make_op() { return recv_op<session_t>{session}; }

    template<typename context_t>
    auto connect() {
        return core::builder::op_list_builder<0>().push_back(make_op());
    }
};

template<typename session_t>
struct recv_op {
    constexpr static bool session_recv_op = true;
    session_t* session;

    template<uint32_t pos, typename context_t, typename scope_t>
    auto start(context_t& ctx, scope_t& scope) {
        session->invoke(do_recv{}, new recv_req{
            [ctx=ctx, scope=scope](auto&& frame) mutable {
                op_pusher<pos+1, scope_t>::push_value(ctx, scope, __fwd__(frame));
            },
            [ctx=ctx, scope=scope](std::exception_ptr ex) mutable {
                op_pusher<pos+1, scope_t>::push_exception(ctx, scope, ex);
            }
        });
    }
};
```

### 6.3 send sender 的 prepare 路径

send sender 的 op::start() 通过 invoke 完成 prepare + submit：

```cpp
template<uint32_t pos, typename context_t, typename scope_t>
auto start(context_t& ctx, scope_t& scope) {
    auto* req = new send_req{fd, std::move(frame), callback};
    session->invoke(prepare_send{}, req);   // unpacker layer 填充 iovec
    session->invoke(do_send{}, req);        // bind layer 提交给 scheduler
}
```

或者合并为单个 tag，由 bind layer 内部依次调用 prepare + schedule。

## 7. Scheduler 适配

session_scheduler 的改动：不再需要了解 session 内部结构，统一通过 invoke 交互。

```cpp
// scheduler 只需要知道 session 满足最基本的 invoke 接口
void submit_read(session_t* s) {
    auto [iov, cnt] = s->invoke(get_read_iov{});
    // io_uring_prep_readv(sqe, s->invoke(get_fd{}), iov, cnt, 0);
    // ...
}

void on_read_complete(session_t* s, int bytes) {
    s->invoke(on_recv{}, bytes);
}

void on_io_error(session_t* s, std::exception_ptr ex) {
    s->broadcast(on_error{}, ex);  // 广播：所有 layer 清理资源
}
```

session_scheduler 的模板参数简化——不再需要 4 个 op template template 参数：

```cpp
// 现在
template<
    template<typename,typename> class join_op_t,
    template<typename,typename> class recv_op_t,
    template<typename> class send_op_t,
    template<typename,typename> class stop_op_t,
    typename session_t
>
struct session_scheduler { ... };

// 变为
template<typename session_t>
struct session_scheduler { ... };
```

因为 op 类型不再需要 friend 访问 scheduler 的 private schedule()——op 通过 session->invoke(do_xxx) 间接提交，bind layer 内部持有 scheduler 引用。

## 8. Transport Trait 适配

RPC 的 transport trait 也简化：

```cpp
// 现在
struct tcp_transport {
    using address_type = default_session<>*;
    static auto recv(auto* sche, address_type addr) {
        return tcp::recv(sche, addr);
    }
    static auto send(auto* sche, address_type addr, void* data, uint32_t len) {
        return tcp::send(sche, addr, data, len);
    }
    static uint64_t address_raw(const address_type& addr) {
        return reinterpret_cast<uint64_t>(addr);
    }
};

// 变为
template<typename session_t>
struct tcp_transport {
    using address_type = session_t*;

    static auto recv(address_type session) {
        return tcp::recv(session);
    }
    static auto send(address_type session, void* data, uint32_t len) {
        return tcp::send(session, data, len);
    }
    static uint64_t address_raw(const address_type& addr) {
        return reinterpret_cast<uint64_t>(addr);
    }
};
```

transport trait 的 `recv`/`send` 不再需要 scheduler 参数——session 自己知道 scheduler。

**注意**：这改变了 transport trait 的签名（去掉 scheduler 参数）。RPC 的 serv.hh / call.hh 中调用 `transport_t::recv(sche, addr)` 需要改为 `transport_t::recv(addr)`。这是一个接口变更，需要同步修改 RPC 模块和 KCP transport trait。

## 9. 与现有 tcp_session concept 的关系

现有的 `tcp_session` concept 要求直接方法（`make_read_iov()`, `on_recv()`, `try_recv()`, `close()`）。

新设计中 `session_t` 统一使用 `invoke(tag, ...)`。两种方式：

**方案 A**：废弃 tcp_session concept，scheduler 直接调用 invoke。
```cpp
s->invoke(on_recv{}, bytes);         // 替代 s->on_recv(bytes)
s->invoke(get_read_iov{});           // 替代 s->make_read_iov()
```

**方案 B**：session_t 提供 concept 要求的方法作为 invoke 的快捷方式。
```cpp
template<typename ...layer_t>
struct session_t {
    // concept 兼容方法
    auto make_read_iov() { return invoke(get_read_iov{}); }
    void on_recv(int bytes) { invoke(on_recv_tag{}, bytes); }
    void close() { invoke(do_close{}); }
    // ...
};
```

**建议方案 A**——concept 是为旧设计服务的，新设计用 invoke 更一致。scheduler 代码量很小，改成 invoke 调用即可。

## 10. 设计约束与解法

### 10.1 invoke 是 first-match：同一个 tag 只能有一个 handler

`invoke` 从 index 0 开始扫描，找到第一个匹配的 layer 就返回。如果一个 layer 想"处理后转发给下一个同 tag 的 handler"，`owner.invoke(same_tag)` 会匹配到自己 → 死循环。

**场景**：带心跳过滤的协议。heartbeat_filter 和 frame_receiver 都想响应 `on_frame`。

**解法**：不同层级用不同 tag 名。

```
ring_buffer 解包后发 → on_raw_frame（或模板化 emit tag）
heartbeat_filter 响应 on_raw_frame，过滤后发 → on_frame
frame_receiver 响应 on_frame
```

ring_buffer 可模板化 emit tag，简单协议直接发 `on_frame`，需要过滤时换 tag：

```cpp
template<typename emit_tag_t = struct on_frame>
struct tcp_ring_buffer { ... };
```

**规则**：一个 composition 中，每个 tag 最多一个 handler（用 `invoke` 的 tag）。需要"链式处理"时用不同 tag 名串联。

### 10.2 生命周期事件用 broadcast

多个 layer 都可能持有需要清理的资源（recv_q、pending_map、timer 等）。`on_error` 必须通知所有 layer。

**规则**：
- 数据路由 / 操作请求 / 查询 → `invoke`（first-match，有返回值）
- 资源清理通知（`on_error`） → `broadcast`（all-match，无返回值）

`do_close` 本身是 **first-match**（只有 bind layer 处理：关 fd + 改 status），bind layer 内部再 `broadcast(on_error{})` 通知所有 layer 清理。

哪些 tag 用 broadcast 由调用方决定（scheduler 或 bind layer）。Layer 不需要区分——它的 `invoke(on_error&, ...)` 既能被 `invoke` 调用也能被 `broadcast` 调用。

**约束**：broadcast 的参数会传给多个 layer，因此必须是可拷贝的（如 `std::exception_ptr`）。Move-only 参数不能用于 broadcast。

### 10.3 on_frame tag 冲突

`frame_receiver` 和 `rpc_trigger_layer` 都响应 `on_frame`。按 10.1 规则，同一个 session 里只能有一个。

- **RPC 服务端 session**：用 `frame_receiver`（FIFO 顺序取帧）
- **RPC 客户端 session**：用 `rpc_trigger_layer`（按 request_id 匹配）
- **不会同时存在**——客户端和服务端是不同的 session 类型

如果需要同时存在（如双向 RPC），插入 demux layer：

```cpp
struct rpc_demux_layer {
    template<typename owner_t>
    auto invoke(const on_frame&, owner_t& owner, net_frame&& frame) {
        auto* hdr = reinterpret_cast<rpc_frame*>(frame._data);
        if (hdr->flags & RESPONSE_FLAG)
            owner.invoke(on_rpc_response{}, std::move(frame));  // → trigger
        else
            owner.invoke(on_rpc_request{}, std::move(frame));   // → receiver
    }
};
```

trigger 响应 `on_rpc_response`，receiver 响应 `on_rpc_request`，不冲突。

## 11. 文件组织

每个 layer 跟着自己的模块走，`session_t` 骨架和通用 layer 放公共位置。

```
src/env/common/
  session.hh                    ← session_t 骨架（tuple + invoke + broadcast）
  session_tags.hh               ← 通用 tag（on_frame, on_error, do_close）
  frame_receiver.hh             ← frame_receiver layer（通用，TCP/KCP/任何协议共用）

src/env/scheduler/tcp/
  common/
    struct.hh                   ← 现有类型（send_req 等）
    layers.hh                   ← tcp_bind, tcp_ring_buffer, tcp_length_prefix_unpacker
    tags.hh                     ← TCP 专用 tag（on_recv, get_fd, get_read_iov, get_status）
  senders/                      ← recv, send, join, stop sender（改用 session->invoke）
  tcp.hh                        ← 公共 API

src/env/scheduler/kcp/
  layers.hh                     ← kcp_bind layer
  tags.hh                       ← KCP 专用 tag（on_kcp_recv, get_conv_id）

src/env/scheduler/rpc/
  layers.hh                     ← rpc_trigger_layer（+ demux layer 如需要）
  tags.hh                       ← RPC 专用 tag（do_wait_response, on_rpc_response 等）
```

原则：
- **谁的数据谁的 layer** — tcp_bind 在 tcp 模块，kcp_bind 在 kcp 模块
- **通用的放 common** — session_t、frame_receiver 不属于任何协议
- **Tag 跟着 layer 走** — TCP tag 在 tcp/，RPC tag 在 rpc/
- **应用层组合** — 应用 `#include` 需要的 layer，拼出自己的 session 类型

## 12. 实现顺序

1. `env/common/session.hh` — session_t 骨架（tuple + invoke + broadcast）
2. `env/common/session_tags.hh` — 通用 tag（on_frame, on_error, do_close）
3. `tcp/common/tags.hh` — TCP 专用 tag
4. `tcp/common/layers.hh` — tcp_bind, tcp_ring_buffer, tcp_length_prefix_unpacker
5. `env/common/frame_receiver.hh` — frame_receiver layer
6. 改造 `tcp/senders/` — 改用 session->invoke
7. 改造 `tcp/tcp.hh` — 公共 API（去掉 scheduler 参数的 recv/send）
8. 改造 `io_uring/scheduler.hh` — 对接新 session（去掉 4 个 op 模板参数）
9. 跑通 echo 测试
10. `rpc/layers.hh` — rpc_trigger_layer
11. `rpc/tags.hh` — RPC 专用 tag
12. 改造 RPC transport trait + serv.hh + call.hh
13. 跑通 RPC 测试
14. `kcp/layers.hh` — kcp_bind layer
15. `kcp/tags.hh` — KCP 专用 tag
16. 跑通 KCP + RPC 测试
