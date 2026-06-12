# 047 - M08 Write Baseline + Inflight Semantics

> 本文对应 `ai_context/inconel/plan/front_wal_development_plan.md` 的 M08
> （旧 step 22 单 batch baseline 写路径 + 旧 step 23 多 batch 并发与
> durable_lsn 推进）。
> 目标不是迁移旧分支的 legacy runtime harness，而是在 M01-M07 已经立起来的
> carrier / coord / front / wal / value / write_path 组合层上，补齐写路径的
> **batch phase 状态机**与**分阶段组合 senders**，使"单 batch 全程语义"和
> "多 batch in-flight、乱序 publish/release、durable_lsn gap-free"成为可被
> 白盒测试驱动的闭环。M09 在此之上组装 production 顶层 `write_batch` sender。

## 1. 范围

M08 覆盖：

- L3 `write_path/write_batch_state.hh`（新文件）：
  - `write_batch_phase` 枚举（batch 写路径阶段状态机）。
  - `write_batch_state` carrier：own `core::batch_ctx` + 当前 phase。
  - checked phase transition helpers（误用 fail-fast）。
- L3 `write_path/sender.hh` 新增 5 个 phase 组合 senders：
  - `write_batch_value_phase(state, nvme)` — Phase A 包装 + 状态推进。
  - `write_batch_wal_phase(state, fronts, wal_space, nvme_by_owner)` —
    all-WAL barrier（fan-out `write_wal_fragment` + 全员完成后裁决）。
  - `write_batch_memtable_phase(state, fronts)` — all-memtable barrier，
    fan-out 前先进入 `memtable_applying`（release 禁止线）。
  - `write_batch_publish(coord, state)` / `write_batch_release(coord, state)`
    — 携带 phase 检查的 terminal 调用（041 §9 要求的 "carry batch phase
    state and call the correct terminal path"）。
- 白盒测试 `inconel_test_m08_write_baseline_inflight`：
  - baseline：单 batch PUT/DEL 全程 value/WAL/memtable/publish。
  - inflight：park（停在 all-WAL barrier 之后）、乱序 finish、release 填洞，
    durable_lsn gap-free。
  - 失败映射：value 失败 → release、WAL 失败 → release、
    `prepare_queue_full` → release（045 §10.4 点名对照项）。
  - phase 误用 fail-fast（publish 早调 / release 晚调）。

M08 不覆盖：

- M09 的 production 顶层 `write_batch` sender：单链
  assign → value → WAL fan-out → memtable fan-out → publish，
  `any_exception` 异常分类（`value_persist_error` / `wal_append_error` →
  release；其他 → fatal）、"submit 前不得产生 owner side effect"、
  fatal 的运行时终止机制、ACK surface。
- M10 point_get、M11 runtime topology / registry front list、M12 seal。
- recovery、flush、tree、value 模块内部任何改动。
- coord / front / wal / value 任何 scheduler 内部行为变更——M08 是纯 L3
  组合 + 新 carrier，L2 全部按 M03/M05/M06/M07 已冻结语义消费。

## 2. 已对照输入

正式设计：

- `design_doc/INDEX.md`（容量/性能硬约束）
- `design_doc/design_overview.md` §6（LSN 语义）、§7（写路径与提交语义，
  含 §7.4 publish 唯一要求）、§8.1（读可见性，测试断言依据）、§11.4（WAL 反压）
- `design_doc/write_path_and_pipeline.md` §1/§2（pipeline 拓扑与三阶段冻结
  约束）、§8（多 batch in-flight、durable_lsn 推进、§8.5 窗口反压）、
  §10（异常分类与 §10.4 memtable 失败 fatal）、§11（持久化顺序论证）
- `design_doc/runtime_state_machine.md` §2（coord）、§3（front）、
  §11.1（post-LSN 失败矩阵）
- `design_doc/cross_doc_contracts.md` §1（handle 签名）、§5（写路径跳转）
- `design_doc/code_modules.md`（L3 write_path/pipeline 职责划分）
- `design_doc/code_quality_standard.md`（热路径预算、§3.5 flat_map 规则、
  §3.6 owner 可见性）

当前分支代码（M01-M07 产物，全部按已冻结语义消费）：

- `apps/inconel/core/batch_carrier.hh`（`batch_ctx` / `front_fragment` /
  `canonical_entry`）
- `apps/inconel/coord/scheduler.hh` + `coord/sender.hh`
  （`assign_batch_lsn` → `batch_ctx`、`publish_batch` / `release_batch` →
  void、`acquire_read_handle`；misuse 抛 `std::logic_error` 的 house style）
- `apps/inconel/front/scheduler.hh` + `front/sender.hh`
  （`insert_memtable_entries`、WAL prepare/install/commit/abort、WAL gate +
  pending prepare FIFO、`wal_pending_prepare_capacity_ = queue_depth`）
- `apps/inconel/front/wal_append.hh`
  （`wal_append_error_reason::prepare_queue_full`）
- `apps/inconel/write_path/sender.hh`
  （M06 `write_wal_fragment`、M07 `persist_put_values(batch_ctx&)`）
- `apps/inconel/value/sender.hh`（`value_persist_error`、`NvmeProvider` 注入）
- `apps/inconel/core/read_catalog.hh` / `core/memtable_lookup.hh`
  （测试可见性断言用）

plan 文档：

- `041_coord_scheduler_assign_publish_release_design.md` §9/§14.3/§19.3
  （release 前提、失败边界、"M08/M09 必须显式携带 batch phase"）
- `043_front_scheduler_memtable_owner_design.md` §6（insert 前置 6：
  all-WAL barrier 已成功）、§14.2（memtable apply failure 不 rollback 不 release）
- `044_wal_append_prepare_bounded_fua_design.md` §7.2/§10
  （`write_wal_fragment` L3 语义、`wal_append_error` → release 映射）
- `045_front_wal_review_fixes_design.md` §5.D.2.7（per-front 并发上限）、
  §7.1（`prepare_queue_full` pre-memtable 可 release）、§10.4
  （点名 M08 必须对照本两项）
- `046_value_persist_read_adapter_design.md` §5.2/§11.1
  （persist 完成点 = durable + settled；M08 把它排在 WAL 之前即满足
  value-before-WAL 因果链，不需要额外屏障；catch `value_persist_error` →
  `coord::release_batch`）

