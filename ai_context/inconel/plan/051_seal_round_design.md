# 051 - M12 Seal / CAT1 / Front Generation Boundary

> 本文对应 `ai_context/inconel/plan/front_wal_development_plan.md` 的 M12
> （旧 step 26：Seal / CAT1 / batch 不跨代）。
> 目标：交付**单链 production `seal_round` sender**——
> `coord(close_gate) → fan-out front(seal_active) → reduce →
> coord(install CAT1) → coord(open_gate)`（OV §9.2 规范步骤），并把
> OV §7.1 补充不变量 4（**同一 batch 不得跨 seal 边界裂成两代
> memtable**）的保证机制在当前 M09 写链形态下真正落地：写路径的
> memtable fan-out 派发收进 coord 串行点。M12 同时建立 front sealed
> gen → tree-local flush 的 bridge（collect/release 面已由 M05 交付，
> 本步给桥接证据），不做 flush/frontier switch/recovery。

## 1. 范围

M12 覆盖：

- L2 `coord/scheduler.hh` + `coord/sender.hh` 增量（四个 production
  owner senders，全部包装既有内部操作）：
  - `close_gate()`：关 gate；已关闭时 `std::logic_error`（编排误用
    fail-fast）。完成值 = 关闭瞬间的 current CAT
    （`shared_ptr<const publish_catalog>`，旧分支 seal_once 同款——
    CAT0 引用与 D0 读取都锚定在 gate 关闭之后）。
  - `install_cat(std::shared_ptr<const core::publish_catalog>)`：
    校验后安装（复用 `validate_replacement_cat` + `cat_epoch_` 推进 +
    `cats_.install_cat`，即 `install_cat_for_testing` 的 owner-queue
    形态）。
  - `open_gate()`：开 gate + 把 seal 期间累积的 `pending_prefix`
    应用到 current CAT（= CAT1）+ `drain_pending_assigns()`（即
    `open_gate_for_testing` 的 owner-queue 形态）。已开启时
    `std::logic_error`。
  - `enter_memtable_phase(batch_lsn)`：**纯串行点 handle**（§4.1
    裁决）——不改任何 owner 状态，cb 即完成；其存在意义是让写链的
    memtable fan-out 派发发生在 coord 单线程的 continuation 上，与
    seal 的 `seal_active` fan-out 派发在 coord 队列上全序。
- L3 `write_path/write_batch.hh` 增量：顶层链在 all-WAL barrier 与
  memtable phase 之间插入 `coord::enter_memtable_phase` hop（§5.2）。
  **五个 phase senders 签名与语义零变更**（m08/m10/m11 既有测试的
  手工梯度驱动不受影响）。
- L3 `pipeline/seal_round.hh`（新文件）：`seal_round_result { cat1 }`
  + 顶层 `seal_round(coord&, fronts span)` sender（§6）。
- L3 `runtime/operations.hh` 增量：`rt::seal_once()`（registry 解析
  coord + fronts，委托 seal_round；与 M11 两个 op 同面同纪律）。
- 测试 `inconel_test_m12_seal_round`（§13）。

M12 不覆盖：

- seal **自动阈值触发**（RSM §2.6/§2.7 的内联监控 + atomic 计数器面；
  §4.2 裁决：显式延期）。
- flush / frontier switch / CAT2 / `release_gens` 的编排消费
  （front `release_gens` handle 本身 M05 已交付，本步只测 pin 语义）。
- recovery（dev plan M12 必须重构 4：只建 bridge）。
- MultiGet/Scan、INC-021/INC-055（维持既有裁决）。
- mock device / 多核 / 真盘（M13）。
- coord `seal_in_progress` 自动调度标志（auto-trigger 配套，随 §4.2
  一起延期；显式 seal 的重入保护由 close_gate 的 fail-fast 承担）。

## 2. 已对照输入

正式设计：

- `design_doc/design_overview.md` §9.1（CAT0→CAT1 三阶段交接）、
  §9.2（seal 规范步骤 + 关键点 4/5）、§7.1（补充不变量：batch 不
  跨代 + 其依赖的两层顺序）、§7.4（publish 唯一要求 / gate 等待）、
  §14.5（seal pipeline 形状）
- `design_doc/write_path_and_pipeline.md` §9（batch 不跨 seal：
  §9.2 两层保证、§9.3 seal 发起约束、§9.4 时序、§9.5 gate 配合）、
  §2.3（冻结约束 3："coord_sched 不得让 seal_active 插在同一 batch
  的两轮 fan-out 之间"）
- `design_doc/runtime_state_machine.md` §2.1/§2.3（coord handles：
  close_gate/open_gate/install_cat 在请求表中）、§2.5（publish_gate
  非 mutex 语义）、§2.6/§2.7（seal 触发与反压——本步延期范围）、
  §3.6（handle_seal_active，M05 已落）、§9.2（seal 完整时序）
- `design_doc/cross_doc_contracts.md` §1（`seal_active` 签名）、
  §5 Seal 跳转路径、§2（publish_catalog/PRS 字段）
- `design_doc/code_modules.md`（pipeline/ Seal 行；coord 职责表含
  close_gate/open_gate/install_cat）
- `design_doc/flush_and_frontier_switch.md` §2（collect eligibility，
  bridge 测试依据）
- `design_doc/code_quality_standard.md`

当前分支代码：

