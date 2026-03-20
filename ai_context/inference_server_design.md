# PUMP 推理服务设计文档

## 1. 目标

基于 PUMP 框架 + llama.cpp/ggml 构建低延迟 LLM 推理服务器。

### 1.1 核心卖点

**零阻塞全异步 LLM 推理**：ggml 的 `graph_compute_async()` 本身非阻塞，但上层 llama.cpp server 用线程+信号量同步等待结果。PUMP 用 polling 替代同步等待，消除线程切换和信号量开销。

一条声明式 pipeline 完成 **网络 → tokenize → GPU推理 → detokenize → 流式发送** 全流程：

```cpp
tcp::recv(session)
    >> then(parse_request)
    >> on(llm_sched.as_task())      // 切到 LLM scheduler 核心
    >> then(tokenize_and_prefill)
    >> flat_map([](auto&& state) {
        return generate_loop(state);  // 自回归 decode 循环
    })
    >> submit(ctx);
```

**对比现有方案的优势**：

| 维度 | llama.cpp server | vLLM | PUMP 推理服务 |
|------|-----------------|------|--------------|
| GPU 同步 | `cudaStreamSynchronize`（阻塞线程） | Python asyncio + CUDA event | `cuEventQuery` 轮询（零阻塞） |
| 线程模型 | 多线程 + mutex | Python GIL + 多进程 | share-nothing 单线程 |
| 网络层 | httplib（阻塞 I/O） | uvloop（Python） | io_uring / epoll（零系统调用） |
| 调度开销 | 线程唤醒 ~3-10μs | Python 协程切换 ~50μs | per_core::queue ~0.1μs |

### 1.2 范围

**包含**：
- 真实 LLM 推理（Qwen3.5 7B、Llama 等，GGUF 格式）
- Token 流式输出（SSE 或自定义二进制协议）
- Continuous batching（多请求共享 GPU）
- CUDA 加速（通过 ggml CUDA backend）

**不包含**：
- 模型训练 / 微调
- 模型格式转换（使用现成 GGUF）
- OpenAI 兼容 API（MVP 阶段用自定义协议，后续可加）
- 多 GPU tensor parallelism（单 GPU 先做通）

---

## 2. ggml 集成方案

### 2.1 核心发现

ggml CUDA backend 的 `graph_compute` 分为两步（`ggml-backend.cpp:1791`）：

```
graph_compute = graph_compute_async + synchronize
                (非阻塞: 提交kernel)   (阻塞: cudaStreamSynchronize)
```

**集成思路**：调用 `graph_compute_async()`（所有 kernel 提交到 ggml 的 CUDA stream）→ 在 ggml stream 上 `cudaEventRecord` → 我们的 advance() 循环 `cuEventQuery` 轮询 → 完成后回调。

**零 ggml 代码修改**：
- `graph_compute_async()` 是 ggml 公开 API
- `cudaStream_t`（Runtime API）和 `CUstream`（Driver API）是同一底层类型，可互操作
- ggml stream: `cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking)`（`common.cuh:1319`）

### 2.2 ggml 已有能力

| 能力 | ggml 实现 | 我们需要做的 |
|------|----------|-------------|
| ~60 种 CUDA 算子 | `ggml_cuda_compute_forward`（switch 分派） | 无 |
| CUDA Graph 捕获/重放 | 内置（warmup → capture → replay） | 无 |
| 量化推理（Q4/Q8等） | ggml 核心能力 | 无 |
| KV cache 管理 | llama.cpp `llama_kv_cache` | 使用 llama.cpp API |
| GGUF 模型加载 | `llama_model_load` | 使用 llama.cpp API |
| Tokenizer | `llama_tokenize` / `llama_token_to_piece` | 使用 llama.cpp API |
| Sampling | `llama_sampler` | 使用 llama.cpp API |

### 2.3 调用链

