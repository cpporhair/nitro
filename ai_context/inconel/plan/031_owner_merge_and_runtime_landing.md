# Step 031 — Owner merge 协程化 + `tree_sched::advance()` 拆结构

> 本文是**实现后**的总结，记录本步把 Phase 9 owner merge 从"advance 里 fire-and-forget 一大条 nvme pipeline"重写成"resumable coroutine + 平链 outer pipeline"的全过程。改动覆盖 `tree/flush_types.hh` / `tree/owner_scheduler.hh` / `tree/sender.hh` 三个文件，涉及 seam 形状、coroutine 机制、pipeline 结构、scheduler advance 组织四个层面。

---

## 1. 动机

### 1.1 入口问题

之前 `tree_sched::advance()` 的 merge 处理把合并结果算出来之后直接在 `start_pending_write()` 里做 fire-and-forget：

```cpp
pump::sender::as_stream(writes)
    >> concurrent(kWriteBatchConcurrency)
    >> flat_map(nvme->write)
    >> all()
    >> then(nvme->flush)
    >> submit(make_root_context());   // ← 孤立 root_context，脱离外层 pipeline
```

`update_superblock_q` 里的 nvme read + write(FUA) 也是同样的 fire-and-forget pattern。两处都违反 PUMP 的基本约束（`feedback_cross_sched_pump_only`）：**advance() 内不能直接 enqueue 到其它 scheduler，跨域 I/O 必须由外层 pipeline 编排**。

### 1.2 只修封装还不够

用户在 review 里逐层加了四条更深的约束，全部必须在这一步一次解决：

1. **Owner merge 可能需要读盘**（`feedback_merge_owner_read_is_v1_core`）——`touched_old_page_for` miss 不能 panic，必须 emit read_desc 让外层 pipeline 去读。这是 v1 核心路径要求，不是 future feature。
2. **算好一批页就应该写**（用户原话："算好了页就可以写,在等nvme的时候同时计算"）——不能 CPU 全算完再一次性交 writes，否则没有 CPU-IO overlap。
3. **协程驱动状态不能读 scheduler 内部**（`feedback_coro_state_via_context`）——`owner->merge_cpu_done()` 这种查询 API 跨核 race + 破坏封装，终止条件必须挂在 outer pipeline context 的 state struct 上。
4. **Read 和 Write 不应该按 kind 串行**（用户原话："这里不应该区分读写,无论是读还是写都应该一次 nvme io 完成"）——一批 IO 应该是一条 concurrent 流投给 NVMe，不管里面是 read 还是 write。

### 1.3 顺带修掉的债

- `advance()` 528 行，7 个 queue loop 全部堆在一个函数里——不拆开没法继续迭代。
- `pending_write_state` / `start_pending_write` / `complete_pending_tree_writes` / `build_commit_tree_writes_result` / `succeed_pending_round` / `fail_pending_round` 一堆 fire-and-forget 时代的中间状态管理函数，语义被新 seam 吸收后直接删掉。
- Phase 9 merge CPU 的 `touched_old_page_for` 里的 `core::panic_inconsistency` 不是断言，是生产路径埋的雷。

---

## 2. 核心数据结构

### 2.1 `merge_coro` — 主动驱动的 C++20 协程

不是 pump 的 `return_yields<T>`，是新写的一个最小协程类型：promise_type 存一个状态码 `merge_yield`，handle 由 scheduler 的 advance() 主动 `resume()`。

```cpp
enum class merge_yield : uint8_t {
    need_io,    // state.pending_ios 已 staged，outer pipeline 去发
    yield_cpu,  // CPU 连续干了太久，让 main loop 转别的 scheduler
    done,       // merge CPU 全结束
};

struct merge_coro {
    struct promise_type {
        merge_yield         current_value = merge_yield::done;
        std::exception_ptr  unhandled;
        // ... get_return_object / initial_suspend (suspend_always) /
        // ... final_suspend (suspend_always) / yield_value(merge_yield)
    };
    // ... move-only handle wrapper with resume() / done() / current()
};
```

**协程不走 pump pipeline**。scheduler 的 merge_step handler 里直接 `coro->resume()`，当场读 `coro->current()` 就知道该发 IO 还是该回 done。这是用户明确要求的"主动触发"。

### 2.2 `merge_round_state` — 协程工作区（tree_sched 侧）

挂在 `tree_state.active_merge` 里的一个 `std::optional<>`，**单 flush 不变量保证全局只有一份**（没有 map，没有 atomic）。协程和 scheduler 都通过它传数据：

