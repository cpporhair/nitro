# Inconel 代码质量标准

> 适用范围：`apps/inconel/` 与 `ai_context/inconel/design_doc/` 对应的全部实现与 review。
>
> 目标：把“代码看起来还行”变成可执行、可否决、可留痕的工程标准。
>
> `v1` 只表示功能范围冻结，不表示质量标准降低。任何已经进入 production 代码或 canonical path 的能力，都必须按 production-grade 标准审查。

## 1. 使用方式

本文是当前分支使用的质量标准，直接服务于实现与 review，不依赖固定的 step 文档流程。

对任何非纯文案修改，至少要回答三件事：

1. 这次改动触及了哪些热路径，新增了什么成本。
2. 新状态与新抽象分别归谁拥有，生命周期是否一眼可见。
3. sender 链是否仍然能直接映射回设计文档里的 owner / scheduler 拓扑。

如果这三件事答不清，就不能判定改动质量合格。

## 2. 四类质量目标

### 2.1 热路径成本

必须明确：

- 是否引入新的 heap allocation
- 是否引入新的 copy / memcpy / materialize
- 是否引入新的 queue hop / I/O / flush

默认要求：

- 热路径新增成本必须可数、可解释
- 冷路径新增成本要标明为什么仍然可接受
- 不允许用“先做出来，后面再优化”替代当前解释

### 2.2 Ownership / Lifetime

必须明确：

- 谁拥有数据
- 谁只持 view / handle
- 谁负责释放
- 哪份 mutable state 跨哪些 async phase 存活

默认要求：

- request / round 私有、跨多阶段存活的 mutable state，优先用 sender context 承载
- 若不用 context，必须说明为什么 request-local handle 更合适
- 不允许把 pipeline object 本身当 mutable request state carrier
- 不允许用 copy 掩盖 ownership 不清

**DMA pool / frame-holder 析构序（058 e2e 顺出的 teardown double-free）**：
RAII frame-holder（`pooled_frame_ptr` cache、`segmented_page_frame` 持有者）析构时会把 frame
**还给其 DMA pool**（`lba_dma_page_pool`）。因此 **pool 必须 outlive 任何持有其 frame 的成员**：
- 首选**声明序**——pool 声明在 frame-holder **之前**（C++ 逆序析构 → pool 后死、frame-holder 先死，
  归还时 pool 仍在）。注意 holder 在子 struct 内时（如 `tree_sched.state.non_leaf_page_cache`），
  pool 须声明在该**子 struct 成员**之前（`frame_pool` 在 `state` 前）。
- 或**显式 dtor drain**——在析构 body 里 drain/clear holder（body 先于成员析构跑），把 frame 还给仍存活的 pool
  （`value_alloc_sched` / `tree_lookup_sched` 用此）。
- 二者择一即可，但**必须有其一**；新代码优先声明序（无运行期成本、不依赖记得 drain）。
- review：凡同一 owner 持 `lba_dma_page_pool` + RAII frame cache/buffer，检查 pool 是否 outlive 全部 holder。

### 2.3 抽象边界

抽象只有在满足下面至少一项时才算合理：

- 承载明确不变量
- 承载 owner / lifetime / scheduler 边界
- 承载重复语义规则
- 明确减少错误空间，而不只是减少代码行数

默认否决：

- 只是把几行代码包进 helper
- 把关键 owner / lifetime / scheduler 信息藏起来
- 让调用点看不出正在切 scheduler
- 用“通用 util”覆盖多个不等价语义

### 2.4 PUMP Sender 结构

必须保持：

- `then` / `flat_map` / `on(...)` 与设计拓扑一致
- owner scheduler 在代码里一眼可见
- 公共 sender API 的归属清楚，不能散落到无关头文件

## 3. 硬规则

### 3.1 热路径分类与预算

对每个触及的热路径，都要明确：

- 新增 allocation 是什么
- 新增 copy 是什么
- 新增 queue hop / I/O / flush 是什么
- 哪些对象必须复用，哪些允许新建

若本次改动不涉及热路径，应明确写 `n/a`，而不是省略。

最低表达粒度应当类似：

- `memtable hit GET: 0 heap alloc, 0 owning copy`
- `tree hit value read: 允许 1 次 copy-out，不允许额外 owning materialize`
- `value durable path: 允许 1 次写入 frame 的 memcpy，不允许重复 copy`

