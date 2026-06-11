# 045 — M01-M06 Review 修正:Trailer 位置裁决、Front WAL 并发门、分层与热路径收敛

> 本文是对 M01-M06(039-044)实现 review 的修正设计,是后续实现的唯一契约。
> Review 结论分两类:F 系列(正确性/契约/分层缺陷)与 P 系列(热路径成本与
> 结构质量)。本文对每一项给出唯一裁决与实现设计;实现 agent 不得在本文之外
> 自行选择替代方案。若实现中发现本文设计无法成立,必须停下报告,不得就地改设计。
>
> 实现以阶段(Phase A-G)推进,每个 Phase 结束必须全量构建并跑绿
> M01-M06 全部测试 target,才能进入下一 Phase。

## 1. 范围与产物

涉及代码:

1. `apps/inconel/core/batch_carrier.hh`
2. `apps/inconel/core/memtable.hh`
3. `apps/inconel/core/wal_stream.hh`(瘦身)
4. `apps/inconel/front/wal_append.hh`(新增)
5. `apps/inconel/front/scheduler.hh`
6. `apps/inconel/front/sender.hh`
7. `apps/inconel/coord/scheduler.hh`
8. `apps/inconel/wal/scheduler.hh`
9. `apps/inconel/write_path/sender.hh`
10. `apps/inconel/core/owner_callback.hh`(新增)

涉及文档(逐字修订见 §5.A):

1. `design_doc/on_disk_formats.md` — 不改(它是对的,作为裁决基准)
2. `design_doc/design_overview.md` §11.3 — 措辞修正
3. `design_doc/runtime_state_machine.md` §3.9 — 措辞修正
4. `design_doc/write_path_and_pipeline.md` §8.3 — 语义补充
5. `design_doc/code_modules.md` — core/front 模块表
6. `design_doc/cross_doc_contracts.md` §1 — WAL handle 行刷新
7. `plan/044_wal_append_prepare_bounded_fua_design.md` §3/§8.4 — 裁决回填

涉及测试(实现 agent 在这些文件上以**测试维护者**角色工作,允许读改;
枚举之外的测试文件禁止打开,见 §8):

1. `apps/inconel/test/test_m06_front_wal_append_prepare.cc`
2. `apps/inconel/test/test_m04_wal_space_scheduler.cc`
3. `apps/inconel/test/test_m03_coord_scheduler_assign_publish_release.cc`
4. `apps/inconel/test/test_m05_front_scheduler_memtable_owner.cc`(当 §5.F/§5.G
   改动触发编译/断言失败时,按声明意图机械适配。追认记录 2026-06-11:初版
   只写了 §5.G,Phase F 回调单通道签名变更必然波及其白盒 req 用例;实施时
   的机械适配(断言意图零变化)经终审复核后追认为本条授权范围)

明确不做:M07+ 的任何功能、value 模块改动、recovery 实现、M08/M09 pipeline、
runtime builder、`memory/frame.hh` 的容器类型变更(§5.C.7 标注的可选项除外,
且该可选项默认**不做**)。

## 2. 发现总表与裁决索引