```cpp
struct merge_round_state {
    flush_round_id                                round_id;
    std::vector<worker_tree_proposal>             worker_proposals;

    std::shared_ptr<_owner::merge_context>        ctx;               // 不完整类型用 shared_ptr 绕
    absl::flat_hash_map<paddr, std::vector<char>> fetched_old_pages; // owner 读回的 bytes，NVMe 直接 DMA 进来

    child_ref                                     combined_root;
    std::shared_ptr<const core::tree_manifest>    new_manifest;
    bool                                          is_root_change = false;
    paddr                                         new_root_base_paddr;
    std::vector<format::range_ref>                allocated_ranges;
    absl::flat_hash_set<paddr>                    retired_slots_seen;
    absl::flat_hash_set<paddr>                    retired_ranges_seen;

    struct walk_frame { child_ref* ref; std::size_t next_child; };
    std::vector<walk_frame>                       walk_stack;

    std::vector<merge_io_desc>                    pending_ios;      // coro→scheduler 握手槽
    bool                                          waiting_for_reads = false;

    flush_stage_status                            st;
    std::optional<merge_coro>                     coro;
};
```

### 2.3 `merge_loop_state` — outer pipeline context（pipeline 侧）

挂在 `with_context(merge_loop_state{...})` 里的一个对象，**跨核安全**（协程终止条件放这里而不是 scheduler 内部，避免 `feedback_coro_state_via_context` 里讲的跨核 race）：

```cpp
struct merge_loop_state {
    flush_round_id                    round_id;
    std::vector<worker_tree_proposal> worker_proposals;   // first merge_step call moves out
    std::atomic<bool>                 cpu_done{false};    // iter handler 写，driver coro 读

    // custom move ctor（atomic<bool> 非-movable），不允许 copy
};
```

`concurrent(8)` 的 iter 完成时可能并发多核写 `cpu_done`，所以是 `atomic<bool>`；driver 协程 `load(acquire)`，写侧 `store(release)`。

### 2.4 `merge_io_desc` — 统一的 NVMe op carrier

```cpp
using merge_io_desc = std::variant<format::read_desc, format::write_desc>;
```

协程把 pre-scan reads 和 walking writes 都 `push_back` 到同一个 `pending_ios` vector，outer pipeline 一条 `as_stream >> concurrent(N) >> visit() >> flat_map(read-or-write)` 链就发出去。**不再按 kind 分桶，不再串行。**

---

## 3. Seam 全景

### 3.1 新增 seam

| Seam | 请求类型 | 返回 | 语义 |
|---|---|---|---|
| `submit_merge_step(merge_loop_state*)` | pointer to outer context | `merge_step_decision = variant<need_io, done>` | 第一次调进来看到 `active_merge` 空，就从 `ls->worker_proposals` move 出 payload seed + 启动协程；之后的调用 resume 协程 |
| `submit_merge_reads_done()` | 无 | 无 | reads batch await 完后外层 ack，清 `waiting_for_reads`，并发 iter 才能再 resume 协程 |
| `submit_finalize_merge({round_id, flush_ok})` | `merge_finalize_request` | `merge_finalize_result = variant<done, root_stable, root_change>` | merge loop + device flush 都完了之后，根据 flush 结果建 commit 分支；transfer `new_manifest` 到 `flush_round_state` |
| `submit_begin_update_superblock(update_superblock_request)` | | `begin_update_superblock_result` | 拆 `update_superblock` 的第一步——latches `inflight`，返回 active/inactive LBA + lba_size |
| `submit_finish_update_superblock(finish_update_superblock_request)` | | `update_superblock_result` | 拆 `update_superblock` 的第二步——清 inflight，返回外层 nvme write 的结果 |

### 3.2 删除的旧 seam

- `submit_flush_merge(flush_merge_request) → flush_merge_result{done | root_stable | root_change}` — 一次性 variant 版本
- `submit_commit_tree_writes({round_id, write_ok}) → commit_tree_writes_result` — 我第一版错误的"分两阶段"中间 seam
- `submit_update_superblock(update_superblock_request)` 单 seam — 里面 fire-and-forget nvme 的那个

### 3.3 保留不变

- `submit_flush_fold(tree_flush_request) → flush_fold_result`
- `submit_finalize_flush_round(finalize_flush_request) → tree_flush_result`

---

## 4. 外层 pipeline（`tree_local_flush`）

全链一条平串，不再嵌套子 pipeline：

