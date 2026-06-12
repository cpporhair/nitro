# 048 - M09 Production Write Batch Sender Pipeline

> 本文对应 `ai_context/inconel/plan/front_wal_development_plan.md` 的 M09
> （旧 step 24A 写路径 pipeline success-path 化 + 旧 step 24B 异常语义接入
> pipeline）。
> 目标：把 M08（047）交付的五个 phase senders 组装成**单链 production
> `write_batch` sender**——一次 submit 驱动
> `coord(assign) → value → front WAL fan-out → front memtable fan-out →
> coord(publish)` 全程，异常按 047 §15.1 冻结的分类规则映射到
> `coord::release_batch` 或 fatal。M08 是语义测试梯度；M09 是 M10/M11
> 将消费的正式写入口。

## 1. 范围

M09 覆盖：

- L3 `write_path/write_batch.hh`（新文件）：
  - `write_batch_result { batch_lsn, entry_count }` ack 类型。
  - `is_releasable_write_failure(exception_ptr)` 失败分类（cold path）。
  - `fatal_write_batch_failure(state, ep)`：`[[noreturn]]`，
    `core::panic_inconsistency` 包装（WP §10.4 的运行时终止落点）。
  - 顶层 `write_batch(...)` sender（§6）。
- L3 `write_path/write_batch_state.hh` 增量：
  - `release_allowed(const write_batch_state&) noexcept -> bool`
    非抛出谓词；既有 `require_release_allowed` 改为委托它（行为不变）。
- 测试 `inconel_test_m09_production_write_batch`（§12）。

M09 不覆盖：

- runtime/registry topology、public operation surface、facade 写入口
  （M11；本步拓扑仍是显式参数）。
- point_get / 读管线（M10）、seal 编排（M12）。
- ACK 的网络/协议形态——本步 ack 就是 sender 完成值。
- memtable 失败的**注入面**：fronts 没有（也不应为测试增加）memtable
  失败注入点，fatal panic 分支不在本步测试驱动范围（§12 排除项）；
  可测试的部分通过分类纯函数单测覆盖。
- 任何 L2 scheduler 行为变更、M08 phase senders 语义变更。

## 2. 已对照输入

正式设计：

- `design_doc/design_overview.md` §7（提交语义）、§14.1（写 pipeline 形状）
- `design_doc/write_path_and_pipeline.md` §2.1（顶层链）、§10.7
  （异常处理编排：release-or-fatal 的规范伪码）、§8（多 batch in-flight）
- `design_doc/runtime_state_machine.md` §11.1（post-LSN 失败矩阵）
- `design_doc/cross_doc_contracts.md` §5 写路径跳转
- `design_doc/code_modules.md`（write_path/ = 写请求专用组合层）
- `design_doc/code_quality_standard.md`（§3.5 flat_map、§3.6 owner 可见性）

当前分支代码：

- `apps/inconel/write_path/write_batch_state.hh` / `sender.hh`
  （M08：phase 状态机 + 五个 phase senders，047 §17 已 review land）
- `apps/inconel/coord/sender.hh`（assign → `batch_ctx`、publish/release）
- `apps/inconel/value/sender.hh`（`value_persist_error`）
- `apps/inconel/front/wal_append.hh`（`wal_append_error`）
- `apps/inconel/core/panic.hh`（`panic_inconsistency`）

plan 文档：

- `047_write_baseline_inflight_design.md` §15.1（M09 接缝：异常分类规则
  冻结）、§7（三层并发 budget）、§17.4（watch-item）
- `041` §14.3（失败边界）、`044` §7.2/§10、`046` §5.2/§11.1

旧分支证据（语义参考，不迁移代码；设计者角色已声明读测试）：

- `inconel:apps/inconel/runtime/operations/write_batch.hh`
  （`write_batch_result`、`release_batch_on_failure` typed-rethrow、
  with_context state、phase 链形状）
- `inconel:apps/inconel/runtime/operations/write_batch_state.hh`
- `inconel:apps/inconel/test/step_24a_write_path_pipeline_contract_test.cc`
  （sender 无同步副作用 / 单 batch 等价 step22 / 多 batch gap-free）