| ID | 问题 | 类型 | 裁决 | Phase |
|----|------|------|------|-------|
| F1 | sealed trailer 写在 write_offset,与 ODF §3.4 / RW §3 / 042 §11.4 冲突(044 §8.4 引入,未记录) | 格式契约 | **文档为准**:trailer 写段尾固定 `usable_end` 区;改代码 + 回填 044;OV/RSM 残留措辞同步清理 | A + C |
| F2 | front 单 pending plan,第二个 prepare 抛错;M06 拆三段后 WP §8.3 的天然串行性消失,并发 batch 撞同 front 会假失败;rotation 窗口(needs_segment→install)同样是多回合临界区 | 并发语义 | front owner 增加 **WAL gate 状态机 + pending prepare FIFO**(plan 粒度串行);install 双重防护;WP §8.3 补充段内 batch 可按 plan 交错 | A + D |
| F3 | `core/wal_stream.hh`(L0)include `memory/`(L1),分层倒置且未记录 | 模块分层 | 文件切分:core 只留 wal/front 共享的纯 segment/geometry 类型;append 侧(stream_state/plan/frame/error)迁 `front/wal_append.hh`;namespace 维持 `apps::inconel::wal` | B |
| F4 | coord assign 对同一输入 4 次全量扫描 + 2 次 ops vector 分配(`validate_assign_input` 全量解析只为 op_count;`client_batch_view::parse` 自身验证/构建双遍) | 热路径 | 单次解析:view 单遍构建;解析结果缓存进 req;`build_batch_ctx` 提供吃现成 view 的重载 | E |
| F5 | **跨线程 pool 归还竞态(本轮新发现)**:plan 持有的 frames 在 write_path context 中析构,而 pop_context 运行在 NVMe 完成回调所在线程 → `pooled_frame_ptr::reset → lba_dma_page_pool::put_frame` 在非 owner 线程改 `free_pages_`(普通 vector),与 front 线程并发 `get_page` 竞争。单核测试掩盖,多核必竞态 | 并发正确性 | frames 所有权随 commit/abort 请求**回front线程销毁**;wal_frame_write 改为按值内嵌 frame,消除堆对象 | C |
| P1.2 | `segment_base_paddr` 每页调用且内部全套 `validate_segment_geometry`(每页 2+ 次运行时除法);pages vector 无 reserve | 热路径 | stream_state 在 install 时缓存 segment base/lba_shift;热路径零重复校验;reserve | C |
| P1.3 | 每页无条件 zero_fill 4K;trailer/tail 多余镜像拷贝 | 热路径 | 按覆盖规则零化:仅首页未覆盖前缀(无 tail 镜像时)与末页未覆盖后缀;中间整覆盖页不 memset | C |
| P2.4 | 每请求 cb+fail 两个 `move_only_function`,捕获 32B > libstdc++ SBO 16B → 每请求 3 次堆分配 + 4 次 refcount RMW | 请求税 | 合并单通道 `owner_outcome<T> = std::expected<T, std::exception_ptr>`;统一 callback 工厂消样板 | F |
| P2.5 | `ensure_versions_for_key` 新 key 两次 btree 全下潜 | 热路径 | `lower_bound` + hinted `try_emplace` 单下潜 | G |
| P2.6 | ready_window 运行时取模、逐位推进;RSM §2.4 点名要 word-scan | 热路径 | 窗口强制 2 的幂 + mask;`countr_one` word 扫描 | G |
| P2.7 | lba_size 任意值除法遍布 scatter/page 计数 | 热路径 | geometry 强制 `lba_size` 为 2 的幂;shift/mask | C |
| P2.8 | 每页 `new segmented_page_frame` | 热路径 | 并入 F5:frame 按值内嵌 wal_frame_write,堆对象消失 | C |
| P3.9 | 路由分组用 `absl::btree_map` | 热路径 | (owner, index) 对 + stable_sort 平铺 | E |
| P3.10 | canonicalization 双排序(其一按 key 字符串比较) | 热路径 | 按 WP §3.2:`flat_hash_map` last-pos + 单次按 position 排序 | E |
| P3.11 | 每次 prepare 重跑全量 entry_indices 校验 O(K×n);fragment 每 plan 按值复制 | 热路径 | 索引在消费点就地校验(总量 O(n));prepare/insert 改借用 `const front_fragment*`,生命周期契约同 entries span | C |
| P3.12 | 每次 lookup 对 snapshot 每 gen 做 stride 反解(div);M04 duplicate-sealed 线性扫描被前置校验证明不可达 | 防御收敛 | lookup 路径只留 owner 等值检查;duplicate 扫描删除并就地写不可达证明 | G |
| P3.13 | write_path 循环每迭代 `just(bool)>>visit()` 只为一次性空 fragment 退化情形 | 结构 | 空 fragment 挡循环外;step 去 variant 化 | C |
| P3.14 | front 的 pending_wal_tail_ 与 stream_state::pending_ 双轨,plan_id 校验两层重复;config 默认 4/4 与 044 建议 16/32 不符 | 抽象边界 | tail 镜像并入 wal_stream_state;默认 16/16 | C |
| m5 | `update_lsn_bounds` 在插入前执行,apply 异常会污染 min/max | 卫生 | 移到插入成功后 | G |
| m6 | coord `pending_assigns_` 无界,与 wal 侧有界不一致 | 卫生 | 显式容量(默认 = ready_window_size),溢出走 pre-LSN fail 回调(RSM §2.7 反压挡 pre-lsn) | E |

## 3. 已对照输入

1. `design_doc/on_disk_formats.md` §3(裁决基准)
2. `design_doc/recovery_and_wal_reclaim.md` §3(trailer 消费方)
3. `design_doc/design_overview.md` §11
4. `design_doc/runtime_state_machine.md` §2.4、§3.5、§3.9、§5
5. `design_doc/write_path_and_pipeline.md` §3、§8
6. `design_doc/code_modules.md`、`code_quality_standard.md`、`cross_doc_contracts.md`
7. `plan/041/042/043/044` 设计
8. 当前实现:§1 列出的全部代码文件
9. libstdc++ 事实:`move_only_function` SBO = `2*sizeof(void*)`(funcwrap.h);
   `std::atomic<shared_ptr>` 非 lock-free(影响见 §10 watch-item,本文不改)

## 4. 裁决记录

### 4.1 F1:trailer 位置以 ODF/RW/042 为准

044 §8.4 "trailer 写在 committed write offset" 是错误裁决,本文废止该句。理由:

1. trailer 的唯一存在价值是 recovery 免扫描定界(OV §11.6);写在 write_end
   处则只有扫完 entries 才找得到它,而那时它已无信息量。固定位置是 hint
   成立的必要条件。
2. 固定位置零额外成本:TRAILER_RESERVED 反正保留,两种方案都是一次单页 FUA;
   且 `usable_end` 必 LBA 对齐(trailer_reserved 按 lba 对齐、segment 按 lba
   对齐),trailer 永远独占页,**不再需要 tail 镜像重建**,实现反而更简。
3. RSM §3.9 / OV §11.3 的"如有剩余空间/如有机会"是预留空间模型下的病句
   (空间永远有),是 044 被带偏的源头,本文一并修正措辞。

`trailer.write_end` **字段值**维持 = 最后 entry 之后的偏移(当前实现已正确),
只改字节落盘位置。

### 4.2 F2:front owner 提供 plan 粒度串行化,不把约束推给 M08/M09

M06 把 WAL fragment 拆成 prepare/issue/commit 多回合后,"front 队列天然串行"
不再成立。把串行化责任写成 M08/M09 的注意事项是地雷;按 042 对 pending FIFO
的同类裁决("必须现在设计实现,不得延后"),本文要求 front owner 自带 WAL gate:

1. 任何时刻 front 的 WAL 面处于 `idle / plan_pending / awaiting_segment` 之一。
2. 非 idle 时到达的 prepare 进 owner-local FIFO,**不报错**;idle 后按序唤醒。
3. `pending_plan_exists` 错误仅保留给同步 `*_for_testing` 路径(对应 042 §13.3
   的 test-only nonblocking 先例)。