旧分支证据（语义参考，不迁移代码；设计者角色已声明读测试）：

- `inconel:apps/inconel/test/legacy_runtime/write_path_baseline.hh`
- `inconel:apps/inconel/test/legacy_runtime/write_path_inflight.hh`
- `inconel:apps/inconel/test/step_22_write_path_baseline_contract_test.cc`
- `inconel:apps/inconel/test/step_23_write_path_inflight_contract_test.cc`

登记问题：INC-054（urgent，与 M08 正交，不动）、INC-056（normal，prefill
owner 链路测试缺口，与 M08 正交，不动）。

## 3. 语义来源对照表

| 项目 | 旧 `inconel` Step 22/23 证据 | 当前 `inconel.new` 现状 | 正式设计依据 | 047 决议 |
|---|---|---|---|---|
| baseline 形态 | `test/legacy_runtime/write_path_baseline.hh`：同步 class，直调 coord/value/wal/front state 方法 + mock_block_device | 无 legacy runtime；M06/M07 已有 PUMP L3 组合 senders | 迁移计划 M08 "不重建 legacy runtime path；baseline 只能作为语义测试梯度"；CQS §3.9 设备访问不得回退同步直调 | baseline = 白盒测试里把 5 个 production phase senders 按序串起来；不落任何同步 harness class，不引入 mock device boundary |
| batch phase 携带 | 旧 inflight class 用 `inflight_` map 隐式表达 "已 WAL 未 memtable" | 无任何 phase 表达；coord 的 release 前提（"memtable phase 未开始"）完全靠调用方自觉 | 041 §9 "Coord cannot infer that phase from an LSN alone; M08/M09 must carry batch phase state and call the correct terminal path"；WP §2.3 冻结约束 3 | 新增 `write_batch_phase` + `write_batch_state`，terminal 调用必须经 checked helpers；这是 M08 的核心 production 交付物 |
| parked/inflight state | 旧 `parked_state` 把每个 fragment 的 entries **整份拷贝**（`std::vector<canonical_entry>` per fragment，第二份 batch owner） | M01 `batch_ctx` 已是 view-based 自包含 owner（input bytes + entries + index fragments） | 迁移计划 M08 "inflight state 不得长期复制第二份完整 batch owner"；CQS §2.2 不允许用 copy 掩盖 ownership | parked state 就是 `write_batch_state` 本体（own `batch_ctx`，move 进出）；零第二份拷贝，测试断言 entries 存储指针 park 前后不变 |
| start/finish/release 切分 | Step 23：`start_batch`（assign→value→WAL→park）/ `finish_batch`（memtable→publish）/ `release_batch`（弃 park→coord release） | 无 | WP §2.3：memtable phase 只能在 all-WAL reduce 成功后启动；OV §7.4 publish 唯一要求 | 同一语义梯度由 phase senders 表达：park = 停在 `wal_durable`；finish = memtable phase + publish；release = checked release。无独立 "inflight 注册表" production 对象——park 表是旧 harness 形态，当前由调用方（测试/未来 pipeline context）持有 state |
| ticket / introspection | `inflight_batch_ticket{lsn, entry_count}`、`inflight_count()`、`has_inflight()` | 无 | 无正式设计对应物（纯 harness 观察口） | 不迁移。白盒测试直接持有 `write_batch_state` 并读 `state.phase` / `state.ctx.batch_lsn` |
| 乱序 resolve gap-free | Step 23 test 2/3：finish 1→durable 1；finish 3→durable 1；finish 2→durable 3；release 填洞同款 | M03 ready window + 连续前缀推进已落地并测过（coord 单元级） | OV §6 规则 6/7、WP §8.4 | M08 在**写路径全链**层面重验：park 的 batch 经真实 value/WAL/memtable 路径后乱序 publish/release，断言 durable_lsn 与可见性（不是只测 coord 位图） |
| value 失败映射 | 旧 baseline/inflight 返回 `nullopt`，caller 自行处理 | M07 `value_persist_error`（oversized_value / round_failed） | RSM §11.1 失败矩阵 1；046 §5.2.5/§11.1 | value phase 异常 → state 停在 `assigned` → `write_batch_release`；测试注入 FUA 失败驱动 |
| WAL 失败映射 | 同上 `nullopt` | M06 `wal_append_error`（device_failure / prepare_queue_full / ...） | RSM §11.1 失败矩阵 2；044 §10.3；045 §7.1/§10.4 | WAL phase 异常 → state 停在 `value_durable` → `write_batch_release`；`prepare_queue_full` 同路（pre-memtable）；测试分别驱动 |
| memtable 失败语义 | 旧 harness 无该路径 | M05 §14.2：apply failure 不 rollback、不 release | WP §10.4 fatal；OV §6 额外约束 7 | memtable fan-out 前先置 `memtable_applying`；此后 release 一律 `std::logic_error` fail-fast；M08 锁死状态机线，运行时终止机制留 M09 |
| 落点 | 旧代码在 `test/legacy_runtime/` | 迁移计划写 `apps/inconel/pipeline/write_batch_state.hh`；但 M06 已裁决 L3 写组合落 `write_path/`（044 §3.2），`code_modules.md` 关键约束明示 "`write_path/` 是写请求专用组合层；`pipeline/` 保留为**其它**顶层 pipeline 编排入口" | code_modules.md 模块总览 + 关键约束 | **落 `apps/inconel/write_path/`**（`write_batch_state.hh` + `sender.hh` 扩展）。计划文档的 `pipeline/` 落点行按本裁决更新（见 §15.3）。M09 顶层 sender 届时同样落 write_path/，由 M09 设计确认 |

## 4. 冲突与裁决

1. **`pipeline/` vs `write_path/`**：见 §3 末行。`code_modules.md` 的
   pipeline/ 职责表里那条"写 pipeline"是 write_path/ 拆分前的旧文本；
   关键约束段（更晚、更具体）已把写请求组合划给 write_path/。047 以关键
   约束段为准，并把 `front_wal_development_plan.md` M08 落点行同步更新。