- `apps/inconel/coord/scheduler.hh`（`publish_gate`
  {close/open_and_take_pending/note_pending}、
  `apply_pending_gate_prefix`、`validate_replacement_cat`、
  `drain_pending_assigns`、`cats_`、gate/install 仅有 `_for_testing`
  形态——production senders 是本步增量）
- `apps/inconel/front/scheduler.hh`（`seal_active()` production
  sender + `handle_seal` / `seal_active_now`、
  `collect_eligible_gens` / `release_gens`，M05 交付；
  `imms_for_testing` / `active_for_testing` 观察口）
- `apps/inconel/write_path/write_batch.hh`（M09 单链——本步插 hop 的
  对象）+ `write_path/sender.hh`（phase senders，不动）
- `apps/inconel/pipeline/point_get.hh`（M10：seal 后读经 PRS imms 的
  验证工具）
- `apps/inconel/runtime/{builder,operations}.hh`（M11：
  build_front_topology fixture 路径 + rt:: 面）
- `apps/inconel/tree/scheduler.hh` + `tree/memtable_fold.hh`
  （`submit_flush_fold` —— bridge 测试的 fold seam）

plan 文档：

- `041`（coord 内部件语义）、`043` §8（seal_active 轮换 + gen_id
  stride）、`047` §6.3（memtable_applying 翻线在 fan-out 前）、
  `048` §6（write_batch 链形态）、`049` §6（point_get）、`050`
  §7（build_front_topology fixture 路径）/§13.4（in-gate 边界先例）

旧分支证据（语义参考，不迁移代码；设计者角色已声明读测试）：

- `inconel:ai_context/inconel/plan/steps/step_26_design.md`
  （必守不变量 1-11、CAT1 构造规则、"受控队列顺序"测试边界）
- `inconel:apps/inconel/runtime/operations/seal_once.hh`
  （close_gate 完成值携带 old CAT、seal_state 按 owner_id 定位、
  build_cat1 复用 tree_guard/继承 D0/epoch+1、install→open 顺序）
- `inconel:apps/inconel/runtime/operations/write_batch.hh`
  （Phase C memtable fan-out 同样由 pipeline continuation 派发——
  旧分支与当前 M09 同形态，见 §4.1 关键发现）
- `inconel:apps/inconel/test/step_26_*`（CAT0/CAT1 分离、gate
  暂停/恢复、受控顺序下不跨代）

登记问题：INC-054 / INC-056 / INC-021 / INC-055 全部正交不动。

## 3. 语义来源对照表

| 项目 | 旧 `inconel` Step 26 证据 | 当前 `inconel.new` 现状 | 正式设计依据 | 051 决议 |
|---|---|---|---|---|
| seal 编排形状 | `seal_once()`：close_gate → with_context(seal_state) → per-front seal_active（按 idx 存 front_read_set）→ install CAT1 → open_gate | coord 仅有 `_for_testing` gate/install；front `seal_active` sender 已交付 | OV §9.2 / §14.5；cross_doc §5 Seal | 保留旧形状：`pipeline/seal_round.hh` 单链；CAT1 构造在 L3（build 函数），coord `install_cat` 只做校验安装。结果按 owner_id 定位（旧不变量 5） |
| close_gate 完成值 | sender 完成值 = 关闭瞬间 current CAT（CAT0） | 无 production close_gate | OV §9.2 步骤 5（CAT1.durable = D0 旧值继承）：D0 必须在 gate 关闭后读取才无撕裂 | 保留：`close_gate()` → `shared_ptr<const publish_catalog>`（CAT0）。L3 从它读 D0/epoch/tree_guard |
| CAT1 构造规则 | 复用 `CAT0.prs->tree_guard`、`durable = D0`、`epoch = CAT0.epoch + 1`、fronts = seal 返回集按 owner 序 | `publish_catalog` ctor 要求 epoch == prs->epoch（M02） | OV §9.1/§9.2；旧 step 26 不变量 3/4/5 | 全部保留；epoch 由 L3 算 old+1，coord `install_cat` 经 `validate_replacement_cat` 校验（含 epoch 单调）后推进 `cat_epoch_` |
| **batch 不跨代的机制** | 旧分支 write_batch 的 memtable fan-out 同样由 pipeline continuation 派发（**不经 coord 队列**）；step 26 测试自限"受控队列顺序下"验证——通用交错窗口在旧分支未机制化关闭 | M09 同形态：all-WAL reduce 在最后完成的 front 核上直接派发 memtable fan-out | **OV §7.1 补充不变量 4 + 依赖声明（coord 单线程顺序 + front 队列顺序）；WP §2.3 冻结约束 3、§9.2 层 1、§9.3** ——规范机制要求两类 fan-out 的**派发**在 coord 上全序 | **不迁移旧分支的"受控顺序"形态**：M12 落规范机制（§4.1）——写链在 memtable phase 前经 `coord::enter_memtable_phase` 串行点，fan-out 派发发生在 coord continuation；seal 的 fan-out 派发同样在 close_gate continuation（coord 线程）。两者在 coord 队列上全序 + 各 front FIFO ⇒ 不变量在任意交错下成立 |
| seal 触发 | step 26 明确不做 trigger/阈值（§2.2） | front 无 memory atomic 计数器；coord 无监控内联 | RSM §2.6"seal 触发是运行时调优，不是正确性约束" | 同旧分支：**显式 `rt::seal_once()` 是 M12 面**；自动阈值触发延期（§4.2），不留 `seal_in_progress` 死字段 |
| runtime 入口 | `seal_round_pipeline(inconel_runtime&)` + `seal_once()` free function | M11 `rt::` 面 + registry | 050 §9 纪律 | 底层 `pipeline::seal_round(coord&, fronts)` 显式参数 + `rt::seal_once()` registry 包装（M11 同款两层） |
| 落点 | `runtime/seal_round_pipeline.hh` | `pipeline/` 留给读/seal/flush 编排（047 §3 裁决链） | code_modules pipeline/ Seal 行 | `apps/inconel/pipeline/seal_round.hh`；op 进 `runtime/operations.hh` |