4. 由此产生的段内交错:不同 batch 的 entries 可能按 plan 粒度交错进同一
   segment。recovery 契约(OV §11.2 约束 4:按 `lsn + entry_count` 重组,不按
   segment 解释 batch 边界)本就覆盖该形态;WP §8.3 的描述性句子按 §5.A.4 修订。

### 4.3 F3:切文件不切 namespace

core 只保留 wal 与 front 两个 L2 模块都需要的纯类型(无 memory 依赖);
append 侧全部迁入 `front/wal_append.hh`。namespace 统一维持
`apps::inconel::wal`(044 已裁决其为领域命名空间,非模块命名空间),调用点零改名。

### 4.4 F5:frame 生命周期闭环到 front 线程

`lba_dma_page_pool` 是 owner-local 非线程安全结构,这是设计本意,不加锁
(PUMP 禁止项)。修复方向是让 frames **只在 front 线程死亡**:commit/abort
请求随身携带 plan.writes(move),front handler 处理完状态后让 req 析构,
pages 在 owner 线程归池。FUA 期间 frames 由 context 中的 plan 钉住,
`all()` 等到所有飞行写完成(含异常元素)后才移出 writes,无悬空窗口。

顺带裁决 P2.8:`wal_frame_write` 从 `pooled_frame_ptr<heap 对象>` 改为
**按值内嵌** `segmented_page_frame` + `pool*`,move 时重指 desc.frame。
每页一次 `new/delete` 整体消失。`memory/frame.hh` 不改。

## 5. 设计详述

### Phase A — 文档修正(纯文档,先行落地)

逐字修订,改完按 cross_doc_contracts 检查出现点:

1. **`044 §8.4` 第 2 条**
   原:"trailer 写在 committed write offset。"
   改:"trailer 写入段尾固定 `TRAILER_RESERVED` 区(起始 `usable_end =
   wal_segment_size - TRAILER_RESERVED`);`trailer.write_end` 字段记录最后一条
   committed entry 之后的字节偏移。位置裁决见 045 §4.1。"
   并在 **044 §3 末尾追加 3.5**:"sealed trailer 位置:044 初版与 ODF §3.4 /
   RW §3 / 042 §11.4 冲突且未记录;045 裁决以 ODF 为准,本节为更正记录。"

2. **`design_overview.md` §11.3 换段时第 2 条**
   原:"如有机会,补写 sealed trailer。"
   改:"在段尾 `TRAILER_RESERVED` 固定区写 sealed trailer(仅为 recovery
   hint,其缺失不影响已 FUA entries 的持久性)。"

3. **`runtime_state_machine.md` §3.9 `seal_current_segment` 第 3 步**
   原:"写 sealed trailer(如有剩余空间):"
   改:"在段尾 `TRAILER_RESERVED` 固定区写 sealed trailer:"

4. **`write_path_and_pipeline.md` §8.3 末尾追加一段**
   "front WAL append 按 plan 粒度串行(见 044/045)。同一 front 上不同 batch
   的 entries 可能按 plan 交错出现在同一 segment 内;这不改变 recovery 契约
   ——重组只按 `lsn + entry_count`(概要 §11.2 约束 4),段内顺序不承载语义。"

5. **`code_modules.md`**
   - core/ 域对象表追加一行:
     `WAL segment carrier | segment_id, wal_segment_state, segment_runtime, segment_alloc_entry, sealed_segment_info, segment_geometry + 几何 helpers | wal(分配/回收), front(append), recovery(future)`
   - front/ 节追加:"`front/wal_append.hh` 为 front 内部组件头(WAL stream
     state、append plan、frame write carrier、append error),经
     `front/sender.hh` 间接可达;其余模块不得直接 include。该文件允许依赖
     `memory/`(L2 依赖 L1,合法)。"

6. **`cross_doc_contracts.md` §1**
   `write_wal_entries` 行尾追加备注:"M06 起由 front
   `prepare_wal_fragment / install_wal_segment / commit_wal_plan /
   abort_wal_plan` + L3 `write_path::write_wal_fragment` 实现(044/045);
   本行保留为概念签名。"

### Phase B — F3 文件切分(机械移动,无语义变化)

1. `core/wal_stream.hh` 保留(namespace `apps::inconel::wal` 不变):
   `kMaxSupportedWalKeyBytes/kMaxSupportedWalEntrySize`、`segment_id`、
   `wal_segment_state`、`segment_runtime`、`segment_alloc_entry`、
   `sealed_segment_info`、`segment_geometry`、`align_up_u32`、
   `trailer_reserved_bytes`、`validate_segment_geometry`、
   `segment_usable_end_offset`、`segment_base_paddr`。
   **删除** `#include "../memory/dma_page_pool.hh"` 与 `batch_carrier.hh`
   include(后者本就未被剩余类型使用则删,有使用则保留并说明)。
2. 新建 `front/wal_append.hh`(namespace 仍 `apps::inconel::wal`),迁入:
   `wal_append_config`、`validate_wal_append_config`、`wal_fragment_cursor`、
   `wal_plan_kind`、`wal_frame_write`、`wal_append_plan`、`wal_prepare_ready`、
   `wal_prepare_needs_segment`、`wal_prepare_result`、
   `wal_append_error_reason`、`wal_append_error`、`wal_stream_state`。
   include:`core/wal_stream.hh`、`format/wal.hh`、`memory/dma_page_pool.hh`。
3. `front/scheduler.hh` include `./wal_append.hh`;`wal/scheduler.hh` 维持
   include `core/wal_stream.hh`(验证其不再触及 append 侧类型)。
4. `write_path/sender.hh` 经 `front/sender.hh` 链可达全部类型,不直接
   include 新头。