### 3.2 Allocation / Copy 证据

只要实现中新增 allocation 或 copy，review 必须回答：

1. 新增了什么
2. 出现在什么路径
3. 为什么不能避免
4. 后续是否需要预算或优化

没有这四项，视为证据不足。

### 3.3 稳定 Metadata 的 Carrier 策略

对构造期已确定、运行期不变的数据，如 topology、routing、常量表，必须显式选择 carrier：

- `this` 捕获：0 成本，但必须证明调用方 lifetime 已经约束了 sender
- `shared_ptr<const T>`：允许，用于 sender 必须自包含的非标量数据
- 标量按值：适用于 trivially copyable 元数据

默认否决：

- 每请求按值复制 `std::vector`、`std::string` 等 owning 容器
- 每请求把 `std::span` / view 物化成 owning 容器
- 为避免 `this` 捕获而引入每请求 heap allocation

### 3.4 Async Lambda Owning Capture

每个非平凡 sender pipeline 的 review，都应清点 lambda 的 owning captures。

必须回答：

- 哪些 capture 持有 heap 内存、引用计数或 move-only 资源
- 这些 capture 是否每请求都真的需要一份新拷贝
- 如果不是，为什么不能换成更轻量 carrier

默认否决：

- 热路径 lambda 按值捕获 `std::vector`、`std::string` 等 owning 容器
- capture 策略让原本自包含的 sender 退化成依赖外部对象存活

### 3.5 `then` / `flat_map` 规则

- 纯值变换用 `then`
- 只有 lambda 必须返回 sender 时才用 `flat_map`
- 不允许把 `flat_map` 当作“万能下一步”

每个新增 `flat_map`，至少要回答：

1. 上游到底是哪一个 runtime value / branch choice 迫使这里必须返回 sender
2. 为什么不能改写成 `then`
3. 为什么不能把 `get_context` / branch value 提到外层
4. 为什么不能写成更直接的 sender 组合

默认否决的形状：

- helper 内部 `just() >> get_context(...) >> flat_map(...)`，只是为了调用一个 owner sender
- 为了把 context / loop index / branch value 传到下一步而额外包一层 sender 壳
- 明明可以外提 `get_context`，却把它藏进 helper

允许的例外：

- 该 `flat_map` 明确形成 phase 边界
- 一次性消除了后续子链的重复 `get_context`
- capture 生命周期仍然清楚，没有引入新的 lifetime contract

### 3.6 Scheduler Owner 可见性

必须保持：

- 每次跨 scheduler 的切换在 sender 链里显式可见
- 不允许用 helper 把 `on(owner_sched)` 藏掉
- 读代码时能直接看出当前状态归哪个 owner

### 3.7 Owner Sender 自包含

对含 scheduler 的模块，公共 sender API 必须与 owner 归属保持一致。

最低要求：

- 模块对外暴露的 sender 能只靠该 owner 的 public header 使用
- `op_pusher` / `compute_sender_type` 等 PUMP 特化不能散落到无关 pipeline 头文件
- 不能把 owner sender 的关键定义藏在调用方模块里

### 3.8 单核 Bring-up 不得折叠语义边界

当前允许单核配置或单核测试，但那只是部署方式，不是程序语义。

默认否决：

- 因为当前同核，就删掉 owner queue / `advance()`
- 因为当前同核，就把跨 owner sender/message 改成同步直调 state
- 因为当前单核，就删掉 runtime binding / semantic routing 元数据

必须保持：

- owner ingress queue
- 显式 sender / owner 边界
- runtime topology 与 typed accessor

### 3.9 设备访问边界不得回退成同步直调

凡是语义上属于 owner / scheduler / pipeline 的设备访问路径，必须保持 async 边界：

- operation / owner sender 发请求
- 对应 owner `advance()`
- 终端 I/O owner 执行实际读写 / flush / trim

默认否决：

- 在 canonical runtime path 里直接调用设备 backend
- 因为 mock backend 很方便，就跳过 owner / advance 边界
- 用同步 wrapper 伪装成“已经接上 I/O owner”

只允许的例外：

1. backend primitive 自身实现
2. 终端 I/O owner 的 `handle_*()`
3. 明确标为 local seam 的测试 / 算法 helper，且不能作为 runtime 正确性证据

### 3.10 禁止 `virtual` / 类多态分发（step 3 / 057 立）