```cpp
submit_flush_fold(req)
  >> flat_map([round_id](flush_fold_result&& fr) {
      return just()
          >> collect_worker_proposals(std::move(fr))
          >> flat_map([round_id](std::vector<worker_tree_proposal>&& ps) {
              return drive_merge_loop(owner, round_id, std::move(ps));
          })
          >> flat_map([]() { return nvme->flush(); })
          >> flat_map([round_id](bool flush_ok) {
              return owner->submit_finalize_merge({round_id, flush_ok});
          })
          >> visit()
          >> continue_after_finalize_merge(owner);
  });
```

### 4.1 `drive_merge_loop` — A2 并发迭代模式

```cpp
just()
  >> with_context(merge_loop_state{round_id, std::move(proposals)})([owner]() {
      return get_context<merge_loop_state>()
          >> then([](merge_loop_state& ls) {
              return pump::coro::make_view_able(drive_merge(ls));
          })
          >> for_each()
          >> concurrent(kMergeIterConcurrency)            // = 8
          >> get_context<merge_loop_state>()
          >> flat_map([owner](merge_loop_state& ls, bool) {
              return owner->submit_merge_step(&ls);
          })
          >> visit()
          >> get_context<merge_loop_state>()
          >> flat_map([owner]<typename D>(merge_loop_state& ls, D&& dec) {
              if constexpr (is_need_io<D>) {
                  return handle_merge_step_need_io(owner, __fwd__(dec));
              } else {  // done
                  ls.cpu_done.store(true, std::memory_order_release);
                  return just();
              }
          })
          >> all();
  });
```

`drive_merge` 是 `pump::coro::return_yields<bool>`，yield `true` 直到 `loop_state.cpu_done.load(acquire)` 变 true，co_return 一个 sentinel。`concurrent(8)` 让多个 iter 并行跑各自的 IO 批次，同时 tree_sched.advance() 把 merge_step 请求串行处理，iter N+1 的 merge_step CPU 能在 iter N 的 IO 还在飞时已经推进。

### 4.2 `handle_merge_step_need_io` — 统一 IO 流

```cpp
just()
  >> as_stream(std::move(io.ios))                              // vector<merge_io_desc>
  >> concurrent(tree_sched::kWriteBatchConcurrency)            // = 32
  >> visit()                                                    // variant → 编译期分支
  >> flat_map([]<typename D>(D&& d) {
      if constexpr (std::is_same_v<D, format::read_desc>)
          return nvme->read(d.lba, d.buf, d.num_lbas);
      else
          return nvme->write(d.lba, d.data, d.num_lbas, d.flags);
  })
  >> all()
  >> then([has_reads](bool) { return has_reads; })
  >> visit()
  >> flat_map([owner]<typename Flag>(Flag&&) {
      if constexpr (Flag is true_type) return owner->submit_merge_reads_done();
      else                             return just();
  });
```

Reads 和 writes 在同一个 `concurrent(32)` 流里，NVMe 一次看到 32 个并发 IO，不是先等 32 个 read all 再放 32 个 write。

### 4.3 `continue_after_finalize_merge` — commit 变体分派

```cpp
flat_map([owner]<typename T>(T&& commit_result) {
    if constexpr (is_done<T>)         return just(std::move(commit_result.result));
    if constexpr (is_root_stable<T>)  return owner->submit_finalize_flush_round(commit_result.finalize_req);
    if constexpr (is_root_change<T>)  return finalize_root_change(owner, commit_result.update_req);
});
```

其中 `finalize_root_change` 是 `begin_update_superblock → nvme->read(active_lba) → CPU 修改 + CRC → nvme->write(inactive_lba, FUA) → finish_update_superblock → finalize_flush_round`，用 PUMP context 带 `superblock_update_state` 串起来。

---

## 5. 协程体 `_owner::run_merge`

原来 merge_q handler 里一大坨 CPU 算法（`merge_old_paddr` / `finalize_root_group` / `prune_child_ref` / `assign_planned_paddrs` / `collect_write_descs` / `build_leaf_order_full` / `build_reverse_topology_full` / `rebuild_slot_map` / `new_manifest`）全部搬进协程里。

### 5.1 协程体阶段

