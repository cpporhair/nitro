# 059 — Gap A e2e 覆盖补全（计划与结果）

> 状态：A1/A2/A3/A5/A7/A8 已实现并真盘验过；A4/A6 推迟（依赖未建的 recovery）；
> A9/MultiGet/Range-Scan 不在范围（未实现）。发现并记录 2 个 production bug（见 §6）。

## 1. 背景

来源：本轮 e2e 覆盖评审。当前两个 e2e harness（`test_steady_e2e` / `test_flush_e2e`）对稳态前向路径 + B+ tree 变形覆盖扎实，但留下两类洞：

- **Gap A** = production **已实现**、却没有任何 e2e 触达的能力（真实测试欠债）。
- **Gap B** = production **尚未实现**的能力（recovery / MultiGet / Range-scan / format_disk / ENOSPC），属功能边界。

本计划只补 **Gap A**。

## 2. 工法（codex 实现 / claude review / 协调）

- **设计 + review + 协调**：claude。**实现**：codex（`codex exec` 后台派发）。
- **逐 Phase 推进**：一次只派一个 Phase；codex 在分支 `inconel.new` 提交，claude 派 review 子 agent + 亲核，再做独立真盘验门，过门后才派下一个。
- **独立验门**：每个 Phase 由 claude 在 `build_real` 编译 + 真盘（scratch `0000:04:00.0`）跑出 `all passed` 才算过。
- **总报告纪律**：codex 收尾报告必须显式声明跳过项 / 未做项 / 撞到的 production-change 阻塞，不许沉默降级。
- **bug 处理（用户工作模式 2026-06-18）**：测试暴露 production bug → **只报告、不在测试阶段顺手修**（除非用户当场授权，如 A5）；不缩小/削弱测试去绕过 bug。

## 3. 硬约束（写进每次 codex 指令）

1. **只改 `apps/inconel/test/` + 顶层 `CMakeLists.txt`（注册 target）。零 production 改动。** 若看似需要改 production（含加 testability seam）→ 立即停下、不提交、报告阻塞点交回裁决；不擅改 production、不加兼容 shim。
2. **不读、不改其它测试文件来"凑过"。** 新测试编译失败只许改新测试本身。
3. **代码标识符不带 step/phase 数字**；测试按"测什么"命名。
4. **PUMP pipeline 规则**：`bind_back` 必须 `just() >>` 前缀；`flat_map` 只用于异步 scheduler sender；并发中禁止共享可变状态（per-slot 写入或 atomic，不用 `concurrent() >> reduce` 共享 accumulator）；资源释放放异常屏蔽之后。
5. **构建/运行**：编译 `build_real`（vendored SPDK 26.1）；真盘 `--pci-addr 0000:04:00.0` + `LD_LIBRARY_PATH=/home/null/work/kv/spdk/build/lib:/home/null/work/kv/spdk/dpdk/build/lib` + `sudo`（vfio）；codex 侧只编译验证，真盘是 claude 独立验门；清残留不用 `pkill -f` 匹配到自身。
6. **提交**：每 Phase 一个 commit，前缀 `nitro: inconel: <一句话>`（本分支统一单一前缀）。
7. **share-nothing 停机**：多线程 harness 退出走硬停（`is_running_by_core[core]=false` 后 join），不 drain；内存正确性靠长跑压力 + 断言，不靠 ASAN。

## 4. 范围（动笔前已核实）

| 能力 | 归属 | 说明 |
|---|---|---|
| A1 并发多核 runtime | ✅ 做 | keystone，后续 phase 复用其 fixture |
| A2 并发多 batch 写 | ✅ 做 | 在 A1 基建上 |
| A3 写背压 | ✅ 做 | 独立 harness |
| A5 seal 与在途写竞争 | ✅ 做 | 独立 harness |
| A7 value 放置多样性 | ✅ 做 | 独立 harness |
| A8 多 shard + split repartition | ✅ 做 | 独立 harness，keystone shadow-CoW |
| A4 value 故障→`release_batch`+orphan | ⏸ 推迟 | orphan 清理依赖未建的 recovery；value 空间耗尽是 fatal-panic（`value/scheduler.hh:1053-1064`，v1 out-of-space=fatal）非 graceful release 入口，与 `write_path §10.2` 文档描述不一致（独立 doc/code mismatch，留待裁决）。recovery 落地后再做。 |
| A6 tombstone 物理 compaction | ⏸ 推迟 | 机制存在（`candidate_build.hh:187-191` 丢 `data_ver≤recovery_safe_lsn` 的 tombstone），但运行时 flush 把 `recovery_safe_lsn` 写死 0（`flush_round.hh:97`→`owner_scheduler.hh:3253` 不覆盖），运行时永不触发。owner 自有可用的运行时 `recompute_recovery_safe_lsn()` 但 flush fold 未读它。判定「v1 故意保守 vs 未接完 wiring（真容量缺口）」涉及 recovery-safety 决定 → 推迟。 |
| A9 长读资源限制 | ❌ 不在范围 | `read_api §8` 读超时/代距告警/堆积背压 production **未实现**（v1 显式延期）。归 Gap B。 |
| MultiGet / Range Scan | ❌ 不在范围 | MultiGet 无 pipeline/op；Range Scan 仅有 front memtable-scan 半成品（`handle_scan`），未接成完整公开 op（无 tree-scan+merge+`operations.hh` 面）。归 Gap B。 |