5. 本 Phase 禁止任何行为变化;全部测试**断言**不改而绿。
6. 测试随迁适配(裁决记录,2026-06-11):`test_m04_wal_space_scheduler.cc`
   中 042 时代的 `wal_stream_state` 系合约测试直接使用 append-side 类型
   (`wal_stream_state`/`wal_append_plan`/`wal_plan_kind`),原先经
   `wal/sender.hh` 传递可见;类型迁移后,允许且仅允许在该文件追加
   `#include "../front/wal_append.hh"` 作为机械跟随,断言与测试体零改动。
   include 适配不视为行为变化。禁止用 `wal/scheduler.hh` 反向 include
   front 头来回避此适配(那会破坏 F3 分层)。

### Phase C — WAL append 路径重做(F1 代码、F5、P1.2/1.3、P2.7/2.8、P3.11/3.13/3.14、m1)

这是核心 Phase,按以下结构落地。

#### C.1 `wal_stream_state` 重做(front/wal_append.hh)

新增/变更职责:tail 镜像并入(P3.14)、install 缓存几何(P1.2)、
kind-aware 两阶段(F1)。

```text
class wal_stream_state {
  // 既有: stream_id_, geometry_, active_seg_, write_offset_,
  //        trailer_reserved_bytes_, header_committed_, seg_min/max_lsn_, pending_
  // 新增缓存(install 时计算一次):
  uint32_t lba_shift_;            // log2(lba_size), geometry 校验保证 pow2
  format::paddr segment_base_;    // segment_base_paddr(geometry, seg->id)
  // tail 镜像(自 front_sched 迁入):
  std::vector<char> committed_tail_page_;   // 大小 = lba_size, 构造时分配一次
  bool     committed_tail_valid_ = false;
  uint64_t committed_tail_index_ = 0;
  std::vector<char> pending_tail_page_;     // 同上, 复用缓冲
  // pending_plan 内新增: bool pending_tail_valid_; uint64_t pending_tail_index_;
  //                      bool clear_tail_on_commit_;
};
```

接口变更:

1. `install_segment(seg)`:新增前置 `active_seg_ == nullptr`(违例
   `std::logic_error`,见 §5.D install 防护);缓存 `segment_base_` 与
   `lba_shift_`;清 tail 状态。
2. `segment_base() const`、`lba_shift() const`:热路径取缓存,不再调用
   会重校验的自由函数。自由函数 `segment_base_paddr` 保留给冷路径/recovery。
3. `tail_image_for(page_index) -> std::optional<std::span<const char>>`:
   committed tail 命中时返回镜像。
4. `begin_pending(const wal_append_plan&, std::span<const char> last_page_bytes)`
   —— kind-aware 校验:
   - 共通:无 pending、active 非空、stream/segment/gen 匹配。
   - `header`:`!header_committed_ && start_offset == 0 && end_offset == WAL_SEGMENT_HEADER_SIZE`。
   - `entries`:`header_committed_ && start_offset == write_offset_ && end_offset <= usable_end`。
   - `trailer`:`start_offset == usable_end_offset() && end_offset == start + WAL_SEALED_TRAILER_SIZE && sealed_on_commit.has_value()`;
     其余 kind 要求 `!sealed_on_commit`。
   - tail 快照:trailer kind 置 `clear_tail_on_commit_`,不快照;
     header/entries kind 把 `last_page_bytes`(调用方传入 plan 最后一页的
     完整页镜像)memcpy 进 `pending_tail_page_`,记录其 page_index。
5. `commit_pending(plan_id)`:在现有 kind 分支基础上完成 tail 落位:
   trailer → 清 tail;header/entries → `swap(committed_tail_page_, pending_tail_page_)`
   + 置 valid/index。原 front_sched 侧的 `pending_wal_tail_` 结构与字段
   **整体删除**,plan_id 校验只剩 stream_state 一层。
6. `abort_pending(plan_id)`:丢弃 pending(含 pending tail),committed 不动。

#### C.2 `wal_frame_write` 按值内嵌(F5/P2.8)

```text
struct wal_frame_write {
  memory::lba_dma_page_pool*    pool = nullptr;
  memory::segmented_page_frame  frame;     // 按值
  memory::frame_write_desc      desc{};    // desc.frame = &frame
  // move ctor/assign: 转移后重指 desc.frame = &frame, 源 pool 置空
  // dtor: pool != nullptr 时 pool->put_frame(std::move(frame))
  // copy 禁止
};
```

约束:

1. desc.frame 指向元素自身 → **FUA 飞行期间禁止移动所属容器**。本设计中
   plan 在 pipeline context 内地址稳定直到 `all()` 完成,之后才整体 move
   writes(此时无飞行写),满足约束;在 `wal_frame_write` 注释中写明该
   不变量。
2. `make_wal_page_write` 重写:`pool->get_frame(id, dirty_append, /*zero_fill=*/false)`
   后立即构造 `wal_frame_write{pool, std::move(*frame)}`,**先取得所有权再做
   任何可能抛出的事**(消 m1 泄漏路径);分配失败仍抛
   `frame_allocation_failed`。tail 镜像/零化不在此函数做(见 C.3)。
3. id 计算用缓存 `stream.segment_base()`,无重复几何校验。

#### C.3 页覆盖与零化规则(P1.3)

prepare 各 plan 构建完成后、`begin_pending` 之前,执行一次 `finalize_pages`:

```text
finalize_pages(pages, start_offset, end_offset, stream):
  first = pages.front(); last = pages.back()
  lba = stream.lba_size()
  // 首页前缀 [page_start, start_offset%lba):
  //   若 stream.tail_image_for(first.index) 命中 → memcpy 镜像整页后,
  //     scatter 已写区间会再次覆盖?否——镜像必须在 scatter 之前铺底。
```

