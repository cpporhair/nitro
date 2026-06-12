# 046 - M07 Value Persist / Read Adapter

> 本文对应 `ai_context/inconel/plan/front_wal_development_plan.md` 的 M07。
> 目标不是迁移旧分支 value allocator，而是把当前 `inconel.new` 已有的
> value 模块（INC-051 之后的 `value_space_manager` + `value_alloc_sched`）
> 接到 M01-M06 已经立起来的 write-path carrier / L3 组合层上，并把
> `known_issues.md` 登记的 INC-048（unbounded `concurrent()`）和
> 045 §10.1 登记的 F5 同型风险（frame 跨线程归还）一并收口。

## 1. 范围

M07 覆盖：

- L2 `value/sender.hh` 表面修正：
  - sender rename `persist_values` → `persist_put_values`（对齐
    `cross_doc_contracts.md` §1 的 handle 名）。
  - leader FUA writes / prefill reads / trim drain 三处 unbounded
    `concurrent()` 全部改为显式 bounded issue（INC-048 收口）。
  - bounded 上限作为 `value_io_policy` 进入 `value_alloc_sched` 构造参数，
    并随 `persist_leader` / `persist_prefill` / `trim_batch` carrier 发布。
  - NVMe 实例改为 `NvmeProvider` 仿函数注入（默认 `rt::local_nvme()`），
    与 M06 `write_path` 的 fake-NVMe 可测试性模式对齐。
- L3 `write_path/sender.hh` 新增 `persist_put_values(core::batch_ctx&)`
  适配 sender：把 M01 `batch_ctx` 的 PUT canonical entries 翻译成
  value 模块的 `put_entry[]`，并定义完成/失败语义供 M08/M09 消费。
- `value_persist_error` 失败类型（与 M06 `wal_append_error` 同型），
  M08/M09 catch 后接 `coord::release_batch`。
- 045 §10.1 登记项的对照结论：value persist / read 路径的 frame
  线程归属论证（见 §7），不引入新防御代码。
- M07 合约测试 `inconel_test_m07_value_persist_read_adapter`。

M07 不覆盖：

- 全链路 `write_batch` 编排与 release/publish 串接（M08/M09）。
- `read_page_values`（INC-021，等 MultiGet/Scan 读管线，M10+）。
- `install_recovered_state` 接线（INC-020，等 recovery）。
- INC-055 的 recently-written residency 优化 tier（read 性能项，
  等 M10 读路径用实测数据再做；M07 只保证 `read_value` 语义正确、
  miss 时 bounded 读盘）。
- `value_space_manager` 内部逻辑与 `value_alloc_sched` 的
  round/commit/rollback 状态机改动 —— 当前实现是 INC-051 落地产物，
  M07 视为正确基线，只动 sender 表面与 carrier 字段。

## 2. 已对照输入

正式设计：

- `design_doc/INDEX.md`（容量/性能硬约束）
- `design_doc/design_overview.md` §7.1（value durable 是写路径第一个
  durable point）、§10.5（value-before-WAL 顺序）
- `design_doc/write_path_and_pipeline.md` §2（Phase A）、§5（分配与
  持久化流程）、§10（异常分类）、§11.1（value-before-WAL 因果论证）
- `design_doc/runtime_state_machine.md` §6（value_alloc_sched 全章，
  含 §6.3 `value_io_policy`）、§11.1（post-LSN 失败矩阵）
- `design_doc/cross_doc_contracts.md` §1（`persist_put_values` /
  `read_value` 签名）、§3（value owner 归属）
- `design_doc/code_modules.md`（L2 互不依赖、write_path 职责）
- `design_doc/code_quality_standard.md`（热路径预算、§3.4 owning
  captures、§3.9 设备访问边界）
- `design_doc/on_disk_formats.md` §5（value object 格式）

当前分支代码：

- `apps/inconel/value/scheduler.hh`（round 状态机、`put_entry`、
  leader/prefill/follower carrier、`handle_read`/`handle_fill`）
- `apps/inconel/value/sender.hh`（INC-048 所在）
- `apps/inconel/value/space_manager.hh`（只读对照，不改）
- `apps/inconel/core/batch_carrier.hh`（M01 `batch_ctx` /
  `canonical_entry.allocated_vr` / `put_entry_indices`）