- `inconel:apps/inconel/test/step_24b_write_path_failure_contract_test.cc`
  （Phase A/B 失败 release + invisible / earlier failed release unblocks
  later success——B 先 publish、durable 卡 0、A release 后跳 2 的构造）

登记问题：INC-054（urgent，正交）、INC-056（normal，正交），均不动。

## 3. 语义来源对照表

| 项目 | 旧 `inconel` Step 24A/24B 证据 | 当前 `inconel.new` 现状 | 正式设计依据 | 048 决议 |
|---|---|---|---|---|
| 顶层形态 | `write_batch_impl`：`with_context(batch_pipeline_state)` 内 assign→value→WAL→memtable→publish 全链，`global_runtime()` 取拓扑 | M08 五个 phase senders + 显式拓扑参数；无 runtime 全局对象 | WP §2.1/§14.1；047 §15.1/§15.2 | 单链组装 M08 phase senders；拓扑保持显式参数（registry 接入留 M11）；state 经 `push_result_to_context` 进 context（WP §2.1 同款） |
| ack 类型 | `write_batch_result { batch_lsn, entry_count }` | 无 | OV §7.1 step 11 ACK | 保留旧 ack 形态：`write_batch_result { batch_lsn, entry_count }`，sender 成功完成值 |
| 失败 → release | `release_batch_on_failure<Result>`：**所有**异常一律 get_context → coord release → typed rethrow；无 fatal 区分 | M08 `write_batch_release` 带 `require_release_allowed`（misuse 抛 logic_error） | RSM §11.1：memtable 失败 fatal 不允许 release；041 §9；047 §15.1 冻结分类规则 | **不迁移旧"无差别 release"**：先分类（仅 `value_persist_error` / `wal_append_error` 可 release），再查 `release_allowed(state)`；任一不满足 → fatal。typed rethrow（release 完成后重抛原始异常给客户端）保留 |
| fatal 机制 | 无（旧分支缺口） | 047 留给 M09 设计 | WP §10.4 运行时终止；panic.hh 是 house fail-fast 入口 | `fatal_write_batch_failure` → `core::panic_inconsistency`（带 phase 名 + 异常 what()）。`std::logic_error` 永不映射 release |
| 旧 raw_ops 重载 | `write_batch(vector<raw_batch_op>&&)` legacy 形态 | M01 起 ingress 冻结为 `client_batch_buffer` | 039/041 | 不迁移；单一 `client_batch_buffer&&` 入口，测试用 `encode_client_batch` |
| 无同步副作用 | step 24A test：构造 sender 不触发任何 owner 状态 | M08 phase senders 全 lazy（检查在链内 then） | 迁移计划 M09 必须重构 5 | 保留为硬约束 + 专测：组合不 submit → 零 owner 副作用（assign 不发生、NVMe 零流量） |
| 失败注入构造 | step 24B 用 mock 设备 fail 注入 + 手工分相驱动 | M08 fake NVMe：FIFO + fail_call 注入 | — | 测试 5 需要"B 先 publish、A 后失败"乱序：fake 增加 test-local `hold_call`（指定 call 暂扣、`release_held()` 时按注入失败/成功处理）；纯测试设施，不进 production |

## 4. 冲突与裁决

1. **落点**：dev plan 写 `apps/inconel/pipeline/write_batch.hh`；按 047 §3
   同一裁决链（code_modules.md 关键约束：write_path/ 是写请求专用组合层）
   落 `apps/inconel/write_path/write_batch.hh`。`pipeline/` 维持留给读/seal
   /flush 顶层编排。plan 落点行随本文回填（§15.3）。
2. **异常 handler 的挂接形态**：旧分支在链尾 `any_exception` 里
   `get_context<state>` 再 release（证明 pump 支持该形态）；048 主推
   **handler 挂在 post-LSN 子链内、直接捕获 `&state`**（state 是外层
   flat_map 的 context 引用参数，无需二次 get_context），等价的
   get_context 形态允许。理由：handler 作用域 = post-LSN 区间，pre-LSN
   失败（assign 校验错）**不**经过它而直接传播（041 §14.1：pre-LSN 失败
   无 LSN 无 release）。