```
Phase 1: pre-scan reads
    make_merge_context(base_manifest, &proposals)
    for each (rb, contribs) in contrib_index:
        if contribs.size() > 1 && !try_get_old_page_bytes(rb):
            fetched_old_pages[rb].resize(page_size)
            pending_ios.push_back(read_desc { lba=rb.lba, buf=fetched_old_pages[rb].data(), num_lbas=page_lbas })
    if (!pending_ios.empty()) co_yield merge_yield::need_io;
    // resume 回来时 fetched_old_pages 已被 NVMe 原地填好

Phase 2: build combined_root（同步 CPU）
    merge_old_paddr(root_range_base) → finalize_root_group → prune_child_ref
    retired_old_values 从 proposals 汇总到 round.retired

Phase 3: iterative post-order walk（融合了 assign_planned_paddrs + collect_write_descs）
    walk_stack.push(&combined_root, 0)
    while !walk_stack.empty():
        frame = top
        if next_child < children.size():
            push next child
        else:
            assign_and_emit_node(node, base_manifest, alloc, geom,
                                 allocated_ranges, pending_ios,
                                 retire_slot, retire_range)
            pop
            if (pending_ios.size() >= kWriteBatchSize) co_yield need_io;
            else if (cpu_budget reached) co_yield yield_cpu;
    if (!pending_ios.empty()) co_yield need_io;

Phase 4: build manifest metadata（同步 CPU）
    build_leaf_order_full / build_reverse_topology_full / rebuild_slot_map
    new_manifest = make_shared<tree_manifest>(...)
    is_root_change = is_root_change(...)

    co_yield merge_yield::done;
    co_return;
```

### 5.2 `touched_old_page_for` 不再 panic

改成分层：

```cpp
try_get_old_page_bytes(ctx, rb, contribs):
    for each contrib: if touched_old_pages has rb → return ptr
    if ctx.fetched_old_pages has rb → return ptr
    return nullptr

touched_old_page_for(ctx, rb, contribs):
    if auto p = try_get_old_page_bytes(ctx, rb, contribs) → return *p
    panic_inconsistency(...)  // 此时只有"pre-scan 漏报" bug 才会到这里
```

Phase 1 的 pre-scan 扫过 `contrib_index`，多 contrib 且没 bytes 的 rb 都 emit 了 read_desc，所以 Phase 2 的 build 阶段跑进来时 `fetched_old_pages` 必然覆盖所有需要的 old internal page。

---

## 6. `tree_sched::advance()` 重组

原来 528 行的 advance 拆成：

### 6.1 通用模板

```cpp
template <typename Q, typename Handler>
bool drain_queue(Q& q, uint32_t max_ops, Handler&& handler) {
    bool progress = false;
    for (uint32_t i = 0; i < max_ops; ++i) {
        auto item = q.try_dequeue();
        if (!item) break;
        handler(*item);
        progress = true;
    }
    return progress;
}
```

### 6.2 `advance()` 骨架 ~50 行

```cpp
bool advance() {
    bool progress = false;
    progress |= drain_queue(fold_q,              ..., [this](auto* r) { handle_fold_req(r); });
    progress |= drain_queue(merge_step_q,        ..., [this](auto* r) { handle_merge_step_req(r); });
    progress |= drain_queue(merge_reads_done_q,  ..., [this](auto* r) { handle_merge_reads_done_req(r); });
    progress |= drain_queue(finalize_merge_q,    ..., [this](auto* r) { handle_finalize_merge_req(r); });
    if (!update_superblock_inflight)
        progress |= drain_queue(begin_update_superblock_q, ..., ...);
    progress |= drain_queue(finish_update_superblock_q, ..., ...);
    progress |= drain_queue(finalize_q,          ..., [this](auto* r) { handle_finalize_flush_round_req(r); });
    return progress;
}
```

### 6.3 Handler 层次

| Handler | 子 helper |
|---|---|
| `handle_fold_req` | `validate_fold_sealed_gens` / `allocate_fold_round` |
| `handle_merge_step_req` | `handle_merge_step_first_call` / `handle_merge_step_resume_call` |
| `handle_merge_reads_done_req` | — |
| `handle_finalize_merge_req` | `validate_finalize_merge_round` / `emit_finalize_merge_failure` / `emit_finalize_merge_success` |
| `handle_begin_update_superblock_req` | — |
| `handle_finish_update_superblock_req` | — |
| `handle_finalize_flush_round_req` | — |

每个 handler 自己负责 `r->cb(...)` + `delete r`，advance() 完全不管。

---

## 7. 容量 / 并发调校

| 常量 | 值 | 意义 |
|---|---|---|
| `kMergeIterConcurrency` | 8 | outer for_each 的并发迭代数 |
| `kWriteBatchConcurrency` | 32 | 单个 iter 内 NVMe IO 的 concurrent fanout |
| `kWriteBatchSize` (in coro) | 32 | 协程攒够几个 write 就 yield need_io |
| `kCpuYieldBudget` (in coro) | 64 | 连续 walk 多少节点就 yield yield_cpu 让 main loop 转别的 scheduler |
| `kMaxMergeStepOpsPerAdvance` | 16 | advance 一次最多处理多少个 merge_step req |
| `kMaxFinalizeMergeOpsPerAdvance` | 4 | 同上 |
| `kMaxBeginUpdateSuperblockOpsPerAdvance` | 1 | begin/finish 成对，每 tick 最多一个 begin |
| `kMaxFinishUpdateSuperblockOpsPerAdvance` | 1 | 同上 |

