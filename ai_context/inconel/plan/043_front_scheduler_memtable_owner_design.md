# 043 — Front Scheduler：Memtable Owner Surface

> 本文是 `front_wal_development_plan.md` 里 M05 的详细设计文档。
> M05 只冻结 `front_sched` 的 memtable owner 面：
> `insert_memtable_entries`、`lookup_memtable`、`batch_lookup`、
> `scan_memtable`、`seal_active`、`collect_eligible_gens`、`release_gens`。
>
> 本文不设计 WAL append / segment header/trailer / FUA issue、Value
> persist/read、`write_batch` pipeline、tree lookup、runtime API、seal round
> 编排、frontier switch 或 recovery。

## 1. 范围

M05 把旧 `inconel` 分支 Step 17 的 CPU-only `front_state` 语义迁移到当前
`inconel.new` 架构，但必须按 M01/M02/M03/M04 已冻结的类型和正式设计文档
重切边界：

1. `apps/inconel/front/scheduler.hh`
   - N 实例的 front owner scheduler。
   - owner-local active / immutable memtable generation 链。
   - request queues、bounded `advance()`、PUMP sender op 类型。
2. `apps/inconel/front/sender.hh`
   - 对外唯一 sender facade。
   - 其他 L2 模块不得 include `front/scheduler.hh`。
3. 可选 `apps/inconel/core/registry.hh`
   - 只在实现需要时加入 front list placeholder / accessor。
   - 这只是 scheduler registry 形状，不是 runtime API。
   - 不实现 runtime builder、public runtime API 或自动注册。

M05 直接消费已有类型：

1. M01 `core::front_fragment`、`core::canonical_entry`、`core::batch_ctx`、
   `core::memtable_gen`、`core::front_read_set`。
2. M02 `core::lookup_memtable()`、`core::scan_memtable()`、
   `core::memtable_lookup_result`、`core::memtable_scan_result`。
3. M03 的错误边界：memtable phase 开始后失败不能映射成
   `coord::release_batch`。
4. M04 的相邻状态：`wal_stream_state` 已定义给后续 front WAL stream 使用，
   但 M05 不把 WAL stream 接入 front memtable owner surface。

## 2. 已检查输入

旧 `inconel` 分支证据：

1. `ai_context/inconel/plan/steps/step_17_design.md`
2. `ai_context/inconel/plan/steps/step_17_test_spec.md`
3. `ai_context/inconel/plan/steps/step_17_review.md`
4. `apps/inconel/runtime/front/state.hh`
5. `apps/inconel/runtime/front/owner_impl.hh`
6. `apps/inconel/test/step_17_front_sched_contract_test.cc`

当前 `inconel.new` 证据：

1. `ai_context/inconel/plan/039_front_wal_phase_a_carrier_inc055_design.md`
2. `ai_context/inconel/plan/040_read_handle_prs_memtable_lookup_design.md`
3. `ai_context/inconel/plan/041_coord_scheduler_assign_publish_release_design.md`
4. `ai_context/inconel/plan/042_wal_space_scheduler_design.md`
5. `apps/inconel/core/batch_carrier.hh`
6. `apps/inconel/core/memtable.hh`
7. `apps/inconel/core/memtable_lookup.hh`
8. `apps/inconel/wal/scheduler.hh`
9. `apps/inconel/core/registry.hh`

正式设计依据：

1. `ai_context/inconel/design_doc/runtime_state_machine.md`
2. `ai_context/inconel/design_doc/write_path_and_pipeline.md`
3. `ai_context/inconel/design_doc/read_api_and_pipeline.md`
4. `ai_context/inconel/design_doc/runtime_memory_and_cache.md`
5. `ai_context/inconel/design_doc/flush_and_frontier_switch.md`
6. `ai_context/inconel/design_doc/cross_doc_contracts.md`
7. `ai_context/inconel/design_doc/code_modules.md`
8. `ai_context/inconel/design_doc/code_quality_standard.md`
9. `ai_context/inconel/known_issues.md`

## 3. 语义来源对照表