2. **baseline 是否落 production helper**：迁移计划说 "baseline 只能作为
   语义测试梯度，不作为最终 public surface"。047 的解读：**分阶段 phase
   senders 是 production 交付物**（M09 直接组装它们，语义完整、命名按约束 B
   对应完整语义）；**baseline/inflight 的"梯度串法"只存在于测试**（先跑到
   哪一阶段、何时 finish/release 是测试驱动顺序，不是 production API）。
   不存在叫 `write_batch` 的 M08 surface——该名字留给 M09 的完整语义。
3. **phase 误用的失败机制**：house style 对照——coord 对 terminal 误用
   （duplicate publish、unassigned LSN）抛 `std::logic_error`（M03 已测）。
   M08 的 phase 误用（publish 早调、release 晚调、phase 顺序错）同属
   "调用方编排 bug"类，统一 `std::logic_error`；拓扑接线错（owner 越界、
   span 尺寸不匹配、空指针）属配置/接线错，抛 `std::invalid_argument`。
   不用 `panic_inconsistency`（那留给"盘面/owner 状态已不可知"类；这里
   状态仍自洽，只是调用方用错了入口），且 logic_error 可直接被测试断言。
4. **value round 共担命运**：并发 batch 的 value phase 可能被 leader-follower
   合并进同一 round；round 失败时所有被合并 batch 一起收到
   `value_persist_error`（M07 §9.8 已测）。这不是 M08 要改的行为——设计上
   每个失败 batch 各自走 release 即可，gap-free 由 ready window 保证。M08
   测试通过"逐个驱动 value phase 完成后再启动下一个 batch"保持确定性，
   不重复 M07 的 round merge 覆盖。

## 5. `write_batch_state` 设计（`write_path/write_batch_state.hh`）

### 5.1 Phase 枚举

```cpp
namespace apps::inconel::write_path {

enum class write_batch_phase : uint8_t {
    assigned          = 0,  // batch_ctx 已由 coord 产出，LSN 已消耗
    value_durable     = 1,  // Phase A 完成（PUT value FUA settled；DELETE-only 短路同样到达）
    wal_durable       = 2,  // all-WAL barrier 成功 —— inflight/park 边界
    memtable_applying = 3,  // memtable fan-out 已投递 —— release 禁止线
    memtable_applied  = 4,  // all-memtable barrier 完成
    published         = 5,  // terminal：coord publish 已接受
    released          = 6,  // terminal：coord release 已接受（pre-memtable 失败）
};
```

语义对应：

- `assigned → value_durable`：WP §2.1 Phase A。
- `value_durable → wal_durable`：WP §2.1 Phase B（all-WAL reduce 成功）。
- `wal_durable → memtable_applying`：WP §2.3 冻结约束 3 的"进入
  memtable phase"瞬间——**第一条 insert 投递之前**就翻线，从此失败语义
  不再是 release（保守界：宁可把"还没真正 insert"也算进禁区，绝不允许
  反向漏过）。
- `memtable_applying → memtable_applied`：all-memtable barrier 完成。
- `memtable_applied → published`：OV §7.4。
- `{assigned, value_durable, wal_durable} → released`：OV §6 额外约束 7 的
  受限 clean abort；与 041 §9 前提一致。

### 5.2 Carrier

```cpp
struct write_batch_state {
    core::batch_ctx   ctx;
    write_batch_phase phase = write_batch_phase::assigned;

    explicit write_batch_state(core::batch_ctx&& assigned_ctx);
    write_batch_state(write_batch_state&&) noexcept = default;
    write_batch_state& operator=(write_batch_state&&) noexcept = default;
    // copy 禁止（batch_ctx 本身不可拷贝；显式 = delete 以固化语义）
};
```

构造校验（违例抛 `std::invalid_argument`，这是"拿非 assigned ctx 建状态"
的接线错）：

1. `ctx.entry_count > 0` 且 `!ctx.canonical_entries.empty()`
   （0-entry batch 在 M03 是 pre-LSN no-op，不可能到达这里）。
2. `!ctx.fragments.empty()`。
3. `ctx.batch_lsn > 0`（coord `next_lsn > durable_lsn ≥ 0`，合法 LSN 从 1 起）。

不变量：

1. `write_batch_state` 是 batch 的**唯一** owner carrier——park 就是持有
   本对象，不存在第二份 entries / fragments 拷贝。
2. `ctx.canonical_entries` / `ctx.fragments` 在构造后只读（value phase 写
   `allocated_vr` 字段是 M01/M07 既有契约，不增删元素），因此 phase
   senders 内部的 `&state` / span 借用全程稳定。
3. `phase` 只能通过 §5.3 的 checked helper 推进；测试可直接读。

### 5.3 Checked transitions

```cpp
// 当前 phase != expected 时抛 std::logic_error（携带 site 与两个 phase 名）。
void require_write_batch_phase(const write_batch_state& state,
                               write_batch_phase expected,
                               const char* site);

// require + 推进，一步完成。
void advance_write_batch_phase(write_batch_state& state,
                               write_batch_phase from,
                               write_batch_phase to,
                               const char* site);

// release 专用前置：phase 必须 ∈ {assigned, value_durable, wal_durable}。
void require_release_allowed(const write_batch_state& state, const char* site);
```

`require_release_allowed` 对 `memtable_applying / memtable_applied /
published / released` 一律 `std::logic_error`——对应 041 §9 "Calling
release_batch after memtable phase starts is a fatal write pipeline bug"
与 §11.1 invalid transitions（terminal 重复 resolve 同样被挡）。

## 6. Phase 组合 senders（`write_path/sender.hh` 扩展）

所有 helper 的 phase 检查都在 **sender 执行期**（链内第一个 `then`）完成，
不在组合期——M09 会把整链在任何 phase 运行前一次composed；组合期检查会
全部空过。

### 6.1 `write_batch_value_phase`

```cpp
template <typename NvmeProvider = value::local_nvme_provider>
inline auto
write_batch_value_phase(write_batch_state& state, NvmeProvider nvme = {}) {
    return just()
        >> then([&state] {
            require_write_batch_phase(state, write_batch_phase::assigned,
                                      "write_batch_value_phase");
        })
        >> flat_map([&state, nvme]() mutable {
            return persist_put_values(state.ctx, nvme);   // M07 L3，已有
        })
        >> then([&state](bool) {
            advance_write_batch_phase(state,
                write_batch_phase::assigned,
                write_batch_phase::value_durable,
                "write_batch_value_phase");
        });
}
```