## 4. 冲突与裁决

### 4.1 batch 不跨代的保证机制（本步最大裁决点）

**问题**：OV §7.1 不变量 4 的成立依赖"coord 对 write-batch fan-out
与 seal-round 发起的单线程顺序 + 各 front 队列顺序"（§7.1 依赖声明、
WP §9.2 层 1）。但 M09 落地的 write_batch 中，memtable fan-out 的
**派发点**在 all-WAL reduce 完成的 continuation 上（最后完成 WAL 的
front 核），不在 coord；seal 的 `seal_active` fan-out 派发在
close_gate continuation（coord 核）。两个派发点无全序 ⇒ 存在交错：
front F1 队列先收 batch X 的 memtable fragment 再收 seal_active，
front F2 反序 ⇒ X 在 F1 进旧 A*、在 F2 进新 N* ⇒ **split batch**，
直接违反系统语义。旧分支同形态、未机制化解决（其 step 26 测试自限
"受控队列顺序"）；M12 引入 seal 的同时必须把规范机制落地。

**裁决**：写链 memtable phase 的派发收进 coord 串行点。

1. coord 新增 `enter_memtable_phase(batch_lsn)` handle：**无状态
   变更**，cb 即完成。它的全部价值在执行域：该 sender 的
   continuation（即顶层链中紧随其后的 `write_batch_memtable_phase`
   的 fan-out 投递）在 coord 的 advance 回调里同步执行——PUMP 语义
   下，cb → push_value → 后续 op 同步推进至下一异步边界，因此该
   batch 的**全部** memtable fragment 入队动作完成于 coord 处理本
   请求的临界区内。
2. seal 侧对称：`close_gate()` 的 continuation（seal_active fan-out
   的全部入队）同样完成于 coord 处理 close_gate 的临界区内（旧分支
   seal_once 已是此形态，本步保留）。
3. 于是"X 的 memtable 派发"与"seal 派发"在 coord 队列上全序；每个
   front 的 per_core FIFO 保序消费 ⇒ X 的所有 memtable insert 要么
   全部先于 seal_active（整体进 A*），要么全部后于（整体进 N*）。
   任意核间交错下成立，不再依赖测试受控顺序。
4. **WAL 早于 seal、memtable 晚于 seal 的 batch 合法进 N\***：gen
   成员资格只由 memtable insert 决定（WAL stream 连续、不绑代；
   OV §5.1/§9.3 对 gen 的约束都在 memtable 维度）。该 batch 的
   publish 落在 CAT1（gate pending 机制），可见性一致。
5. 实现落点：`write_path/write_batch.hh` 顶层链
   `write_batch_wal_phase >> flat_map(enter_memtable_phase) >>
   write_batch_memtable_phase`；**phase senders 签名零变更**——
   m08/m10/m11 fixture 的手工逐相驱动（无并发 seal）继续成立，
   m09 测试 1 的成功路径语义等价保持（多一跳对断言不可见）。
6. 规范文本同步：WP §2.1 的伪码链没有画出这一跳（§2.1 sketch 与
   §7.1/§9.2 的机制声明存在内部分歧，规范侧以语义章节为准）；
   wrap-up 在 WP §2.3 冻结约束处补一行机制落点注记，并在
   cross_doc §1 增 `enter_memtable_phase` 行（见 §15.3）。

**否决的替代**：(a) front 侧按 epoch/gen 标签拒插重路由——sealed gen
不可写、单方改道破坏跨 front 一致、拒绝即 post-WAL fatal，全不可行；
(b) seal 等待全部 in-flight batch 排空（coord 计数 barrier）——把
seal 延迟挂在最慢 batch 上且仍需 enter 类 hop 计数，复杂度更高收益
更低；(c) 维持现状 + 文档声明"单线程驱动下成立"——违反 v1 语义
（多核是 M13 即至的部署形态，不变量是系统语义不是测试假设）。

### 4.2 seal 自动阈值触发：显式延期

RSM §2.6 把触发条件定义为全局阈值（active 内存和 / WAL 使用率 /
总 memtable 上限），并明确"seal 触发是运行时调优，不是正确性约束"；
其实现前提（front `active_memory_usage` atomic、wal_space
`used_segment_count` atomic、coord assign 内联采样）是一组尚不存在
的仪表面。旧 step 26 同样不做 trigger。裁决：M12 交付**显式
`rt::seal_once()`**（M13 e2e 与未来调度器的原语）；自动触发与其
仪表面、`seal_in_progress` 调度标志一起延期到运行时调优步（需要
部署 workload 数据给阈值依据——与 INC-055 tier 同一"无实测不定参"
纪律）。重入安全不依赖延期项：并发 seal_once 的第二个 `close_gate`
在已关 gate 上 `std::logic_error` fail-fast。