| 项目 | 旧 Step 17 证据 | 当前 `inconel.new` 状态 | 正式设计依据 | 043 决议 |
|---|---|---|---|---|
| 模块落点 | `runtime/front/state.hh` 是同步 CPU class；`owner_impl.hh` 混入旧 WAL mock 和 point_get helper。 | 当前没有 `apps/inconel/front/`。 | `code_modules.md` 要求 L2 `front/` 模块，模块间只暴露 `sender.hh`。 | 新 front 落到 `apps/inconel/front/{scheduler.hh,sender.hh}`。旧 `runtime/front` 路径不迁移。 |
| scheduler 形态 | Step 17 明确不实现 PUMP sender；后续 `owner_impl.hh` 有旧 request pool。 | M03/M04 已落地当前 sender/op_pusher/compute_sender_type 模式。 | PUMP 规范：共享可变状态放 scheduler，sender 只做调度边界。 | M05 必须实现真实 front scheduler sender surface，而不是只落 CPU helper。 |
| insert 输入 | Step 17 接收已路由 `canonical_entry` span 或 pointer span。 | M01 fragments 用 stable `entry_indices`，不保存 entry 指针。 | 039 §4.3；WP §4/§6.2。 | `insert_memtable_entries` 消费 M01 `front_fragment` + borrowed `canonical_entries` span；front 不重新 hash，不恢复 pointer carrier。 |
| value payload | Step 17 PUT materialize `value_handle{durable, hot_blob}`。 | M01 已移除 hot value body；memtable 只有 `value_ref durable`。 | INC-055；RSM §3.3；RAP §4.4。 | 旧 hot_blob 是语义来源但不迁移。PUT 只写 `value_handle{durable = entry.allocated_vr}`。 |
| DELETE | Step 17 写 tombstone，不回退旧 value。 | M01 `memtable_entry::kind::tombstone` 已存在。 | RSM §3.5；RAP tombstone 规则。 | 保留。tombstone winner 遮蔽旧 value；memtable insert 不读 tree/current DB。 |
| lookup/scan 来源 | Step 17 调用 Step 8 helper，搜索 caller-supplied `front_read_set`。 | M02 已实现 `core::lookup_memtable` / `scan_memtable`。 | `cross_doc_contracts.md` §4 明确不是 front 当前 active/imms。 | front owner 只在 scheduler 线程上调用 M02 helper；禁止读取 self state 代替传入 PRS snapshot。 |
| `batch_lookup` | Step 17 顺序包装多个 point lookup，输出保输入顺序。 | M02 未实现，留给 M05/M10。 | RSM §3.7；RAP §5.3。 | M05 定义 front-local `batch_lookup_result`；不引入新 visibility 语义。 |
| seal active | Step 17: active -> sealed，新 active = old gen_id + 1，old push_front imms，返回新 topology。 | 当前 `memtable_gen` 有 `front_owner_index`，formal RSM 要求 gen_id 全局唯一。 | RSM §3.6；024 D17/D18；FF consumer 会检查 duplicate gen_id。 | 轮换语义保留，但 gen_id 生成改为全局唯一 stride：`local_epoch * front_count + owner_id + 1`。单 front 下等价 old+1。 |
| `front_owner_index` | 旧 Step 17 没有该字段。 | M01/current code 已有 `front_owner_index`，默认 `UINT32_MAX` invalid。 | 024 D17；RSM §3.2；OV §5.1。 | front 创建每个 live gen 时必须赋 `owner_id`；M05 不允许 live gen 保留 invalid sentinel。 |
| collect eligible | Step 17 只从 current `imms` 选择 `max_lsn <= durable_lsn`，保序。 | M01 `imms` newest -> oldest。 | RSM §3.8；FF §2.1。 | 保留。active 永不参与；返回 shared_ptr pins。 |
| release gens | Step 17 只删除 current `imms` 中匹配 gen，不影响旧 snapshot。 | M02 PRS/read_handle 通过 shared_ptr pin gens。 | RMC §3/§8.4；FF §4.3。 | 保留。unknown/duplicate gen_id 输入幂等处理；不得强制释放对象。 |
| WAL stream | Step 17 明确不做；旧 `owner_impl.hh` 后续混入 mock WAL。 | M04 已在 `wal/scheduler.hh` 定义 `wal_stream_state`。 | RMC §7.1 说 WAL tail frame 归 front；CM 又要求 L2 模块互不依赖。 | M05 不 include `wal/scheduler.hh`，不把 `wal_stream_state` 放入 front memtable owner 实现。M06 前需要裁决 stream state 的头文件归属。 |
| failure boundary | Step 17 只是同步 contract。 | M03 已修 callback exception 与 state commit 边界。 | WP §10.4：memtable phase 失败 fatal。 | M05 sender 必须先提交状态再调用 callback；callback 抛异常不得伪装成 memtable apply 失败。memtable phase apply 失败不能走 release。 |