3. **push 窗口的 bad_alloc**：assign 成功后、state 构造/push 完成前的
   `std::bad_alloc` 落在 handler 作用域之外——该窗口只有 move + 一次
   context 节点分配，且 bad_alloc 本就是 041 §5.2 的 process/resource
   failure（fatal 类）。调用方契约：write_batch 抛出的**非**
   `value_persist_error` / `wal_append_error` 异常一律按 fatal 类处理，
   不得重试、不得吞掉。写入 write_batch.hh 头注释。
4. **release 完成后向客户端报错的形态**：release 子链完成后 typed
   rethrow 原始异常（旧分支同款），保证"客户端收到错误"先于"槽位已
   resolved"成立顺序——release 是 coord 已接受的 terminal，rethrow 时
   slot 必已 mark。

## 5. 新增类型与 helper

### 5.1 `write_batch_result`（write_batch.hh）

```cpp
struct write_batch_result {
    uint64_t batch_lsn = 0;
    uint32_t entry_count = 0;
};
```

成功完成值；从 publish 后的 state 提取。

### 5.2 `release_allowed`（write_batch_state.hh 增量）

```cpp
[[nodiscard]] inline bool
release_allowed(const write_batch_state& state) noexcept {
    switch (state.phase) {
    case write_batch_phase::assigned:
    case write_batch_phase::value_durable:
    case write_batch_phase::wal_durable:
        return true;
    default:
        return false;
    }
}
```

`require_release_allowed` 改为 `if (!release_allowed(state)) throw ...`，
错误文案与行为不变（M08 测试 10/11 必须不改而绿，作为等价性证据）。

### 5.3 失败分类（write_batch.hh）

```cpp
[[nodiscard]] inline bool
is_releasable_write_failure(const std::exception_ptr& ep) noexcept {
    try {
        std::rethrow_exception(ep);
    } catch (const value::value_persist_error&) {
        return true;
    } catch (const wal::wal_append_error&) {
        return true;
    } catch (...) {
        return false;
    }
}
```

冷路径（只在失败时执行一次）。`std::logic_error`（编排 bug）、
`std::invalid_argument`（接线错）、其余未知异常一律 false → fatal。

### 5.4 fatal 落点（write_batch.hh）

```cpp
[[noreturn]] inline void
fatal_write_batch_failure(const write_batch_state& state,
                          const std::exception_ptr& ep);
// 提取 what()（catch 任意异常），调用
// core::panic_inconsistency("write_path::write_batch",
//     "fatal failure at phase=%s lsn=%llu: %s", ...)
```

语义依据：WP §10.4——all-WAL durable 之后 memtable/publish 区间的失败，
继续服务会让 live runtime 与 recovery 分叉，必须终止交给 recovery 收敛。
不可分类异常按同类保守处理。

## 6. 顶层 `write_batch` Sender

### 6.1 签名

```cpp
template <typename nvme_sched_t = nvme::runtime_scheduler,
          typename NvmeProvider = value::local_nvme_provider>
[[nodiscard]] inline auto
write_batch(coord::coord_sched& coord_sched,
            std::span<front::front_sched* const> fronts,
            wal::wal_space_sched& wal_space,
            std::span<nvme_sched_t* const> nvme_by_owner,
            core::client_batch_buffer&& input,
            NvmeProvider value_nvme = {});
```

完成值 `write_batch_result`。`input` move 进 assign op（sender 自包含）；
fronts/nvme_by_owner spans 与 schedulers 由调用方保活到 sender 终结
（M11 起由 runtime topology 承担）。

### 6.2 链结构（语义展开）