### 4.3 collect bridge 的 in-gate 证据形态

dev plan M12 完成测试 3 要求"collect eligible gens 可喂当前
tree-local flush"。完整 `tree_local_flush` 含 NVMe 写（经
`rt::local_nvme()` 真实类型，fake 不可注册——M10 §4.6 同一事实）。
裁决：bridge 测试主形态 = **直驱 fold seam**：
`collect_eligible_gens` 产物组装 `tree_flush_request { base_guard =
CAT0 tree_guard, sealed_gens, recovery_safe_lsn = 0 }` →
`tree_sched::submit_flush_fold`（CPU-only，tree_sched 以 heap DMA
allocator 构造、shard map 装占位）→ 断言 fold 接受并产出覆盖写入
key 的 partitions。这是"gens 可被 flush 消费"的最强无盘证据；
fold 之后的 worker/owner/写盘段不驱动（flush_e2e/M13 范畴）。若
fold seam 在 fixture 内不可达（runtime 依赖超出 §13.1 所列），降级
为 eligibility/shape 断言并在总报告显式声明——不得静默。

### 4.4 `install_cat` 的 epoch 纪律

`publish_catalog` ctor 冻结 `epoch == prs->epoch`（M02），故 epoch
必须在 L3 构造时确定（old+1，旧 step 26 不变量 3）；coord
`handle_install_cat` 复用 `validate_replacement_cat`（含非空/epoch
合法性）并把 `cat_epoch_` 推进到新值——与 `install_cat_for_testing`
同一内部路径，owner sender 只是把它放上 coord 队列。RSM §2.3 的
"install 内部 ++cat_epoch_" 表述按当前实现形态（校验外部 epoch 并
采纳）解读，不改既有 ctor 契约。

## 5. L2/L3 增量

### 5.1 coord 新 senders（coord/scheduler.hh + coord/sender.hh）

| Sender | 输入 | 完成值 | handle 语义 |
|---|---|---|---|
| `close_gate()` | — | `shared_ptr<const publish_catalog>`（关闭瞬间 current CAT） | gate 已关 → `std::logic_error`（cb fail 路径）；否则 `gate_.close()` 后回传 `cats_.current_cat()` |
| `install_cat(cat)` | `shared_ptr<const publish_catalog>` | void | `validate_replacement_cat(cat)`；`cat_epoch_ = cat->epoch`；`cats_.install_cat` |
| `open_gate()` | — | void | gate 已开 → `std::logic_error`；否则 `apply_pending_gate_prefix(gate_.open_and_take_pending())` + `drain_pending_assigns()` |
| `enter_memtable_phase(batch_lsn)` | `uint64_t` | void | 纯串行点：不读不写 owner 状态，cb 即完成（lsn 仅入诊断文案）。注释必须写明它的价值在执行域（§4.1.1），防止后人当死码删除 |

四个 handle 走既有 coord 队列（复用某条既有队列或新增一条，按
house 模式 bounded drain；实现自选并在报告声明）。误用 fail-fast
均经 cb fail 路径抛 `std::logic_error`（M03 house style）。

### 5.2 write_batch 顶层链增量（write_path/write_batch.hh）

```text
…write_batch_wal_phase(state, fronts, wal_space, nvme_by_owner)
 >> flat_map([&coord_sched, &state](bool) {
        // §4.1：memtable fan-out 派发必须经 coord 串行点，与 seal
        // 的 fan-out 派发在 coord 队列上全序（OV §7.1 不变量 4）。
        return coord::enter_memtable_phase(coord_sched,
                                           state.ctx.batch_lsn);
    })
 >> flat_map([&state, fronts]() {
        return write_batch_memtable_phase(state, fronts);
    })
 >> …（publish / ack / any_exception 原样）
```

约束：

1. hop 位于 all-WAL barrier 之后、`memtable_applying` 翻线之前——
   `enter_memtable_phase` 区间的失败（理论上仅进程级）不改变
   M09 §10 失败矩阵：phase 仍停 `wal_durable`，release 合法。
   `is_releasable_write_failure` 对 `logic_error` 恒 false → fatal，
   与既有纪律一致。
2. phase senders（`write_batch_value_phase` /
   `write_batch_wal_phase` / `write_batch_memtable_phase` /
   `write_batch_publish` / `write_batch_release`）签名语义零变更。
3. `enter` 的 continuation 在 coord 核上执行 memtable fan-out 投递
   后，各 front insert 的 cb 仍在各 front 核——`state` 的写仍是
   request-private 串行点序（M08 §9 论证延伸：相邻写读的
   happens-before 由 per_core queue 边提供，新增的 coord 边同型）。

### 5.3 `pipeline/seal_round.hh`

```cpp
struct seal_round_result {
    std::shared_ptr<const core::publish_catalog> cat1;
};

[[nodiscard]] inline auto
seal_round(coord::coord_sched& coord_sched,
           std::span<front::front_sched* const> fronts);
```

链结构（语义展开；与旧 seal_once 同形，按当前 house 风格落）：