## 4. Front Owner State

M05 `front_sched` 概念字段：

```cpp
class front_sched {
    uint32_t owner_id;
    uint32_t front_count;

    uint64_t next_local_gen_epoch;
    std::shared_ptr<core::memtable_gen> active;
    std::vector<std::shared_ptr<core::memtable_gen>> imms; // newest -> oldest

    // request queues:
    // insert / lookup / batch_lookup / scan / seal / collect / release
};
```

### 4.1 Construction Preconditions

构造必须 fail-fast：

1. `front_count > 0`。
2. `owner_id < front_count`。
3. `queue_depth > 0`。
4. initial active 非空。
5. initial active `st == core::memtable_gen::state::active`。
6. initial active `front_owner_index == owner_id`。
7. initial active `gen_id` 符合 M05 gen id 规则，或 recovery/test 构造显式提供
   `next_local_gen_epoch` 并证明不会与其他 front 冲突。

这些是 runtime 配置 / recovery install / test helper 错误，不是客户端请求错误。
实现必须抛 `std::invalid_argument` / `std::logic_error` 或调用
`core::panic_inconsistency`；不能依赖 release build 会消失的 `assert`。

### 4.2 Generation ID

正式 RSM 写明 `memtable_gen::gen_id` 是全局唯一递增。旧 Step 17 的
`new_gen_id = old_gen_id + 1` 在单 front 测试里成立，但多 front 下会让每个
front 的 `gen_id` 碰撞。后续 flush fold 已经把重复 `gen_id` 视为
fail-fast correctness error，因此 M05 必须从一开始修正。

M05 使用固定 stride 规则：

```text
gen_id(owner_id, front_count, local_epoch)
  = local_epoch * front_count + owner_id + 1
```

规则：

1. `local_epoch` 从 0 开始单调递增。
2. `gen_id != 0`，便于保留 0 作无效/默认诊断值。
3. `front_count` 是 runtime lifetime 内固定参数。
4. 单 front (`front_count == 1`) 时，`gen_id = local_epoch + 1`，与旧 Step 17
   的 `old + 1` 语义一致。
5. seal 创建新 active 时使用当前 `next_local_gen_epoch`，然后递增。
6. 乘法/加法溢出必须 fail-fast，不能 wrap。

恢复路径如果要安装已有 gens，必须由 recovery/builder 提供足以保持该 stride
不变量的 `next_local_gen_epoch`。M05 不设计 recovery scanner，也不允许 front
在运行中改变 `front_count`。

### 4.3 `make_memtable_gen`

M05 实现应提供 owner-local helper，避免测试和后续代码漏填字段：

```cpp
std::shared_ptr<core::memtable_gen>
make_front_memtable_gen(uint32_t owner_id,
                        uint32_t front_count,
                        uint64_t local_epoch,
                        core::memtable_gen::state st);
```

helper 必须填：

1. `gen_id = gen_id(owner_id, front_count, local_epoch)`。
2. `st = active/sealed`。
3. `front_owner_index = owner_id`。
4. `min_lsn = UINT64_MAX`、`max_lsn = 0`。

禁止 production path 创建 `front_owner_index == UINT32_MAX` 的 live gen。

## 5. Sender Surface

生产 facade：

```cpp
namespace apps::inconel::front {

struct batch_lookup_item {
    std::string_view key;
    core::memtable_lookup_result result;
};
using batch_lookup_result = std::vector<batch_lookup_item>;

[[nodiscard]] auto insert_memtable_entries(
    front_sched& sched,
    core::front_fragment fragment,
    std::span<const core::canonical_entry> canonical_entries);

[[nodiscard]] auto lookup_memtable(
    front_sched& sched,
    std::string_view key,
    uint64_t read_lsn,
    core::front_read_set frs);

[[nodiscard]] auto batch_lookup(
    front_sched& sched,
    std::span<const std::string_view> keys,
    uint64_t read_lsn,
    core::front_read_set frs);

[[nodiscard]] auto scan_memtable(
    front_sched& sched,
    std::string_view begin,
    std::string_view end,
    uint64_t read_lsn,
    core::front_read_set frs);

[[nodiscard]] auto seal_active(front_sched& sched);
[[nodiscard]] auto collect_eligible_gens(front_sched& sched, uint64_t durable_lsn);
[[nodiscard]] auto release_gens(front_sched& sched, std::vector<uint64_t> gen_ids);

}
```