```text
write_batch(...) =
  coord::assign_batch_lsn(coord_sched, std::move(input))      // pre-LSN 区
  >> then([](core::batch_ctx&& ctx) {
         return write_batch_state(std::move(ctx));            // 构造校验由 047 §5.2 保证恒过
     })
  >> push_result_to_context()                                  // state 入 context（WP §2.1 同款）
  >> get_context<write_batch_state>()
  >> flat_map([=, &coord_sched, &wal_space](write_batch_state& state) {
         // ── post-LSN 区：phase 链 + 异常映射，全部以 &state 借用 ──
         return write_batch_value_phase(state, value_nvme)
             >> flat_map([&state, fronts, &wal_space, nvme_by_owner](bool) {
                    return write_batch_wal_phase(state, fronts,
                                                 wal_space, nvme_by_owner);
                })
             >> flat_map([&state, fronts](bool) {
                    return write_batch_memtable_phase(state, fronts);
                })
             >> flat_map([&coord_sched, &state](bool) {
                    return write_batch_publish(coord_sched, state);
                })
             >> then([&state](bool) {
                    return write_batch_result{
                        .batch_lsn   = state.ctx.batch_lsn,
                        .entry_count = state.ctx.entry_count,
                    };
                })
             >> any_exception([&coord_sched, &state](std::exception_ptr ep) {
                    if (!is_releasable_write_failure(ep) ||
                        !release_allowed(state)) {
                        fatal_write_batch_failure(state, ep);  // [[noreturn]]
                    }
                    return write_batch_release(coord_sched, state)
                        >> then([ep](bool) -> write_batch_result {
                               std::rethrow_exception(ep);     // 客户端收原始错误
                           });
                });
     })
  >> pop_context();
```

### 6.3 设计要点

1. **owner 边界一眼可见**（CQS §3.6 / 迁移计划必须重构 1）：链上每个
   phase helper 名即 owner 跳转（coord → value → front×F → front×F →
   coord）；不引入隐藏 `on(...)`。
2. **两个 barrier 分开**（必须重构 2）：由 M08 `write_batch_wal_phase` /
   `write_batch_memtable_phase` 各自的 reduce-all 保证，M09 只做顺序
   组装，phase 状态机（`memtable_applying` 翻线在 fan-out 前）原样生效。
3. **submit 前零 owner 副作用**（必须重构 5）：组合期只发生 sender 值
   构造与 input move；assign 的入队发生在 op.start（submit 后）。专测
   覆盖（§12 测试 7）。
4. **flat_map 论证**（CQS §3.5）：四个 sequencing `flat_map` 各自构成
   phase 边界（前一 phase 的 barrier 完成是后一 phase 的启动前提，且
   lambda 必须返回 owner sender，无法 `then` 化）；外层
   `get_context >> flat_map` 是一次性的 context 提取，消除了后续每个
   phase 重复 `get_context`（047 §6 各 helper 已自带运行期检查，M09 不
   再包第二层壳）。
5. **异常作用域**：handler 只覆盖 post-LSN 子链。pre-LSN（assign 校验、
   malformed input）异常直接传播给调用方，无 LSN 无 release（041
   §14.1）；fatal 分支 `[[noreturn]]`，不产生 recovery sender。
6. **release 子链复用 M08 checked terminal**：`write_batch_release` 内部
   `require_release_allowed` 是第二道防线（handler 已先查谓词，理论不可
   达；保留作 fail-closed）。

### 6.4 与 M08 的等价性

成功路径逐相语义 = M08 测试 1/5 的手工梯度（同一批 phase senders、同一
顺序、同一 barrier）；M09 增量仅是单链组装 + 异常映射 + ack 提取。
等价性以 §12 测试 1/2 对照 M08 同款断言验收。

## 7. 并发与背压

M09 不新增任何闸门；多 batch 并发 = 多个 write_batch sender 并发 submit。
上限叠加沿 047 §7 三层 budget：

1. 全局 in-flight ≤ coord ready window（assign 容量；超出进 pending
   FIFO，再超出 pre-LSN fail 回调）。
2. 同一时刻处于 WAL phase 且落在同一 front 的 batch 数 ≤ 1 pending plan
   + `queue_depth` 个排队 prepare；溢出 → `prepare_queue_full` → 本链
   release → 客户端收错误。**这就是 M09 形态下的过载背压**：过载表现为
   "该 batch 失败、槽位 resolved、客户端可重试"，不是死锁也不是 LSN
   hole。M11 设计对外并发度时必须对照（047 §17.4 watch-item 1 仍开放）。