- DELETE-only batch：`persist_put_values` 短路 `just(true)`，phase 照常推进
  到 `value_durable`（"Phase A 完成"对 DELETE-only 平凡成立）。
- 失败：`value_persist_error` 从 `persist_put_values` 抛出，`advance` 不
  执行，phase 停 `assigned`，release 合法。
- `flat_map` 论证（CQS §3.5）：phase 检查必须先于 value owner 投递这个
  运行期顺序约束，构成 phase 边界；`persist_put_values` 是 owner sender，
  无法改写为 `then`。
- 实现记录（2026-06-12 review）：落地形态为"显式 provider 重载 + 无参
  重载（默认 `local_nvme_provider`）"，与本节默认实参草图等价；模板参数
  名为 `nvme_sched_t`（语义实为 NvmeProvider），cosmetic 命名出入，
  review 接受不返工，M09 组装时可顺手对齐 M07 命名。

### 6.2 `write_batch_wal_phase`

```cpp
template <typename nvme_sched_t = nvme::runtime_scheduler>
inline auto
write_batch_wal_phase(write_batch_state& state,
                      std::span<front::front_sched* const> fronts,
                      wal::wal_space_sched& wal_space,
                      std::span<nvme_sched_t* const> nvme_by_owner);
```

语义展开：

```text
1. then：require phase == value_durable；
   拓扑校验（std::invalid_argument）：
     fronts 非空、nvme_by_owner.size() == fronts.size()、
     ∀ frag ∈ ctx.fragments: frag.owner < fronts.size()，
     fronts[frag.owner] / nvme_by_owner[frag.owner] 非空。
2. fan-out（all-WAL barrier）：
   loop(ctx.fragments.size())
     >> concurrent(ctx.fragments.size())
     >> flat_map([&state, fronts, &wal_space, nvme_by_owner](auto i) {
            auto& frag = state.ctx.fragments[i];
            return write_wal_fragment(*fronts[frag.owner], wal_space,
                                      nvme_by_owner[frag.owner], frag,
                                      entries_span(state.ctx))
                >> then([](bool) { return std::exception_ptr{}; })
                >> any_exception([](std::exception_ptr ep) {
                       return just(std::move(ep));   // 转正常值，元素不中断流
                   });
        })
     >> reduce(std::exception_ptr{},
               [](std::exception_ptr acc, std::exception_ptr e) {
                   return acc ? acc : e;             // first-error wins
               })
3. then：若 first_error 非空 → rethrow（phase 停 value_durable）；
   否则 advance value_durable → wal_durable。
```

要点：

1. **fan-out 载体用 `loop(n)` + 索引**，不用
   `for_each(state.ctx.fragments)` / `as_stream(lvalue 成员)`——后者的
   引用推导悬垂是 M07 修过的坑（trim span lifetime）。索引进 lambda 后
   取 `state.ctx.fragments[i]` 引用，state 由调用方保活（§8）。
2. **并发界 = `ctx.fragments.size()` ≤ front_count**：每个 fragment 目标
   front 互不相同（M01 route 按 owner 分组），单 batch 在每个 front 上
   至多一个 fragment，扇出本身无放大。这就是 WP §2.1
   `concurrent(front_sched_count)` 的逐 batch 精确形态——显式、可数，
   满足迁移规则 11 的 bounded 要求；页级 FUA 并发由 `write_wal_fragment`
   内部的 `max_fua_inflight`（M06/045）继续约束，两层 budget 不混。
3. **异常聚合先于 reduce**：per-element `any_exception` 把异常转为正常
   值，规避 CLAUDE.md 陷阱 6（reduce 不因异常终止流）；barrier 必须等
   **全部** fragment 终结（成功或 abort 完成）后才裁决——否则 release
   时仍有 fragment 的 FUA/plan 在飞，frames 生命周期与 front WAL gate
   会悬空。`write_wal_fragment` 内部失败时已自行 `abort_wal_plan`（M06），
   本层只负责收齐后 rethrow 第一个错误。
4. 真并发性：fan-out 元素是 owner senders（front/wal/nvme 队列异步），
   `concurrent(N)` 声明允许 N 个在飞，与 RPC pipelining 模式同款；不需
   要 `on(task)`。

### 6.3 `write_batch_memtable_phase`

```cpp
inline auto
write_batch_memtable_phase(write_batch_state& state,
                           std::span<front::front_sched* const> fronts);
```

语义展开：

```text
1. then：require phase == wal_durable；拓扑校验同 §6.2（无 nvme）。
   advance wal_durable → memtable_applying     // fan-out 之前翻线
2. fan-out（all-memtable barrier，形态同 §6.2）：
   loop(fragments.size()) >> concurrent(fragments.size())
     >> flat_map(i → front::insert_memtable_entries(*fronts[owner], frag,
                                                    entries_span(ctx))
                  >> then(→ exception_ptr{})
                  >> any_exception(→ just(ep)))
     >> reduce(first-error)
3. then：若 first_error 非空 → rethrow，phase 停 memtable_applying；
   否则 advance memtable_applying → memtable_applied。
```

失败语义（WP §10.4 / M05 §14.2）：

1. 任何 insert 异常 → barrier 等全员终结后 rethrow；phase 停
   `memtable_applying`；`require_release_allowed` 从此恒拒——该 batch 只
   剩 fatal 路径。M08 的交付边界是**状态机锁死**（release/publish 都被
   `std::logic_error` 挡住），运行时终止机制由 M09 production 链设计。
2. 部分 front 已插入、部分失败时：batch 的 LSN 永不 resolve，durable_lsn
   被它挡住，已插入 entries 对任何 reader 不可见（`data_ver > read_lsn`
   恒成立）；WAL 已全量 durable，运行时终止后 recovery 会把它作为完整
   batch 重放收敛——这正是 §10.4 要求 fatal 而非 release 的原因，写入
   设计注释。

### 6.4 `write_batch_publish` / `write_batch_release`

