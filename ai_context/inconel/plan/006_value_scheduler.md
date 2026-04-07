# 006 — Value Scheduler

> 实现第六步。基于 002 的 format 工具和 001 的 mock_nvme，实现 value 持久化与读取的 PUMP scheduler。

## 文件结构

```
format/
└── value_object.hh         — value_object_header + encode/decode + slot/offset helper
value/
├── allocator.hh            — 纯 placement state（per_class、bump_head）
├── scheduler.hh            — 单一 PUMP scheduler，4 套 6 组件 + 4 个 handle
└── sender.hh               — 对外两个高层 sender：persist_values、read_value
core/
└── registry.hh             — 加 value::scheduler* singleton + value_sched()
runtime/
└── builder.hh              — 构造 value scheduler 在 cores[0]，其他 core nullptr 占位
test/
├── test_value.cc           — 6 个 case（value 单元测试）
└── test_tree_value.cc      — 4 个 case（tree lookup → value read 集成）
```

## 设计决策

| 决策 | 选项 |
|------|------|
| 架构层数 | **两层**：纯 allocator + 单一 PUMP scheduler |
| 对外接口 | scheduler 公开区只有：构造、`advance()`、4 个 sender 工厂；**没有** 同步业务方法、**没有** inspector hook |
| persist 流程 | 拆 `prepare_persist` + `finalize_persist` 两 sender，pipeline 显式编排 NVMe write |
| read 流程 | 拆 `prepare_read` + `fill_and_decode` 两 sender，pipeline 显式编排 NVMe read |
| sender 输出 | variant + `visit()` + `if constexpr` 编译期分支 |
| Cache 实现 | inconel 风格 `std::map<paddr, vector<char>>`，无界，**不复用** step 4 page_cache（下一步再接） |
| Writable 结构 | `vector<optional<page_data>> open_pages_` + `vector<vector<page_data>> ready_pages_`，按 class_idx 索引 |
| value class 范围 | 任意大小（sub-LBA / LBA-equal / multi-LBA 三种都支持），构造时 assert 对齐边界 |
| Singleton placement | 硬编码 cores[0]，未来配置文件接管 |
| value_alloc_sched 模板化 | 不模板化（不用 page_cache_concept），单一类，无 base 拆分 |

## scheduler 内部数据

| 字段 | 类型 | 用途 |
|------|------|------|
| `alloc_` | `value_allocator` | per_class { open, whole_pool } + bump_head |
| `open_pages_[ci]` | `vector<optional<page_data>>` | 每 class 当前 active 页（可分配） |
| `ready_pages_[ci]` | `vector<vector<page_data>>` | 备用 active 页（commit 时若 open 已占用就堆这里） |
| `readonly_cache_` | `map<paddr, vector<char>>` | 已写满 / 从 NVMe 读上来的只读页 |
| `inflight_rounds_` | `map<round_id, unique_ptr<round>>` | leader-follower 在飞 round |
| `next_round_id_` | `uint64_t` | 单调递增 round id |
| `persist_q_/finalize_q_/read_q_/fill_q_` | `per_core::queue<req*>` | 4 套请求队列 |

**Invariant**：任何 `paddr` 同时只在一处 — `open_pages_` / `ready_pages_` / `readonly_cache_`，三者不重叠。
- 满页 finalize 时从 writable 移到 readonly（move 整个 image 所有权）
- 全空页从 writable 通过 `recycle_whole_page` 移回 allocator 的 whole_pool

## Pipeline 编排

### 写路径（persist_values）

```
prepare_persist(entries)                       → variant<persist_leader, persist_follower>
  >> visit()                                    展开 variant
  >> flat_map(generic lambda):
       if leader:
         as_stream(writes) >> concurrent()
           >> flat_map([](d) { return local_nvme()->write(...FUA); })
           >> all()
           >> flat_map([](nvme_ok) { return finalize_persist(rid, nvme_ok)
                                            >> then([nvme_ok]{return nvme_ok;}); })
       if follower:
         just(static_cast<bool>(alt.ok))
```

leader 走 NVMe + commit + 末尾 then 把 nvme_ok 转出 bool；follower 通过
`persist_follower::ok` 拿到同一 round 的 leader nvme_ok（同一个 FUA write 的
verdict），让两条分支输出统一的 bool。两个分支最终 value_type 都是 `bool`。