## 5. 结果总表（真盘 `0000:04:00.0`）

| Phase | Gap | commit | 真盘结果 | Review |
|---|---|---|---|---|
| 1 | A1 | `7ff4840` | 真多核 `rt::run` 下写/读/seal/flush 全链 + 分区 oracle 全量正确 + bounded/无泄漏，`all passed` | — |
| 2 | A2 | `45f3b52` | burst `max_inflight=192`、durable 单调、静默后 durable==highest、乱序下全量正确，`all passed` | — |
| 3 | A3 | `7155c32` | `coord_ready_window=8` + 256 in-flight：`durable final==highest`（背压下前缀闭合）、overflow 路径实际触发并干净失败、`all passed` | APPROVE-WITH-NITS |
| 4 | A5 | `a416414`(test) `951c97c`(prod fix) `ef6eb0e`(断言修正) | 45 seal 全部 race 在途写、`durable final=2048==highest`、全量 oracle 命中、`all passed`。**修复了 BUG-1**（见 §6） | APPROVE-WITH-NITS |
| 7 | A7 | `66c6750` | value body 跨全 5 size class + sub-LBA + 4-LBA multi-LBA；`partial_into_hole=234 open=74`（reuse 路径强触发）、全量 byte-exact read-back、`all passed` | APPROVE |
| 8 | A8 | `f04cecd` | 32768 key / 4 round / K=4：round 1-2 跨轮回读 8192/16384 key 全 byte-exact（shadow-CoW 不变量 A/B 成立）、partition map 1→4；**round 3 暴露 BUG-2 → panic**（见 §6） | APPROVE-WITH-NITS |

> 统一测试（一次性跑全部 + 基线 `test_steady_e2e`）：5/6 `all passed`，A8 复现 BUG-2 panic。

## 6. 发现的 Production Bug

### BUG-1 — empty-round flush 崩溃（A5 发现，**已授权修复**，commit `951c97c`）

- **现象**：`flush_once failed: pipeline::flush_round_once: tree flush returned null manifest`。
- **根因链**：`front::seal_active_now()`（front/scheduler.hh:752）无条件 seal active gen → 激进 seal 把"无新写"的 active 封成**空 sealed gen**（`memtable_gen::max_lsn=0`，memtable.hh:233）→ `collect_eligible_gens`（`0≤durable_lsn`）纳入 eligible → `build_flush_branch`（flush_round.hh:124，仅 0 eligible 才 noop）建**真** flush 请求 → tree fold `st==ok` 但 `workset.empty()` → 短路无 coro（owner_scheduler.hh:3337）→ `emit_finalize_merge_success`（:3512）合法返回 `{st:ok, new_manifest:nullptr}` → `require_successful_tree_flush`（flush_round.hh:135）把 `ok+null` 当 fatal 抛。
- **性质**：production bug。public 维护 op + 稳态后台环路定时调用即命中；抛后空 gen 永不从 `imms_` 释放 → 累积。属 v1-core「Flush 全流程」+ 设计「empty round 可 fold 后返回 success(empty delta)」（flush §3.3.3）。
- **修复（spec 依据；tree 侧 nullptr=empty-round 信号是设计意图，不改）**：
  1. `flush_round.hh` `require_successful_tree_flush`：`st==ok && new_manifest==nullptr` 视为合法 empty round，不抛（仅保留 `st!=ok` 抛）。
  2. `coord/scheduler.hh` `handle_frontier_switch`：`new_manifest==nullptr` 时复用 `cat->prs->tree_guard->manifest`（layout 不变）构造 G1，仍 subtract+release flushed gens、bump epoch、install CAT2（§4.2 中 `new_manifest==old manifest` 退化）。
  - blast radius：仅在 `new_manifest==null` 激活（此前必抛）→ 非空轮零行为变化。