3. 页级 FUA 并发由 `wal_append_config.max_fua_inflight` 约束（不变）。

value round 合并：并发 batch 的 value phase 可能合并进同一 round，round
失败共担（M07 §9.8 语义）——每个受累 batch 各自走本链 release，gap-free
不受影响。M09 不为此加测试（M07 已覆盖 owner 级，M08 §4.4 已记录）。

## 8. Lifetime 契约

| 对象 | Owner | 必须活到 | 保障方式 |
|---|---|---|---|
| `input`（client_batch_buffer） | move 进 assign op → `batch_ctx.input` → `write_batch_state.ctx` | batch terminal | sender 自包含，调用方 move 后不再持有 |
| `write_batch_state` | pipeline context（push_result_to_context 节点） | pop_context / context 析构 | 框架管理；post-LSN 子链全部 `&state` 借用，context 节点地址稳定 |
| fronts / nvme_by_owner spans、coord/wal_space 引用 | 调用方（测试 fixture；M11 起 runtime） | sender 终结（含异常路径） | 调用方契约，头注释写明 |
| `value_nvme` provider | 按值捕获（空仿函数） | — | 零成本 |

## 9. 内存序与并发安全

M09 零新增 atomic、零新增发布协议。state 仍是 request-private 单写者
（047 §9 论证原样适用——M09 只是把相邻 phase 间的 happens-before 从
"测试逐相 submit"换成"同链顺序推进"，约束更强不更弱）。durable_lsn
可见性沿 M03 release/acquire 链。

## 10. 错误 / 失败语义总表

| 场景 | 客户端可见结果 | LSN 槽位 | 备注 |
|---|---|---|---|
| pre-LSN：malformed input / 空 canonical batch | 原异常（`std::invalid_argument` 等）直接传播 | 未消耗 | 041 §14.1；不经过 release handler |
| value phase 失败（oversized / round_failed） | release 完成后重抛 `value_persist_error` | released-empty | phase 停 assigned → release |
| WAL phase 失败（device_failure / prepare_queue_full / encode） | release 完成后重抛 `wal_append_error` | released-empty | phase 停 value_durable |
| WAL segment 耗尽 | 无失败；链挂起等待 M04 分配 | 保持 in-flight | 044 §10.2 |
| memtable / publish 区间任何异常 | 进程 panic（不返回） | 永不 resolve（runtime 已终止） | WP §10.4；WAL 已全量 durable，recovery 重放收敛 |
| 不可分类异常（logic_error / 未知） | 进程 panic | 同上 | 047 §15.1：logic_error 绝不映射 release |
| push 窗口 bad_alloc（理论） | 异常传播；调用方按 fatal 类处理 | 未 resolve | §4.3 调用方契约 |

## 11. 热路径预算与容量估算

新增 runtime carrier：无（`write_batch_result` 是 16B 按值返回）。

写路径热路径增量（对照 M08 §11 预算，只列 M09 新增）：

| 路径 | M09 新增成本 | 说明 |
|---|---|---|
| 每 batch submit | **1 次 root submit**（M08 测试梯度是每 phase 一次，共 5 次；M09 收敛回 production 形态） | 净减少 4 个 root context/scope |
| state 入 context | 1 次 `push_result_to_context` 节点（state move 进节点，无 copy） | WP §2.1 拓扑的固有成本 |
| phase 顺序组装 | 4 个 sequencing `flat_map` + 1 个外层提取 `flat_map`，各一次 runtime sub-scope（框架既有成本，对应 WP §2.1 链形状） | 零新增 owning copy / heap 业务分配 |
| ack 提取 | 16B 按值 | 零 heap |
| 失败分类 / fatal | 仅失败冷路径执行（一次 rethrow/catch + 字符串构造） | 热路径 0 |
| 成功路径异常设施 | `any_exception` op 静态存在，成功路径只是值穿透 | 0 |