`finalize_persist` 自身**不抛 nvme 失败异常**：commit/rollback 都按本地状态机
执行，统一 cb() 返回 void。只有 unknown round_id（不变量被破坏）才 abort。
nvme 失败的语义由上层 bool 信号承载。

### 读路径（read_value）

```
prepare_read(vr)                               → variant<read_hit, read_miss>
  >> visit()                                    展开 variant
  >> flat_map(generic lambda):
       if hit:  just() >> forward_value(__mov__(alt.body))   // 经 then-lambda
                                                              // 避开 just(string) UAF
       if miss: flat_map([]{ return local_nvme()->read(buf); })
                >> false_to_exception(runtime_error("...NVMe read failed"))
                >> flat_map([](true) { return fill_and_decode(vr, buf); })
```

cache 命中直接 decode；miss 通过 pipeline 调 NVMe read 后回 value sched 写 cache + decode。

**异常路径：**
- NVMe read 返回 false → `false_to_exception` 抛 `std::runtime_error`，
  `fill_and_decode` 不被执行 → readonly_cache_ 不污染
- decode_value_object 返回 corruption status (bad_magic / bad_body_len /
  bad_crc / truncated) → handle_fill 通过 `try_decode_value` 检测后
  `item->fail()` 抛异常，**cache 不被污染**（先验证再 commit）
- 同理 handle_read 三个 hit 路径用 `serve_hit_or_fail` helper，corruption
  → fail 而不是静默返回空 string

## 关键 PUMP 注意点（采坑记录）

1. **自定义 sender 不能直接 `>>` 到 `just()`**：PUMP 的 `operator>>` 重载只接受 `(sender, bind_back fn)` 形式。`just() >> sched->prepare_persist(...)` 会编译失败因为右边是 sender 不是 bind_back。
   解决：把整个 pipeline 包在 `flat_map(lambda)` 内或顶层 `just() >> flat_map([](){ return sender_chain; })`。

2. **lambda 返回类型不一致**：if/if constexpr 两个分支返回不同 sender 类型，普通 then/flat_map 编译失败。
   解决：用 variant 输出 + `visit()` + generic lambda + `if constexpr`。各分支 sender 类型可以不同但**最终 value_type 必须一致**（因为下游 op_pusher 是按 alternative 0 推导的）。

3. **just<T> 内部传 lvalue ref**：`just(string)` push_value 时通过 std::apply 传 string& 给下游。下游 lambda 必须用 `string` (by value) 或 `string&`，**不能用 `string&&`**。
   测试代码的 `then([](std::string&& s){...})` 会失败 —— 改成 `then([](std::string s){...})`。

3a. **just(non-trivial value) 在 flat 内部会触发 use-after-free**：这是个真实的坑。
    - flat 内 lambda `return just(std::move(string_value))` 会创建 sub-scope，op_tuple = `[just::op<string>, pop_pusher_scope_op]`
    - just::op 持有 string 在 sub-scope 内
    - just::op_pusher 通过 std::apply 传 op 内 string 的 **lvalue ref** 给下游
    - 下游 pop_pusher_scope_op 先 `delete scope.get()`（析构 just::op，string 跟着 free heap buffer），再 `__fwd__(v)` 把 dangling ref 传给 base scope 的下一个 op
    - 用户 lambda 收到 ref 时拷贝构造 → 从 freed memory 拷贝 → 得到 size 正确但内容是垃圾的 string
    - bool/int 不暴露因为 trivial copy，string/vector 等 owning heap 类型必爆
    - **修复**：用 `just() >> then([body = std::move(...)]() mutable { return std::move(body); })` 代替 `just(string)`。
      then 的 `op.func()` 返回的临时 string 在调用栈上（绑定到 push_value 参数引用），生命周期延长到完整表达式结束，pop_pusher_scope_op 删除 scope 时只析构 lambda 闭包（已 moved-from），不影响栈上临时 → 下游安全访问
    - **隐含约束**：sub-scope 内不要把 owning heap 数据放进 just sender。需要传 string 时用 then-lambda 返回 by-value，或用 push_context/get_context 机制。

4. **as_stream + flat_map 内 lambda 也是 by value**：流元素传给 flat_map 内 lambda 是 lvalue ref，lambda 不能用 `&&` 接。

5. **`payload()` 是 inconel 分支的 request_pool 模式**：inconel.new 用裸 `req*`，op::start 的 cb 直接 `item->cb(...)` / `item->fail(...)`，不需要 `item->payload()->cb`。