- `apps/inconel/write_path/sender.hh`（M06 L3 组合层先例）
- `apps/inconel/nvme/frame_io.hh`（`write_frame_range_bounded_fua`）
- `apps/inconel/coord/sender.hh`（`release_batch`，M08 对接点）
- `apps/inconel/runtime/facade.hh` / `runtime/builder.hh`

旧分支证据（语义参考，不迁移代码）：

- `inconel:apps/inconel/runtime/operations/write_batch.hh`
  （Step 26M/N：`prepare_value_persist → flush_value_persist`
  leader/follower 编排 + `commit_persist`）
- `inconel:apps/inconel/runtime/value_alloc/*`（仅接口历史背景）

登记问题：

- `known_issues.md` INC-048（normal）：本步收口。
- `045_front_wal_review_fixes_design.md` §10.1：F5 同型风险对照，
  本文 §7 给出结论。
- INC-052 / INC-054：与本步正交，不动。

## 3. 语义来源对照表

| 项目 | 旧 `inconel` 证据 | 当前 `inconel.new` 现状 | 正式设计依据 | 046 决议 |
|---|---|---|---|---|
| persist 编排 | Step 26M/N `flush_value_persist`：leader 分支 `loop(writes) >> concurrent()`（同样无界）+ `commit_persist`；follower 分支 `just()` | `value/sender.hh::on_persist_leader` `as_stream >> concurrent()` 无界（INC-048）；prefill/trim 同病 | WP §5.4/§5.7；RSM §6.4；M06 §9 bounded issue 先例 | leader writes 用 `nvme::write_frame_range_bounded_fua`；prefill reads 用新增 `nvme::read_frame_range_bounded`；trim 用 `concurrent(max_trim_inflight)`。上限随 carrier 发布（§4.3） |
| persist sender 名 | `prepare_value_persist` / `commit_persist` | `persist_values(span<put_entry>)` | cross_doc §1：`persist_put_values(batch PUT entries) → durable value_refs` | L2 sender rename 为 `persist_put_values`；scheduler 内部 handle 名（`prepare_persist`/`finalize_persist`/`continue_persist`）不动 —— 它们是 round 协议步骤，不是 contract handle |
| persist 输入 carrier | `batch_pipeline_state.put_entries`（`canonical_view_entry*`） | `put_entry { string_view body; value_ref* out_vr; }`，borrowed | WP §2.2 `canonical_entry.allocated_vr`；039 §4.4 `put_entry_indices` | L3 adapter 从 `batch_ctx.put_entry_indices` 构造 context-owned `std::vector<value::put_entry>`，`body = entry.value`（指向 `ctx.input.bytes`），`out_vr = &entry.allocated_vr`。vector 与 batch_ctx 都必须活到 persist sender 完成（§6） |
| 完成语义 | leader `commit_persist` 后 sender 完成；follower 在 leader commit 时解锁 | `persist_values` 终值 `bool`（true = round committed） | OV §7.1 step 5：FUA 完成、拿到 durable `value_ref` 后才允许 fan-out WAL | L3 sender 完成 ⇔ 该 batch 所属 round 已 settle（commit 或 rollback）且成功时所有 `out_vr` 已填。`false` → `value_persist_error{round_failed}` 异常，绝不静默吞掉 |
| DELETE-only batch | 旧分支由 pipeline state 的 put_entries 为空自然跳过 | `handle_persist` 对空 entries 会走 `allocate_batch` 空返回 → panic | 计划 M07 完成测试 3；约束 A fail-fast | L3 adapter 对 `put_entry_indices.empty()` 短路返回，不投递 value owner；owner 的空-entries panic 保留为 invariant 断言 |
| oversized PUT | 旧分支无显式上限检查 | `class_for_body_len` 失败 → 整个 round（含无辜 followers）一起 fail | RSM §11.1 注：pre-LSN 可拒绝；ODF §5.2 class 上限是格式参数 | L3 adapter 构造 put_entry 前用 `rt::value()->max_body_len()` 预检，超限抛 `value_persist_error{oversized_value}`，不进 owner、不连坐并发 batch。owner 内 fail 路径保留为第二道防线 |
| NVMe 注入 | 全局 `runtime_mock_nvme_by_device_core` | `rt::local_nvme()` 硬编码在 sender 内 | M06 `write_path` 模板注入先例；测试需统计 max inflight / 注入失败 | `NvmeProvider` 仿函数模板参数（默认 `local_nvme_provider{}` 返回 `rt::local_nvme()`），在 leader/prefill/trim continuation 内调用 —— 保留"惰性解析到 value owner 核心"语义（§7 依赖此性质） |
| async persist reset（Step 26N） | prefill → continue 异步链 | 已有 `persist_prefill` / `continue_persist` | RSM §6.4 `nonresident_partial` | 形态保留，只把 prefill reads 改 bounded |
| value read | 旧 read 路径基于 hot_blob 时代结构 | `read_value(vr)` 完整（round/resident/cache hit + NVMe miss + `fill_and_decode`） | RSM §6.5；RAP §4.5 | 保留现状结构；加 `NvmeProvider` 注入；§7 给出 frame 线程论证。`read_page_values` 仍按 INC-021 延后 |