```cpp
inline auto
write_batch_publish(coord::coord_sched& coord, write_batch_state& state) {
    return just()
        >> then([&state] {
            require_write_batch_phase(state,
                write_batch_phase::memtable_applied, "write_batch_publish");
        })
        >> flat_map([&coord, &state] {
            return coord::publish_batch(coord, state.ctx.batch_lsn);
        })
        >> then([&state] {
            advance_write_batch_phase(state,
                write_batch_phase::memtable_applied,
                write_batch_phase::published, "write_batch_publish");
        });
}

inline auto
write_batch_release(coord::coord_sched& coord, write_batch_state& state) {
    return just()
        >> then([&state] {
            require_release_allowed(state, "write_batch_release");
        })
        >> flat_map([&coord, &state] {
            return coord::release_batch(coord, state.ctx.batch_lsn);
        })
        >> then([&state] {
            state.phase = write_batch_phase::released;   // 经 helper 推进
        });
}
```

- release 的最终推进同样走 `advance_write_batch_phase`（from 为进入时
  快照的 pre-memtable phase）或等价的 checked 写法——实现细节自由，但
  不允许出现绕过检查的裸 `state.phase =` 第二入口。
- publish/release 完成 = coord 已接受 terminal 信号（M03 语义：closed
  gate 下可见性推迟到 open，不影响本状态机）。

### 6.5 `entries_span` 小工具

`std::span<const core::canonical_entry>(ctx.canonical_entries.data(),
ctx.canonical_entries.size())` 的命名 helper，消除两处 fan-out 重复拼写。
不引入任何新语义。

## 7. In-Flight 边界与背压对照（045 §10.4 点名项）

M08 自身不新增任何并发闸门；它必须把既有三层 budget 的叠加关系写清并测到：

| 层 | 闸门 | 越界行为 | M08 映射 |
|---|---|---|---|
| 全局 in-flight batch 数 | coord ready window（M03 §5.3 assign capacity，045 P2.6 后为 2 的幂） | assign 进 pending FIFO（不消耗 LSN）；FIFO 满 → pre-LSN fail 回调 | parked batch 数 ≤ window；测试 window 取 ≥ 并发数的 2 的幂 |
| per-front WAL phase 并发 | front WAL gate：1 个 pending plan + `wal_pending_prepare_capacity`（= ctor `queue_depth`）个排队 prepare（045 §5.D） | 超容 → `wal_append_error{prepare_queue_full}`，**pre-memtable** | 该异常使 wal phase 失败、phase 停 `value_durable` → `write_batch_release`；**这就是 045 §7.1/§10.4 要求的 prepare_queue_full → release 映射**，测试 m08_7 直接驱动 |
| 页级 FUA 并发 | `wal_append_config.max_fua_inflight`（M06/045，默认 16） | 自然排队 | M08 不触碰，沿用 |

补充两条边界事实（写进设计注释，M09 设计 in-flight 上限时引用）：

1. parked（已过 all-WAL barrier）的 batch **不占** front WAL gate——gate
   以 plan 为串行单元，commit 后即释放；park 只占 ready window 槽位。
   因此"长期 park 大量 batch"压的是 coord 窗口，不是 front。
2. WAL segment 耗尽（M04 pending FIFO）不是失败：wal phase 挂起等待，
   不 release（044 §10.2 / OV §11.4 规则 5）。M08 不为此加测试（M04/M06
   已覆盖），仅在注释中区分"挂起等待"与"prepare_queue_full 失败"两类。

## 8. Lifetime 契约

| 对象 | Owner | 必须活到 | 保障方式 |
|---|---|---|---|
| `write_batch_state`（含 `batch_ctx`） | M08 测试：测试侧容器（`std::unique_ptr` / vector）；M09：pipeline context | 该 batch terminal（published/released）且最后一个引用它的 phase sender 完成 | 调用方契约（M06 fragment/entries、M07 puts vector 同款）；phase senders 全部以 `&state` 借用，文档 + 注释写明 |
| `state.ctx.canonical_entries` / `fragments` 存储 | `batch_ctx` | 同上 | 构造后不增删元素（value phase 只写 `allocated_vr` 字段值），span/引用稳定 |
| fan-out lambda 捕获 | 借用（`&state`、按值 span/指针） | 单次 phase sender 完成 | CQS §3.3/§3.4：零 owning capture，无每请求容器拷贝 |
| coord/front/wal/value scheduler 指针 | runtime（测试 fixture） | 进程级 | 既有契约 |

注意一条顺序约束：park 容器若用 `std::vector<write_batch_state>`，**严禁
在任何 phase sender 在飞期间触发扩容搬移**（`&state` 会悬空）。测试统一用
`std::vector<std::unique_ptr<write_batch_state>>` 或预 reserve，设计注释
写明该坑（与 045 C.2 "FUA 飞行期间禁止移动所属容器"同源）。

## 9. 内存序与并发安全

1. `write_batch_state` 是 request-private 单写者对象：phase 与 ctx 的每次
   读写都发生在 pipeline 推进链上的某个确定点，相邻写读之间由 PUMP
   per-core queue 的入队/出队边提供 happens-before（与 M01 `allocated_vr`
   由 value owner 写、front 读的既有论证同构）。不引入任何 atomic。
2. fan-out 期间各 fragment 的子链只读共享 `ctx.canonical_entries`（此时
   已无写者）；对 `state.phase` 的写只在 barrier 前后的串行点。
3. 异常聚合走 `reduce` fold（CODING_GUIDE §2.2 文档化的并发聚合模式），
   不引入共享可变 error slot。
4. durable_lsn 可见性沿用 M03 release/acquire 链；M08 不新增发布协议。

## 10. 错误 / 失败语义总表

| 场景 | 抛出点 | 类型 | phase 终态 | 后续合法动作 |
|---|---|---|---|---|
| value oversized / round failed | value phase | `value_persist_error` | `assigned` | `write_batch_release` |
| WAL encode/validation 失败 | wal phase | `wal_append_error` | `value_durable` | release |
| WAL FUA 失败（abort 完成后） | wal phase | `wal_append_error{device_failure}` | `value_durable` | release |
| per-front prepare FIFO 超容 | wal phase | `wal_append_error{prepare_queue_full}` | `value_durable` | release（045 §7.1） |
| WAL segment 耗尽 | —（不抛） | 挂起等待 M04 分配 | 停留 `value_durable`，sender 未完成 | 等待；不 release |
| memtable insert 异常 | memtable phase | 原异常 rethrow | `memtable_applying`（锁死） | 仅 fatal；release/publish 均 `std::logic_error` |
| publish 在非 `memtable_applied` | publish helper | `std::logic_error` | 不变 | 修编排 bug |
| release 在 `memtable_applying` 及之后 | release helper | `std::logic_error` | 不变 | 同上（041 §9 fatal pipeline bug） |
| 拓扑接线错（owner 越界/尺寸/空指针） | phase helper 校验 | `std::invalid_argument` | 不变 | 修 fixture/topology |
| 非 assigned ctx 构造 state | ctor | `std::invalid_argument` | — | 修调用方 |