## leader-follower round 机制

```text
[t0] 第一个 prepare_persist req 进入 → handle_persist 成为 leader
     drain persist_q 拿到当前所有 req → followers
     合并所有 entries
     对每个 entry: find_min_class → acquire_round_page → encode → 填 vr
     build_write_fua: 把 page images 转 nvme write_desc
     inflight_rounds_[rid] = round (含 followers 列表)
     leader.cb({persist_leader{rid, writes_span}})
     followers 留着不调

[t1] pipeline 拿到 leader 的 writes → as_stream + concurrent + nvme.write_fua + all
[t2] pipeline 调 finalize_persist(rid, nvme_ok) → handle_finalize
     erase round from inflight_rounds_
     if nvme_ok: commit_pages: 满页移到 readonly_cache_，未满页装回 open/ready
     else:       rollback_pages: fresh_bump 丢弃，open/ready 还原到 round 前
     leader.cb()                                       (无参，正常退出)
     for follower : round.followers:
         follower.cb({persist_follower{nvme_ok}})       (传播 leader 的 verdict)
```

unknown round_id（不变量破坏）→ `std::abort()`，不再走异常路径。

## 跳过功能（v6 不实现）

- `freed_slots` / `recycle_whole` / `hole_pool` / `deferred_freed`（hole reuse 路径，需要 tree_sched 调用）
- `install_recovered_state`（boot recovery）
- `data_area_heads` 共享碰撞检测（无 tree_sched，bump 撞到 base 就 fail）
- multi-device routing
- `canonical_entry` / `batch_ctx` 上层域对象（用最简 `put_entry { body, out_vr* }`）
- `request_pool` / `inconel::owner_impl` 风格的 node 包装
- DMA pool / `value_page_frame` / `frame_id`（用 `vector<char>` 直接当 page image）

## 已知缺口（写进 plan）

1. **NVMe write 失败的 follow-up**：finalize_persist 现在 commit/rollback 都走
   void 出口；nvme_ok 通过 leader 末尾 then + persist_follower::ok 字段一起
   传给所有同 round 的 caller。但 rollback 后 caller 持有的 out_vr 字段值
   还是 prepare 时填的，依赖 caller 看到 ok=false 主动丢弃。未来可在
   rollback_pages 时遍历 followers + entries 主动 zero out_vr 让悬空更显式。
2. **follower 路径运行时未覆盖**：单 sender 测试只走 leader 分支，
   `for follower : round.followers` 循环和 `persist_follower{}` 分支永远
   不被执行。等 step 7+ 写 pipeline 来时（fan-out 多 batch 必然产生并发入队）
   自然覆盖。
3. **readonly_cache_ 未接 page_cache**：当前用 inconel 简单
   `std::map<paddr, vector<char>>`，无界、无 LRU、无 buf 复用。下一步换成
   step 4 的 page_cache_concept（clock/slru）+ buf 复用池。届时
   value_alloc_sched 需要模板化在 Cache 上 + 拆 base 类。
4. **Bump 失败有内存泄漏**：persist 失败时（out of space）已经从 bump 拿过
   的 fresh page 是丢的，bump_head 已经下移。v6 接受这个泄漏，因为这是
   终极失败场景。
5. **测试同步用 sleep hack**：`sleep_for(10ms) × 200` 等待 atomic counter，
   不是严格同步。等 NVMe 接入后重写为黑盒接口测试。
6. **NVMe write 失败端到端测试缺**：mock_nvme 没有 IO 失败注入接口，所以
   value persist 路径的 NVMe write 失败 → finalize rollback → leader/follower
   bool=false 这一整条还没回归覆盖。read 路径已用越界 LBA 覆盖到了
   `false_to_exception` 分支（test_tree_value case_4），write 同样可以用越界
   LBA 但需要在 prepare 阶段构造一个会落到越界的 entry —— 等 step 7+ 接 batch
   pipeline 时一起加。

## 测试覆盖

### `apps/inconel/test/test_value.cc` — 6 个 case（value 单元）