10 亿 KV 量级下 writing phase 一个 round 的 write_desc 数 ~16M（4K page）或 ~4M（16K page）。协程每 32 条 write yield 一次，对应 500K~125K 次 yield/resume，对应同数量级的 iter 往返。concurrent(8) × concurrent(32) = 256 个 in-flight NVMe write，跟 NVMe 队列深度接近，饱和 NVMe 带宽不会是瓶颈。

---

## 8. 映射到 memory 约束

这一步落地的同时把四条 feedback 约束固化成代码形状：

| Memory | 固化方式 |
|---|---|
| `feedback_cross_sched_pump_only` | advance() 里彻底没有 `submit(make_root_context())`；所有跨 scheduler I/O 走外层 pipeline |
| `feedback_merge_owner_read_is_v1_core` | `run_merge` pre-scan + `fetched_old_pages` + `try_get_old_page_bytes` 分层；`touched_old_page_for` 只在不可达情况 panic |
| `feedback_coro_state_via_context` | `merge_loop_state` 在 `with_context` 里，`drive_merge` 读 `loop_state.cpu_done`，tree_sched 没有 `is_xxx_done()` 查询 |
| `feedback_pipeline_start_vs_operator` | `drive_merge_loop` 是一条平链：`just() >> with_context(...)(body) >> ...`，body 内部也是一条平链；没有在 flat_map 里嵌套子 pipeline |

---

## 9. 文件改动汇总

| 文件 | 净变化 |
|---|---|
| `tree/flush_types.hh` | +~100 行：`merge_loop_state` / `merge_io_desc` / `merge_step_need_io` / `merge_step_done` / `merge_finalize_request` / `merge_finalize_result` / `begin_update_superblock_result` / `finish_update_superblock_request` |
| `tree/owner_scheduler.hh` | 总量 ~+1500 行：新增 `merge_yield` / `merge_coro` / `merge_round_state`；`_flush_merge` + `_commit_tree_writes` + `_update_superblock` 三个 namespace 替换为 `_merge_step` / `_merge_reads_done` / `_finalize_merge` / `_begin_update_superblock` / `_finish_update_superblock` 五个；新增 `run_merge` 协程体 + `assign_and_emit_node` + `try_get_old_page_bytes`；advance() 重组 + 7 个 handler 成员函数 |
| `tree/sender.hh` | +~200 行：`handle_merge_step_need_io` / `drive_merge` / `drive_merge_loop` / `continue_after_finalize_merge` / `finalize_root_change` / `perform_superblock_io` / `superblock_update_state`；旧的 `continue_after_merge` / `write_tree_pages_then_commit` / `merge_worker_proposals` 删除；`tree_local_flush` 主链重写 |

---

## 10. 验证

- 所有 non-preexisting-broken 的 inconel test binary 全部 Built target：`inconel_test_runtime` / `inconel_test_tree_lookup` / `inconel_test_tree_lookup_multicore` / `inconel_test_value` / `inconel_test_page_cache` / `inconel_test_superblock_format` / `inconel_test_tree_page_format` / `inconel_test_wal_format`
- 2 条 `-Wsubobject-linkage` warning 属于 `run_merge` 协程帧里 local lambda（`retire_slot` / `retire_range`）的老问题，跟本次改动无关；可以在下一步把 lambda 改成协程体内的 struct 消掉
- 预先就坏的 `test_candidate_build` / `test_flush_carriers` / `test_tree_value` / `test_leaf_mapping` 跟本次改动无关（`git stash` 对照过）

---

## 11. 未覆盖 / 下一步

- **没写模块级 e2e 测试**。按 `feedback_module_complete_gates_next_module` 的规则，本步的 `run_merge` 协程 + `drive_merge_loop` + 所有 seam 需要有专门的 e2e test 覆盖到 flush 成功 / flush 过程中 NVMe write 失败 / shared-ancestor 触发 owner read / flush_ok=false rollback 各条路径再推进下一步。
- **Performance profile 没跑**。按 `feedback_perf_profile_on_module_e2e` 的规则，模块完成阶段要附带 standalone profile binary 测 10 亿 KV 量级的 flush 时间分布。当前还在"功能能 compile" 阶段，profile 在 e2e 之后。
- **`retire_slot` / `retire_range` 作为协程帧内 lambda** 目前产生 linkage warning。把它们移出协程体（协程体用参数或 struct 替代）是最直接的清理。