公共纪律：所有失败都不得让 barrier 提前返回（§6.2 要点 3）；所有 phase
推进都在对应 owner 动作**完成回调之后**（state commit 先于上层观察，与
M03/M05 callback 纪律同向）。

## 11. 热路径预算与容量估算

新增 runtime carrier 估算（10 亿 KV 基线）：

| Carrier | 数量级 | 内存 |
|---|---|---|
| `write_batch_state` | 每 in-flight batch 一个（≤ coord ready window，部署配置） | `sizeof(batch_ctx)` + 1B phase（padding 后 +8B 内）；batch_ctx 的 input bytes / entries 本就是 M01 已计成本，M08 增量仅 phase 字节。**无任何常驻/每 manifest carrier** |
| phase senders | 无新长期状态 | 0 |

写路径热路径增量（对照 M06/M07 已核销预算，只列 M08 新增）：

| 路径 | M08 新增成本 | 说明 |
|---|---|---|
| state 构造 | 1 次 `batch_ctx` move（指针交换级） | 无 heap、无 copy |
| value phase 包装 | +2 次 phase 函数调用（比较 + 赋值） | 委托 M07，预算不变 |
| wal phase | 每 batch 1 条 `loop+concurrent+flat_map+reduce` 链的框架 op 状态；每 fragment +1 个 `exception_ptr` 值传递（2 指针）与 1 次 fold 比较 | 0 新 heap（fan-out lambda 零 owning capture；fragments/entries 全借用）；页级成本仍由 M06 预算覆盖 |
| memtable phase | 同 wal phase 形态 | 同上；insert 本体是 M05 预算（probe-then-allocate，热路径 1 次 btree 下潜） |
| publish/release 包装 | +1 次 phase 检查 | coord terminal 路径维持 M03 "0 alloc 0 payload copy" |
| **M08 测试梯度专属** | 每个 phase 单独 `submit`（root context/scope 各一次分配） | 仅测试驱动方式的成本；M09 单链组装后整 batch 只有一次 submit，phase helpers 本身不强制多 submit |

无新增 queue hop：phase senders 没有引入任何额外 scheduler 跳转——跳转
序列与 WP §2.1 拓扑逐点一致（coord → value → front×F → front×F → coord）。

## 12. 测试计划

Target：`inconel_test_m08_write_baseline_inflight`
（`apps/inconel/test/test_m08_write_baseline_inflight.cc`，CMake 照
m01-m07 模式注册，Release + ASAN 两套构建都进回归门）。

### 12.1 Fixture

1. fake NVMe：复用 m07 测试的 instrumented fake（read/write/trim 计数、
   max_active、按序失败注入、**按 lba 捕获写入字节供解码回读**）。允许
   按 046 §9 先例提炼共享 test helper 或复制进 m08 文件；不得放 production
   目录。WAL 路径要求 fake 满足 `write_frame(frame, flags)` 隐式 concept
   （M06 fake 同款），value 路径经 `NvmeProvider` 返回同一 fake。
2. 拓扑（默认）：`front_count = 2`；每 front 一个 `front_sched`（显式
   `initial_active` gen + `configure_wal_for_testing`）+ 共享
   `wal_space_sched`（同一 `segment_geometry`）+ 单 `value_alloc_sched`
   （m07 同款 class 配置，注册进 `core::registry`）+ `coord_sched`。
3. 初始 CAT：`prs.fronts[i].active` 必须与 `front[i]` 的当前 active 是
   **同一** `shared_ptr<memtable_gen>`（m03 的 make_cat 模式 + front 3-arg
   ctor 传入同一 gen），否则可见性断言测的是假拓扑。`durable_lsn = 0`，
   coord `next_lsn = 1`，ready window 取 2 的幂（如 64）。
4. 驱动：m07 同款 promise + 手动 advance 轮询
   （coord / fronts / wal_space / value / fake nvme）。
5. owner 路由断言一律用 `core::key_hash(key) % front_count` 推导，不准
   硬编码猜测。

### 12.2 测试列表

1. `m08_baseline_single_batch_put_delete_full_path`（≈ 旧 step 22）
   混合 batch（同 key 两次 PUT 取后者、一个 DELETE、一个第二 front 的
   PUT）→ 依次 value/wal/memtable/publish 四个 phase sender：
   - canonicalization：entry_count、last-op-wins、DELETE value 为空。
   - phase 轨迹：每个 phase sender 完成后 `state.phase` 逐级推进，最终
     `published`。
   - value：PUT 的 `allocated_vr` 非默认且互不重叠；DELETE 的保持默认。
   - WAL：从 fake 捕获的段字节解码——两个 front 的 segment header
     stream_id 正确；每条 entry 的 `lsn == 1`、`entry_count == 全局值`、
     op/value_ref 与 ctx 一致；未触达 front 的 WAL 无 entry 字节。
   - 可见性：publish 后 `acquire_read_handle().read_lsn == 1`；经 PRS
     snapshot lookup：PUT key 命中 value（`value_ref` 与 ctx 一致、
     `data_ver == 1`）、DELETE key 命中 tombstone、未写 key miss。
   - durable_lsn == 1。
2. `m08_delete_only_batch_full_path`
   DELETE-only batch 全程：value phase 短路（fake 在 value 阶段零写入）、
   WAL 有 DELETE entry、publish 后 tombstone 可见、durable_lsn 推进。
3. `m08_state_owns_ctx_without_second_copy`
   park 前记录 `ctx.canonical_entries.data()` / `ctx.input.bytes.data()`
   等存储指针，state move 进容器（park）后再 move 出，断言指针不变——
   "inflight 不复制第二份 batch owner"的结构证据；同时 static_assert
   不可拷贝、nothrow move。