为保持单遍写入,规则落为:

1. `wal_page_for` 创建**首页**时:若 `tail_image_for(index)` 命中 → 整页
   memcpy 镜像铺底;否则若 `start_offset % lba != 0` → 仅零化
   `[0, start_offset % lba)` 前缀(理论上不可达:offset 非页对齐当且仅当该页
   是 committed tail 页,必有镜像;保留零化作防御并注释)。
2. `wal_page_for` 创建**任何页**时不再整页 memset。
3. plan 构建完成后零化末页后缀 `[end_offset % lba, lba)`(`end_offset % lba == 0`
   则无后缀)。中间页被 scatter 全覆盖,不动。
4. trailer plan 独占页:无镜像、零前缀(start 必页对齐)、零化 33 字节后的后缀。
5. `begin_pending` 的 `last_page_bytes` 快照在零化完成后取,保证镜像 = 实际落盘页。

#### C.4 trailer 位置(F1 代码)

`prepare_wal_trailer_plan`:

```text
write_end_field = stream.write_offset()          // 字段语义不变
disk_pos        = stream.usable_end_offset()     // 落盘位置 = 段尾固定区
trailer = make_wal_sealed_trailer(gen, write_end_field, min, max)
scatter_to_wal_pages(pages, disk_pos, trailer_bytes)
plan.start_offset = disk_pos
plan.end_offset   = disk_pos + WAL_SEALED_TRAILER_SIZE
plan.sealed_on_commit = sealed
```

trailer 页与 entries 页永不重叠(usable_end 页对齐),断言之。

#### C.5 prepare 校验收敛(P3.11)与借用 fragment

1. 删除 `validate_wal_fragment_request` 中的全量 `entry_indices` 循环;
   保留 owner/entry_count/cursor 标量检查。索引越界改为在 entry 规划循环中
   消费点检查(越界 → `fragment_entry_index_out_of_range`,此时尚未
   `begin_pending`,局部 pages 析构自动归池,无状态损伤)。
2. `prepare_wal_fragment` 与 `insert_memtable_entries` 的 sender/op/req 把
   `core::front_fragment` 改为 `const core::front_fragment*` 借用,生命周期
   契约与 `canonical_entries` span 完全相同(由 batch_ctx / write_path
   context 钉住),在 sender 注释处写明。`front/sender.hh` facade 签名改为
   `const core::front_fragment&`。

#### C.6 scatter 除法消除(P2.7)

1. `validate_segment_geometry` 新增 `std::has_single_bit(lba_size)`,违例
   `std::invalid_argument`(真实设备 512/4096 均满足;测试几何同步检查)。
2. `scatter_to_wal_pages` / `unique_page_count_after` 用
   `stream.lba_shift()` 移位与 `lba-1` mask 取代 `/ %`。

#### C.7 F5 所有权回流链(write_path/sender.hh + front commit/abort)

`wal_plan_issue_result` 增加 `std::vector<wal::wal_frame_write> writes`
(整结构变 move-only)。链重排:

```text
issue_and_finish_wal_plan(front, nvme, plan):
  just() >> push_context(std::move(plan))
  >> get_context<wal_append_plan>()
  >> flat_map([nvme](wal_append_plan& p) {
       // p 是 context 引用,FUA 全程地址稳定
       return nvme::write_frame_range_bounded_fua(nvme, span(p.writes), p.config.max_fua_inflight, ...)
         >> then([&p, scalars](bool ok) {
              return wal_plan_issue_result{scalars..., ok, {}, std::move(p.writes)};
            })
         >> any_exception([&p, scalars](std::exception_ptr ep) {
              // all() 已等齐全部飞行元素(异常元素归 false),此刻 move 安全
              return just(wal_plan_issue_result{scalars..., false, std::move(ep), std::move(p.writes)});
            });
     })
  >> flat_map([front](wal_plan_issue_result&& r) {
       return finish_wal_plan_after_issue(front, std::move(r));  // writes 随 r 走
     })
  >> pop_context();
```

`finish_wal_plan_after_issue`:commit/abort 分支把 `issue.writes` move 进
对应 req。`_front_wal_commit::req` / `_front_wal_abort::req` 各增
`std::vector<wal::wal_frame_write> writes` 字段;handler 完成状态操作后让
req 析构 → frames 在 front 线程归池。`*_for_testing` 同步路径不带 frames
(测试自持 plan,单线程,无竞态),注释标明 production 契约。

#### C.8 循环简化(P3.13)

`write_wal_fragment`:空 `entry_indices` 在 push_context 前直接
`return just() >> then([]{return true;})` 等价短路;`write_wal_fragment_step`
删除 `just(bool) >> visit()` 外壳,直接以 `front::prepare_wal_fragment(...)`
开链(循环生成器已保证 done 后不再进入 step)。

#### C.9 默认值(P3.14)

`wal_append_config` 默认改 `{max_fua_inflight = 16, max_pages_per_plan = 16}`;
prepare 的局部 `pages` 容器 `reserve(config.max_pages_per_plan)`(单条
oversized entry 允许超出,自然增长)。

### Phase D — F2:front WAL gate 与 pending prepare FIFO

#### D.1 状态机

```text
            +--------- commit/abort ----------+
            v                                 |
  IDLE --prepare(ready)--> PLAN_PENDING ------+
   |  \
   |   +--prepare(needs_segment)--> AWAITING_SEGMENT --install--> IDLE
   |                                                              |
   +<-------------- prepare(validation error, 状态不变) ----------+
```