## 4. L2 Surface 修正（`value/sender.hh` + `value/scheduler.hh`）

### 4.1 `value_io_policy`

```cpp
// value/scheduler.hh
struct value_io_policy {
    uint32_t max_write_inflight = 32;  // leader FUA writes
    uint32_t max_read_inflight  = 32;  // prefill reads
    uint32_t max_trim_inflight  = 16;  // trim batch
};
```

- `value_alloc_sched` 构造参数新增 `value_io_policy io_policy = {}`，
  存为 const 成员；`value_alloc_sched_base` 暴露
  `const value_io_policy& io_policy() const`。
- 三个字段都必须非 0；构造时校验，0 直接
  `panic_inconsistency`（部署配置错误，不是运行时可恢复状态）。
- RSM §6.3 的 `value_io_policy` 概念在本步只落地 I/O inflight 子集；
  cache admission / read prefill caps 等字段等对应功能再加，不预埋
  dead field。
- `runtime/builder.hh` 把 policy 接进 `build_options`（带默认值），
  构造 `value_alloc_sched` 时传入。

### 4.2 Carrier 携带 bound

```cpp
struct persist_leader {
    uint64_t                            round_id;
    std::span<memory::frame_write_desc> writes;
    uint32_t                            max_write_inflight;
};

struct persist_prefill {
    uint64_t                           round_id;
    std::span<memory::frame_read_desc> reads;
    uint32_t                           max_read_inflight;
};

struct trim_batch {
    uint64_t             batch_id;
    std::span<trim_desc> trims;
    uint32_t             max_trim_inflight;
};
```

owner 在 `publish_round` / `publish_prefill` / `handle_continue` /
`prepare_trim_batch` 填入自身 `io_policy()` 的对应字段。sender 端
从 carrier 读 bound，不回头查全局状态 —— round 的执行参数随 round
carrier 走，跨线程只读一份按值数据。

### 4.3 Bounded issue

`on_persist_leader` 改为：

```cpp
template <typename NvmeProvider>
inline auto
on_persist_leader(persist_leader&& alt, NvmeProvider nvme) {
    uint64_t rid = alt.round_id;
    return nvme::write_frame_range_bounded_fua(
               nvme(), alt.writes, alt.max_write_inflight,
               [](memory::frame_write_desc& d) { return d; })
        >> flat_map([rid](bool nvme_ok) {
            return rt::value()->finalize_persist(rid, nvme_ok)
                >> forward_value(nvme_ok);
        });
}
```

- `write_frame_range_bounded_fua` 已由 M06 提供（`nvme/frame_io.hh`），
  内部 `as_stream >> concurrent(max_inflight)`，FUA flag 统一附加。
- `on_persist_prefill` 同型改造，使用新增的
  `nvme::read_frame_range_bounded(sched, span, max_inflight, get_desc)`
  （与 write 版本对称，加在 `nvme/frame_io.hh`；read 不带 FUA flag）。
- `drain_trim_pending` 的 `as_stream(alt.trims) >> concurrent()` 改为
  `concurrent(alt.max_trim_inflight)`（trim 不是 frame 接口，无需新
  helper）。
- 三处都禁止再出现无参 `concurrent()`。

### 4.4 `NvmeProvider` 注入