Value types：

1. `insert_memtable_entries(...) -> void`
2. `lookup_memtable(...) -> core::memtable_lookup_result`
3. `batch_lookup(...) -> batch_lookup_result`
4. `scan_memtable(...) -> core::memtable_scan_result`
5. `seal_active(...) -> core::front_read_set`
6. `collect_eligible_gens(...) -> std::vector<std::shared_ptr<core::memtable_gen>>`
7. `release_gens(...) -> void`

### 5.1 Borrowed Inputs

`insert_memtable_entries` 的 request 拥有 `front_fragment` by value，但
`canonical_entries` 是 borrowed span。span 指向 M01 `batch_ctx.canonical_entries`，
而每个 `canonical_entry.key/value` 又指向 `batch_ctx.input.bytes`。

因此调用方必须保证：

1. `batch_ctx` 在 sender callback 完成前存活。
2. `batch_ctx` 不在 sender pending 期间被 move 到会使 span 本身失效的位置。
3. value persist 已经为每个 PUT 写入 `allocated_vr`。

这是后续 write pipeline context 的职责。M05 不复制 value bytes，不把 fragment
indices 展开成 owning entries，也不保存 request 超过 callback。

`lookup_memtable` / `batch_lookup` / `scan_memtable` 的 key/range views 同样是
borrowed input。read pipeline 必须用 request context 持有 API key bytes 到
front sender 完成。`batch_lookup_result.key` 是输入 key 的同一 view；它不是
owning string。

### 5.2 Request Queues

M05 front scheduler 使用独立 request queue：

1. `insert_q`
2. `lookup_q`
3. `batch_lookup_q`
4. `scan_q`
5. `seal_q`
6. `collect_q`
7. `release_q`

每个 queue 在单次 `advance()` 中必须 bounded drain。可以仿 M03/M04 用固定
`kMaxDrainPerQueue`，避免单 queue 饥饿其他 front work。队列容量耗尽是 runtime
配置错误；fail-fast，不丢 request。

`advance()` 队列顺序建议：

```text
insert -> seal -> lookup -> batch_lookup -> scan -> collect -> release
```

这个顺序不是跨 scheduler 可见性来源；PUMP 入队顺序和 coord/pipeline 编排才是
正确性来源。实现可以选不同固定顺序，但必须 bounded 且可解释，不能把某个 queue
整条吃空导致其他 queue 长期饥饿。

## 6. `insert_memtable_entries`

### 6.1 Contract

```text
front::insert_memtable_entries(fragment, canonical_entries) -> void
```

前置：

1. `fragment.owner == front_sched.owner_id`。
2. `fragment.entry_count == canonical_entries.size()`。
3. 每个 `fragment.entry_indices[i] < canonical_entries.size()`。
4. fragment 由 M01 `build_batch_ctx` 产生；front 不重新 hash / reroute。
5. 对每个 PUT，`allocated_vr` 已由 value persist phase 填入。
6. all-WAL barrier 已经成功；memtable phase 已经开始。

实现必须在 mutation 前完成 1~3 的 validation。若 validation 失败，不能留下
partial insert。

duplicate index 是 M01 builder 的内部不变量。M05 hot path 不要求每次分配
scratch 去查重；如果未来增加非 M01 fragment builder，查重必须放在该 builder
边界。

### 6.2 Algorithm

```text
validate fragment owner / entry_count / index bounds

for idx in fragment.entry_indices:
    entry = canonical_entries[idx]
    key = entry.key

    if entry.op == put:
        core::insert_value(*active, key, fragment.batch_lsn, entry.allocated_vr)
    else:
        core::insert_tombstone(*active, key, fragment.batch_lsn)
```

`core::insert_value` / `insert_tombstone` 已经包含：

