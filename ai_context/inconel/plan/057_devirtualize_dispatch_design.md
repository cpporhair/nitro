# 057 — 去除 inconel production 虚函数（step 3）详细设计

> 稳态后台环 step 3。原 step 3（INC-054 allocator floor）经用户裁决延后（仍 blocked）；本 step 改为
> **消除 production 里所有不合理虚函数 + 立未来规避规则**。
> 起因：step 2 我在 056 D1 引入 `core::reclaim_sink` 纯虚接口；用户指出 inconel/PUMP 是编译期 flat
> OpTuple + 无锁 + 热路径预算（RocksDB×5）设计，虚函数的 vtable 间接跳转违背 ethos、破坏内联、压热路径。
> 经 codex + 我开会审计：production 共 4 处虚函数，**4 处全必改、无一有充分保留理由**。
> 设计角色文档；实现阶段禁读测试。

---

## 0. 一句话

把 production 仅有的 4 处类多态虚函数（`reclaim_sink` + 3 个 step-030 `*_base`）全部去虚——
照本代码库已有的非虚范式（非多态 base + 数据成员 + concrete tuple 驱动 advance，见
`value_alloc_sched_base`；跨层回调用函数指针 handle）——并立一条 review gate 规则防复发。

## 1. 范围

### 1.1 做什么
1. 去掉 4 处虚函数（§3 逐项）。
2. 立未来规避规则，写进 `code_quality_standard.md` + `CLAUDE.md`（§4）。
3. review gate：`rg -n '\bvirtual\b|\boverride\b' apps/inconel`（排除 test）应为空（§6）。

### 1.2 不做（明确划界）
- **不**决定 production 是否从 PUMP `share_nothing::start` 切到 inconel `run.hh` thunk loop——去虚**不依赖**它（两种 loop 都按 concrete tuple 指针调 `advance`，base virtual 本就 vestigial）。这是相邻但独立的决定，不混进本 step。
- **不**动 `std::move_only_function`（scheduler req 的 cb/fail continuation，非类多态；规则只约束它不得进 per-record 内层循环，§4）。
- **不**碰 INC-054（step 3 原项，用户已延后/blocked）。

### 1.3 性质
纯**重构**（语义等价最高约束，pump CLAUDE.md 重构原则）：去虚不改任何可观测行为（输出/顺序/异常路径/副作用）。这是它低风险的根据，也是验收标准（全套回归零变化）。

---

## 2. 审计结果（codex + 我，已逐项对代码核实）

production（非 test）类多态虚函数**仅 4 处**：

| # | 虚函数 | 文件 | 来源 | 根因 | 调用频率 |
|---|---|---|---|---|---|
| 1 | `core::reclaim_sink`：`post_retired`/`post_gen_losers`（纯虚）+ 虚析构 | `core/memtable.hh:113-116` | **step 2（056 D1，我引入）** | 跨层回调：core L0 的 guard/gen 析构要 post 到 tree L2 | guard/gen 析构（flush/seal 生命周期，非 per-KV） |
| 2 | `core::tree_read_domain_base::advance()`（纯虚）+ 虚析构 | `core/tree_read_domain.hh:187-188` | step 030 | registry（非模板 core）对 Cache 模板参数类型擦除 | **vestigial**：production 无 `base*->advance()` 调用，PUMP share_nothing loop 按 concrete tuple 指针调 |
| 3 | `tree::tree_lookup_sched_base::read_domain_index()`（纯虚）+ 虚析构 | `tree/lookup_scheduler.hh:199,207` | step 030 | 同 2 | 冷：唯一调用在 geometry-mismatch panic path（`lookup_scheduler.hh:552`） |
| 4 | `tree::tree_worker_sched_base::read_domain_index()`（纯虚）+ 虚析构 | `tree/worker_scheduler.hh:126,133` | step 030 | 同 2 | production **无调用点**（仅声明/override） |

**确认无其它 production 多态/间接分发**：无 `std::function` / `function_ref` / `dynamic_cast` / `typeid` / `std::any`。
**已有非虚范式（本 step 的模板）**：
- `value::value_alloc_sched_base`（`value/scheduler.hh:445`）是**非多态 base**（只持队列 + 非虚方法 + sender factory），sender 持 `base*` 调非虚方法，`advance` 经 concrete tuple 驱动具体 `value_alloc_sched<Cache>`。
- runtime 驱动 advance 全程按 **concrete tuple 指针**（PUMP `share_nothing.hh:19`；inconel `run.hh:77` 的 thunk 同理，捕获 concrete T）→ coord/front/wal/tree_sched/value 全无虚。
- `destroy_runtime` 删 read_domain 用 `static_cast<tree_read_domain<TreeCache>*>(rd)`（concrete，`builder.hh`）→ 去掉 base 虚析构安全。