```cpp
struct local_nvme_provider {
    nvme::runtime_scheduler* operator()() const { return rt::local_nvme(); }
};

template <typename NvmeProvider = local_nvme_provider>
inline auto persist_put_values(std::span<put_entry> entries,
                               NvmeProvider nvme = {});

template <typename NvmeProvider = local_nvme_provider>
inline auto read_value(value_ref vr, NvmeProvider nvme = {});

template <typename NvmeProvider = local_nvme_provider>
inline auto drain_trim_pending(NvmeProvider nvme = {});
```

- 生产调用点零改动负担：默认参数即旧行为。
- `nvme()` 的调用点必须保持在 value owner cb 之后的 continuation 内
  （leader/prefill/miss 分支里），不得提前到 sender 构造期 —— 这保证
  生产路径解析到的是 value owner 核心的 `nvme_sched`（§7 的论证
  依赖此性质）。fake provider 在测试里返回 fake scheduler 指针。
- fake scheduler 只需满足 `write_frame(frame, flags)` /
  `read_frame(frame, flags)` / `trim(lba, n)` 返回 sender 的隐式
  concept（M06 测试的 fake NVMe 同款约束）。

### 4.5 Rename

- `value::persist_values` → `value::persist_put_values`。
- production 调用点同步更新（当前仅 value 模块内部 + e2e harness；
  harness 更新由实现方处理，见 §11.3）。
- 不保留旧名 alias —— 项目规则禁止 compatibility shim。

## 5. L3 Adapter（`write_path/sender.hh`）

### 5.1 失败类型

```cpp
// value/sender.hh（与 wal::wal_append_error 同型，归 value 模块所有）
enum class value_persist_error_reason : uint8_t {
    oversized_value,   // 预检失败：body 超过最大 size class
    round_failed,      // FUA 失败 / prefill 失败 / follower ok=false
};

struct value_persist_error : std::runtime_error {
    value_persist_error_reason reason;
    value_persist_error(value_persist_error_reason r, const char* what);
};
```

### 5.2 `write_path::persist_put_values`

```cpp
template <typename NvmeProvider = value::local_nvme_provider>
inline auto
persist_put_values(core::batch_ctx& ctx, NvmeProvider nvme = {});
```

语义展开：

```text
persist_put_values(ctx):
1. if ctx.put_entry_indices.empty():
       return just(true)                      // DELETE-only：零 value 调用
2. max_len = rt::value()->max_body_len()      // 构造期常量，跨线程只读
   for idx in ctx.put_entry_indices:
       if ctx.canonical_entries[idx].value.size() > max_len:
           throw value_persist_error{oversized_value}
3. 构造 std::vector<value::put_entry> puts:
       puts[i] = { .body   = ctx.canonical_entries[idx].value,
                   .out_vr = &ctx.canonical_entries[idx].allocated_vr }
4. push_context(std::move(puts))
   >> get_context<std::vector<value::put_entry>>()
   >> flat_map([nvme](auto& puts) {
          return value::persist_put_values(
              std::span<value::put_entry>(puts), nvme);
      })
   >> then([](bool ok) {
          if (!ok) throw value_persist_error{round_failed, ...};
          return true;
      })
   >> pop_context()
```

约束与理由：

1. **`puts` 必须 context-owned**：value owner 的 `round.entries_flat`
   保存 `put_entry*` 裸指针，prefill 场景下 `handle_continue` 的
   encode 还要回读 `put_entry.body`。`pop_context` 在 sender 完成
   （= round settle）之后才发生，指针全程有效。禁止把 `puts` 写成
   lambda 局部变量或临时 span。
2. **`ctx` 由调用方保活**：与 M06 `write_wal_fragment` 的
   `fragment`/`canonical_entries` 引用参数同款契约 —— M08 的
   write_batch pipeline state / context 持有 `batch_ctx` 跨全部
   phase。`canonical_entries` 在 build 之后不再增删，
   `&allocated_vr` 与 `value` view 稳定。
3. **完成点 = durable + settled**：步骤 4 的 `bool` 来自
   `finalize_persist` 之后（leader）或 leader settle 时的 follower
   通知。M08 把本 sender 排在 WAL phase 之前，因果链即满足概要
   §10.5 的 value-before-WAL；M07 自身不做跨 phase 编排。
4. **oversized 预检在 owner 之前**：避免一个非法 batch 连坐同
   round 合并的并发 batch（owner 现有 fail 路径对整组 items 失败）。
   预检与 owner 内第二道防线并存。