inconel 是编译期 flat OpTuple + position-based dispatch + 无锁 + 热路径预算设计。vtable 间接跳转违背 ethos、破坏内联、压热路径。**production 禁止新增 `virtual` / `override` / 虚析构 / 纯虚接口。**

替代范式（都在库里有现成范本）：

| 需求 | 范式 | 范本 |
|---|---|---|
| Cache / 模板参数类型擦除 | **非多态 base**（持队列/数据/factory + 非虚方法）+ runtime **concrete tuple** 驱动 `T::advance()` | `value::value_alloc_sched_base`；`tree_read_domain_base`（057 去虚后） |
| 运行时闭集分支 | `std::variant` + `visit()`（编译期） | tree lookup variant、frontier_switch no-op |
| 跨层 / 跨域回调 | `{void* self; fn* thunks}` 函数指针 handle，或已有 concrete registry 指针 | `core::reclaim_sink`（057 去虚后）、`run.hh` thunk |

`std::move_only_function` **仅**允许作 PUMP scheduler req 的 `cb`/`fail` continuation；**禁止**放进 per-record / per-KV 内层循环（那是热路径间接调用，按 §3.1 预算单独论证）。

**Review gate（必跑）**：`rg -n '\bvirtual\b|\boverride\b' apps/inconel --glob '!**/test*' --glob '!**/*test*'` 任何命中**默认 fail**。唯一例外须在命中处注释写明充分理由（性能 / ABI 论证，**"代码更简单"不算**）并登记 allowlist。

> 背景：step 2 一度引入 `reclaim_sink` 纯虚接口；step 3（057）审计发现 production 仅 4 处虚函数（reclaim_sink + 3 个 step-030 `*_base`），全部去虚——因为它们都不是不可避的热路径多态、且都有上表的非虚范式。立此规则防复发。

## 4. 推荐证据

不是每次改动都要 benchmark，但每次都应留下可审计证据。可接受的证据包括：

- 热路径对象预算表
- allocation / copy 清单
- 白盒测试断言某路径不发生某类行为
- counted allocator / copy counter
- sender 拓扑与设计拓扑的逐步 mapping
- 对 owner / lifetime 的文字说明与代码引用

## 5. Review 清单

review 至少回答下面问题：

1. 这段代码对应哪条设计文档约束
2. 这一步引入了哪些新 allocation
3. 这一步引入了哪些新 copy
4. 它们是否位于热路径
5. ownership / lifetime 是否一眼可见
6. 新抽象是否真的承载边界或不变量
7. sender 链是否还能映射回设计文档的 scheduler 拓扑
8. 哪些 `flat_map` 是新增的，它们是否都必要
9. 是否存在“helper 内部 `get_context + flat_map`”的额外 sender 壳
10. 热路径 lambda 闭包里有哪些 owning captures，它们是否都必要
11. 稳定 metadata 用了什么 carrier，是否引入了不必要的 heap alloc 或 lifetime contract
12. 当前是否因为单核 bring-up 或 mock backend 而偷偷折叠了 owner / sender / I/O 边界

任一项答不清，都不应直接判 `pass`。

## 6. 必须停下的质量问题

出现下面任一情况，必须先澄清再继续：

- 无法解释热路径中的新 allocation 或新 copy
- 无法解释某个抽象为什么存在
- 无法解释某个 `flat_map` 为什么不是 `then`
- 无法解释某个 `flat_map` 为什么不能外提 `get_context`
- 无法看出某段状态到底归哪个 scheduler owner
- 需要靠“应该很便宜”来为成本辩护
- 需要靠“后面优化”来为结构混乱辩护
- 热路径 lambda 按值捕获 owning 容器且说不清为何不能换 carrier
- owner 的公共 sender 脱离 owner header 不可编译
- free function 通过临时 pipeline object 返回 sender，并隐含依赖该 object 存活
- 因为单核 bring-up 或 mock backend 而删除 / 绕过 owner、sender、runtime binding、I/O owner 边界

## 7. 与其他文档的关系

- [design_overview.md](design_overview.md) 定义系统语义与 pipeline 拓扑
- [cross_doc_contracts.md](cross_doc_contracts.md) 用于验证签名、字段与路径一致性
- [code_modules.md](code_modules.md) 定义模块职责、依赖方向与公共接口边界

本文不替代这些文档；本文回答的是“实现质量是否过关”，而不是“系统语义是什么”。