**`std::move_only_function`**：60 处，均为 scheduler req 的 `cb`/`fail` continuation 形态（自建 scheduler 六组件模式），非类多态、非本 step 对象；规则单列（§4）。

---

## 3. 逐项裁决（4 处全必改，无 keep）

> 裁决纪律：可保留虚函数但理由须充分，"代码更简单"不算。**经审计，4 处均无充分保留理由**（都不是不可避的热路径多态，且都有现成非虚范式），故全改。

### 3.1 `tree_read_domain_base::advance()`（必改）
- **现状**：`virtual bool advance() = 0`，concrete `tree_read_domain<Cache>` override；但驱动方是 concrete tuple（PUMP share_nothing），virtual 从不经 base* 调 → vestigial。
- **改法**：
  - base：删 `virtual bool advance()=0` 和虚析构；base 退化为**非多态**，保留 `read_domain_index_`（见 §3.2）、`partitions`、`lookup_sched`/`worker_sched`（base 指针，routing 用）、`invalidate_q` + 非虚 `submit_invalidate_range`/`schedule_invalidate`（step 2 V1 已是非虚）。
  - derived `tree_read_domain<Cache>`：`advance()` 改为**非虚**普通方法（去 `override`）；内部 `lookup->advance()`/`worker->advance()` 本就是 concrete 调用，不变。
  - registry list `tree_read_domain_base*` 仅用于 routing / invalidate fan-out / publish_shard_partitions，均不需 advance 虚。
- **风险**：低。production 无 base* advance 调用；测试 harness 若曾 `base*->advance()` 需迁到 concrete（test-maintenance，本 step 实现期处理）。

### 3.2 `tree_lookup_sched_base::read_domain_index()`（必改）
- **现状**：`virtual uint32_t read_domain_index() const = 0`，唯一实际调用在 geometry-mismatch panic path（`lookup_scheduler.hh:552`）。
- **改法**：base 加数据成员 `uint32_t read_domain_index_`，ctor 接收并存；`read_domain_index()` 改非虚 accessor（或调用点直接读字段）；`tree_read_domain<Cache>` 构造 lookup 时传 rdi。删虚析构（base 非拥有，lookup 由 read_domain `unique_ptr<concrete>` 持有）。
- **风险**：极低（冷 path，纯数据化）。

### 3.3 `tree_worker_sched_base::read_domain_index()`（必改）
- **现状**：production 无调用点，仅声明/override。
- **改法**：同 §3.2（数据成员），或直接删除该 getter（若确无诊断需求）。删虚析构。
- **风险**：极低。

### 3.4 `core::reclaim_sink`（必改）
- **现状**：纯虚接口 + 虚析构，唯一实现者 `tree_sched`（`owner_scheduler.hh:2360`）；非 per-KV/per-request 热路径（guard/gen 析构发 reclaim）。无 plugin ABI / 多实现需求。
- **为什么也改**（非热路径仍改）：(a) 它是 step 2 新引入的、用户点名的坏先例；(b) 与 ethos / 已有非虚范式不一致；(c) 去虚成本极低。"非热路径所以可留虚"**不成立**——没有功能上需要 vtable 的理由，且留着会被后续模仿。
- **改法**：函数指针 handle（无 vtable/inheritance）：
  ```cpp
  struct reclaim_sink {
      void* self = nullptr;
      void (*post_retired)(void*, retired_objects&&) = nullptr;
      void (*post_gen_losers)(void*, retired_value_refs&&) = nullptr;
  };
  ```
  - `tree_sched` 持成员 handle，thunk `+[](void* s, retired_objects&& r){ static_cast<tree_sched*>(s)->post_retired_impl(std::move(r)); }`。
  - 进程级 `std::atomic<reclaim_sink*> active_reclaim_sink_cell`（或存 handle by-value 的 atomic——handle 是 POD），build 时 release-store，`destroy_runtime` **首步** null（沿用 step 2 B2 的 teardown 钩子，已早于 tree_sched delete）。
  - guard/gen 析构：load handle，非空则 `handle->post_retired(handle->self, std::move(retired))`。
