# Inconel — Known Issues

> 已确认存在 + 已确认延后的问题登记。一行一条，方向一句话。
> 真正动手修时回 audit / spec / 代码再研究；本文不是实施手册。

## Priority

- `urgent`  不被 dependency 阻塞，尽快处理
- `blocked` dependency 锁住，等解封条件
- `normal`  标准 backlog
- `low`     改善向

## Open

> 按“实现前提”分组，而不是按 priority 排序。优先级仍看 `Priority` 列；分组只回答“现在做是否会顺、会不会返工、在什么模块边界上做最合适”。

### 现在就可以实现：低依赖清理 / 命名 / 文档

这些项不需要等新模块落地，主要是局部清理、命名对齐或文档化。

| ID | Issue | 来源 | Priority | 方向 |
|----|-------|------|----------|------|

### 现在就可以实现：当前运行时路径改进

这些项都建立在当前已有 tree/value/runtime 路径上，不依赖 future 模块，属于现在就能直接收敛的行为或结构问题。

| ID | Issue | 来源 | Priority | 方向 |
|----|-------|------|----------|------|

### 未来功能的基础，建议提前设计 + 实现

这些项现在不一定立刻“被用到”，但会冻结 format、runtime 边界或核心抽象。晚做通常会让后续大功能先搭在错误地基上。
`INC-031` / `INC-034` / `INC-036` 已分别由 step 016 / step 017 / step 018+019 收敛；当前本组暂无 open 项。

| ID | Issue | 来源 | Priority | 方向 |
|----|-------|------|----------|------|

### 最好等 Tree 写侧 / Allocator / Reclaim 模块

这些项要么本身就是 tree 写侧，要么强依赖 tree allocator / reclaim 真实落地后才能做对。

| ID | Issue | 来源 | Priority | 方向 |
|----|-------|------|----------|------|
| INC-001 | tree multi-level descent 在 production 0 验证 | audit/tree.md F3 | `blocked` (tree_sched 写侧 + property test) | tree_sched 写侧落地后补 property test 强制 ≥3 层；mitigation 期加 depth assert + 模块 scope 注释 |
| INC-011 | `tree_manifest::has_root()` 用 `lba == 0` 当 sentinel，依赖隐式不变量 | audit/tree.md F10 | `blocked` (tree_sched 写侧 / page allocator) | 等 allocator 落地知道哪些 lba 合法后定方案（optional<paddr> 或显式 bool 字段） |
| INC-022 | tree 模块缺 `flush` handle 整套（spec RSM §4 + FF §3：tree_allocator / checkpoint_guard / fold / consolidation / frontier_switch / new manifest） | audit/tree.md F4（扩展） | `urgent` | 实现 tree_sched 写侧基础设施（allocator + checkpoint_guard + retire queues）+ flush handle；这是 INC-001 的真正解封路径 |
| INC-023 | tree 模块缺 `reclaim` handle（spec RSM §4.2：TRIM 旧 slot/range + 调用 value 的 freed_slots / recycle_whole） | audit/tree.md F4（扩展） | `urgent` | 实现 reclaim handle，dispatch 到 INC-018 / INC-019 的 value 接收方；TRIM 顺序遵循 spec §6.9 |
| INC-024 | tree 模块缺 `update_superblock` handle（spec RSM §4.2：root-change flush 后异步更新 superblock + 推进 superblock_safe_lsn） | audit/tree.md F4（扩展） | `low` (Phase 1 不重启) | 等 superblock 持久化路径整体落地时实现 |

### 最好等 NVMe / BufferPool / Device Abstraction 模块

这些项的正确边界在 `apps/inconel/nvme/`、设备抽象和 DMA buffer ownership 上，提前做很容易再返工。

| ID | Issue | 来源 | Priority | 方向 |
|----|-------|------|----------|------|
| INC-002 | mock_nvme 路径上的硬耦合：(a) tree::lookup_scheduler 用 `std::make_unique` 非 DMA 内存 buffer；(b) runtime `start_options.device` 类型是 `mock_nvme::mock_device*` | audit/tree.md F5 + audit/runtime.md R3 | `blocked` (apps/inconel/nvme/) | nvme/ 落地时同时引入 device + buffer 双抽象（virtual base 或 template），lookup_scheduler 迁到 BufferPool，runtime 接 device 抽象不接 mock_nvme 类型 |
| INC-016 | tree + value 两侧 cache buffer ownership 都是 workaround：tree 用 free_bufs_ + owned_bufs_ 析构隐式释放；value `~scheduler()` 手动 evict_one drain | audit/tree.md F15 + audit/value.md V9 | `blocked` (与 INC-002 共生命周期) | nvme/ + BufferPool 抽象落地时一起重做 cache_concept 的 buffer ownership 模型，覆盖 tree + value 两边 |