| Case | 验证目标 | 同步方式 |
|------|---------|---------|
| 1. write_path | sender 写一批不同 class value，`test_read_raw` 验证 NVMe 字节正确 | sleep + atomic counter |
| 2. read_miss | `test_write_raw` 造数据，sender 读，验证 body + read_count 涨 | 同 |
| 3. cache_hit | 接 case_2，第二次读同 vr，验证 read_count 不再涨 | 同 |
| 4. write_then_read | sender 写完立即读，验证 read_count == 0（命中 open_pages_） | 同 |
| 5. sub_lba_same_page | 写 2 个 sub-LBA value，验证 vr.base 相同、byte_offset 不同 | 同 |
| 6. cross_class | 写 5 个不同 class（含 multi-LBA），验证落到不同 page 且字节正确 | 同 |

测试拓扑：cores = {2, 4, 6} 3 worker；value sched 在 core 2（cores[0]）；mock_nvme + tree_lookup 在每个 core；测试主线程 this_core_id = 0 提交 sender。

`mock_device` 已有 `get_read_count()` / `get_write_count()` / `reset_io_counters()`，作为间接 inspector 使用。

### `apps/inconel/test/test_tree_value.cc` — 4 个 case（tree+value 集成）

| Case | 验证目标 | 同步方式 |
|------|---------|---------|
| 1. lookup_then_read | 单 pipeline: `lookup → flat_map(stream → flat_map(read_value) → reduce)` 一次 submit 跑通完整 GET | manual advance loop |
| 2. missing_key | 不存在的 key → `lookup_absent`，read 不被触发 | 同 |
| 3. corrupt_value_page | `test_write_raw` 把 value page magic 字节抹零，read 走 NVMe miss → handle_fill 的 try_decode_value 失败 → `item->fail()` 抛 corruption 异常；第二次 read 同 vr 必须**再次走盘**（read_count 增）证明 cache 没被污染 | manual advance loop |
| 4. nvme_read_failure | 越界 LBA value_ref 触发 mock_device 边界检查返回 false → `false_to_exception` 抛 NVMe 失败异常；第二次 read 必须**也抛**证明 cache 不持有 stale 项 | manual advance loop |

测试用直接铺盘 + 单核 manual advance 模式（参考 test_tree_lookup.cc），不走 share_nothing/jthread，避免 ASAN leak。`value::persist_values` 不被使用 — value page 通过 `mock_device::test_write_raw` + `build_subLba_page_image` 直接写盘，leaf page 通过 `tree::leaf_page_builder` + `dev.do_write` 直接写盘。

## 全部回归

- `inconel_tests`                       — step_01 7/7 通过
- `inconel_test_page_cache`             — clock + slru 全过
- `inconel_test_runtime`                — clock + slru e2e 400+400 通过
- `inconel_test_tree_lookup`            — 单 + 缓存压测全过
- `inconel_test_tree_lookup_multicore`  — 400 hit + 100 miss 通过
- `inconel_test_value`                  — 6 cases 通过
- `inconel_test_tree_value`             — 4 cases 通过 ✓ 新增

Release + Debug 两种构建均验证（CHECK 宏不依赖 NDEBUG）。ASAN 暂不跑 ——
share_nothing.run 的硬停语义在测试 teardown 下会留 unreachable leak，
跟应用代码无关，详见 `feedback_share_nothing_no_drain` memory。

## 实现遇到的 PUMP 类型陷阱总结（步骤心得）

1. 自定义 scheduler sender 必须**作为 pipeline 起点或包在 flat_map lambda 内返回**。不能用 `just() >> sched->xxx_sender(...)` 的形式 — `>>` 重载只接受 `(sender, bind_back fn)`，自定义 sender 不是 bind_back。

2. 写**多分支 pipeline** 必须用 variant + visit() sender + generic lambda + if constexpr。不要试图用普通 then/flat_map 让 lambda 返回不同类型 sender。

3. **下游 lambda 参数类型** 取决于上游 sender push_value 的传值方式。`just<T>` 通过 std::apply 传 lvalue ref；`for_each` 流元素也传 lvalue ref。lambda 用 by value (`T`) 或 lvalue ref (`T&`)，**不要用 rvalue ref (`T&&`)**。

4. inconel.new 的 PUMP scheduler 用裸 `new req*` / `delete r`，不用 `request_pool` 也没有 `node->payload()` 包装。op::start 的 cb 直接 `item->cb(...)`。

5. lambda **必须捕获 ctx 和 scope**：op::start 内 cb 是异步触发的，ctx/scope 必须在 lambda 中按值捕获以延长生命周期。

6. variant 输出 sender 的 op::start 直接 `push_value(ctx, scope, std::move(variant))`，**不要在 cb 内手动 std::visit 展开**。下游 visit() sender 会处理。