front_sched 新增:

```text
bool wal_awaiting_segment_ = false;
std::deque<_front_wal_prepare::req*> wal_pending_prepares_;   // 显式容量
std::size_t wal_pending_prepare_capacity_;                    // ctor 参数,默认 = queue_depth
bool wal_busy() const { return (wal_ && wal_->has_pending_plan()) || wal_awaiting_segment_; }
```

#### D.2 行为

1. `handle_wal_prepare`:`wal_busy()` → 入 FIFO(不回调、不报错);容量满 →
   通过 fail 通道返回 `wal_append_error{prepare_queue_full}`(新增 reason)。
   非 busy → 执行 `_now`;结果为 `needs_segment` → 置 `wal_awaiting_segment_`;
   `ready` → stream 已 pending(busy 由 has_pending_plan 表达)。
2. `handle_wal_install`:生产路径要求 `wal_awaiting_segment_ == true`
   (否则 `std::logic_error` fail-fast);成功后清标志、执行 install、
   `drain_wal_pending_prepares()`。
3. `handle_wal_commit` / `handle_wal_abort`:状态操作成功后
   `drain_wal_pending_prepares()`。
4. `drain_wal_pending_prepares()`:`while (!fifo.empty() && !wal_busy())`
   取队首执行 prepare 逻辑(校验失败 → fail 回调,继续 drain;ready/needs_segment
   → busy,停止)。回调在状态提交后调用,沿用既有纪律。
5. `prepare_wal_fragment_for_testing` 维持同步语义:busy 时抛
   `pending_plan_exists`(对应 042 §13.3 test-only nonblocking 先例),注释标明。
6. `install_wal_segment_for_testing` 绕过 awaiting 标志(测试 boot 安装),
   但仍受 C.1 的 `active_seg_ == nullptr` 防护。
7. 不变量与上层耦合写入注释与 044 相邻事项:**单 front 并发 WAL fragment 数
   ≤ wal_pending_prepare_capacity**;M08/M09 设计 in-flight 上限时必须对照
   (WP §8.5 窗口反压是全局上限,本容量是 per-front 上限)。
8. 析构清理 FIFO 中残留 req。

#### D.3 正确性论证(写入代码注释)

1. 串行化单元 = plan;同一时刻至多一个 pending plan 或一个 awaiting 窗口,
   消除双 alloc/install 冲突。
2. rotation 期间他人 prepare 排队;install 后被唤醒者可能先写 header plan,
   原 rotation 发起者随后继续——entries 跨 batch 交错,recovery 按
   `lsn + entry_count` 重组,无语义依赖段内顺序(§4.2)。
3. pipeline 死亡导致 plan 永不 commit/abort → front WAL 面 wedge:与
   pending-plan 模型固有风险一致,属 runtime fatal 类,不在本文解决。

### Phase E — coord/M01 解析与分组(F4、P3.9、P3.10、m6)

1. **`client_batch_view::parse` 单遍化**:边构建 `ops_` 边校验,任何错误抛
   `std::invalid_argument`(view 弃用,无半成品可见);保留 record-count
   预检(`count > (size-4)/kRecordHeaderBytes`)防 reserve 放大。
2. **req 缓存解析结果**:`_coord_assign::req` 增 `std::optional<core::client_batch_view> parsed`。
   `handle_assign` 构造 view 一次(失败 → fail 回调);成功后存入 req。
   pending 路径安全性:req 以指针存放,对象不动,view 的 span 指向
   `req->input.bytes` 堆缓冲,稳定。
3. **`build_batch_ctx` 新重载**
   `build_batch_ctx(client_batch_buffer&& input, client_batch_view&& parsed, uint64_t lsn, uint32_t front_count)`
   ——契约:`parsed` 必须由该 `input` 生成(注释写明,由 coord 唯一调用点保证);
   内部不再调用 `input.view()`。原重载保留并改为转调(解析一次)。
   `try_complete_assign` 走新重载,消第二次解析。
4. **canonicalization 重写(P3.10,行为不变)**:
   ```text
   absl::flat_hash_map<std::string_view, uint32_t> last;   // key -> 最大 original_position
   last.reserve(ops.size());
   for i in ops: last[ops[i].key] = i;                      // 后写覆盖
   keep = values(last); sort(keep);                          // 单次整数排序
   ```
   输出与现算法逐字节等价(same-key last-writer-wins、按 winning position
   升序);M01 既有测试必须不改而绿,作为等价性证据。
5. **owner 分组平铺(P3.9)**:
   ```text
   absl::InlinedVector<std::pair<uint32_t,uint32_t>, 64> route; // (owner, canonical_index)
   for i in canonical: route.push_back({key_hash(key) % front_count, i});
   std::stable_sort(route, by owner);                            // index 已升序,stable 保持
   按 owner 连续段切 fragments
   ```
   路由函数 `key_hash`(FNV)不变——M01 测试断言 `owner == key_hash % front_count`,
   不得更换哈希。
6. **m6**:`coord_sched` ctor 增 `pending_assign_capacity`(默认 =
   `ready_window_size`);`handle_assign` 入队前检查,满 → fail 回调携带
   `std::runtime_error("coord assign backpressure overflow")`。这是 pre-LSN
   reject,符合 RSM §2.7(反压挡在 assign 之前),不产生 LSN hole。

### Phase F — P2.4:owner 回调单通道