### 最好等 Front / WAL 模块

这些项和 front owner、WAL append 路径的真实接口一起定最稳妥。

| ID | Issue | 来源 | Priority | 方向 |
|----|-------|------|----------|------|

### 最好等 Runtime Format / Recovery 模块

这些项要和“如何格式化盘面、如何恢复 superblock / recovered state”一起落地。

| ID | Issue | 来源 | Priority | 方向 |
|----|-------|------|----------|------|
| INC-035 | runtime/ 缺 `format_disk()` 路径，spec ODF §7 定义的格式化流程（计算区域边界 + TRIM 整盘 + 写 superblock A/B）完全缺失；mock_nvme zero buffer 当"已格式化"使蒙混过去，真 nvme 上线必需 | audit/runtime.md R7 | `blocked` (superblock POD 已有；仍依赖 nvme/ 落地) | 按 ODF §7 实现 `format_disk()`：算 region 边界、TRIM 整盘、写 superblock A/B；与 INC-020 install_recovered_state 是同组（format/recovery 配套） |

### 最好等 Value Reclaim / Recovery / Read Pipeline 模块

这些项属于 value 模块缺失的 handle 族，最好作为成组功能落地，而不是零散插入当前实现。

| ID | Issue | 来源 | Priority | 方向 |
|----|-------|------|----------|------|
| INC-018 | value 模块缺 `freed_slots` handle（sub-LBA slot 回收，spec RSM §6.7） | audit/value.md V3 | `urgent` | 实现 `handle_freed_slots`（dirty / in-hole / not-tracked 三 case），同步加 `hole_pages` + `dirty_pages` + `deferred_freed` 状态机 |
| INC-019 | value 模块缺 `recycle_whole` handle（整页回收，spec RSM §6.8） | audit/value.md V3 | `urgent` | 实现 `handle_recycle_whole`，把 paddr 注入 `whole_pool`；调用方约定 TRIM 已完成（spec §6.9） |
| INC-020 | value 模块缺 `install_recovered_state` handle（boot recovery 入口，spec RSM §6.2） | audit/value.md V3 | `low` (Phase 1 dev 不重启从 0 init) | 等 recovery 路径整体落地时实现 |
| INC-021 | value 模块缺 `read_page_values` handle（MultiGet/Scan 批量读，spec RSM §6.5） | audit/value.md V3 | `low` (Phase 2 用) | 等 read pipeline + Scan/MultiGet 落地时实现 |

### 最好跟 Cache Concept 重构同批处理

这些项单独做收益有限，跟核心抽象一起收才不容易再次漂移。
`INC-038` 已由 step 018 跟 `INC-036` 同批收敛；当前本组暂无 open 项。

| ID | Issue | 来源 | Priority | 方向 |
|----|-------|------|----------|------|

### 最好等 PUMP 框架 / Sender Contract 修正

这些项本质上不是 Inconel 业务逻辑 bug，而是当前 `pump/` sender contract 或实现边界不够清楚；Inconel 侧可以先绕开，但最终要回框架层拍板。

| ID | Issue | 来源 | Priority | 方向 |
|----|-------|------|----------|------|
| INC-041 | `PUMP` 的 `concurrent() >> reduce(...)` / `to_vector()` 对共享 accumulator 的 contract 不明确且当前实现不安全：`reduce` 回调直接原地改同一个 result，对 `vector<worker_tree_proposal>` 这类非线程安全容器会 data race；同时 `reduce_sender` 构造还把初值按拷贝路径放进 `result`，move-only accumulator 无法实例化 | 2026-04-16 `029_owner_closure` owner pipeline 调试；`pump/sender/reduce.hh` + `pump/sender/concurrent.hh` | `blocked` (pump/) | 在 `pump` 层明确并收紧 contract：要么禁止 `concurrent` 后直接接共享 `reduce`/`to_vector`，要么提供 per-branch local reduce + final combine 的安全 collector；同时修 `reduce_sender` 对 move-only 初值的构造语义并补文档。在框架修好前，Inconel 侧不要再用 `concurrent() >> reduce(...)` 收集 `worker_tree_proposal` 这类 move-only / 非线程安全结果 |