1. probe-then-allocate key arena。
2. `data_ver = batch_lsn`。
3. `min_lsn/max_lsn` 更新。
4. `value_handle` 只保存 durable `value_ref`。

禁止事项：

1. 不复制 value bytes。
2. 不创建 hot_blob / value_view。
3. 不读 tree / current DB 来决定 DELETE。
4. 不修改 `imms`。
5. 不把 validation / allocation failure 映射成 `coord::release_batch`。

### 6.3 Empty Fragment

正常 M01 `build_batch_ctx` 不会为无 entries 的 owner 创建 fragment。若测试或未来
adapter 传入空 `entry_indices`，`insert_memtable_entries` 是 no-op：

1. 不更新 active `min_lsn/max_lsn`。
2. 不创建 key。
3. 不 publish。
4. 不 rotate active。

## 7. Lookup / Batch Lookup / Scan

### 7.1 Point Lookup

```text
front::lookup_memtable(key, read_lsn, frs) -> core::memtable_lookup_result
```

语义完全复用 M02：

1. 只搜索传入 `frs.active + frs.imms`。
2. 不读取 `front_sched.active` 或 `front_sched.imms`。
3. winner 是所有命中中 `data_ver <= read_lsn` 的最大 `data_ver`。
4. value winner 返回 durable `value_ref`。
5. tombstone winner 返回 `core::memtable_tombstone`。
6. 无 winner 返回 `core::memtable_miss`。

dispatch 到 front scheduler 执行的原因是线程安全：PRS snapshot 里的 active gen
可能仍是当前 front 正在写的 active，必须与 insert 串行。

### 7.2 Batch Lookup

```text
front::batch_lookup(keys, read_lsn, frs) -> batch_lookup_result
```

语义：

1. 对输入 keys 按原顺序逐个调用 `core::lookup_memtable(key, read_lsn, frs)`。
2. 输出顺序与输入顺序完全一致。
3. 不引入新的 visibility 规则。
4. `batch_lookup_item.key` 是输入 `std::string_view` 的同一 view。
5. 空 keys 返回空 vector。

### 7.3 Scan

```text
front::scan_memtable(begin, end, read_lsn, frs) -> core::memtable_scan_result
```

语义完全复用 M02：

1. 半开区间 `[begin, end)`。
2. `begin >= end` 返回空。
3. 只搜索传入 PRS snapshot。
4. 同一 key 只输出最大 visible version。
5. 输出 key 升序。
6. tombstone item 保留，由后续 memtable-over-tree merge / API formatting 过滤。

scan result 中的 `key` view 指向 pinned `memtable_gen::kv_arena`。调用方必须持有
对应 `front_read_set` / `read_handle` 到结果消费完成。

## 8. `seal_active`

### 8.1 Contract

```text
front::seal_active() -> core::front_read_set
```

M05 只实现 local front 的 rotate。seal 发起、publish gate close/open、CAT1
install、batch 不跨代编排属于 M12。

### 8.2 Algorithm

```text
old = active
old->st = sealed

new = make_front_memtable_gen(owner_id,
                              front_count,
                              next_local_gen_epoch,
                              core::memtable_gen::state::active)
next_local_gen_epoch++

active = new
imms.insert(imms.begin(), old)

return front_read_set {
    active = active,
    imms = imms,
}
```

规则：

1. old active 可以为空表；空 sealed gen 仍可进入 imms。
2. old active `front_owner_index` 必须等于 owner_id。
3. new active `front_owner_index` 必须等于 owner_id。
4. imms 顺序为 newest -> oldest。
5. 返回 snapshot 必须包含新 active 与 rotate 后的完整 imms。
6. seal 不发布 CAT，不关闭 gate，不等待 WAL，不触发 tree flush。

## 9. `collect_eligible_gens`

```text
front::collect_eligible_gens(durable_lsn)
  -> std::vector<std::shared_ptr<core::memtable_gen>>
```

语义：

1. 遍历当前 `imms`，不看 active。
2. 只选择 `gen->st == sealed` 且 `gen->max_lsn <= durable_lsn` 的 gens。
3. 输出顺序保持当前 imms 相对顺序，也就是 newest -> oldest 过滤后保序。
4. 返回 `shared_ptr` pins；flush reducer 持有后，后续 release/seal 不会提前释放。
5. empty sealed gen 的 `max_lsn == 0`，因此在 `durable_lsn >= 0` 下 eligible。
   这是允许的：empty-delta flush 必须仍能 release 这些 gens。