```text
seal_round(...) =
  coord::close_gate(coord_sched)                  // 完成值 = CAT0
  >> then(构造 seal_state{ old_cat, results(fronts.size()) })
  >> push_result_to_context()
  >> get_context<seal_state>()
  >> flat_map([fronts](seal_state& st) {
         // 拓扑校验：fronts 非空且 size == old_cat->prs->fronts->size()
         //（违例 invalid_argument——接线错）
         return just()
             >> loop(fronts.size())
             >> concurrent(fronts.size())
             >> flat_map([&st, fronts](size_t i) {
                    return front::seal_active(*fronts[i])
                        >> then([&st, i](core::front_read_set&& frs) {
                               st.results[i] = std::move(frs);  // owner_id 定位
                           });
                })
             >> reduce()                            // all-front barrier
             >> flat_map([&st, &coord](bool) {
                    st.cat1 = build_cat1(st.old_cat, std::move(st.results));
                    // 复用 tree_guard、durable = D0（从 old_cat 在
                    // gate 关闭后读取）、epoch = old + 1、fronts 按
                    // owner_id 序
                    return coord::install_cat(coord, st.cat1);
                })
             >> flat_map([&coord]() { return coord::open_gate(coord); })
             >> then([&st]() { return seal_round_result{ std::move(st.cat1) }; });
     })
  >> pop_context();
```

要点：

1. owner 边界一眼可见：`coord → front×N → coord → coord`，与
   cross_doc §5 Seal 路径逐点一致。
2. seal_active fan-out 的入队动作全部发生在 close_gate continuation
   （coord 核）——§4.1.2 的机制半边。
3. `loop(n)` 带 `just() >>` 前缀；results 按 index 写入互不重叠
   （per-branch 独立槽位，无共享聚合竞争——CODING_GUIDE §2.2 形态）。
4. 异常路径：seal_active / install / open 的异常向调用方传播；gate
   可能停留关闭态（seal 编排失败 = 运行时干预事件，不做自动回滚——
   旧 step 26 同口径；注释写明）。
5. fronts span 与 PRS 尺寸一致性校验在链内第一步。

### 5.4 `rt::seal_once()`（runtime/operations.hh）

```cpp
[[nodiscard]] inline auto
seal_once() {
    return pipeline::seal_round(
        *core::registry::coord_sched_singleton(),
        core::registry::fronts_span());
}
```

compose 期解析、零运行期增量（M11 §9 同纪律）。

## 6. 错误 / 失败语义总表

| 场景 | 抛出点 | 类型 | 备注 |
|---|---|---|---|
| close_gate on closed / open_gate on open | coord handle | `std::logic_error` | 并发 seal_once 的第二个、编排 bug；gate 状态不变 |
| install_cat 校验失败（null/epoch 倒退/PRS 形态） | coord handle | `std::invalid_argument` / `logic_error`（按 validate_replacement_cat 既有分类） | 不安装 |
| seal_round 拓扑不符（fronts 空 / 与 PRS 尺寸不一致） | 链内首检 | `std::invalid_argument` | 接线错 |
| seal_active 异常 | front handle | 原样传播 | gate 停留关闭：运行时干预事件（§5.3.4） |
| enter_memtable_phase 异常（队列满等） | coord | 既有 fail-fast | 写链视角：non-releasable → fatal（M09 分类不变） |
| 写链其余 | — | 与 048 §10 总表逐条一致 | hop 不引入新失败形态 |

## 7. Lifetime 契约

| 对象 | Owner | 必须活到 | 保障方式 |
|---|---|---|---|
| seal_state（old_cat、results、cat1） | pipeline context | pop_context | 框架管理；`&st` 借用经 context 节点地址稳定（M09/M10 同款） |
| CAT0 | shared_ptr（close_gate 完成值持有） | seal_state 析构后由旧 reader 决定 | 旧 reader 继续 pin（OV §9.2 关键点 1） |
| CAT1 | shared_ptr：coord `cats_` + seal_round_result | 下一次 install 替换后由 reader 决定 | M02 语义 |
| 新旧 memtable gens | PRS0/PRS1 各自 shared_ptr | gen 生命周期规则 | RSM §8.4；release_gens 只动 front 本地 imms |
| fronts span / coord 引用 | 调用方（registry / fixture） | sender 终结 | M09/M10/M11 同款 |

## 8. 内存序与并发安全

零新增 atomic。新增的并发论证只有一条（§4.1.3）：batch 不跨代的
全序由「coord 单线程处理 `enter_memtable_phase` 与 `close_gate` 的
先后 + 两者 continuation 内完成全部 fan-out 入队 + per_core FIFO
消费」构成；PUMP cb→push_value 的同步推进保证 continuation 不被
同 owner 的下一请求打断。gate 与 pending_prefix 全部 coord 私有
（RSM §2.5）。seal_state.results 按 index 独立写、reduce 后单线程
消费。

## 9. 热路径预算与容量估算

| 路径 | M12 新增成本 | 说明 |
|---|---|---|
| **每写 batch** | **+1 coord queue hop**（enter_memtable_phase：1 次 per_core 入队 + 1 次 drain 回调 + 1 个 req new/delete） | §4.1 机制的最小成本。量级：hop 亚微秒级 vs batch 既有 2 轮 FUA（每页 10-30μs）——关键路径占比 < 2%；且该跳是 OV §7.1 系统语义的机制落点，非可选优化项。fan-out 入队从「最后完成 WAL 的 front 核」移到 coord 核——入队次数不变（N 个 fragment），无新增 copy/alloc |
| coord 吞吐 | enter 是 O(1) 空 handle | coord 既有每 batch 2 次 handle（assign/publish）变 3 次；assign 含 canonicalize（重活），enter/publish 皆轻——coord 序列化点余量不受实质影响 |
| seal round（冷路径） | 1 次 seal_state（results vector N 项）+ 1 个 context 节点 + N 个 front req + 2 个 coord req + CAT1/PRS1 构造（fronts vector N 项 + 3 个 shared_ptr 对象） | 与旧 step 26 质量约束一致（允许一次 PRS fronts vector；不复制 memtable 内容——`front_read_set` 只含 shared_ptr） |
| seal_active（front 侧） | M05 既有：新 gen `make_shared` + imms push_front | 本步零增量 |