```
PUMP llm_scheduler
  │
  ├─ 初始化时
  │    llama_model_load(gguf_path)    → llama_model*
  │    llama_init_from_model(model)   → llama_context*（含 ggml CUDA backend）
  │
  ├─ prefill（一次性处理 prompt tokens）
  │    llama_batch_init(n_tokens)
  │    llama_batch_add(batch, token_id, pos, ...)
  │    ggml_backend_sched_graph_compute_async(sched, graph)  ← 非阻塞
  │    cudaEventRecord(event, ggml_stream)                   ← 标记完成点
  │    → 返回，advance() 轮询 event
  │
  ├─ decode（逐 token 生成）
  │    llama_batch = {1 token}
  │    ggml_backend_sched_graph_compute_async(...)            ← 非阻塞
  │    cudaEventRecord(event, ggml_stream)
  │    → 轮询完成 → sample → 检查 EOS → 流式发送 token
  │
  └─ 销毁
       llama_free(ctx)
       llama_model_free(model)
```

---

## 3. 架构

### 3.1 Scheduler 布局与核心绑定

参考 KV 应用的 `by_core[]` + JSON 配置模式，每个 scheduler 通过配置文件绑定到特定核心。

**配置文件**（`config.json`）：

```json
{
    "tcp": {
        "core": [0, 1, 2, 3],
        "port": 8080
    },
    "llm": {
        "core": [4],
        "model": "qwen3.5-7b-q4_k_m.gguf",
        "ctx_size": 4096,
        "gpu_layers": 999
    }
}
```

**布局**：

```
┌──────────────────────────────────────────────────────────┐
│  Core 0..3: TCP scheduler + task_scheduler               │
│    每核独立 accept/recv/send (share-nothing)              │
│    per_core::queue 投递推理请求到 LLM 核心                │
│                                                          │
│  Core 4: LLM scheduler + task_scheduler (独占)           │
│    持有 llama_model* + llama_context*                     │
│    drain req_q → 调用 ggml async compute                 │
│    advance() 轮询 CUevent → 触发 callback                │
│    token 生成后通过 session 流式发回客户端                 │
│    ggml graph_compute_async ~0.5-1ms CPU 提交时间         │
│    独占核心 → 不影响其他 scheduler                        │
│                                                          │
│  (Phase 3) Continuous batching 在 LLM scheduler 内实现    │
│    合并多个请求的 token 到同一 llama_batch                 │
│    prefill 和 decode 请求混合调度                          │
└──────────────────────────────────────────────────────────┘
```

**初始化**（参考 KV 的 `init_schedulers`）：

```cpp
struct scheduler_objects {
    struct { tcp::scheduler* by_core[MAX_CORES]; } tcp_schedulers;
    struct { task::scheduler* by_core[MAX_CORES]; } task_schedulers;
    struct { llm_scheduler*   by_core[MAX_CORES]; } llm_schedulers;
};

inline void init_schedulers(const config& cfg) {
    for (auto c : cfg.tcp.core) {
        tcp_schedulers.by_core[c] = new tcp::scheduler(...);
        task_schedulers.by_core[c] = new task::scheduler(c);
    }
    for (auto c : cfg.llm.core) {
        llm_schedulers.by_core[c] = new llm_scheduler(cfg.llm);
        task_schedulers.by_core[c] = new task::scheduler(c);
    }
}
```

**每核心 advance 循环**（参考 KV 的 `task_proc`）：

```cpp
void core_loop(uint32_t core) {
    pump::core::this_core_id = core;
    while (running) {
        task_schedulers.by_core[core]->advance();
        if (tcp_schedulers.by_core[core])
            tcp_schedulers.by_core[core]->advance();
        if (llm_schedulers.by_core[core])
            llm_schedulers.by_core[core]->advance();
    }
}
```

LLM scheduler 独占 Core 4：`graph_compute_async()` 的 ~0.5-1ms CPU 提交时间不影响 TCP 核心。GPU 计算（10-30ms/token）是真正瓶颈，多核心跑 LLM scheduler 无意义——吞吐提升靠 continuous batching（合并多请求到同一 `llama_batch`），不靠多核心。