### BUG-2 — reclaim invalidate 完成队列溢出（A8 发现，**已报告，未修**）

- **现象**：A8 round 3 flush 成功后 panic `tree::tree_sched::enqueue_reclaim_invalidate_done: invalidate completion queue full`。
- **根因**：`process_reclaim_task`（owner_scheduler.hh:2684-2697）对每个退休 slot/range 发一次**异步** `submit_reclaim_invalidate`，**无任何 in-flight 上限**；完成回调从多个 read_domain 核心并发塞进固定 **256 容量** 的 `reclaim_invalidate_done_q`（owner_scheduler.hh:382）；消费端每 advance 仅 drain `kMaxReclaimInvalidateCompletePerAdvance=64`（:2370）；大 flush 退休 >256 页 → `try_enqueue` 失败 → 硬 panic（:2582-2586）。
- **性质**：production bug，**非测试 pacing 问题**（硬 panic 非背压）。10亿 KV 目标下每个大 flush 退休远超 256 必然触发，属 reclaim 路径真实背压/容量缺口。
- **shadow-CoW 核心不受影响**：崩在 reclaim 路径；前两轮 16384 跨轮 key 读回全对，不变量 A/B 未违反。
- **建议修复方向（留待后续）**：给 invalidate 提交加背压——在飞 invalidate 数 ≤ 完成队列容量，`process_reclaim_task` 改 resumable（到上限暂停，drain 腾位后续），复用现有 `pending_invalidations` 计数。单纯调大队列 / drain 全部 都只是推迟 panic，撑不住规模。

## 7. 各 Phase 设计依据与断言（spec 锚点）

> 保留每个 phase 的 spec 依据与断言分档，作为"为什么这样测"的参考；运行结果见 §5。

### A1（Phase 1）— 并发多核 runtime keystone
- **问题**：`test_steady_e2e` 是单线程 cooperative driver、`test_flush_e2e` 绕过写/读 pipeline，故 production 写/读/seal/flush 全链从未在真多线程 runtime 下跑过，`runtime_state §10` 并发安全论证无 e2e 兜底。
- **依据**：`write_path §2/§6`、`read_api §2/§4`、`runtime_state §9/§10`、`cross_doc_contracts §1/§4/§5`；范式 `test_flush_e2e`（`rt::run` 多核起停）+ `test_steady_e2e`（build/submit/oracle）。
- **oracle 确定性**：keyspace 按 writer 分区（writer t 只写自己分区）→ 单 key 最终值 = 该 writer last-write-wins，无需全局 LSN 交错；强断言只在静默后做。
- **断言**：静默后全量 oracle 一致；全程无 panic / 无 `per_core::queue full`；多轮 bounded（live gen / tree_head / value_head 有界、reclaim idle）；硬停 join 无崩溃。
- **基建**：多线程 fixture、writer/reader 线程 + 非 advance 核选择、分区 oracle、并发提交 helper（后续 phase 复用）。

### A2（Phase 2）— 并发多 batch 写语义
- **依据**：`write_path §8`（ready_bitmap、durable 连续前缀、window）、`design_overview §6`、`runtime_state §2`。
- **断言**：durable 单调非降；静默后连续前缀闭合（durable==最高已分配 batch_lsn）；in-flight 深度 >1 真发生；乱序完成下全量正确。
- **observability**：durable 经现有 `acquire_read_handle → read_lsn`；无现成只读面则降级 + 报告，禁加 production accessor。

### A3（Phase 3）— 写背压
- **依据**：`write_path §7.4`（WAL 池耗尽）/`§8.5`（ready-window 上限）、`overview §11.4`、`runtime_state §2/§5`。
- **seam**：ready-window 满 → assign 排队进 `pending_assigns_`（不报错）；`pending_assigns_` 也满（≥`pending_assign_capacity_`）才抛 `runtime_error("coord assign backpressure overflow")`。
- **断言**：核心——小 window + 大 burst → liveness（全完成）+ durable 单调 + 静默后 ==highest + 全量正确（证 graceful queue→drain）；硬上限溢出 best-effort（触发到则断言干净失败无 LSN 洞，未触发 log 不 fail）；WAL 池背压可选。
- **observability**：`next_lsn_` 无 race-free 读面 → 由"全完成 + 连续前缀闭合 + 正确性"间接证；禁加 production accessor。