---

## 12. Post-031 改进

Step 031 落地后，用户把这条分支的 runtime 可用性推到"能真的按目标拓扑跑起来 + 业务 pipeline 不再拿 scheduler 指针"的程度。下面四轮改动都在 step 031 commit 之后做，**严格的前后顺序**——每一轮都以上一轮为基础，回归基线也是在上一轮的结果上继续跑。

### 12.1 非对称 runtime 拓扑（role cores）

**动机**。生产目标拓扑：`core 0 = value + 业务`、`cores 2/4/6 = read_domain`、`core 8 = owner`、每核一个 NVMe。之前 `build_runtime` 把所有列出来的核都当成"每核都跑 read_domain + NVMe"，`value_alloc_sched` / `tree_sched` 硬绑在 `cores[0]`——完全无法表达这个角色化拓扑。

**改动**。

| 文件 | 内容 |
|---|---|
| `apps/inconel/runtime/builder.hh` | `build_options` 新增三个可选字段：`read_domain_cores` / `value_core` / `owner_core`（sentinel 默认走老的对称拓扑）。`validate_build_inputs` 增加 tier 4：角色 core 必须是 `cores` 的成员、`read_domain_cores` 禁止重复。`build_runtime` 重写为按角色分派：每核无条件建 NVMe；只有 `read_domain_cores` 里的核建 `tree_read_domain`；`value_core` / `owner_core` 各自独立构造 singleton；非角色核在对应 tuple slot 填 `nullptr`。`shard_partition_map` 的 bootstrap shard 数从 `cores.size()` 改为 `read_domain_cores.size()`。 |
| `apps/inconel/test/test_runtime_topology.cc`（新） | 专门验证非对称拓扑的集成测试：registry by_core 映射、PUMP 每核 tuple 槽位、所有 scheduler 正常 `advance()` 不 panic、200ms dwell 后全核 `is_running_by_core[core]=false` 清停、`destroy_runtime` 清场。硬件核数 < 9 自动 skip。 |
| `CMakeLists.txt` | 注册 `inconel_test_runtime_topology`。 |

**验证**。`inconel_test_runtime_topology` 通过；`inconel_test_runtime`（对称拓扑）回归通过；两套拓扑共用同一条 `build_runtime` 路径。

### 12.2 shard routing 的批量 API 与 facade 统一

**动机**。用户列出四条 runtime 可用性要求：

1. value / owner 只有一个实例——必须有函数或全局变量封装。
2. read_domain 按 key range 分；要提供**按 key 路由**的入口；flush 场景要能**对有序 key 以最快速度分组**。
3. 每核本地 NVMe 的 O(1) 入口。
4. 每核 advance 要预先知道推哪些，不能每轮判空（单独在 12.3 解决）。

(1) / (3) 已经有 `core::registry::value_sched()` / `tree_sched_singleton()` / `local_nvme()`，但散落在 core/registry.hh 里、看起来像内部细节。(2) 的单 key 路由已存在（`current_shard_partitions()->route(key)`），**批量分组没有**——会导致 flush 路径的 fold → worker fan-out 退化成 N 次二分 + 桶合并。

**改动**。

| 文件 | 内容 |
|---|---|
| `apps/inconel/core/tree_read_domain.hh` | `std::shared_ptr<const shard_partition_map> partitions` 从 `tree_read_domain<Cache>` 上移到 `tree_read_domain_base`。非模板调用方（publish 双刷路径、register list 迭代）不再需要把 base 指针 `static_cast` 回 `<Cache>` 派生类去写 `partitions`。 |
| `apps/inconel/core/shard_partition.hh` | 新增 `shard_partition_map::partition_sorted_keys(std::span<const std::string_view> sorted_keys, Sink&& sink)`：有序 keys + 有序 fences 线性归并 `O(N+S)`，对每个非空 shard 恰好调用一次 `sink(shard_idx, lo, hi)`。空 key 列表不调 sink；空 map 或缺少 +∞ sentinel 都 panic_inconsistency，和单 key `route()` 的失败语义对齐。 |
| `apps/inconel/runtime/facade.hh`（新） | `apps::inconel::rt::` 命名空间作为业务 pipeline 的单一入口层，全部是 inline forwarder：<br>• `rt::value()` / `rt::owner()` — singleton<br>• `rt::local_nvme()` — 本核 NVMe<br>• `rt::route_to_read_domain(key)` — 单 key 路由 `tree_read_domain_base*`<br>• `rt::partition_sorted_keys(sorted_keys, sink)` — 批量路由<br>• `rt::publish_shard_partitions(map)` — 两步契约：先 `install_shard_partitions` 设全局，再遍历 `tree_read_domains.list` 把新 snapshot 推到每个 read_domain。rebuild 站点只需一行。 |
| `apps/inconel/runtime/builder.hh` | 构造完 read_domain 后 `rt::publish_shard_partitions(bootstrap_map)` 走一遍，bootstrap 和未来的 tree_sched rebuild 走完全一致的两步路径；不再依赖 rebuild 站点自己记得刷 read_domain。 |
| `apps/inconel/test/test_runtime.cc` | 原来手工"install + 遍历所有 read_domain 反向写 `->partitions`"整段换成 `rt::publish_shard_partitions(...)` 一行。 |