### 3.2 llm_scheduler 设计

遵循 PUMP 自建 scheduler 六组件模式：

```cpp
namespace inference {

// req 类型
namespace _llm_op {
    struct req {
        enum class type { generate, cancel };
        type op_type;
        // generate: prompt tokens + 回调（流式，每 token 调一次）
        std::vector<llama_token> prompt_tokens;
        std::move_only_function<void(llama_token, bool is_eos)> on_token;
        // 或 cancel: 取消某个 request_id
        uint64_t request_id;
    };
}

// scheduler
struct llm_scheduler {
    // ggml/llama 资源
    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    CUevent completion_event;           // 用于轮询 GPU 完成

    // 请求队列
    core::per_core::queue<_llm_op::req*> req_q;

    // 当前 in-flight 状态
    struct inflight_request {
        _llm_op::req* req;
        int n_decoded = 0;              // 已生成 token 数
        enum state { prefill, decode_submitted, idle } state;
    };
    inflight_request* current = nullptr; // 单 stream → 同时只有一个 GPU 任务

    void schedule(_llm_op::req* r) { req_q.try_enqueue(r); }

    bool advance() {
        // 1. 检查 GPU 完成
        if (current && current->state == inflight_request::decode_submitted) {
            if (cuEventQuery(completion_event) == CUDA_SUCCESS) {
                // GPU 完成 → sample token
                auto token = sample_token();
                bool is_eos = (token == llama_token_eos(model));
                current->req->on_token(token, is_eos);
                if (is_eos) {
                    finish_request(current);
                    current = nullptr;
                } else {
                    // 提交下一个 decode step
                    submit_decode(current, token);
                }
                return true;
            }
            return false; // GPU 还在跑
        }

        // 2. 接收新请求
        return req_q.drain([this](_llm_op::req* r) {
            start_prefill(r);
        });
    }

private:
    void start_prefill(_llm_op::req* r) {
        // llama_batch_add all prompt tokens
        // ggml_backend_sched_graph_compute_async(...)
        // cudaEventRecord(completion_event, ggml_stream)
        current = new inflight_request{r, 0, inflight_request::prefill};
    }

    void submit_decode(inflight_request* inf, llama_token token) {
        // llama_batch = {token, pos=inf->n_decoded++}
        // ggml_backend_sched_graph_compute_async(...)
        // cudaEventRecord(completion_event, ggml_stream)
        inf->state = inflight_request::decode_submitted;
    }

    llama_token sample_token() {
        // llama_sampler_sample(sampler, ctx, -1)
    }
};
}
```

### 3.3 请求生命周期

```
客户端: "讲个故事" (TCP send)
  │
  ▼ Core i: TCP scheduler
  tcp::recv(session) → net_frame → parse prompt text
  │
  ▼ per_core::queue 投递
  llm_scheduler.schedule(req{tokenize(prompt), on_token_cb})
  │
  ▼ Core L: llm_scheduler advance()
  start_prefill: llama_batch(all prompt tokens)
    → ggml_backend_sched_graph_compute_async()  (非阻塞)
    → cudaEventRecord(event, ggml_stream)
  │
  ▼ Core L: advance() 轮询
  cuEventQuery(event) == SUCCESS
    → sample token[0] = "从"
    → on_token_cb("从", false) → tcp::send(session, "从")
    → submit_decode(token[0])
    → cudaEventRecord(event)
  │
  ▼ Core L: advance() 轮询
  cuEventQuery == SUCCESS
    → sample token[1] = "前"
    → on_token_cb("前", false) → tcp::send(session, "前")
    → submit_decode(token[1])
  │
  ... (重复 decode → sample → send)
  │
  ▼ token[N] = EOS
  on_token_cb(EOS, true) → tcp::send(session, EOS marker)
  → 释放 request
```