10 亿 KV 校准：per-batch 临时量不变（M01/M08 已计），无常驻新增。

## 12. 测试计划

Target：`inconel_test_m09_production_write_batch`
（`apps/inconel/test/test_m09_production_write_batch.cc`，CMake 照
m01-m08 模式注册）。

### 12.1 Fixture

复用 M08 测试的 fixture 模式（fake NVMe + 拓扑 + registry + advance
驱动）。fake NVMe 在 M08 形态上增加 test-local 暂扣能力：

```text
hold_call（op_kind + call_index 指定）：命中的 req 移入 held 池不处理；
release_held(fail_with_false / fail_with_exception / succeed)：
  按指定结果处理 held req。
```

用于测试 5 的"后失败"乱序构造；其余测试不用。fixture 驱动统一为
"submit write_batch 单链 + advance_all 轮询"。

### 12.2 测试列表

1. `m09_success_path_matches_baseline_semantics`（≈ 旧 24A test 2）
   与 047 测试 1 同款混合 batch（last-op-wins + PUT→DEL 折叠 + 双 front
   + untouched front）经**单链** write_batch：ack == {lsn 1, entry_count
   3}；durable==1；可见性三态、WAL 字节解码（stream_id/lsn/entry_count/
   vr）、value vr 非默认且 DELETE 默认——断言级别与 M08 测试 1 对齐。
2. `m09_concurrent_batches_publish_gap_free`（≈ 旧 24A test 3）
   三个单 PUT batch 并发 submit（先后 submit、交替 advance）→ 三个 ack
   的 lsn 为 1/2/3；durable==3；三 key 可见且 `data_ver` 各为其 lsn。
3. `m09_value_failure_releases_and_stays_invisible`（≈ 旧 24B test 1）
   注入 value FUA 失败 → sender 以 `value_persist_error` 失败（原始
   错误可见）；随后成功 batch 后 durable 越过 released 槽位；失败 key
   全程 invisible，其 front memtable 无 entry。
4. `m09_wal_failure_releases_and_memtable_invisible`（≈ 旧 24B test 2）
   注入 WAL FUA 失败（value 写已成功后）→ `wal_append_error`；断言
   value NVMe 流量 > 0（失败发生在 value durable 之后）、memtable 零
   污染、release 生效、后续 batch 成功。
5. `m09_release_of_failed_earlier_batch_unblocks_later_success`
   （≈ 旧 24B test 3）A（front 0，DELETE-only）WAL 首写被 hold；B
   （front 1）全程完成 publish → durable==0、B invisible（read_lsn 0）
   → release_held(fail) → A 链内 release → durable==2 → B 可见、A
   invisible。
6. `m09_pre_lsn_failure_propagates_without_lsn_or_release`
   malformed buffer（截断/空 canonical）→ write_batch 以
   `std::invalid_argument` 失败；随后正常 batch ack.lsn == 1（LSN 未被
   消耗，无 release 发生）。
7. `m09_compose_without_submit_has_no_owner_side_effect`（≈ 旧 24A
   test 1）构造 write_batch sender 后直接销毁（不 submit）→ fake NVMe
   `total_calls()==0`、各 front memtable 空、随后正常 batch ack.lsn==1
   （next_lsn 未动）。
8. `m09_failure_classification_unit`
   `is_releasable_write_failure`：`value_persist_error` /
   `wal_append_error` → true；`std::logic_error` /
   `std::invalid_argument` / `std::runtime_error` / 任意未知 → false。
9. `m09_oversized_value_fails_with_release`
   超 max class 的 PUT → `value_persist_error{oversized_value}`（预检
   路径，phase 停 assigned）→ release 生效（后续 batch 后 durable 越过
   槽位）、fake NVMe 零 value 流量。

排除并声明理由：memtable-phase fatal panic 无 production 注入面（fronts
不提供 memtable 失败注入，为测试加注入面违反"不为测试改 production"），
本步以分类单测（测试 8）+ M08 测试 10 的状态机锁线为替代覆盖；panic
路径留给未来集成 death-test 基建时补。