若在 imms 中发现 null gen、非 sealed gen 或 `front_owner_index != owner_id`，
这是 owner state corruption，必须 fail-fast。

## 10. `release_gens`

```text
front::release_gens(gen_ids) -> void
```

语义：

1. 只从当前 `imms` 移除 matching `gen_id`。
2. 不修改 active。
3. 不修改旧 `front_read_set` snapshot。
4. 不 drain `loser_durable_refs`；value reclaim / guard retired consumer 不属于 M05。
5. unknown gen_id 忽略。
6. duplicate gen_id 输入幂等。
7. 保留未移除 imms 的原相对顺序。

`release_gens` 不是释放对象的命令；它只是 front owner 放弃当前 runtime list 中的
引用。真正析构由 `std::shared_ptr` refcount 决定。

## 11. State Machines

### 11.1 Memtable Generation

```text
ACTIVE(owner=F, gen=G)
  -- insert_memtable_entries -->
ACTIVE(owner=F, gen=G, table grows)

ACTIVE(owner=F, gen=G)
  -- seal_active -->
SEALED(owner=F, gen=G, in current imms)

SEALED(owner=F, gen=G, in current imms)
  -- collect_eligible_gens pins -->
SEALED(owner=F, gen=G, pinned by flush round)

SEALED(owner=F, gen=G, in current imms)
  -- release_gens(G) -->
REMOVED_FROM_FRONT_IMMS
  -- last PRS/flush/front shared_ptr drops -->
DESTRUCTED
```

Invalid transitions:

1. `SEALED -> ACTIVE`。
2. `ACTIVE -> REMOVED_FROM_FRONT_IMMS`。
3. `release_gens` 删除 active。
4. `insert_memtable_entries` 写入 sealed gen。
5. live gen 没有 valid `front_owner_index`。

### 11.2 Insert Request

```text
NEW
  -- sender start -->
QUEUED
  -- advance dequeue -->
VALIDATING
  -- validation ok -->
APPLYING
  -- all entries inserted -->
COMMITTED
  -- callback -->
DONE
```

validation error 在 `APPLYING` 前发生，不得留下 partial mutation。allocation /
unexpected exception 如果发生在 `APPLYING` 中，已经处于 memtable phase；上层不得
release 该 LSN，必须按 fatal path 处理。

### 11.3 Snapshot Query

```text
NEW
  -- sender start -->
QUEUED
  -- front owner thread -->
SEARCH_CALLER_FRS
  -- callback result -->
DONE
```

查询不会读取 current topology。`release_gens` 之后旧 snapshot 仍可读，是此状态机的
核心不变量。

## 12. Invariants

### 12.1 Owner / Routing

1. same key -> same front scheduler 由 M01 routing 保证。
2. front scheduler 不重新 hash，也不修正 wrong-owner fragment。
3. `fragment.owner != owner_id` 是 caller / pipeline bug，fail-fast。
4. 每个 live `memtable_gen.front_owner_index == owner_id`。
5. 每个 live `memtable_gen.gen_id` 全局唯一；M05 使用 stride 生成。

### 12.2 Visibility

1. `data_ver == fragment.batch_lsn`。
2. lookup/scan winner 只由 `data_ver <= read_lsn` 最大值决定。
3. tombstone 不回退旧 value。
4. topology order 不代表 version order；active 命中不能提前返回。
5. `release_gens` 不影响任何已发布 PRS snapshot。

### 12.3 Memory / Ownership

1. `kv_arena` 只保存 key bytes。
2. memtable 不保存 value bytes、hot_blob、value_view 或 client input pointer 作为 value body。
3. PUT entry 只保存 durable `value_ref`。
4. `front_read_set` / collect result 通过 shared_ptr pin gens。
5. `batch_lookup_result.key` 和 `scan_result.key` 都是 non-owning views；调用方负责持有相应 input / read_handle。

### 12.4 Queue / Callback

1. request node 生命周期由 `std::unique_ptr` 或等价 RAII 管理；callback 抛异常不能泄漏 request。
2. state commit 后再调用 callback。
3. callback exception 按 PUMP exception path 传播；不得转成 scheduler semantic failure。
4. queue full fail-fast，不 drop。
5. `advance()` bounded；不能无界 drain 单 queue。