**验证**。`inconel_test_runtime_topology` / `inconel_test_runtime` / `inconel_test_tree_lookup_multicore` 全通过。`partitions` 上移改动面很小（只有 builder、test_runtime.cc 两处真正在写），其他 `.partitions` / `->partitions` grep 命中的是 `flush_round_state` / `flush_fold_result` 同名字段，和本改动无关。

### 12.3 每核 advance 列表（inconel 自己的 `rt::run`）

**动机**。`pump::env::runtime::run` 的热循环每轮都对每个 tuple 槽位判空：

```cpp
while (running) {
    std::apply([runtime](auto*... sche) {
        if (!(... | (sche ? advance_one(runtime, sche) : false))) yield();
    }, runtime->schedulers_by_core[core]);
}
```

tuple 元素是运行时 `nullptr`，编译器没法消掉判空；角色化拓扑下每核 tuple 很多 slot 是空的，4 槽位就有 4 次无意义的 load + nullptr 比较。用户明确要"循环外判断一次、循环内不判断"。

**范围选择**。不碰 PUMP（`pump::env::runtime::run` / `share_nothing::start` 其它 app 还在用）。inconel 自己写一个 run。

**改动**。

| 文件 | 内容 |
|---|---|
| `apps/inconel/runtime/run.hh`（新） | `apps::inconel::rt::run(runtime, core, on_init)`：while 之前 `std::apply` + 折叠把每个非空 scheduler 指针捕获到栈上 `std::array<step, K>`（`step = { void* obj; bool(*fn)(void*); }`），thunk 是无捕获 lambda 衰变成的函数指针，静态绑定到 `T::advance()`。while 内只剩 `is_running_by_core[core].load()` + `n` 次 indirect call，没有 nullptr 判断、没有 tuple 元素访问。`fn` 签名刻意用单 obj 参数—inconel 所有 scheduler 都有无参 `advance()`；未来如果出现 preemptive（需要 runtime 指针）再扩 `(void*, void*)` 携带 runtime。2-arg 重载 `run(runtime, core)` 里用 `::apps::inconel::rt::run(...)` 全限定名转发，避免 ADL 把 `pump::env::runtime::run` 拉进重载集造成歧义。 |
| `apps/inconel/test/test_runtime_topology.cc` | `pump::env::runtime::run` → `rt::run`。 |
| `apps/inconel/test/test_runtime.cc` | `pump::env::runtime::run` → `rt::run`。 |
| `apps/inconel/test/test_value.cc` | `pump::env::runtime::run` → `rt::run`。 |

**约束**。"scheduler 指针 write-once"是这个优化的前提——build_runtime 完后不能再往某 tuple slot 填新指针。inconel 当前满足；其他 app 不受影响。

**验证**。`inconel_test_runtime_topology` / `inconel_test_runtime` / `inconel_test_value` / `inconel_test_tree_lookup_multicore` 全通过。`inconel_test_value` 13 个 case（value scheduler 的各种 persist/read/cache/allocator 路径）全通过，验证了新 run loop 下 scheduler drain 语义和原 PUMP 路径等价。

### 12.4 singleton 指针从 sender 表面消失

**动机**。既然 `rt::value()` / `rt::owner()` 是 assert-非空的 singleton getter、业务 pipeline 的单一入口层已经建好，`value/sender.hh` 和 `tree/sender.hh` 对外 API 还要 caller 传 `value_alloc_sched_base*` / `tree_sched*` 就完全是冗余——caller 每次都要先 `core::registry::value_sched()` 再塞进去。