### 12.3 回归门

每 Phase：`cmake --build build` 全 target +
`inconel_test_m01..m09` 九个二进制全 PASS（M08 测试 10/11 不改而绿是
§5.2 重构的等价性证据）；收尾另跑 `build_asan` 同名九个全 PASS。
`inconel_test_flush_e2e` 维持编译通过。

## 13. 实现顺序（每 Phase 一个提交）

```text
Phase A  write_batch_state.hh：release_allowed 谓词 + require_release_allowed 委托改写
Phase B  write_path/write_batch.hh：result/分类/fatal + 顶层 write_batch 链
Phase C  测试 fixture（含 fake 暂扣扩展）+ 测试 1/2/7（成功路径 + 无副作用）
Phase D  测试 3/4/9（失败 → release 路径）
Phase E  测试 5/6/8（乱序 unblock + pre-LSN + 分类单测）
Phase F  全量回归（Release + ASAN，m01-m09）+ 总报告（声明跳过项）
```

依赖：B 依赖 A；C 依赖 B；D/E 依赖 C。Phase A/B 是 production 实现阶段，
禁止打开任何测试文件；Phase C-E 以 M09 测试作者身份工作，允许读的既有
测试仅限：`test/check.hh`、`test_m08_write_baseline_inflight.cc`
（fixture/fake/解码 helper 模式）、`test_m07_value_persist_read_adapter.cc`、
`test_m03_coord_scheduler_assign_publish_release.cc`；不得修改任何既有
测试文件。

## 14. 排除范围

1. registry/facade topology 注入、public operation surface、runtime
   builder 接线（M11）。
2. point_get / MultiGet / Scan（M10）；seal/gate 编排（M12）。
3. memtable 失败注入面与 panic death-test 基建。
4. 网络/协议层 ACK 形态。
5. M08 phase senders 的任何语义修改（§5.2 的委托改写除外，行为不变）。

## 15. 相邻事项

1. **M10**：point_get 消费 `coord::acquire_read_handle` + front lookup +
   `value::read_value`；write_batch 的 ack 让 M10 测试能精确知道写入
   lsn。
2. **M11**：把本步显式拓扑参数替换为 registry/facade 提供的 runtime
   topology，并定义对外 operation surface；write_batch 签名预期收敛为
   facade 包装层，本步签名保持为底层形态不动。
3. **plan 回填**：`front_wal_development_plan.md` M09 节落点
   `apps/inconel/pipeline/write_batch.hh` 更新为
   `apps/inconel/write_path/write_batch.hh`，并标注"M09 的详细设计文档
   是 048_production_write_batch_pipeline_design.md"。
4. **known_issues**：无预期变更。

## 16. 需要人工判断的点

无阻塞项。fatal 机制（panic_inconsistency）、ack 形态（旧分支
`write_batch_result` 保留）、异常分类规则（047 §15.1 已冻结）、落点
（047 §3 已预裁决）均有唯一依据。若实现发现 §6.2 链形态在 pump 下无法
成立（如 any_exception 与 context 借用的组合问题），必须停下报告，不得
就地改设计。

## 17. Review 对账（2026-06-12，M09 实现 land 记录）

实现提交：`46216a9`(A release_allowed 谓词) → `aa6f18b`(B 顶层
write_batch 链) → `488e999`(C fixture+hold 扩展+测试 1/2/7) →
`e8a35f0`(D 测试 3/4/9) → `f45ce72`(E 测试 5/6/8) → `8614485`(F 回归
marker)。production 变更仅 `write_path/write_batch.hh`（新，131 行）+
`write_batch_state.hh`（+12 行谓词委托）；`pump/`、`ai_context/`、既有
测试零触碰。§16 点名的 any_exception + context 借用组合无障碍。

### 17.1 语义对照结论

§12.2 的 9 个测试全部落地，**无跳过无降级**（实现方总报告声明，review
独立核对成立）。逐条对照通过的要点：