容量：无新增常驻 carrier（seal 全部请求态）。10 亿 KV 校准 n/a
（每轮 seal 的临时量 O(front_count)）。

## 10-12.（并入上文：§6 错误表 / §7 lifetime / §8 内存序 / §9 预算）

## 13. 测试计划

Target：`inconel_test_m12_seal_round`
（`apps/inconel/test/test_m12_seal_round.cc`，CMake 照 m11 模式注册，
link `inconel_real_nvme`）。

### 13.1 Fixture

m11 蓝本：`build_front_topology` 经 registry 构造（production 路径）
+ fake NVMe + value sched + L3/rt 驱动 helpers + `advance_all`；
shard map 占位安装（bridge 测试 fold 需要）；bridge 测试另构造
`tree::tree_sched`（heap DMA allocator，无设备）。读回断言用
`rt::point_get` / `pipeline::point_get`（M10 面）。

### 13.2 测试列表

1. `m12_seal_round_installs_cat1_and_preserves_readers`
   写 2 个 batch（durable D0=2）→ 旧 handle 获取 → `rt::seal_once()`
   → result.cat1：epoch == old+1、durable == D0、tree_guard 与 CAT0
   **同一** shared_ptr、`prs->fronts[i].active` 为新 gen 且
   `imms[0]` 为旧 active（按 owner_id 序）；旧 handle 仍 pin CAT0；
   新 handle 拿 CAT1；**seal 后 `rt::point_get` 仍读回 pre-seal
   数据**（读经 PRS imms——M10 borrowed lookup 跨代读的首次真实
   行使）。
2. `m12_publish_parked_while_gate_closed_lands_on_cat1`
   X 驱到 `memtable_applied`（m08 梯度）→ production
   `coord::close_gate()`（拿 CAT0）→ `write_batch_publish(X)` 完成
   但 CAT0.durable 不动 → 手工 seal_active fan-out + build/install
   CAT1 → `coord::open_gate()` → CAT1.durable == X.lsn（pending
   应用到新 CAT，OV §9.2 步骤 7 / 旧不变量 6/7）；CAT0.durable 永
   冻结在 D0。
3. `m12_batch_lands_wholly_in_old_gens`（不跨代·序 a）
   双 front 跨 owner batch X 全链 submit、推进到 publish 完成 →
   `rt::seal_once()` → X 的两个 key 都在各自 front 的 imms[0]
   （旧代）中可查、新 active 无残留；read_lsn 下 point_get 读回。
4. `m12_batch_lands_wholly_in_new_gens`（不跨代·序 b，**机制
   靶心**）双 front 跨 owner batch X：hold 其首个 WAL 写 → X 停在
   WAL phase → `rt::seal_once()` 完成（seal 派发先于 X 的 enter）→
   release held → X 完成全链（publish 落 CAT1）→ X 的两个 key 都在
   **新** active 中、两个旧 imm gen 均无 X——WAL 字节早于 seal
   写下而 memtable 整体进 N*（§4.1.4 合法形态的直接断言）；
   point_get 读回。
5. `m12_concurrent_seal_once_second_fails_fast`
   gate 关闭态下直接 `coord::close_gate()` → `std::logic_error`；
   gate 状态不变、后续正常 seal 成功（检查只读不破坏）。
   `open_gate` on open 同断言。
6. `m12_collect_bridge_feeds_flush_fold`（§4.3）
   seal → publish 推进使旧 gen eligible →
   `collect_eligible_gens(durable)` 返回 sealed 旧 gens →
   组装 `tree_flush_request{CAT0.tree_guard, gens, 0}` →
   `submit_flush_fold` → fold 产出 partitions 覆盖写入 keys（数量/
   key 成员断言）。fold 不可达时按 §4.3 降级并声明。
7. `m12_release_gens_keeps_pinned_gen_alive`
   seal → 旧 handle 持 CAT0 → `front::release_gens(F ids)` →
   front `imms_for_testing` 不再含 F，但旧 handle 经 PRS0 仍可
   lookup F 数据（shared_ptr pin，dev plan 测试 4）；释放旧 handle
   后 weak_ptr 观察 gen 析构。
8. `m12_seal_round_compose_without_submit_no_side_effect`
   compose `rt::seal_once()` 后销毁 → gate 仍开、CAT 未换、零
   front 活动（house 纪律）。
9. `m12_multiple_seals_accumulate_imms`
   连续两轮 seal（夹写入）→ imms 序 [F2, F1]、epoch 递增、读回
   三代数据各自正确。

### 13.3 回归门

每 Phase：`cmake --build build` 全 target +
`inconel_test_m01..m12` 十二个全 PASS（m08/m09/m10/m11 不改而绿
——write_batch 插 hop 后的等价性证据：m09 测试 1/2 断言面不含跳数）；
收尾 `build_asan` 同名十二个全 PASS。`inconel_test_flush_e2e` 两套
构建维持编译。