4. `m08_inflight_parks_after_wal_without_publish`（≈ 旧 step 23 test 1）
   三个单 PUT batch 逐个驱动到 `wal_durable` 后 park：
   - lsn 依次 1/2/3；`durable_lsn == 0`；
   - 各 front `active_for_testing()->table` 为空（或 lookup 全 miss +
     read_lsn == 0）；
   - WAL 字节已可解码出三条 entry（durable 先于可见的直接证据）；
   - 三个 state 的 phase 均为 `wal_durable`。
5. `m08_out_of_order_finish_advances_durable_lsn_gap_free`（≈ step 23 test 2）
   接 4 的形态：finish(1)（memtable+publish）→ durable 1；finish(3) →
   durable 仍 1，但 batch 3 的 entries 已进 memtable 且在 read_lsn=1 下
   不可见（`data_ver > read_lsn` 遮蔽的直接断言）；finish(2) → durable 3，
   三个 key 全部可见且 `data_ver` 各为其 lsn。
6. `m08_release_parked_fills_hole_without_visibility`（≈ step 23 test 3）
   A park（lsn 1）、B 完整 finish（lsn 2，durable 停 0）→
   `write_batch_release(A)` → durable == 2；A 的 key 在 read_lsn=2 下
   miss；A 的目标 front memtable 无该 entry；A.phase == released。
7. `m08_wal_failure_maps_to_release_and_unblocks_later_batch`
   fake 注入 WAL FUA 失败 → wal phase 抛 `wal_append_error`、phase 停
   `value_durable`、目标 front memtable 零变化 → release → durable 推进；
   随后同 front 新 batch 全程成功（abort 后 committed cursor 复用，M06
   语义在全链层面的回归）。
8. `m08_value_failure_maps_to_release`
   fake 注入 value FUA 失败 → value phase 抛 `value_persist_error`、
   phase 停 `assigned` → release → durable 推进；后续 batch 正常。
9. `m08_prepare_queue_full_overflow_releases_without_blocking_others`
   （045 §10.4 对照）`front_count = 1`、front `queue_depth = 2`、四个
   DELETE-only batch（绕开 value round 合并干扰）：挂住 fake NVMe 不放行
   → batch1 占 pending plan、batch2/3 进 prepare FIFO（软容量 =
   queue_depth = 2）、batch4 prepare 溢出 →
   `wal_append_error{prepare_queue_full}` → release(4)，此时 durable
   仍为 0（1-3 未 resolve，不跳号）；放行 NVMe → batch1/2/3 依次完成
   WAL、memtable、publish；终态 durable_lsn == 4（1/2/3 published，
   4 released-empty）。
   > 参数修正记录（2026-06-12 review）：初版写 `queue_depth = 1` + 三个
   > batch，不可实现——pump 的 `spsc::queue`（`per_core::queue` 底层）用
   > `next == head` 满判定，可用槽 = 容量 - 1；容量 1 时 0 可用槽，首个
   > sender submit 即 queue-full fail-fast，根本到不了 WAL prepare FIFO。
   > 实现方按本条现行参数落地并在总报告显式声明，review 核实 ring 语义
   > 后采纳为正式参数。front ctor 当前以同一 `queue_depth` 同时决定请求
   > ring 容量与 `wal_pending_prepare_capacity_`（045 §5.D），两者解耦
   > 与否留给 M09/M11 runtime 配置设计，本步不动。
10. `m08_release_after_memtable_phase_starts_is_rejected`
    驱动某 batch 跑完 memtable phase（或构造停在 `memtable_applying`：
    对 `memtable_applied` 的 state 直接调 release 亦覆盖 applied 分支）→
    `write_batch_release` 链以 `std::logic_error` 失败（经
    `expect_throws` 风格断言），coord durable/ready 状态零变化。
11. `m08_publish_before_memtable_applied_is_rejected`
    对停在 `wal_durable` 的 parked state 调 `write_batch_publish` →
    `std::logic_error`；该 lsn 仍可正常 finish（检查只读不破坏状态）。

### 12.3 回归门

每个实现 Phase 结束：`cmake --build build` 全 target 编译 +
`inconel_test_m01..m08` 八个二进制全部 PASS；收尾 Phase 另跑
`build_asan` 同名 target 全 PASS（fan-out 异常聚合、state 借用与 park
容器路径必须 ASAN 干净）。`inconel_test_flush_e2e` 维持编译通过。
（修正记录：初版还列了 `inconel_test_value_space_manager`，该 target 在
当前 CMakeLists 中不存在——046 §9 的旧引用笔误，本文删除。）

## 13. 实现顺序（每 Phase 一个提交）

```text
Phase A  write_path/write_batch_state.hh（carrier + phase 机 + checked helpers）
Phase B  write_path/sender.hh 五个 phase senders（含 entries_span）
Phase C  测试基建（fake NVMe 复用/提炼 + 拓扑 fixture）+ 测试 1-3（baseline）
Phase D  测试 4-6（inflight park / 乱序 finish / release 填洞）
Phase E  测试 7-11（失败映射 + phase 误用 fail-fast）
Phase F  全量回归（Release + ASAN）+ 总报告（必须声明跳过项）
```

依赖：B 依赖 A；C 依赖 B；D/E 依赖 C。Phase A/B 是 production 实现阶段，
**禁止打开任何测试文件**；Phase C-E 以新测试作者身份工作，允许读的既有
测试仅限：`test/check.hh`、`test_m07_value_persist_read_adapter.cc`（fake
NVMe / value fixture / registry 模式）、`test_m06_front_wal_append_prepare.cc`
（WAL fake / 段字节解码模式）、`test_m03_coord_scheduler_assign_publish_release.cc`
（CAT 构造 / expect_throws 模式）、`test_m05_front_scheduler_memtable_owner.cc`
（front fixture 模式）；不得修改任何既有测试文件。

## 14. 排除范围

1. production 顶层 `write_batch` sender、异常分类 `any_exception` 链、
   ACK、fatal 运行时终止机制（M09）。
2. runtime builder / registry front list / facade 写路径入口（M11）。
3. seal 与写路径的交互、publish gate 编排（M12；M03 已冻结 gate 行为）。
4. point_get / 读管线（M10）。
5. 任何 L2 scheduler 行为变更、任何格式变更、recovery。
6. coord ready window 之外的新背压机制（不存在的需求）。

实现若需要以上任何一项才能编译，必须按迁移规则用显式命名的窄 shim 并
fail-fast，不得用通用名伪装。