## 13. Memory Ordering

M05 的 correctness state 是 single-owner：

1. `active`、`imms`、`next_local_gen_epoch` 只在 owning `front_sched` thread 上读写。
2. `memtable_gen::table` 和 `kv_arena` 只在 front owner thread 上 mutation。
3. sealed gens 对 table/arena immutable。
4. PRS/CAT 发布和 read_handle acquire 的跨线程顺序由 M02/M03 的 atomic
   `shared_ptr` / `durable_lsn` acquire-release 链保证；M05 不新增发布协议。
5. shared_ptr control block refcount 是 memtable pin 链唯一跨线程 atomic gate。
6. PUMP queue handoff 负责 request 从 caller 到 owner thread 的调度边界；禁止加
   ad hoc mutex。

M05 不需要新 atomic。若实现顺手暴露 RSM §2.6 提到的
`active_memory_usage` / memtable pressure counter，只能作为 heuristic：

1. front owner `store(memory_order_relaxed)`。
2. coord seal trigger `load(memory_order_relaxed)`。
3. stale value 只影响 seal 触发时机，不影响 visibility / recovery correctness。
4. 该 counter 不属于 M05 验收项；不能把它作为 release/publish/read 的正确性 gate。

## 14. Error / Failure Semantics

### 14.1 Construction Errors

fail-fast：

1. invalid `owner_id/front_count`。
2. null active gen。
3. active gen state 不为 active。
4. active gen owner mismatch。
5. gen id stride 不满足全局唯一方案。
6. queue depth 为 0。

### 14.2 Insert Errors

validation failure：

1. fragment owner mismatch。
2. fragment entry_count mismatch。
3. fragment index out of range。
4. active missing / active not active / owner mismatch。

这些必须在 mutation 前检测。

apply failure：

1. allocation failure。
2. container invariant failure。
3. impossible op enum。

这些发生在 memtable phase。M05 不 rollback，不 release LSN，不把异常变成可继续的
client error。上层 write pipeline 必须按 WP §10.4 fatal。

`allocated_vr` 的完整合法性需要 value area profile / value allocator truth。
front owner 不自建第二套 validator；它只复制 caller 提供的 durable `value_ref`。
value persist / write pipeline 必须保证 PUT entry 已初始化。

### 14.3 Lookup / Scan Errors

lookup/batch/scan 正常情况下不失败。以下是 caller bug / state corruption：

1. `front_read_set` 中某 gen owner mismatch。
2. result key view 被调用方在消费前释放。
3. scan result 离开 read_handle lifetime 后继续使用。

M05 可以在 owner mismatch 时 fail-fast；不能为了“看起来成功”改查 current state。

### 14.4 Seal / Collect / Release Errors

fail-fast：

1. seal 时 active null / wrong owner / already sealed。
2. gen id overflow。
3. collect 时 current imms 出现 null / active state / wrong owner。
4. release 时 current imms 出现 null / wrong owner。

unknown release id 不是错误。duplicate release id 输入幂等。

### 14.5 Callback Exceptions

所有 handle 遵循 M03 修正后的模式：

```text
try:
    result = compute_and_commit_state()
catch:
    delete req
    push_exception(...)
    return

cb(result)   // outside compute catch
delete req   // RAII even if cb throws
```

对 insert 来说，`compute_and_commit_state()` 一旦进入 mutation 后抛异常，语义上属于
fatal memtable phase failure。实现不应尝试“清理一半 entries 然后 release”。

## 15. 测试计划

新增 target 建议：

```text
inconel_test_m05_front_scheduler_memtable_owner
```

覆盖：

1. construction：
   - owner/front_count validation。
   - initial active 必须 state active、owner 正确。
   - live gen 必须填 `front_owner_index`。
   - 多 front stride gen_id 不碰撞；单 front 下等价 old+1。
2. insert：
   - PUT/DEL 写入当前 active。
   - `data_ver == batch_lsn`。
   - durable `value_ref` 原样进入 memtable。
   - no hot/value body field 参与。
   - empty fragment no-op。
   - wrong owner / bad index 在 mutation 前失败。