### 13.4 声明的 in-gate 边界

bridge 测试止于 fold seam（写盘段需真实 NVMe，M13/flush_e2e 关闭）；
seal 与真实 flush/frontier switch 的全链交互属 M13+。总报告原样
声明。

## 14. 实现顺序（每 Phase 一个提交）

```text
Phase A  coord/scheduler.hh + coord/sender.hh：close_gate / install_cat /
         open_gate / enter_memtable_phase 四个 owner senders（§5.1）
Phase B  write_path/write_batch.hh：顶层链插 enter_memtable_phase hop（§5.2）
Phase C  pipeline/seal_round.hh + runtime/operations.hh 增 rt::seal_once（§5.3/§5.4）
Phase D  CMake 注册 m12 target + fixture + 测试 1/2/5/8（seal 基本面 + gate + 误用 + 纪律）
Phase E  测试 3/4/6/7/9（不跨代两序 + bridge + pin + 多轮）
Phase F  全量回归（Release + ASAN，m01-m12）+ 总报告（声明跳过项与 §13.4 边界）
```

依赖：B 依赖 A；C 依赖 A；D 依赖 B/C；E 依赖 D。**Phase A-C 是
production 实现阶段，禁止打开任何测试文件**；Phase D/E 以 M12 测试
作者身份工作，允许读的既有测试白名单：`test/check.hh`、
`test_m11_runtime_topology_operations.cc`（fixture 蓝本）、
`test_m10_point_get_live_read.cc`（读回断言面）、
`test_m09_production_write_batch.cc`（hold/全链驱动）、
`test_m08_write_baseline_inflight.cc`（phase 梯度）、
`test_m03_coord_scheduler_assign_publish_release.cc`（expect_throws）；
不得修改任何既有测试文件。

## 15. 相邻事项

1. **M13**：mock device 分层 + 多核矩阵（seal 与写并发的真并发
   验证）+ `rt::write_batch` 带 I/O e2e + seal→collect→真实
   tree-local flush 全桥（dev plan 测试 13）。
2. **flush/frontier switch 编排**（M-line 之外或 M13 后）：消费
   `collect_eligible_gens` → `tree_local_flush` →
   `coord::frontier_switch`（CAT2）→ `release_gens`；coord 的
   frontier_switch/capture_flush_frontier handles 届时按 RSM §2.3
   增补。
3. **plan 回填**：M12 节标注"M12 的详细设计文档是
   051_seal_round_design.md"。
4. **cross_doc 增补（wrap-up）**：§1 增 `enter_memtable_phase`
   （`(batch_lsn)` → void，serialize memtable dispatch vs seal
   dispatch，出现点 WP §2.3/OV §7.1）；§5 写路径跳转补该 hop。
5. **WP 注记（wrap-up）**：§2.3 冻结约束处补"memtable fan-out 的
   投递经 coord 串行点（M12/051 机制落点）"一行，消除 §2.1 sketch
   与 §7.1/§9.2 的表述分歧。
6. **known_issues**：无新增预期；auto-trigger 延期不登记（属未
   实现功能的开发顺序，不是缺陷）。

## 16. 需要人工判断的点

无阻塞项。batch 不跨代机制（§4.1 enter hop）、auto-trigger 延期
（§4.2）、bridge 测试形态（§4.3）、epoch 纪律（§4.4）均有唯一依据。
两条硬停线：（a）若 PUMP 的 cb→continuation 同步推进语义不成立
（即 enter/close_gate 的 continuation 可能被同 owner 下一请求打断，
§4.1.1/§8 的临界区论证失效），停下报告——这是机制根基，不得用
"概率上没问题"继续；（b）若 m09 既有测试因顶层链插 hop 而需要修改
才能绿，停下报告（等价性破裂 = 设计判断错误，不得改测试迁就）。

## 17. Review 对账（2026-06-12，M12 实现 land 记录）

实现提交：`31b6462`(A coord 四 senders + 事件队列重构) → `5d2d844`(B
write_batch 插 enter hop) → `f95725e`(C seal_round + rt::seal_once，
一次 rebase 替换 51cbf77：reduce 完成值 bool 进 flat_map 参数的编译
修正) → `1c5518d`(D fixture + 测试 1/2/5/8) → `f875c22`(E 测试
3/4/6/7/9，一次 rebase 替换 d37470e：测试 4 形态改写，见 §17.1.4) →
`bec624a`(F 空回归 marker，随 E 重放)。production 变更 5 文件 +
CMake + 新测试；`pump/`、`ai_context/`、tree/value/front 模块、既有
m01-m11 测试零触碰。§16 两条硬停线均未触发（cb→continuation 同步
推进语义成立；m01-m11 不改而绿）。

### 17.1 语义对照结论

§13.2 的 9 个测试全部落地（实现方总报告声明并经 review 独立核对）。
要点与接受的实现形态：

1. **Phase A 超额重构（接受）**：把既有 publish/release/read 三条
   per_core 队列与新四类合并为单条 `event_q_`（variant 七型 +
   `kMaxEventPerAdvance = 512` bounded drain）。收益：(a) 净省
   per_core 预分配（3+4 条 → 1 条，每条 ~1MB 量级）；(b) 给
   enter 与 close_gate **到达序全序**——比 §4.1 所需的"任意全序 +
   continuation 原子性"更强且对 publish 乱序观察更直观；(c)
   variant 槽 16B + 一次 visit 间接分支，对热路径可忽略。行为等价
   性由 m03 不改而绿证实；总报告已声明。