1. 新建 `core/owner_callback.hh`:
   ```cpp
   template <typename T>
   using owner_outcome = std::expected<T, std::exception_ptr>;   // T 可为 void

   // 统一 op 回调工厂:消 11 处样板,closure 从 2 个降为 1 个
   template <uint32_t pos, typename scope_t, typename T, typename ctx_t>
   auto make_owner_pusher(ctx_t& ctx, scope_t& scope) {
       return [ctx = ctx, scope = scope](owner_outcome<T>&& r) mutable {
           if (r.has_value()) {
               if constexpr (std::is_void_v<T>)
                   pump::core::op_pusher<pos + 1, scope_t>::push_value(ctx, scope);
               else
                   pump::core::op_pusher<pos + 1, scope_t>::push_value(ctx, scope, std::move(*r));
           } else {
               pump::core::op_pusher<pos + 1, scope_t>::push_exception(ctx, scope, std::move(r.error()));
           }
       };
   }
   ```
2. coord/wal/front 全部 req 的 `cb + fail` 双字段合并为单
   `std::move_only_function<void(owner_outcome<T>&&)> cb`;handler 成功路径
   `cb(owner_outcome<T>(...))`、失败路径 `cb(std::unexpected(ep))`。
   **纪律不变**:state commit 之后才调成功回调;失败回调前无状态变更;
   callback 异常照旧传播出 advance(),req 由 unique_ptr 兜底。
3. 每模块逐 op 机械替换;op::start 内联 `make_owner_pusher`。
4. 收益核算(写入提交说明):每请求堆分配 3 → 2,shared_ptr RMW 4 → 2,
   front/scheduler.hh 样板预计 -300 行以上。

### Phase G — 微优化与防御收敛(P2.5/2.6、P3.12、m5)

1. **P2.5** `ensure_versions_for_key`:
   ```cpp
   auto it = gen.table.lower_bound(key);
   if (it != gen.table.end() && it->first == key) return it->second;
   const auto arena_key = gen.kv_arena.allocate(key.data(), key.size());
   it = gen.table.try_emplace(it, arena_key);   // hinted, 单下潜
   return it->second;
   ```
2. **m5**:`insert_value` / `insert_tombstone` 把 `update_lsn_bounds` 移到
   `versions.push_back(...)` 成功之后。
3. **P2.6** `ready_window`:
   - ctor:`std::has_single_bit(window_size)` 必须成立,否则
     `std::invalid_argument`;存 `mask_ = window_size - 1`。
   - `bit_index = lsn & mask_`。
   - `advance_contiguous_prefix` word 扫描:
     ```text
     loop:
       idx = lsn & mask_; w = idx >> 6; b = idx & 63;
       run = std::countr_one(bits_[w] >> b);
       take = min(run, 64 - b);
       if take == 0: break
       bits_[w] &= ~(((take == 64 ? ~0ull : ((1ull << take) - 1)) << b));
       lsn += take; base_lsn_ = lsn;        // base 与消费前缀同步
       if take < 64 - b: break              // run 在 word 内中断
     return lsn - 1 if advanced else scan_from
     ```
     注意 window 环回:下一迭代 idx 重新按 mask 计算,自然换 word;
     `mark_resolved` 的窗口界检查保证不会消费到未分配区。
4. **P3.12a**:`validate_snapshot_owner` 拆出
   `validate_snapshot_owner_fast`(仅 `front_owner_index == owner_id_`
   等值比较),lookup/batch_lookup/scan 走 fast 版;stride 反解保留在
   construction/seal/collect/release 冷路径。
5. **P3.12b**:删除 `wal_space_sched::sealed_segment_already_recorded` 及其
   调用,原位置写不可达证明注释:"重复 seal 同一 {id,gen} 必先撞
   `slot.st != active`(已 sealed)或 `segment_gen` 不匹配(已带新 gen 复用),
   故 duplicate 检查被状态机前置校验完全覆盖。"对应 M04 测试若依赖
   duplicate 专属错误文案,改断言为状态机错误(意图不变:重复 seal 必
   fail-fast)。

## 6. 容量与热路径预算(改后)

新增 runtime carrier 估算(10 亿 KV 基线):

| Carrier | 容量 | 内存 |
|---|---|---|
| `wal_pending_prepares_` | queue_depth(默认 1024)× 8B 指针/front | 8KB/front |
| stream_state tail 双缓冲 | 2 × lba_size(自 front_sched 迁移,非新增) | 8KB/front |
| coord `parsed` 缓存 | 每 in-flight assign 一个 view(ops vector,本就要建) | 0 新增 |

改后热路径预算(对照 review 基线):

| 路径 | 改前 | 改后 |
|---|---|---|
| coord assign / batch | 4 次扫描、~8 堆分配、2 排序(1 次字符串比较) | 1 次扫描、~5 堆分配、1 次整数排序 + 1 次 hash 表 |
| WAL entries plan(P 页) | P+3 堆分配;P 次 memset + ≤2 次 4K copy;每页 2+ 除法 | **0 frame 堆分配**;≤1 次镜像 copy + 1 次快照 copy + 末页后缀零化;0 除法(shift/mask) |
| 每 owner 请求 | 3 堆分配 + 4 RMW | 2 堆分配 + 2 RMW |
| memtable 新 key insert | 2 次 btree 下潜 | 1 次 |
| memtable hit GET | 0 alloc 0 copy(达标) | 不变,且每 gen 少 1 次 div |

## 7. 错误语义增量

1. 新增 `wal_append_error_reason::prepare_queue_full`(per-front 并发超容,
   pre-memtable,上层 release 路径可用)。
2. `install_wal_segment` 在已有 active segment 时 `std::logic_error`
   fail-fast(防泄漏 ACTIVE lease)。
3. 生产 install 未处于 awaiting_segment 窗口 → `std::logic_error`。
4. coord assign pending 溢出 → fail 回调(pre-LSN,无 hole)。
5. 其余错误语义(release 边界、memtable fatal、callback 传播)不变。