### A5（Phase 4）— seal 与在途写竞争 / batch 不跨 seal
- **依据**：`write_path §9.1-9.5`、`design_overview §9.1`、`runtime_state §2.5/§3.6`。
- **不变量**：(1) 同一 batch 的 memtable inserts 全落同一套 active gens（不跨 pre/post-seal）；(2) in-flight batch 不因 close_gate→open_gate 丢 publish。
- **seam**：seal 由 coord 发起（close_gate → fan-out seal_active → build_prs1/install_cat1 → open_gate），coord 单线程保证 seal_active 不被插进同 batch 两轮 fan-out 之间。
- **断言**：核心——liveness 全 ACK + 静默后 `final_durable==highest`（publish 不丢）+ 全量 oracle（劈分/丢失会让跨 gen merge 的 per-key winner 错 → mismatch）；seal 真并发（`seal_rounds≥K` + overlap + `non_noop_flushes≥2`）；gate 闭合观测 best-effort（现成 `gate_open_for_testing()`，禁新增）。
- **断言修正**：reclaim 静默判据用 `reclaim_idle()`，**不**用 `partial_into_untracked==0`（后者按 `cross_doc_contracts §value-side reclaim` 是可观测 counter、稳态≈0、明确不加 liveness 校验；激进 seal+eviction 下合法 small-but-nonzero）。

### A7（Phase 7）— value 放置多样性
- **依据**：`on_disk §5`（size class / sub-LBA）、`runtime_memory §9`（放置+状态耦合）/`§10`（zero-copy 读）、`read_api §9`。
- **seam**：value 空间耗尽是 fatal-panic（不可作正常路径）；放置可观测面 `rt::value()->inspect_reclaim_stats()`（含 `partial_into_hole/cache/open` 等 reclaim 分类）；size-class 直方图无现成 accessor。
- **断言**：核心——写覆盖全 size class + 大量小值（sub-LBA packing）+ multi-LBA + overwrite 驱动 hole；全量 byte-exact read-back（覆盖 zero-copy + CRC）。放置路径被走到——`partial_into_hole+cache+open > 0`（确定性触发，证非全落 fresh page）。prefill best-effort。
- **observability**：正确性走 `rt::point_get`；reclaim 分类走 `inspect_reclaim_stats()`；禁加 production accessor。

### A8（Phase 8）— 多 shard + split 驱动 repartition（keystone shadow-CoW）
- **依据**：`design_overview §10`（shadow CoW / slot_map / consolidation）、`flush §9`（leaf/internal/root split）、`runtime_state §4`、`read_api §4`。
- **不变量（`project_inconel_shadow_cow_core`）**：A 普通 leaf update 不级联；B internal `child_base==child 的 range_base`（lookup 经 `manifest->resolve(child_base)` 走 slot_map）。跨 round 读是其可证伪落点。
- **seam**：split 真实现（`candidate_build.hh:890`、owner split 输出、`is_root_change`、page-chunk split-on-overflow）；`current_shard_partitions()` → `shard_partition_map`（`shards.size()` / `shard_count()` / `route()`）；bootstrap 单 shard placeholder → 首次 flush 后基于 `leaf_order` 重建。
- **harness**：多 read_domain（K≥2）+ 数万 key + 多 flush round；每轮 + 终局全量跨 round read-back。
- **断言**：核心——每轮 + 终局全量 byte-exact read-back（跨 round resolve 验不变量 B）+ `durable final==highest` + 无 panic 跑完多 round。split+repartition 真发生——`shards.size()` 增长 + `shard_count()>1` + `non_noop_flushes≥2`（注：`shards.size()` 饱和于 `min(K,leaf 数)`，是 repartition 证据但非 split 量级证据；真 split 证据是跨 round read-back + non-noop）。root change best-effort。
- **observability**：正确性走 `rt::point_get`；partition map 走 `current_shard_partitions()`；tree height/leaf count 无直接面 → 间接证；禁加 production accessor。

## 8. Review gate 与验证门

- **Review gate（每 Phase commit 后，claude 派子 agent + 亲核）**：① 正确性（断言真覆盖语义、oracle 无 race）② 零 production 改动（`git diff` 只含 `test/` + `CMakeLists.txt`）③ 未动其它 `test_*.cc` ④ PUMP 用法 ⑤ 运行效率轴 ⑥ spec 主轴回核（断言对应设计章节，不是"能跑过的最简形状"）。
- **验证门（独立）**：claude 在 `build_real` 编译 + 真盘 `0000:04:00.0` 跑出 `all passed` 才算过门；过门前不派下一个 Phase。