### 3.4 Token 流式输出 Pipeline

```cpp
// 服务端主 pipeline（每个连接）
tcp::recv(session)
    >> then([](net_frame&& frame) {
        // 解析 prompt，tokenize
        auto prompt = parse_prompt(frame);
        auto tokens = llama_tokenize(model, prompt);
        return generate_request{std::move(tokens), session};
    })
    >> on(llm_sched.as_task())    // 切到 LLM scheduler 核心
    >> flat_map([session](generate_request&& req) {
        // 返回一个 sender，流式产出 token
        // 每个 token 生成后通过 session 发回客户端
        return llm::generate(&llm_sched, std::move(req))
            >> then([session](token_result&& r) {
                // 编码并发送 token
                auto frame = encode_token(r.token, r.is_eos);
                return tcp::send(session, frame.data, frame.len);
            }) >> flat();
    })
    >> submit(ctx);
```

---

## 4. 协议

### 4.1 方案选择

**Phase 1：自定义二进制协议**（最快实现）

```
请求帧:
┌──────────────┬──────────────┬───────────────────┐
│ total_len(4) │ req_type(1)  │   payload(变长)    │
│              │ 0=generate   │   UTF-8 prompt     │
│              │ 1=cancel     │   request_id(8)    │
└──────────────┴──────────────┴───────────────────┘

响应帧（流式，每 token 一帧）:
┌──────────────┬──────────────┬───────────────────┐
│ total_len(4) │ flags(1)     │   payload(变长)    │
│              │ 0=token      │   UTF-8 token text │
│              │ 1=eos        │   (空)             │
│              │ 2=error      │   error message    │
└──────────────┴──────────────┴───────────────────┘
```

**Phase 3+：SSE over HTTP**（兼容性好，可直接对接前端）

```
GET /v1/completions
Content-Type: text/event-stream

data: {"token": "从"}
data: {"token": "前"}
...
data: [DONE]
```

### 4.2 帧大小估算

- 请求：5B header + prompt（通常 <4KB）
- 响应：每 token 5B header + 1-12B UTF-8 = ~10-17B/token
- 7B 模型生成 512 tokens：~8KB 总响应（逐帧发送，无需缓冲）

---

## 5. 优化路线

### 5.1 Level 1：单请求逐 token 生成

最简实现：一次只处理一个请求，每个 decode step 调一次 `graph_compute_async` + poll。

```
GPU 利用率: ~5-15% (decode 阶段 batch=1，GPU 大量空闲)
吞吐量: ~30-50 tokens/s (受 decode 延迟制约)
首 token 延迟: ~100-500ms (取决于 prompt 长度)
```

**价值**：验证端到端正确性，跑通流式输出。

### 5.2 Level 2：Continuous Batching

多个请求的 decode step 合并到同一 batch：

```
时刻 T:
  req1 在 decode 第 15 个 token
  req2 在 decode 第 3 个 token
  req3 刚完成 prefill
  → 合并为 batch{token_15_of_req1, token_3_of_req2, token_0_of_req3}
  → 一次 graph_compute_async 处理 3 个 token
  → 各自 sample → 各自流式发送
```

**实现**：LLM scheduler 维护 active_requests 列表，每次 advance 把所有 idle 请求的 next token 合入同一 `llama_batch`。

```
GPU 利用率: ~30-60% (batch=8-32)
吞吐量: ~200-500 tokens/s (7B 模型, batch=16)
```

### 5.3 Level 3：Prefill/Decode 分离调度

Prefill（长 prompt，计算密集）和 Decode（单 token，带宽密集）有不同特征：

- Prefill 中断 decode batch → 所有 decode 请求延迟飙升
- 解法：prefill 拆分为 chunk（如 512 tokens/chunk），与 decode 交替执行

```
时间线:
  [prefill chunk 512] [decode batch x16] [prefill chunk 512] [decode batch x16] ...
```

---

## 6. Benchmark 策略

### 6.1 对标对象