5. **失败传播**：`round_failed` 时 `out_vr` 内容视为无效；上层
   （M08）catch `value_persist_error` 后走 `coord::release_batch`。
   此时可能已有 value 页 durable —— 即 pre-WAL orphan，按概要
   §9.5/§10.6 留给 recovery，M07 不加清理路径。

### 5.3 模块依赖

`write_path/sender.hh` 新增 `#include "../value/sender.hh"` —— L3
依赖 L2，合法。`value/sender.hh` 现有对 `runtime/facade.hh` 的
include 维持现状（registry 访问糖），不在本步重整层级；登记为
watch-item（§11.4）。

## 6. Lifetime 契约汇总

| 对象 | Owner | 必须活到 | 保障方式 |
|---|---|---|---|
| `batch_ctx`（input bytes + canonical_entries） | M08 pipeline context（M07 测试中为测试栈/上层 context） | 该 batch 所有 phase 完成 | 调用方契约（M06 同款），文档 + 测试断言 |
| `std::vector<value::put_entry>` | L3 adapter 的 `push_context` | persist sender 完成（round settle） | `pop_context` 在链尾；vector 不再 grow，`put_entry*` 稳定 |
| `put_entry.body`（view → `ctx.input.bytes`） | `batch_ctx.input` | 同上（prefill encode 晚于 publish） | batch_ctx 保活推论 |
| `out_vr` 目标（`canonical_entry.allocated_vr`） | `batch_ctx.canonical_entries` | 同上 | 同上 |
| `persist_leader.writes` span 指向的 `round.writes` | value owner `inflight_rounds_` | finalize cb | round 在 settle 前不被 owner 触碰（既有 round 协议） |
| round frames（裸 `segmented_page_frame*`） | value owner（round → cache/resident/destroy） | — | 全程不离开 owner 线程（§7） |
| `read_miss.frame`（`pooled_frame_ptr`） | read pipeline context | `fill_and_decode` 移交回 owner，或失败路径析构 | §7 论证析构线程 = owner 线程 |

## 7. F5 对照结论（045 §10.1 登记项）

WAL 侧 F5 的根因是：plan frames 的 `pooled_frame_ptr` 进入 write_path
context，而 context 弹栈发生在 NVMe 完成回调线程，`put_frame` 在非
owner 线程改 pool 的 `free_pages_`。value 模块逐路径对照：

1. **persist 路径：无跨线程析构。** round frames 是 owner-local 裸
   指针（`round_page.frame`），pipeline 只拿到
   `span<frame_write_desc>`（非 owning 描述符）。frame 的
   alloc（`translate_claims_into_round`）、归还/晋升
   （`commit_round` / `rollback_published_round`）全部发生在 owner
   handle 内。
2. **persist/read continuation 不离开 value owner 核心。**
   `prepare_persist` / `prepare_read` 的 cb 由 value owner 的
   `advance()` 触发，后续 visit/flat_map continuation 在同一线程
   执行；NVMe 实例经 `NvmeProvider` 在该 continuation 内解析为
   `rt::local_nvme()` —— 即 value owner 核心的 `nvme_sched`
   （RSM §1：各 scheduler 用本核实例）。share-nothing 主循环里该
   核心同时 advance value owner 与其 nvme_sched，因此 FUA/read
   completion 及其后续（`finalize_persist` / `fill_and_decode` 的
   enqueue 与 cb）仍在同一核心。
3. **read miss 失败路径的 frame 析构在 owner 线程。**
   `read_miss.frame`（`pooled_frame_ptr`）随 context 析构的位置只有
   两处：成功路径 `fill_and_decode` 把 frame 交回 owner（handle 内
   release/析构）；失败路径（NVMe read false → 异常弹栈）在抛出点
   线程析构 —— 由 2，抛出点就是 value owner 核心。两处的
   `frame_pool_.put_frame` 都在 owner 线程。
4. **由此冻结一条结构约束**：`value/sender.hh` 内部的
   leader/prefill/read-miss continuation **禁止插入 `on(...)` 跨核
   切换**，`NvmeProvider` 的解析点禁止提前到 sender 构造期。任何
   后续改动如果把这两条打破，F5 竞态立即重现。本文把这条写进
   `value/sender.hh` 的文件头注释。