1. 链结构与 §6.2 逐行对应：handler 在 post-LSN 子链内捕获 `&state`，
   分类 + `release_allowed` 双闸，fatal `[[noreturn]]` panic，release
   后 typed-rethrow 原始错误。
2. pre-LSN 失败（malformed input）绕过 handler 直接传播，LSN 零消耗
   （测试 6 直接证明：失败后下一 batch 拿 lsn 1）。
3. 组合期零 owner 副作用（测试 7：compose 后销毁 → NVMe 零流量 +
   next_lsn 不动）。
4. 失败测试比设计稿更强一档：单链内 release 自动完成，sender 失败返回
   时 durable 已越过该槽位（无需后续 batch 触发观察），并补了
   `wal_has_key(failed)==false` 清洁度断言；WAL 失败测试保留旧 24B 的
   "失败发生在 value durable 之后"前置断言（writes.calls > 1）。
5. 测试 5 精确复刻旧 24B test 3 乱序形态：A 首写 hold → B 全程完成
   ack lsn 2 但 durable 卡 0、B invisible → release_held(fail) → A 链
   内 release → durable 0→2 → B 可见 A invisible。
6. M08 测试 10/11 不改而绿 = `require_release_allowed` 委托改写的等价
   性证据（§5.2 验收项）。

接受的实现形态记录：测试 1 以"WAL 解码 vr ↔ memtable 命中 vr ↔ body
长度"三角一致性替代 M08 的 ctx 直读断言（单链下 ctx 封装于 context，
production 视角等价）；fake NVMe 的 hold/release_held 为 test-local
扩展（§12.1 授权范围内）。

### 17.2 运行效率审计（独立小节）

- **热路径增量对照 §11 预算逐项核销**：每 batch 1 次 root submit（较
  M08 测试梯度净省 4 次 root context/scope）；1 次
  `push_result_to_context` 节点（state move 进节点，WP §2.1 拓扑固有
  成本）；4+1 个 flat_map sub-scope（框架链形状成本，与 §11 表一致）；
  ack 16B 按值。**零新增 owning copy、零新增业务 heap、零新增 queue
  hop**。
- **捕获清单核查**：顶层与各 sequencing lambda 捕获 = span 按值
  （ptr+size）、scheduler 引用、provider 空仿函数/裸指针包装按值——无
  owning capture（CQS §3.4 过）。
- **失败冷路径**：分类（一次 rethrow/catch）与 fatal 的 `std::string`
  what() 提取只在失败/终止路径执行；成功路径上 `any_exception` op 仅
  值穿透。`is_releasable_write_failure` 的 null 防御为 0 成本早退。
- **并发参数**：M09 不新增闸门；并发 submit 上限叠加 = 047 §7 三层
  budget 原样（ready window / per-front gate / max_fua_inflight），
  §7 已写明 prepare_queue_full → 客户端错误即过载背压形态。
- **隐藏成本点名**：无新增 push_context 节点（仅 push_result 一处）；
  state 构造校验 4 次标量比较（恒过，assign 后不变式）；
  `release_allowed` 谓词为单 switch，misuse 文案构造仅冷路径。

### 17.3 独立验门记录（不采信实现方自报）

`cmake --build build` 全 target 0 错；Release `inconel_test_m01..m09`
9/9 PASS；`cmake --build build_asan` 全 target 0 错；ASAN 同名 9/9
PASS、无 leak/UAF；变更范围核查（4 个预期文件，production 净增 143
行）与残留扫描（无 TODO/stub/step 字样标识符）通过；
`inconel_test_flush_e2e` 两套构建维持编译。

### 17.4 遗留 watch-item

1. memtable-phase fatal panic 分支无 production 注入面，未被测试驱动
   （§12.2 排除项，分类单测 + M08 状态机锁线为替代覆盖）；若未来引入
   death-test 基建，补一条 panic 路径集成测试。
2. M11 接管拓扑参数时，write_batch 签名保持底层形态，facade 只做包装
   （§15.2）；047 §17.4.1 的 queue_depth 双用途解耦问题仍开放。
3. 不新增 known_issues 条目：本步无 production 缺陷级发现。