- **代价**：仍是一次函数指针间接调用（与虚调同量级），但去掉 inheritance/vtable/RTTI，是"显式类型擦除"，与 run.hh thunk 同风格。语义等价。
- **风险**：低到中——重点是 handle 生命周期（teardown null 顺序，step 2 已有）。

---

## 4. 未来规避规则（写进 `code_quality_standard.md` + `CLAUDE.md`）

> **Inconel production 禁止新增 `virtual` / `override` / 虚析构 / 纯虚接口。**
> - **Cache 或模板参数类型擦除**：统一用"非多态 base（存队列/数据/factory + 非虚方法）+ runtime concrete tuple 驱动 `T::advance()`"范式（范本 `value_alloc_sched_base`），**不用**虚 base。
> - **运行时闭集分支**：`std::variant + visit()`（编译期），不用虚多态。
> - **跨层 / 跨域回调**：用 `{void* self; fn* thunks}` 函数指针 handle，或已有 concrete registry 指针；**不用**继承接口。
> - **`std::move_only_function`**：仅允许作为 PUMP scheduler req 的 `cb`/`fail` continuation；**禁止**放进 per-record / per-KV 内层循环（那是热路径间接调用预算项）。
> - **Review gate（必跑）**：`rg -n '\bvirtual\b|\boverride\b' apps/inconel --glob '!**/test*' --glob '!**/*test*'` 任何命中**默认 fail**；唯一例外须在命中处注释写明充分理由（性能/ABI 论证，非"简单"）并登记 allowlist。

---

## 5. 实现分 patch + 风险

| patch | 内容 | 风险 |
|---|---|---|
| **A** | §3.1/3.2/3.3 三个 step-030 `*_base` 去虚（一批，同源） | 低：production 路径无 base* 虚调；编译面 + test harness 迁移 |
| **B** | §3.4 `reclaim_sink` → 函数指针 handle | 低-中：全局 handle 生命周期 / teardown null 顺序（step 2 B2 钩子已在） |
| 顺手 | 清理 `tree_read_domain.hh` 里"virtual advance 在 outer loop 可接受"的陈旧论证注释 | 无 |

A、B 可同一 step 内两 commit。**实现期铁律**：纯重构、语义等价；不改任何行为；若发现去虚牵连出行为变化，停下报告（说明不是等价重构）。

## 6. 验证

1. **grep gate 空**：`rg -n '\bvirtual\b|\boverride\b' apps/inconel --glob '!**/test*'` → 0 命中。
2. **full build 全绿**（含所有测试 target；若 test harness 曾用 `base*->advance()` / 旧接口，迁移之，test-maintenance）。
3. **全套回归零变化**：`inconel_test_*`（reclaim / flush_round / m01-m14 / e2e / multicore）全 pass——重构语义等价的证据。
4. 不新增测试（纯重构，行为不变，现有套件即覆盖）。

## 7. 冲突与裁决记录

| # | 点 | 裁决 |
|---|---|---|
| 1 | 4 处是否都改 | **全改**。无一有充分保留理由（都不是不可避热路径多态，都有非虚范式）。 |
| 2 | reclaim_sink 非热路径能否留虚 | **不能**。"非热路径"不是保留 vtable 的充分理由；坏先例 + ethos 不一致；改本成本极低。 |
| 3 | `std::move_only_function`（60 处）算不算 | **不算本 step 对象**（scheduler req continuation，非类多态）；但规则单列条款防其进热路径。 |
| 4 | 是否顺便切 run.hh thunk loop | **不切**。去虚不依赖；相邻独立决定，不混入本 step（§1.2）。 |
| 5 | base 去虚析构是否安全 | **安全**。read_domain 由 `destroy_runtime` 按 concrete static_cast 删；lookup/worker 由 read_domain `unique_ptr<concrete>` 持有；base 全程非拥有指针。 |

## 8. 路线位置
- step 1 ✅ frontier_switch / step 2 ✅ 物理 reclaim / **step 3（本文）= 去虚 + 规则**（替原 INC-054，后者用户延后）/ step 4 = boot recovery（未启动）。
- 本 step 把 step 2 引入的 reclaim_sink 虚函数一并清掉，相当于 step 2 的质量收尾 + 全局 ethos 对齐。