3. snapshot lookup：
   - old snapshot 在 seal + release 后仍可读。
   - lookup 不读 current active/imms。
   - active 命中不能提前返回；更大 visible imm 版本能赢。
   - tombstone winner 不回退旧 value。
4. batch lookup：
   - 输入顺序保持。
   - 空输入返回空。
   - result key view 与输入 key 对齐。
5. scan：
   - `[begin,end)`、`begin >= end` 空结果。
   - sorted winners。
   - tombstone 保留。
   - release 后旧 snapshot scan 仍可见。
6. seal：
   - old active -> sealed。
   - new active owner/gen_id/state 正确。
   - old push_front imms。
   - 返回 `front_read_set` 是 rotate 后 topology。
   - empty active seal 被允许。
7. collect/release：
   - collect 只看 current imms，active 永不返回。
   - `max_lsn <= durable_lsn` 过滤。
   - empty sealed gen eligible。
   - release unknown/duplicate id 幂等。
   - release 不破坏旧 read_handle / old FRS pin。
8. sender facade：
   - 每个 M05 sender 通过 `submit + then + advance` smoke。
   - `op_pusher` / `compute_sender_type` value type 正确。
   - callback 抛异常后 request 不泄漏，已提交 state 不回滚。
9. queue/fairness：
   - bounded advance 不无界吃空单 queue。
   - queue full fail-fast。

旧 Step 17 测试迁移时必须改掉两个旧语义：

1. PUT hot_blob 内容断言删除，改为 durable `value_ref` 断言。
2. 多 front 下 `seal_active` 新 gen_id 不能断言 old+1；只能在 `front_count == 1`
   的测试里保留该断言。

## 16. 排除范围

M05 不做：

1. WAL append / prepare / segment header / sealed trailer / FUA issue。
2. `write_wal_entries` sender。
3. `wal_stream_state` 接入 front scheduler。
4. Value persist / value read / value cache residency。
5. top-level `write_batch` pipeline。
6. point_get / multiget / scan public runtime API。
7. tree lookup sender 或 memtable-over-tree merge。
8. seal round、publish gate close/open、CAT1 install。
9. flush frontier switch / `capture_flush_frontier` / `frontier_switch`。
10. recovery scanner、recovered memtable install、front_count 变更处理。
11. runtime builder / thread placement / core affinity。

## 17. 设计冲突与后续裁决

### 17.1 WAL Stream Header Boundary

正式 RSM 的完整 `front_state` 包含 `wal_stream_state`，M04 又把
`wal_stream_state` 定义在 `apps/inconel/wal/scheduler.hh`。但
`code_modules.md` 明确 L2 scheduler 模块之间互不依赖，且其他模块只能 include
目标模块的 `sender.hh`。

M05 决议：

1. M05 front scheduler 只包含 memtable owner state。
2. M05 `front/scheduler.hh` 不 include `wal/scheduler.hh`。
3. M05 不实现 `write_wal_entries`，也不持有 WAL tail frame。

M06 前必须人工裁决/落文档的选项：

1. 把 front-local `wal_stream_state` 移到 `front/`。
2. 抽出非 scheduler 的 `wal/types.hh` / `core/wal_stream_state.hh`，供 front 与 wal
   共同 include。
3. 修改 `code_modules.md`，明确允许 front 对 wal stream carrier 的特例依赖。

在这个裁决前，实现 agent 不应通过直接 include `wal/scheduler.hh` 绕过模块边界。

### 17.2 Gen ID 规则

旧 Step 17 与正式 RSM 在 gen_id 唯一性上有差异。M05 已按正式 RSM 和后续
flush duplicate-gen fail-fast 选择 stride gen_id。若实现 agent 认为必须沿用
old+1 per-front，需要先申请人工判断，不能直接落地。

## 18. 相邻项提醒

1. M06 是自然下一步：在同一个 front scheduler 上接 WAL append prepare + bounded
   FUA issue。但 M06 必须先解决 §17.1 的 WAL stream 头文件归属。
2. M12 会消费 M05 的 `seal_active` / `collect_eligible_gens` / `release_gens`
   做 seal/CAT1/frontier switch；M05 只冻结 local handle 语义。
3. RSM §2.6 的 memtable pressure counter 更适合在 M12 seal trigger 设计中统一
   收敛；M05 不应为通过测试发明临时正确性 gate。