**Baseline A：llama.cpp 自带 server**

```bash
./llama-server -m qwen3.5-7b-q4_k_m.gguf -c 4096 --port 8080
```
- HTTP + SSE 输出
- 多线程 + mutex
- `cudaStreamSynchronize` 阻塞等待
- 预期差距：首 token 延迟 1.2-2x，token 吞吐持平或略高（同一 ggml backend）

**Baseline B：vLLM**

```bash
vllm serve Qwen/Qwen3.5-7B --quantization gptq
```
- Python 运行时 + PagedAttention
- 预期差距：首 token 延迟 2-5x（Python 开销），token 吞吐可能更高（更成熟的 batching）

### 6.2 测量指标

| 指标 | 说明 |
|------|------|
| TTFT (Time to First Token) | 从请求发送到收到第一个 token 的延迟 |
| TPS (Tokens Per Second) | 单请求 token 生成速率 |
| 并发吞吐量 | N 个并发请求下的总 TPS |
| P50/P99 TTFT | 不同并发下的首 token 延迟分布 |
| GPU 利用率 | `nvidia-smi` 或 CUDA profiling |

### 6.3 Benchmark 客户端

```cpp
// PUMP RPC 压测：N 个并发 generate 请求
for_each(loop(N))
    >> concurrent(N)
    >> on(tcp_sched.as_task())
    >> flat_map([](uint32_t i) {
        auto session = connect(server_addr);
        auto start = now();
        return send_prompt(session, prompts[i])
            >> recv_tokens_until_eos(session)
            >> then([start, i](auto&& tokens) {
                record_ttft(i, first_token_time - start);
                record_tps(i, tokens.size() / elapsed(start));
            });
    })
    >> reduce()
    >> then(print_stats)
    >> submit(ctx);
```

### 6.4 核心优势体现点

PUMP 推理服务的优势集中在**系统层**而非**计算层**（计算都是 ggml，相同的 kernel）：

| 场景 | 优势来源 | 预期收益 |
|------|---------|---------|
| 单请求 TTFT | polling 替代线程同步 | 减少 3-10μs/step |
| 高并发 TTFT | share-nothing 零竞争 | P99 延迟显著更低 |
| 长连接流式 | io_uring 零拷贝发送 | 降低 CPU 开销 |
| batch 调度 | 单线程 advance() 天然无锁 | 消除 mutex 开销 |

**关键洞察**：单请求差距不大（ggml 计算主导），高并发下优势放大（系统开销占比增大）。Benchmark 重点放在并发场景。

---

## 7. 实现路线

### Phase 1：端到端 MVP（单请求，验证集成）

- 编译链接 llama.cpp（作为库）
- llm_scheduler 基础骨架：加载模型、prefill、decode loop、poll event
- 自定义二进制协议（generate + token stream）
- TCP 服务端：recv prompt → generate → stream tokens
- 固定 prompt 测试（"Hello, world"）
- **产出：能加载 GGUF 模型，跑通 prompt → 流式生成 token 的完整 pipeline**

### Phase 2：完整功能 + Benchmark

- 多连接支持（请求排队，单 stream 顺序执行）
- 请求取消（cancel 帧）
- 生成参数（temperature、top_p、max_tokens）
- C++ benchmark 客户端
- 对比 llama.cpp server（同模型同量化）
- **产出：TTFT / TPS 对比数据，证明 polling 架构的延迟优势**

### Phase 3：Continuous Batching

- 应用层 batch scheduler（Leader/Follower 变体）
- 多请求 decode 合并到同一 `llama_batch`
- Prefill chunking（与 decode 交替调度）
- 多核 TCP（share-nothing）
- 高并发 benchmark（16/32/64 并发）
- **产出：并发吞吐量曲线，GPU 利用率提升数据**

### Phase 4：生产化

- SSE / OpenAI 兼容 HTTP API
- 请求优先级
- KV cache 容量管理（eviction）
- 长上下文支持
- **产出：可作为 drop-in replacement 的推理服务**