5. **不加防御代码**：不给 `lba_dma_page_pool` 加锁或线程断言 ——
   与 INC-052 的 trust-boundary 哲学一致，正确性边界在上述结构
   约束上，加锁会掩盖违规调用且违反"禁止给框架/池加锁"的同源
   规则。

结论：value 模块当前没有 F5 竞态；M07 的改动（bounded +
NvmeProvider）保持上述性质。045 §10.1 的 watch-item 以本节关闭。

## 8. 热路径预算

| 路径 | 新增成本 | 说明 |
|---|---|---|
| L3 persist adapter | 每 batch 2 次 request-scoped heap 分配：`vector<put_entry>` 本体（24B × PUT 数，reserve 单次）+ `push_context` 的 context 节点；另加一遍 oversized 比较 | 与 M06 `write_wal_fragment` 的 `push_context(state)` 同型成本；随链尾 `pop_context` 释放；无 per-entry heap、无 value bytes copy。10 亿 KV 校准：per-batch 临时量，无常驻 carrier。（review 修正：初版写"1 次 vector 分配/16B"，漏记 context 节点分配，且 `put_entry` 实际 24B = string_view 16B + value_ref* 8B） |
| L2 bounded issue | 0 新增 allocation/copy；`concurrent(N)` 替换 `concurrent()` 只改并发上限 | 修复 INC-048 的 queue-overflow 风险；吞吐上限由 N × FUA latency 决定，N 可调 |
| carrier 加 bound 字段 | 每 carrier +4B 按值字段 | 零 heap |
| `NvmeProvider` | 空仿函数，inline 后零成本 | — |
| oversized 预检 | O(put 数) 次整数比较，在 coord 侧 continuation 执行 | 防止整 round 连坐 |
| read 路径 | 0 改动（除 provider 注入） | copy-out 语义不变 |

无新增 queue hop；persist 的 NVMe 提交次数不变（同样的 writes，只是
inflight 受限）。

## 9. 测试计划

Target：`inconel_test_m07_value_persist_read_adapter`
（`apps/inconel/test/test_m07_value_persist_read_adapter.cc`，CMake
照 m01-m06 模式注册）。fake NVMe 复用 M06 测试已有的
instrumented fake（统计 max simultaneous inflight + 可注入失败）；
若 M06 的 fake 是 test-local，允许提炼成共享 test helper 或按需复制，
不得把 fake 塞进 production 目录。

必测：

1. `m07_put_batch_persists_and_fills_value_refs`
   多 PUT（覆盖 ≥2 个 size class，含 sub-LBA 同页多 entry）batch_ctx
   → L3 `persist_put_values` → sender 成功；每个 PUT 的
   `allocated_vr` 非默认值且互不重叠；sender 完成时 fake NVMe 已收到
   全部 FUA 写（完成点 = durable + settled 的验收）。
2. `m07_read_value_round_trip`
   1 之后对每个 `allocated_vr` 走 `value::read_value` → 返回原始
   body（逐字节比对）；同页多 value 的 cache/round 命中路径与
   NVMe miss 路径都要覆盖（miss 路径用"新建 value owner 实例 +
   预写盘镜像"或显式逐出构造，手段由实现定，不得砍掉 miss 覆盖）。
3. `m07_delete_only_batch_skips_value_module`
   DELETE-only batch_ctx → L3 短路成功；fake NVMe 零写入；value
   owner 无 round 痕迹（`inflight_rounds_` 空可经 inspection helper
   或行为断言验证）。
4. `m07_mixed_batch_only_puts_persist`
   PUT+DELETE 混合 → 只有 PUT 填 vr，DELETE entries 的
   `allocated_vr` 保持默认。
5. `m07_bounded_write_inflight`
   PUT 量足以产生 ≫ `max_write_inflight` 个 page writes（用小
   `max_write_inflight` 配置）→ fake NVMe 统计的最大并发 ≤ 配置值；
   全部写完成且 vr 正确。