## 8. 测试计划

实现 agent 对 §1 枚举的测试文件以测试维护者角色工作;每项改动列明意图,
禁止顺手重写无关断言。

### 8.1 必改(行为变更跟随)

1. `test_m06` rotation:trailer 断言改为 (a) `plan.start_offset ==
   segment_size - trailer_reserved`;(b) 从段尾固定区 memcpy 解析 trailer,
   `inspect_wal_sealed_trailer == ok` 且 `write_end == 最后 entry 之后偏移`;
   (c) 断言 trailer 页与 entries 页 index 不相交。
2. `test_m06` 涉及 `wal_write_offset_for_testing` 初值/复位的断言随 C.1
   语义核对(committed 语义未变,预期无需改;若编译失败按本条意图修)。
3. `test_m03`:若存在非 2 幂 ready window 用例,窗口值改为最近的 2 幂
   (语义断言不变);新增 ctor 拒绝非 2 幂的失败用例。
4. `test_m04`:duplicate-sealed 用例改为断言状态机错误(意图:重复 seal
   fail-fast);新增 install-over-active fail-fast 用例(经 stream state)。
5. `test_m04`(Phase B,§5.B.6):追加 `front/wal_append.hh` include,
   仅此一行,断言零改动。

### 8.2 必增

1. `m06_trailer_at_fixed_tail_region_decodes_for_recovery_view`:
   按 RW §3 的读法(段尾固定区)定位 trailer 并校验 gen/CRC/write_end。
2. `m06_concurrent_fragments_serialize_through_wal_gate`:两个 batch 的
   fragment 经 PUMP sender 并发提交到同一 front(submit 两条
   `write_path::write_wal_fragment`,交替 advance front/wal/fake_nvme);
   断言两者都成功、无 `pending_plan_exists` 异常逃逸、两个 batch 全部
   entries 可按 `lsn+entry_count` 从段字节解码重组。
3. `m06_prepare_fifo_wakes_after_rotation_install`:fragment A 触发
   rotation 进入 awaiting 窗口期间,fragment B 的 prepare 入队;install 后
   B 被唤醒,最终两 fragment 完成。
4. `m06_frames_return_to_pool_on_front_commit`:FUA 完成但 front 尚未
   advance commit 时 pool free 页数处于低位;front advance 处理 commit 后
   恢复(单线程内验证所有权回流结构)。
5. `m06_middle_pages_not_zeroed_but_suffix_zeroed`:构造跨 ≥3 页 plan,
   断言末页 `[end%lba, lba)` 全零、全部 entries decode ok(中间页正确性由
   decode 覆盖)。
6. `m06_prepare_queue_full_fails_with_explicit_reason`(容量 1 的小配置)。
7. `m03_assign_parses_input_exactly_once`:以 instrumented 输入不可行,改为
   行为等价断言 + `pending` 路径(满窗入队再唤醒)下 malformed 输入仍在
   入队前失败、合法输入唤醒后成功且只消耗一个 LSN。
8. `m03_pending_assign_overflow_fails_pre_lsn`:容量 1,溢出请求收到 fail
   且 `next_lsn` 不动。
9. `m01` 不新增不修改:canonicalization/routing 重写必须在既有断言下全绿,
   这是 P3.10/P3.9 的等价性证据。

### 8.3 全程回归门

每个 Phase 结束:`cmake --build build` 全 target +
`inconel_test_m01..m06` 六个二进制全部 PASS。Phase C/D 另跑
`build_asan` 同名 target(本仓 ASAN 配置已存在;F5 的回流链与 FIFO 清理
必须在 ASAN 下无 leak/UAF)。

## 9. 实现顺序

```text
Phase A  文档修正(无代码)
Phase B  F3 文件切分(零行为变化,全测试不改而绿)
Phase C  WAL append 重做(F1/F5/P1.2/P1.3/P2.7/P2.8/P3.11/P3.13/P3.14/m1)
Phase D  WAL gate + FIFO(F2)
Phase E  coord/M01 解析与分组(F4/P3.9/P3.10/m6)
Phase F  owner 回调单通道(P2.4,机械全模块)
Phase G  微优化与防御收敛(P2.5/P2.6/P3.12/m5)
```

依赖:C 依赖 B(文件位置);D 依赖 C(gate 基于新 stream_state);
E/F/G 相互独立,可与 review 并行排队,但每 Phase 单独成提交。

## 10. 排除范围与遗留 watch-item

1. value 模块的同型 frame 跨线程归还风险:F5 的竞态模式可能同样存在于
   value persist/read 路径,**本文不改**,登记为 M07 设计必须对照的输入
   (在 044 相邻事项处追加一行引用)。
2. `catalog_store` 的 atomic<shared_ptr> 读侧扩展性(全核竞争单 control
   block):设计层议题,留待读路径性能阶段(M10+)统一处理,不在本文动。
3. `memory/frame.hh` pages 容器改 InlinedVector(可再省 frame 内 1 次
   vector 分配):影响 tree/value 公共类型,默认不做;若实现 agent 验证
   全仓编译与 m01-m06 + flush e2e 全绿且 diff 仅容器类型,可单独成提交
   提请 review,不与本文其他 Phase 混合。
4. M08/M09 必须对照本文 §5.D.2.7 的 per-front 并发上限与
   `prepare_queue_full` → release 映射。

## 11. 需要人工判断的点

无未决项。F1 的格式裁决(以 ODF/RW/042 为准)即本文 §4.1,由委托本设计的
review 结论承担;若后续对 trailer 位置另有意见,必须先改本文与 §5.A 文档,
不得直接改代码。