2. coord handle 细节比设计稿强：`close_gate` 先关再读 CAT0（D0
   冻结序正确）；`install_cat` epoch 单调校验（§4.4）；
   `enter_memtable_phase` 的"价值在执行域"注释原样落下。
3. seal_round 拓扑校验加了 `fronts[i]->owner_id() == i` 的 owner 序
   断言（registry 序与 owner 序的一致性钉在 seal 边界）；D0 在
   build_cat1 经 acquire 读取——gate 全程关闭使其与"关闭瞬间读"
   等值。
4. **测试 4 形态偏差（review 评估后接受）**：设计稿写"release 后
   X 完成全链"；落地形态为 m08 式手工分相（assign → value →
   hold 首 WAL → seal 完成 → release → 显式 `enter_memtable_phase`
   → memtable → publish）。评估：单线程 advance harness 下两种形态
   的证据价值等同（全链形态里 enter 与 seal 在时间上本就不构成真
   竞争——真并发竞争只能 M13 多核行使，§13.4 已声明该边界）；手工
   形态额外给了新 coord sender 的 cb 路径直接覆盖。断言面不变：
   两 key 整体落新 active、旧 imm 零残留、publish 落 CAT1、
   point_get 读回。机制的生产链接线由 Phase B diff + m09-m11 全绿
   背书。总报告如实描述了该形态。
5. **测试 7 补充形态（接受）**：为观察 weak_ptr 析构，额外安装一个
   不引用目标 gen 的 CAT2 以移除 coord current-CAT 对 CAT1 的
   pin——这是 §7 生命周期表的正确推论（CAT1 由 coord 持有到下一次
   install），补充不削弱 release_gens/PRS0 pin 的原断言，反把 pin
   链验证推到 weak 过期的全生命周期。
6. fold bridge（测试 6）走 §4.3 主形态（直驱 `submit_flush_fold`，
   partitions 覆盖写入 key + winner data_ver/kind 断言），未降级。
7. **执行纪律偏差（自报，记录）**：Phase C production 阶段一次搜索
   命令范围误含 `pump`，输出出现 PUMP 测试路径（路径级、非
   inconel 测试内容）；实现方声明未依赖该输出、未修改任何测试。
   review 评估：对"测试反推 spec"失败模式无可信污染路径（seal_round
   结构逐行对应 051 §5.3），接受并留档。

### 17.2 运行效率审计（独立小节）

- **每写 batch 新增 = 恰 1 次 coord 事件**（enter：1 个 req
  new/delete + variant 槽入队/出队 + 空 handle cb），对照 §9 预算
  逐项核销：无新增 alloc/copy 于 payload 路径，fan-out 入队次数
  不变（N 个 fragment，从 front 核移到 coord 核执行）。hop 量级
  亚微秒 vs batch 既有 2 轮 FUA（每页 10-30μs）——<2%，为 OV §7.1
  系统语义的机制最小成本。
- **事件队列合并的热路径影响**：publish/release 从专属 8B 槽队列
  变 16B variant 槽 + visit 一次间接分支；换得 3+4→1 的 per_core
  预分配节省与跨事件到达序。净评估为改善。
- **seal 冷路径**：seal_state（results N 槽）+ 1 context 节点 +
  N front req + 3 coord req + CAT1/PRS1 构造（fronts vector +
  3 个 shared_ptr 对象）——与 §9 表一致，无 memtable 内容复制
  （front_read_set 只含 shared_ptr）。
- **捕获清单**：seal_round 全部 lambda 捕获 = span 按值 + `&st`
  （context 节点稳定）+ coord 引用，零 owning capture；per-index
  槽位写入无共享聚合（INC-041 规避形态）。
- **隐藏成本点名**：无新增 push_context 循环（仅 seal_state 一处
  push_result）；enter 的 lsn 仅作诊断字段。

### 17.3 独立验门记录（不采信实现方自报）

最终树（bec624a）上重跑：`cmake --build build` 全 target 0 错；
Release `inconel_test_m01..m12` 12/12 PASS；`build_asan` 全 target
0 错；ASAN 同名 12/12 PASS、无 AddressSanitizer/LeakSanitizer
输出（E 重写前的首轮验门同样 12/12 ×2，重写后按最终树复验）；
`inconel_test_flush_e2e` 两套构建维持编译；变更范围核查（7 个预期
文件）、既有测试零 diff、production 无 TODO/stub/step-phase 残留、
无残留进程，全部通过。

### 17.4 遗留 watch-item

1. **M13 关闭项**：不跨代机制的真并发行使（多核矩阵）、
   `rt::write_batch` 带 I/O 全链、seal→collect→真实 tree-local
   flush 全桥（§13.4 边界）。
   （2026-06-12 已全部关闭：052 phase D 的 seal 交错探针（120 跨
   front batch × 3 轮真并发 seal 逐批代纪一致）+ phase E 矩阵 13
   真实 flush 全桥与 tree 读回。）
2. seal 自动阈值触发 + 仪表面（§4.2 延期，待部署 workload 数据）。
3. coord 事件队列合并后的跨事件公平性（512 统一预算 vs 旧分型
   预算）：当前无饥饿证据，留真实负载观察，不预改。
4. 不新增 known_issues 条目：本步无 production 缺陷级发现。