**改动**。

| 文件 | 内容 |
|---|---|
| `apps/inconel/value/sender.hh` | `persist_values(sched, entries)` → `persist_values(entries)`；`read_value(sched, vr)` → `read_value(vr)`；内部 helper `on_persist_leader` / `on_read_miss` 同步去掉 sched 形参。内部 `core::registry::local_nvme()` 统一换成 `rt::local_nvme()`；`rt::value()->prepare_persist(...)` 等替代原先从参数拿 sched。 |
| `apps/inconel/tree/sender.hh` | `tree_local_flush(owner, req)` → `tree_local_flush(req)`；`finalize_root_change` / `perform_superblock_io` / `handle_merge_step_need_io` / `continue_after_finalize_merge` / `drive_merge_loop` 全部去掉 `tree_sched* owner` 形参。**per-shard 指针保留**：`on_decision_need_read(tree_lookup_sched_base*, ...)` / `shard_lookup(tree_lookup_sched_base*, ...)` / `submit_flush_work(tree_worker_sched_base*, ...)` 不改——这些不是 singleton，而是 `current_shard_partitions()->route(key)` 算出来的 per-shard scheduler，显式传递是正确的。<br>`tree::lookup(keys, manifest)` 对外签名早就没 sched 参数（INC-003 / INC-040 收敛），本轮不动。 |
| `apps/inconel/test/test_value.cc` | 所有 `value::persist_values(sched, ...)` → `value::persist_values(...)`；所有 `value::read_value(sched, vr)` / `(sched, vr1)` / `(sched, vr2)` / `(sched_base, vr_in)` → 去掉第一个参数。 |
| `apps/inconel/test/test_tree_value.cc` | 所有 `value::read_value(sched, lv.vr)` / `(&env.value_sched, env.refs[0])` / `(&env.value_sched, bogus)` / `(&env.value_sched, vr)` → 去掉第一个参数。 |
| `apps/inconel/test/test_tree_pipeline_compile.cc` | `tree::tree_local_flush(owner, req)` → `tree::tree_local_flush(req)`；去掉本来作为编译哨兵的 `tree_sched* owner = nullptr`。 |

**保留的指针参数**。总结 sender.hh 里"应该继续传指针"的对象和理由：

| 对象 | 类型 | 为什么还是 caller 传 |
|---|---|---|
| `tree_lookup_sched_base*` 在 `on_decision_need_read` / `shard_lookup` | per-shard | 由 `lookup()` 的 `build_route_plan` 按 key 路由算出来，每次可能是不同 shard |
| `tree_worker_sched_base*` 在 `submit_flush_work` | per-shard | 同上，由 flush 的 per-partition fan-out 算出来 |

**验证**。改过签名的四个 test 都跟着改并通过：`inconel_test_value`（所有 13 case）、`inconel_test_tree_lookup` / `inconel_test_tree_lookup_multicore`、`inconel_test_tree_pipeline_compile`、`inconel_test_runtime`（对称 + 非对称 e2e）。预先就坏的 `inconel_test_tree_value` / `inconel_test_flush_carriers` / `inconel_test_leaf_mapping` / `inconel_test_candidate_build`（`git stash` 对照过，不是本轮引入）不在本轮修复范围。

---

### 12.5 留给后续 step 的问题

- **本地 NVMe 访问的"本核约束"没有编译期保护**。`rt::local_nvme()` 依赖 `pump::core::this_core_id`，如果业务 pipeline 在一个跨 scheduler 跳转之后错把上一段的 `local_nvme()` 指针捕获进 lambda、在新核上执行，`this_core_id` 已经变了但指针还是老的 — 只能运行时 panic 不能编译期拦。现在的 sender helper 里都是 **在 lambda 体里** 调 `rt::local_nvme()`，保证每次执行时都是当前核，这是靠约定维持的不变量。
- **`fn` 签名单 obj 参数**。`rt::run` 的 step thunk 选了 `bool(*)(void*)` 而不是 `bool(*)(void*, void*)` 带 runtime。一旦 inconel 接入 preemptive scheduler（需要 runtime 指针去 any_scheduler），就得扩签名。属于受限实现，符合"收窄实现必须显式声明"的约束（`run.hh` 开头的 comment 里已经写了）。
- **`rt::publish_shard_partitions` 还没有 rebuild 调用方**。当前只有 bootstrap 一个 caller。step 030 §6.7 的 rebuild 真正接通时，站点直接调这个 facade 就行，不需要再发明新 API——这是本轮让 bootstrap 也走 publish 的主要目的。