6. `m07_bounded_prefill_read_inflight`
   构造 `nonresident_partial` 场景（先 persist → reclaim 部分 slot →
   逐出 resident frame → 同 class 再 persist 触发 prefill），断言
   prefill reads 并发 ≤ `max_read_inflight` 且 round 正确完成。
   若该构造在当前 value 模块 API 下不可达，必须在总报告中显式声明
   并给出原因，不允许静默跳过。

   > review 记录（2026-06-12）：落地实现降级为 helper 级覆盖（直接
   > 测 `nvme::read_frame_range_bounded` 的 bounded 行为与数据正确
   > 性），未走 value owner 的 prefill 链路；实现方总报告因进程挂起
   > 未产出，该降级由 review 独立发现。owner prefill 链路（含
   > prefill 失败的 follower 通知 + rollback）的单元级覆盖缺口登记
   > 为 INC-056，不阻塞 M07 land：bounded read 机制本身已被覆盖，
   > owner 链路另有真盘 `flush_e2e` 大量级 + reclaim 的间接覆盖
   > （INC-051 的 3 个 prefill bug 即由此暴露）。
7. `m07_persist_failure_maps_to_error_and_rolls_back`
   fake NVMe 注入 FUA 失败 → L3 sender 抛 `value_persist_error`
   （reason=round_failed）；随后同一 owner 上的新 persist round 正常
   成功（rollback 干净的行为证据）。
8. `m07_follower_round_merge`
   两条 persist pipeline 并发 submit 到同一 value owner（单核交替
   advance）→ 合并为一个 round（fake NVMe 只见一组写）或两个独立
   round 均可，但两个 batch 各自拿到正确且不重叠的 vr；失败注入时
   两个 batch 都收到失败。
9. `m07_oversized_put_fails_fast`
   body 超最大 class → L3 预检抛 `value_persist_error`
   （reason=oversized_value）；fake NVMe 零流量；value owner 无
   round 痕迹。
10. `m07_trim_drain_bounded`
    触发 trim batch（persist → 全页 reclaim → `drain_trim_pending`）
    → trim 并发 ≤ `max_trim_inflight`，完成后 manager 状态推进
    （`complete_trim` 被调）。

回归门：`cmake --build build` 全 target 编译 +
`inconel_test_m01..m07` 全 PASS + `build_asan` 同名 target 全 PASS
（frame 生命周期与 context-owned vector 必须 ASAN 干净）。
`inconel_test_value_space_manager` / `inconel_test_flush_e2e` 必须
继续编译通过；flush_e2e 的真盘运行不在本步门槛内（§11.3）。

## 10. 实现顺序

1. `value/scheduler.hh`：`value_io_policy` + 构造参数 + 校验 +
   `io_policy()` / `max_body_len()` 访问器；carrier 三处加 bound
   字段并在 publish/prepare 点填入。
2. `nvme/frame_io.hh`：补 `read_frame_range_bounded`（与 write 版
   对称）。
3. `value/sender.hh`：rename + `NvmeProvider` + 三处 bounded 改造 +
   `value_persist_error` + 文件头线程约束注释（§7.4）。
4. `runtime/builder.hh`：`build_options` 接 `value_io_policy`。
5. `write_path/sender.hh`：L3 `persist_put_values(batch_ctx&)`。
6. 测试：fake NVMe 基建复用/提炼 → 按 §9 列表落测试。
7. `known_issues.md`：INC-048 移入 Resolved（注明 M07/046）；
   045 §10.1 的 watch-item 在本文 §7 关闭。

每个阶段独立提交（前缀 `nitro: inconel: 046 ...`），保持每步全仓
编译绿。

## 11. 相邻事项

1. **M08/M09 对接**：write_batch 编排把
   `write_path::persist_put_values(ctx)` 放在 WAL fan-out 之前；
   catch `value_persist_error` → `coord::release_batch`。完成点
   语义（§5.2.3）使 value-before-WAL 由因果链保证，M08 不需要
   额外屏障。
2. **M10 读路径**：point_get 的 memtable/tree hit 统一走
   `value::read_value`；`read_page_values`（INC-021）与 INC-055
   residency tier 到时一起评估。
3. **flush_e2e harness 的 `kPersistChunkPuts = 1024` 兜底**：
   production 修复后该兜底逻辑上可撤，但撤除需要真盘 e2e 验证；
   本步不动 harness 行为（只随 rename 修编译），撤除动作挂在
   INC-048 Resolved 条目的备注里，等下次真盘 e2e 一并做。
4. **watch-item**：`value/sender.hh` include `runtime/facade.hh`
   形成 L2→L3 头文件反向依赖（facade 实质只是 core registry 的
   糖）。与 M06 之后的整体 include 层级清理一起处理，本步不动。
5. **INC-054 / INC-052**：与本步正交，未触碰。