---

## 8. 文件结构

```
apps/inference/
├── main.cc                           ← 入口：初始化、加载模型、启动 schedulers
├── llm/
│   ├── scheduler.hh                  ← llm_scheduler: ggml 异步调度 + event 轮询
│   ├── model.hh                      ← 模型加载/卸载 RAII 封装
│   └── sampler.hh                    ← sampling 参数封装
├── server/
│   ├── protocol.hh                   ← 帧编解码
│   ├── handler.hh                    ← 请求处理 pipeline
│   └── batch_scheduler.hh            ← continuous batching (Phase 3)
├── bench/
│   ├── client.cc                     ← C++ benchmark 客户端
│   └── run_benchmark.sh              ← 自动化对比脚本
└── CMakeLists.txt                    ← 链接 llama.cpp
```

### 8.1 llama.cpp 集成方式

```cmake
# 方案：作为子目录编译
add_subdirectory(third_party/llama.cpp)
target_link_libraries(inference PRIVATE llama ggml)
```

llama.cpp 以源码形式包含（git submodule 或直接复制），确保 CUDA backend 编译启用：

```cmake
set(GGML_CUDA ON)
```

---

## 9. 待讨论

### 9.1 ggml stream 获取方式

需要在 `graph_compute_async()` 之后在 ggml 的 CUDA stream 上 `cudaEventRecord`。获取 stream 的方式：

**方案 A：从 ggml_backend_cuda_context 获取**
```cpp
// ggml_backend_cuda_context 在 common.cuh 中定义
// stream 存储在 streams[device][stream_id]
auto* cuda_ctx = static_cast<ggml_backend_cuda_context*>(backend->context);
cudaStream_t stream = cuda_ctx->stream();  // 返回 streams[device][0]
```
依赖 ggml 内部结构，可能随版本变化。

**方案 B：llama.cpp 提供 stream 访问 API**
如果 llama.cpp 暴露了获取 backend stream 的 API 则用之。

**方案 C：用 cudaStreamQuery 全局查询**
`cudaEventRecord` 在默认 stream 上，但 ggml 用 `cudaStreamNonBlocking` 的 stream，不与默认 stream 同步。此方案不可行。

**倾向方案 A**：ggml 内部结构虽然可能变，但 `stream()` 方法是稳定的。或者可以在初始化时获取一次并缓存。

### 9.2 KV Cache 容量策略

7B 模型 4096 context：
- KV cache ≈ 2 × 32 layers × 32 heads × 128 dim × 4096 × fp16 = ~2GB
- 显存预算（8GB GPU）：模型 ~4GB (Q4) + KV cache ~2GB = 6GB，余 2GB 给中间结果

多请求时 KV cache 是瓶颈：
- 每个并发请求需要独立 KV cache
- 8 个并发请求 = 16GB KV cache → 显存不够

**Phase 1**：单请求，固定 4096 context，不考虑并发
**Phase 3**：PagedAttention 或 prefix sharing（需要修改 llama.cpp 或等其原生支持）

### 9.3 Prefill 与 Decode 的调度策略

Prefill 计算量与 prompt 长度成正比，可能需要 100ms+。此期间 GPU 被占满，其他请求的 decode 被阻塞。

**方案 A：Prefill 不拆分（Phase 1）**
- 简单实现
- 高并发时 decode 延迟抖动大

**方案 B：Prefill chunking（Phase 3）**
- 每次处理 512 tokens → 让出 GPU → 处理 decode batch → 继续 prefill
- 需要管理 prefill 的中间状态

### 9.4 量化格式选择

本机测试建议：
- **Q4_K_M**：模型大小 ~4GB，质量/速度平衡最好
- Q8_0：~7GB，质量更好但需要更多显存
- Q2_K：~2.5GB，显存紧张时的选择

huggingface 上有现成的 GGUF 文件，无需自己量化。