## 15. 相邻事项

1. **M09 对接**：M09 把五个 phase senders 组装成单链 production
   `write_batch`，异常映射规则固定为——catch `value_persist_error` /
   `wal_append_error` 且 `require_release_allowed` 通过 → release；其余
   异常或 release 检查失败 → fatal。`std::logic_error`（编排 bug）绝不
   允许被映射成 release。M09 测试 1 "success path 语义等价 M08" 以本文
   测试 1/5 为基准。
2. **M11**：phase senders 的拓扑参数（fronts span / nvme_by_owner）届时
   由 registry/facade 提供；M08 的显式参数形态保持不变，M11 只是换调用方。
3. **plan 文档回填**：`front_wal_development_plan.md` M08 节落点行
   `apps/inconel/pipeline/write_batch_state.hh` 更新为
   `apps/inconel/write_path/write_batch_state.hh`（裁决见 §3/§4.1），并
   标注 "M08 的详细设计文档是 047_write_baseline_inflight_design.md"。
4. **known_issues**：M08 无新增/关闭项预期；若实现中发现新问题按惯例登记。

## 16. 需要人工判断的点

无阻塞项。落点（write_path/ vs pipeline/）、失败机制（logic_error vs
panic）、baseline 形态（production phase senders + 测试侧梯度串法）均可由
既有文档与 house style 唯一裁决，已在 §4 记录。若 reviewer 对 §4.2 的
"phase senders 进 production"有异议，必须先改本文，不得在代码里改道。

## 17. Review 对账（2026-06-12，M08 实现 land 记录）

实现提交：`98b3ad4`(A 状态机) → `d21b18d`(B phase senders，含一次 amend：
`loop(n)` 补 `just() >>` 前缀，bind_back-needs-prev 规则) → `d8fd920`(C
fixture + 测试 1-3) → `b2a8858`(D 测试 4-6) → `ca57ef7`(E 测试 7-11) →
`71d72a7`(F 回归 marker)。production 变更仅
`write_path/write_batch_state.hh` + `write_path/sender.hh` 两文件；
`pump/`、`ai_context/`、既有测试零触碰。

### 17.1 语义对照结论

§12.2 的 11 个测试全部落地；唯一参数级修正是测试 9（见 §12.2.9 修正
记录，根因是本文初版参数与 pump ring 满判定语义冲突，实现方按总报告
显式声明降级、review 核实后采纳为正式参数）。逐条对照通过的要点：

1. all-WAL barrier 严格先于 memtable fan-out：`memtable_phase` 入口
   require `wal_durable`，且 fan-out 前先翻 `memtable_applying`。
2. value/WAL 失败 → release 映射（含 `prepare_queue_full`）、memtable
   后 release/早 publish 被 `std::logic_error` 锁死，coord 状态零变化。
3. 乱序 finish/release 下 durable_lsn gap-free，遮蔽规则有
   `data_ver > read_lsn` 的直接断言（不是只断"看不见"）。
4. parked state 即 `write_batch_state` 本体：存储指针 park 前后恒等，
   无第二份 batch owner 拷贝。

接受的实现偏差（均已在总报告声明）：value phase 双重载替代默认实参；
wal phase 无默认模板参数（span 推导足够）；release 终态推进用
post-release switch + checked helper（§6.4 已预留"等价 checked 写法"）。

### 17.2 运行效率审计（独立小节）

- **热路径增量对照 §11 预算逐项核销**：state 构造 = 1 次 `batch_ctx`
  move + 1 字节 phase；五个 phase senders 零新增 heap allocation、零新增
  owning copy、零新增 queue hop（跳转序列与 WP §2.1 拓扑逐点一致）。
  fan-out lambda 捕获清单核查：`&state` + span 按值（ptr+size）+ 裸
  指针，无 owning capture（CQS §3.4 过）。
- **异常聚合成本**：成功路径传递的是空 `exception_ptr`（无 control
  block，零 refcount 流量）；失败路径 move 传递，无额外 RMW。fold 形态
  （by-ref + void 返回）与 `pump/apps/test/concurrent_copy_test.cc`
  Test 4 验证的框架契约一致。
- **并发参数数字依据**：batch 级 fan-out `concurrent(fragments.size())`
  ≤ front_count（每 front 至多 1 fragment，无放大）；页级 FUA 仍由
  `max_fua_inflight = 16`（M06/045）约束；per-front WAL phase 并发由
  gate（1 pending plan + queue_depth 个排队 prepare）约束——三层 budget
  不混，§7 表格成立。
- **隐藏成本点名**：M08 自身未新增任何 `push_context` 节点（value/WAL
  内层的 context 节点属 M06/M07 已核销预算）；测试梯度的"每 phase 一次
  submit"（每次一个 root context/scope 分配）是测试驱动方式成本，M09
  单链组装后整 batch 一次 submit。冷路径上每次 phase 调用有 O(F +
  fragments) 的拓扑校验比较与误用时的 error string 构造，均不在热路径
  预算内。
- **容量**：无新增常驻 carrier；in-flight 内存 = ready window ×
  batch_ctx（M01 已计），M08 增量仅 phase 字节。10 亿 KV 基线无影响。

### 17.3 独立验门记录（不采信实现方自报）

`cmake --build build` 全 target 0 错；Release `inconel_test_m01..m08`
8/8 PASS；`cmake --build build_asan` 全 target 0 错；ASAN 同名 8/8
PASS、无 leak/UAF 输出；`inconel_test_flush_e2e` 两套构建编译通过；
残留扫描（production 无 TODO/stub/step 字样标识符、变更范围仅 4 个
预期文件）通过。m08 测试 Release 运行 78ms。

### 17.4 遗留 watch-item

1. front ctor 以单一 `queue_depth` 同时决定请求 ring 容量与 WAL
   prepare FIFO 软容量（§12.2.9 修正记录）；是否解耦留 M09/M11。
   （2026-06-12 关闭：M11/050 §5.2 以
   `wal_append_config.pending_prepare_capacity` 解耦，0 = 跟随
   queue_depth 保持既有形态，production builder 默认 64。）
2. M09 异常分类规则固定见 §15.1；`std::logic_error` 不得映射成 release。
3. 不新增 known_issues 条目：本步无 production 缺陷级发现。