## Resolved

| ID | Issue | 来源 | 解决 |
|----|-------|------|------|
| INC-003 | tree lookup sender 让 caller 自由传 sched 指针，没有内部路由解析 | audit/tree.md F7 | 2026-04-16 step 030 最终形态：公开 API 稳定为 `tree::lookup(keys, manifest)`（无 sched 指针）；sender 内部 `build_route_plan` 改用 `current_shard_partitions()->route(key)` 分组，fan-out 目标从 `tree_lookup_at(idx)` 换成 `tree_read_domain_at(idx)->lookup_sched`。与 INC-040 一并收敛 |
| INC-040 | front→tree_lookup 的路由 spec 和实现都写成 hash-based (`home_tree_lookup(front_owner)`)，与设计意图不符。正确意图是 key-range based：同一 leaf page 的所有 key 路到同一 read_domain，保证一张 page 在系统里只 cache 一次 | 用户澄清（2026-04-15）；当时写在 RSM §4.7、design_overview read 伪码、registry.hh L209-217 注释 stub、audit/tree.md F7；plan/027 Gap 3 讨论副产出 | 2026-04-16 step 030 最终形态：(1) routing carrier 从 `leaf_order.find_leaf_for_key(key) % K`（前一轮 INC-040 修过的 ordinal modulo）彻底换成**全局 `shard_partition_map`**：`current_shard_partitions()->route(key)` 一次二分拿 `shard_idx`，lookup / flush fold 两条路径**共用同一张 map**（`memtable_fold::build_key_partitions` 同步切换），保证 "同 key → 同 shard" 不变量。(2) 立起 `core::tree_read_domain<Cache>`，own `tree_lookup_sched` / `tree_worker_sched` + `node_cache` + routing snapshot；PUMP runtime tuple 折成单 read_domain 槽位；registry 只留 `tree_read_domains.list`，旧 `tree_lookup_scheds` / `tree_worker_scheds` / `tree_lookup_at` / `tree_worker_at` / `route_tree_lookup_for_key` 全删。(3) `inconel_test_tree_lookup` / `inconel_test_tree_lookup_multicore` / `inconel_test_runtime` 三个验收测试 pass；spec 同步覆盖 RSM §1/§4/§4.7/§4.8/§9.3/§10.3、design_overview §1.7/§1.8/§8.1/§14、read_api §4/§5/§5.4/§9.2、code_modules、INDEX；audit/tree.md F7 标记 resolved |
| INC-042 | Phase 9 空树 bootstrap flush 未闭：`tree/memtable_fold.hh:234` `build_key_partitions` 对 `!has_root() \|\| leaf_order.empty()` 直接返 `unsupported_shape_change`，导致 fresh-formatted disk 起第一次 flush 无法产出 tree；worker `initialize_worker` 与 owner `run_merge` 也没有 `has_root()==false` 分支 | 2026-04-19 step 033 harness `inconel_test_flush_e2e` 首次跑暴露（st=2, leaf_order.size=0, root_slot.lba=0） | 2026-04-19 解决：(1) `memtable_fold.hh::build_key_partitions` 拿掉 narrowing，空 base_manifest 也走 `shard_partition_map` 路由。(2) `candidate_build.hh` 新增 `_wb::build_leaves_from_sorted_keys_impl` + `_wb::initialize_worker_bootstrap`，空 `leaf_order` 时 worker 直接把 sorted key_groups chunk 成若干 fresh leaf page，单 `leaf_work` 带多 `built_leaves` 喂给 `finalize_root` 已有 wrap 逻辑；同时在 `process_flush_round` 开头加 `s.all_done` 早退，修复协程 driver 二次进入会把 moved-out `built_leaves` 再 wrap 一次导致 panic 的顺带问题。(3) `owner_scheduler.hh::run_merge` 按 `is_bootstrap=!has_root()` 分支：bootstrap 用新的 `make_merge_context_bootstrap`（跳过 `index_root_group`）+ `combine_worker_roots_for_bootstrap`（按 `first_key_of_worker_root` 排序 worker proposal、`build_internal_pages_owner` 包成 fresh internal 层）；Phase 3/4 post-order walk + `build_leaf_order_full` + `build_reverse_topology_full` + `rebuild_slot_map` 原样复用（bootstrap 下没有 paddr-ref 子节点，所有复用分支都只走 unique_ptr 路径）。`is_root_change` 对 bootstrap 返 true → 进 root-change 流程异步更新 superblock。(4) `tree/sender.hh::tree_local_flush` 在 `continue_after_finalize_merge()` 后追加 `then` 调 `rebuild_and_publish_shard_partitions`，基于 `tree_flush_result.new_manifest->leaf_order` + `tree_read_domain_count()` 重建 `shard_partition_map` 并 `rt::publish_shard_partitions` 替换全局 + 每个 read_domain 的 snapshot，production 路径不再依赖 harness 预先手动 publish。全部 15 个 `inconel_test_*` 及 `inconel_test_flush_e2e` pass |
| INC-004 | tree + value 检测到 corruption 时走 silent fallback 或 throw 而不是 panic：tree::process_entries 把 CRC/magic/zero 失败映射成 `lookup_absent`；value::scheduler `handle_fill` / `serve_hit_or_fail` 走 cb/fail dual callback 抛异常进 PUMP exception path | audit/tree.md F1 + audit/value.md V1 + V7 | step 009 已完成：新增 `core/panic.hh::panic_inconsistency`，tree page corruption 与 value decode corruption 统一改为 fail-fast panic，并携带 `tree_page_status` / `value_decode_status` 具体 reason |
| INC-005 | `tree_manifest::resolve` 用 assert，Release 退化 UB | audit/tree.md F2 | step 009 已完成：`tree_manifest::resolve()` 在 miss 时改为 `panic_inconsistency(...)`，不再依赖 Release 会消失的 `assert` |
| INC-007 | `tree_manifest` 模块归属 core/ vs tree/ 不明，spec 没明说 | audit/tree.md F6 | step 011 已完成：在 `core/tree_manifest.hh` 文件头明确记录归属决策，拍板继续留在 `core/`，不做文件迁移 |
| INC-008 | spec ODF §4.2 的 `internal_record` 没有 import 到 `format/tree_page.hh`，layout 散在 page_builder/reader 4 处散文 memcpy（leaf record 是正确范本） | audit/tree.md F8 | step 009 已完成：`format/tree_page.hh` 新增 `internal_record` / `internal_record_size()` / key-child helper，`tree/page_builder.hh` 与 `tree/page_reader.hh` 全部收敛到这些 helper |
| INC-009 | `loading_pages_` / `slot_map` 用 std::unordered_*，spec 用 flat_hash_map | audit/tree.md F9 | step 008 已完成：`tree::lookup_scheduler::loading_pages_` → `absl::flat_hash_set`，`core::tree_manifest::slot_map` → `absl::flat_hash_map` |
| INC-010 | spec 用 abseil 容器词汇（flat_hash_map/set、btree_map、small_vector），代码用 std::* | spec 全文 | step 008 已完成当前已实现代码的容器对齐：shared page_cache `index_`、value `classes_` / `class_sizes_storage_` / `inflight_rounds_` 已切到 Abseil；future 模块首次落地时直接按 spec 选型，不再作为当前 open mismatch issue |
| INC-012 | leaf/internal page reader 全 linear scan，热路径每页 ~134/185 次比较 | audit/tree.md F11 | step 014 已完成：tree page layout 冻结为 `tree_slot_header + full slot directory + payload`，builder finalize materialize directory，reader 改成 directory + binary search，`rightmost_child()` 改 O(1) 取尾部 child，并新增 `inconel_test_tree_page_format` 锁定容量边界与 CRC/offset 语义 |
| INC-013 | `lookup_scheduler::handle` 的 `if (s.first_call)` 分支 + `lookup_state::first_call` 字段是 dead code（make_lookup_state 已处理空 manifest，coroutine 短路掉了 handle 入口） | audit/tree.md F12 | step 011 已完成：删除 `lookup_state::first_call` 字段和 `handle()` 中对应的 dead-code 分支，空 manifest 继续由 `make_lookup_state()` 初始化完成态 |
| INC-014 | tree (`lookup_scheduler` / `lookup_scheduler_base`) 和 value (`scheduler` / `scheduler_base`) 都用裸命名，跟 spec 名字 `tree_lookup_sched` / `value_alloc_sched` 不一致；user 报告 context 大时 AI 曾混淆这类裸命名 | audit/tree.md F13 + audit/value.md V4 | step 011 已完成：tree/value scheduler 类型统一 rename 到 spec 名字，并同步更新 sender / registry / runtime / 测试引用，不保留 compatibility alias |
| INC-015 | `tree/lookup.hh::decision_need_cache` variant 分支永不产生永不专门处理（fall-through 等同 done），暴露在 sender 类型上易被新 scheduler 抄成"预留 variant" pattern | audit/tree.md F14 | step 009 已完成：删除 `decision_need_cache`，`batch_decision` 收敛为 `variant<decision_done, decision_need_read>`，`tree/sender.hh` 的 visit 分发同步简化 |
| INC-017 | `value::handle_persist` rollback 时 fresh_bump / whole_page source 的 LBA 永久泄漏（silent state degradation），且 out-of-space 走 fail() 给 caller 而不是 panic（v1 没 reclaim 撞底不可恢复） | audit/value.md V2 | step 010 已完成：allocator 增加 `push_back_bump(...)`，rollback 逆序归还 `fresh_bump` / `whole_page` / `writable` 三类页；out-of-space 改为 `panic_inconsistency(...)` |
| INC-025 | value `writable_pages_` 是对 spec `hole_pages` + `open_frames` 双概念的简化（合成单一 per-class queue），但 spec ↔ 代码对应关系没文档化 | audit/value.md V6 | step 010 先补明旧 `writable_pages_` 简化与 spec 的对应关系；step 019 进一步删除 `page_data` / `writable_pages_` 主路径，改成 `value_page_frame` + `open_frames_` / `allocatable_frames_`，把 dirty active resident frame 与 clean allocatable resident frame 拆开 |
| INC-026 | `value::handle_persist` 用 `goto round_failed` + 嵌套循环 + 复杂错误传播，user 报告读不懂、无法有效 review 正确性 | audit/value.md V8 | step 010 已完成：`handle_persist` 拆成 `collect_round_items → build_round → finalize_round_writes → publish_round` 四阶段，去掉 `goto`，并改为显式 `persist_entry_status` 分流 |
| INC-027 | `value::scheduler` 的 `class_sizes_storage_` + `class_sizes_view_` 双字段冗余（vector 可直接 decay 到 span，view 字段没必要） | audit/value.md V10 | step 010 已完成：删除 `class_sizes_view_` 成员，改成按需从 `class_sizes_storage_` 构造临时 `span` |
| INC-028 | `value::handle_persist` 的 leader-follower 合并把整个 persist_q_ 吃光，单 round 无上限 → leader latency 不可控（特别影响 perf tuning 的 tail latency） | audit/value.md V14 | step 010 已完成：`handle_persist` 增加私有常量 `kMaxFollowersPerRound = 64`，单轮只合并 leader + 64 followers，剩余请求留给下一轮 `advance()` |
| INC-029 | tree + value 的 `advance()` 用 `.drain()` 把单 queue 吃光，多 queue 之间无 fairness 保证，单次 advance 延迟不可控（一个 queue 满了会饿死其他 queue 的处理） | audit/value.md V14 + tree 同源 | step 012 已完成：tree `cache/lookup` 与 value `finalize/persist/read/fill` 全部改成 bounded per-queue loop，保留原队列顺序与 `advance()` 返回契约，不再单轮吃空整条 queue |
| INC-030 | format/ 缺 WAL 三个 POD type：`wal_segment_header` (ODF §3.2, 26B) / `wal_entry_header` (ODF §3.3, 25B + 编解码) / `wal_sealed_trailer` (ODF §3.4, 33B)，front_sched / batch PUT 路径需要 | audit/format.md F2-F4 | step 015 已完成：新增 `format/wal.hh`，落地 3 个 packed POD、header/trailer CRC + inspect helper、PUT/DELETE entry size/encode/decode helper 与 reason-aware status；同步 ODF §3.3/§3.4 命名；新增 `inconel_test_wal_format` 锁定 golden layout、CRC 与 failure status 分类 |
| INC-031 | format/ 缺 `superblock` POD（ODF §2.2 定义 ~120B 的 packed struct） | audit/format.md F1 | step 016 已完成：新增 `format/superblock.hh`，落地 packed `superblock` POD、CRC/status helper 与 A/B 选择 helper；新增 `inconel_test_superblock_format` 锁定 layout、CRC 分类和 same-generation conflict 语义 |
| INC-032 | `format/crc.hh::crc32c` 用 raw SSE4.2 intrinsics，缺标准 CRC-32C 的 init/xor conditioning，跟外部工具（btrfs/ext4/iSCSI 等）不匹配；spec ODF §1.3 含糊 | audit/format.md F5 | step 009 已完成：删除 `format/crc.hh`，`tree_page` / `value_object` 调用点切到 `absl::ComputeCrc32c`，并在 ODF §1.3 明确标准 CRC-32C + init/xor conditioning 语义 |
| INC-033 | `build_runtime` + `start.hh::run_with` 一连串 raw `new` 没 RAII，`start()` 抛或某个 scheduler 构造抛 → 已分配的 rt + scheduler 全 leak | audit/runtime.md R1 | step 011 已完成：`runtime::run_with()` 统一 catch `std::exception` / unknown exception 后直接 `panic_inconsistency(...)`，把 init failure 定义为 process-fatal，由 OS 回收 leaked 资源 |
| INC-034 | `build_options` 把 4 个 disk format 字段（`value_class_sizes` / `lba_size` / `value_data_area_base` / `value_data_area_end`）暴露为 runtime config，但它们是 disk format 决定，一旦写数据就锁死不能改；当前还有"传空 → silent disable" 等 fail 路径 | audit/runtime.md R2 | step 017 已完成：新增 `format/format_profile.hh` 作为 bootstrap disk-format 单一来源，从 `build_options` / `start_options` 移除 4 个 disk-format 字段；标准 `build_runtime()` 总是构造 value scheduler，并对 device/profile 不匹配做 build 阶段 fail-fast |
| INC-036 | `cache_concept`（page_cache.hh）value 类型是 raw `char*`，缺 `pin_count` 概念、缺 frame state machine（dirty_append/dirty_hole_fill/writeback_inflight/clean_readonly），跟 spec RMC §6.1 + §11 期望的 `lru_or_clock<frame_id, page_frame*>` + frame state 不符；与 INC-016 (buffer ownership / DMA 替换) 是正交两层 | audit/core.md C1 | step 018 已完成 readonly frame cache core：新增 `memory/frame.hh` 的 `frame_id` / `frame_state` / `page_frame` / `frame_pin`；`cache_concept` 从 raw `char*` / `get()` 迁到 `frame_id -> page_frame*` + `pin()` / `put(page_frame*)` / `drain_one()`；clock/slru 改为 pin-aware eviction，same-key replacement 也拒绝替换 pinned old frame；tree/value readonly cache 已迁到 `page_frame*`。step 019 完成 value resident frame state：新增 `value_page_frame`，value write/read 路径改为 `open_frames_`（dirty / writeback_inflight active frame）+ `allocatable_frames_`（clean_allocatable partial frame）+ readonly cache（clean_readonly full page）的显式状态机 |
| INC-037 | `slru_cache::evict_one` 从 protected 段 evict 时不重置 `in_protected`，`free_node` 也不重置；recycled node 带 stale `in_protected==true` 进 free list，下一次 alloc_node + link_probation_head 后 get() 会走错 `if (n.in_protected)` 分支调 `unlink_protected` 操作 probation 列表的节点 → 破坏 protected 列表指针 + 错误递减 prot_size_；当前不可达（evict_one 仅 teardown 用），但 latent + AI 抄走会扩散 | audit/core.md C2 | step 009 已完成：`slru_cache::free_node()` 清 `in_protected=false`，`alloc_node()` 增加 `assert(!in_protected)`，把 stale state 与不变量一起收紧 |
| INC-038 | `cache_concept::evict_one` 命名跟语义漂移：clock_cache 走 `index_.begin()` 非确定性（不按 clock policy），slru_cache 走 probation tail 后 protected tail（LRU 序），page_cache.hh 注释说 "teardown drain" 但名字像 "按 policy evict 一个"；concept 没规范，AI 给新 cache 实现 evict_one 时会两边各自漂 | audit/core.md C3 | step 018 已完成：`evict_one` 全面 rename 为 `drain_one`（cache_concept + clock_cache + slru_cache + tree/value use site），`page_cache.hh` 明确 `drain_one()` 是 teardown-only drain，不承诺 policy victim；runtime eviction 语义只保留在 `put()` 内部 |
| INC-039 | tree lookup inflight 仍是 `loading_pages_ + waiters_head_` 的简化模型，没对齐 spec RSM §4.7 的 `inflight_reads` single-flight 结构 | audit/tree.md F9 + design_doc/runtime_state_machine.md §4.7 | step 013 已完成：tree lookup 的 inflight bookkeeping 改成按 `paddr` 聚合的 `inflight_reads_` single-flight map，用 `wait_gen + wake_enqueued` 处理多页等待、旧注册失效和重复唤醒，不再扫描全局 waiter 链 |
