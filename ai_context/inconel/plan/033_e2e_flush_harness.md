# Step 033 — Tree-Local Flush 端到端 Harness (`test_flush_e2e`)

> 状态：**harness landed + 首轮端到端 PASS**（2026-04-19）。`./build/inconel_test_flush_e2e` 在目标拓扑（cores 0/2/4/6/8，rd 2/4/6，value 0，owner 8）上对 1000-key 单轮 flush 走通，`st=0 / leaf_order=12 / root_slot.lba=4015 / readback 51 strided samples OK`。首跑暴露的 Phase 9 空树 bootstrap 缺口 INC-042 已在同日后续会话关闭，改动落在 production 侧四处（`memtable_fold` / `candidate_build` / `owner_scheduler` / `sender.hh`）——详见 §12.4。readback 从最初的 5 个 boundary 点扩到均匀步长 ~50 samples（含首尾兜底，1000 keys 实际 51 samples），确保 `leaf_order` 每张 page 都被读回至少一次——详见 §12.6。所有 scope 决策见 §9；实现后观察见 §12。
>
> **定位：033 只交付端到端 test harness，production 代码一律不改。** 跑 harness 发现的 production bug 新起会话修（比如 Phase 9 没闭的 empty-tree bootstrap 路径、merge 对空 base_manifest 的处理、shard_partition rebuild 等等），**不在本 step 内**预先改。
>
> Phase 9 的验收口径在 `feedback_module_complete_gates_next_module` / `feedback_perf_profile_on_module_e2e` 下是 "有 e2e 端到端覆盖才能说 flush 模块完成"。本 step 是 flush 模块的第一份 e2e harness，名字 `test_flush_e2e` 不挂 key 数量——后续 multi-round / 大 key 量 / 偏斜分布 / value 集成验证都继续往这个 binary 里扩。
>
> 设计依据：`design_overview.md` §9 / §10 / §14（flush pipeline + frontier switch）、`flush_and_frontier_switch.md` §3 / §4、`runtime_state_machine.md` §3 / §4、`runtime_memory_and_cache.md` §9、`flush_module_guide.md`、`flush_development_plan.md` §2 / §3、step 030 `tree_read_domain` 拓扑契约、step 031 §12 runtime 可用性收敛、step 032 `make_formatted_storage`。

---

## 1. 目标与非目标

### 1.1 目标

从 `make_formatted_storage` 产出的空盘起，用非对称拓扑（3 个独立 `tree_read_domain` 核）起 runtime，构造一个带 N 个 PUT 的 sealed `memtable_gen`，手动安装一张 3-shard `shard_partition_map`，然后调一次 `tree::tree_local_flush(req)` 走完整 flush pipeline，最后用 `tree::lookup` 抽样读回验证。

| 目标 | 说明 |
|---|---|
| **走通 tree-local flush pipeline 第一轮** | `submit_flush_fold → collect_worker_proposals → drive_merge_loop → nvme flush → submit_finalize_merge` 整条串能从 empty base_manifest 收敛到 `tree_flush_result.st == ok` + non-null `new_manifest`。 |
| **3 个 worker 都被自然驱动** | 3-shard `shard_partition_map` + fold 侧 `build_key_partitions` + `collect_worker_proposals` 的 fan-out 本身已经做这件事——harness 不加显式计数器，只用 "3-shard map 覆盖 1000 keys 均分到每 shard ~333 keys" 这个输入保证路径必然被触发。 |
| **readback 抽样校验** | 随机挑几个 key 走 `tree::lookup` 读回，value bytes 对得上，证明 leaf page 写出去 + slot_map / leaf_order / node_cache 读回都 wire up 了。 |
| **Harness 可扩展** | key 数量 / shard fence / gen 构造 / 单轮 flush 全都函数化参数化（见 §3），让未来多轮 flush、100K+ key、偏斜分布、多 gen 合并、value 集成直接在本 binary 里加。 |

### 1.2 非目标

| 非目标 | 原因 |
|---|---|
| 改任何 production 代码 | 用户明确：跑起来发现 bug 新会话修，不要预先改。`build_key_partitions` 对空树的 `unsupported_shape_change` narrowing、`owner_scheduler::run_merge` 对空 base_manifest 的处理、shard_partition rebuild 等全部视为"production 假设已工作"，harness 真跑时若 break 则打印上下文 fail-fast。 |
| 100K+ key 压测 / profile binary | 后续 step 负责，本 step 只验 1000 keys 的功能正确性。profile 的前提是功能验证先过。 |
| 多轮 flush / 多 gen 合并 | 033 只跑 1 轮，但 §3 / §4 的函数拆法保留扩展空间。 |
| WAL / coord scheduler integration | 两个模块还没写。本 harness 只 exercise tree / value / mock_nvme / runtime 四层。 |
| value 集成验证（读回对比字节） | 本 harness 会 persist 1000 个真 value 并在 flush 前构 `value_handle.durable`；readback sample 会对比 bytes——已经顺手覆盖最小 value 集成，但不做大规模 value / size-class / sub-LBA 覆盖。 |
| 偏斜分布 / 长 key / sub-LBA value 混合 | 033 用最干净的均分 + 定长 key + 定长 value，把变量控到最少。后续扩 harness 时再加。 |
| 显式 "3 worker 各进了 submit_flush_round N 次" 计数 | 要给 `tree_worker_sched` 加字段，属于 production 改动。key_range 分区自然展开已经隐式覆盖。 |
| 硬件核数 skip 保护 | 用户确认当前机器 >= 9 核，未来走配置文件。 |

---

## 2. 拓扑（硬编码）

```
cores              = {0, 2, 4, 6, 8}
read_domain_cores  = {2, 4, 6}
value_core         = 0
owner_core         = 8
```

| 选择 | 理由 |
|---|---|
| 跳跃式核号 | 对齐 step 031 §12.1 "生产目标拓扑"；不让 harness 把 "连续核" 当隐式假设。 |
| `read_domain_cores.size() == 3` | 直接决定 `shard_count() == 3`（bootstrap 时 builder 用 `read_domain_cores.size()` 作 shard 数），与 3-shard `shard_partition_map` 一一对应。 |
| value / owner 各独占一核 | 避免跟 worker 挤同核形成隐式串行化假象；同时顺手 exercise 非对称拓扑下 `advance()` loop（step 031 §12.1 / §12.3 的 `rt::run`）。 |
| 未来走配置文件 | WAL / coord 落地后拓扑会再改，到时改 harness。 |

---

## 3. Harness 结构

单文件 `apps/inconel/test/test_flush_e2e.cc`，内部按下列 layer 组织，每 layer 对应一个小函数 / 小 struct，不让主 flow 内联大段构造逻辑。

### 3.1 CLI 参数

```cpp
struct harness_options {
    uint32_t num_keys = 1000;   // --num-keys
};
harness_options parse_argv(int argc, char** argv);
```

只有一个 knob。解析失败直接 `std::exit(2)` 打 usage。

### 3.2 Key / value 生成（按 `num_keys` 动态 derive）

```cpp
struct key_spec {
    uint32_t num_keys;
    uint32_t key_digits;   // = ceil(log10(num_keys))，至少 3
    // key_i = "key_{i:0{key_digits}d}"
};

struct kv_record {
    std::string key;      // 拥有字节，用于分区 fence 引用 & memtable_gen arena copy
    std::string value;
};

std::vector<kv_record>
generate_kv_records(const key_spec& spec);  // ascending by key
```

理由：
- `num_keys=1000 → key_digits=3 → key_000..key_999`；
- `num_keys=100000 → key_digits=5 → key_00000..key_99999`；
- `num_keys=1_000_000 → key_digits=6`；
- 保证 ascii `<` 比较顺序和 `uint32 i` 顺序一致，分区 fence 写起来不踩坑。

Value 暂时用 `"val_" + same_suffix`，长度和 key 数量同阶——简单可复现。

### 3.3 3-shard `shard_partition_map` 构造

```cpp
std::shared_ptr<const core::shard_partition_map>
build_harness_shard_map(const std::vector<kv_record>& sorted_records);
```

策略：

```
N = sorted_records.size()
fence_0 = sorted_records[N / 3].key          // shard 0 upper
fence_1 = sorted_records[2 * N / 3].key      // shard 1 upper
shard 0 = (-∞, fence_0)
shard 1 = [fence_0, fence_1)
shard 2 = [fence_1, +∞)   — fence_upper_len=0 sentinel
```

`fence_pool` 里依次 append `fence_0` / `fence_1`；`shards[2].fence_upper_len == 0` 作 +∞ sentinel（`shard_partition_map::route` 契约 §3.1）。

**扩展性**：未来想做 skewed / M-shard，直接改这个函数，核心 pipeline 不变。

**覆盖 bootstrap 的方式**：`build_runtime` 完后调 `rt::publish_shard_partitions(my_map)`（facade 已有 API），在 spawn advance 线程之前做，保证所有 read_domain snapshot 都见到新 map。

### 3.4 Value persist：1000 条 real value_ref

Flush 路径的 winner 会把 `value_handle.durable: value_ref` 透传到新 leaf 的 `leaf_record.value_ref`。harness 必须先跑一次 `value::persist_values(entries)` 把 1000 条 value 真正写到 NVMe value area，拿到合法 value_ref，再构 `memtable_entry.vh.durable`——不能 dummy，否则 readback 时 value scheduler load 会 CRC 失败。

```cpp
struct persisted_kvs {
    std::vector<format::value_ref> durables;   // index-aligned with records
};

persisted_kvs
persist_all_values(const std::vector<kv_record>& records,
                   pump::core::root_context ctx);
// 内部：组 value::persist_entry[] → just() >> value::persist_values(entries)
//       >> then(promise.set_value) >> submit(ctx)
//       主线程 future.get() 拿回 durables
```

抽成函数是因为多 gen / 多轮 flush 要反复用。

### 3.5 `memtable_gen` 构造

```cpp
std::shared_ptr<core::memtable_gen>
build_sealed_gen(uint64_t gen_id,
                 uint32_t front_owner_index,
                 uint64_t lsn_start,               // data_ver 从此起 +1 每条
                 std::span<const kv_record>     records,
                 std::span<const format::value_ref> durables);
// 输出 gen:
//   - st = sealed
//   - kv_arena 拷贝一遍 key 和 value bytes
//   - table[arena_key_view] = { data_ver, kind::value, vh{ durable, hot{arena_value_view} } }
//   - min_lsn = lsn_start, max_lsn = lsn_start + records.size() - 1
```

理由：
- 未来多轮 flush 就是调这个函数两次，gen_id / lsn_start / records span 不同；
- `kv_arena` 为什么必须显式 allocate：`memtable_entry.vh.hot` 要指向 gen-owned bytes（`core/memtable.hh` 注释明确 kv_arena 拥有 key 和 value 字节），不能借用 harness 栈上的 `std::string`——一旦 gen 还活着 harness 这边就释放了就 use-after-free。

### 3.6 `checkpoint_guard` + `tree_flush_request`

```cpp
tree::tree_flush_request
make_flush_request(std::shared_ptr<core::memtable_gen> gen);
// 构造 empty manifest + empty guard（base_guard.manifest = empty_manifest）
// sealed_gens = { std::move(gen) }
// recovery_safe_lsn = 0
```

首轮 flush 的 base_guard 用 `tree_manifest::empty(&kBootstrapTreeGeometry)` 包进 `shared_ptr<const checkpoint_guard>`。

**未来多轮 flush** 把本轮 `tree_flush_result.new_manifest` 包进下一轮的 `base_guard`——手动做这个包装也留在 harness 里一个 helper 函数（本 step 不实装，只预留注释）。

### 3.7 单轮 flush 执行

```cpp
struct flush_round_outcome {
    tree::tree_flush_result result;
    // 抽 readback 需要的字段也放这里，避免 harness 到处拿 result
    std::shared_ptr<const core::tree_manifest> manifest_for_readback;
};

flush_round_outcome
run_one_flush_round(std::shared_ptr<core::memtable_gen>  gen,
                    std::shared_ptr<const core::checkpoint_guard> base_guard,
                    pump::core::root_context ctx);
// 内部：just() >> tree::tree_local_flush(req) >> then(promise.set)
//       >> submit(ctx); future.get()
```

**这是留给多轮 flush 的关键 seam**——本 step 调一次，后续 step 调多次。

### 3.8 Readback 抽样

```cpp
struct readback_sample {
    std::string_view key;
    std::string      expected_value;
};

void
verify_readback_samples(const core::tree_manifest* manifest,
                        std::span<const readback_sample> samples,
                        pump::core::root_context ctx);
// 组 just() >> tree::lookup(keys, manifest) >> then(assert) >> submit(ctx)
// mismatch 直接 std::abort 并打印 key / expected / got
```

抽样策略：`sample_keys = { "key_000", "key_{N/4}", "key_{N/2}", "key_{3N/4}", "key_{N-1}" }` — 横跨所有 3 个 shard，每 shard 至少 1 个。

### 3.9 主 flow

```cpp
int main(int argc, char** argv) {
    auto opts = parse_argv(argc, argv);
    auto spec = key_spec{ .num_keys = opts.num_keys, .key_digits = ceil_log10(opts.num_keys) };
    auto records = generate_kv_records(spec);

    // ── disk ───────────────────────────────────────
    auto fmt_opts = derive_format_options();               // §3.10
    auto buf = format::make_formatted_storage(fmt_opts, kNamespaceBytes);
    mock_nvme::mock_device device(std::move(buf), kNamespaceBytes, fmt_opts.lba_size);

    // ── runtime ────────────────────────────────────
    runtime::build_options bopts = build_topology(&device);   // §2 硬编码
    auto* rt = runtime::build_runtime<core::clock_cache, core::clock_cache>(bopts);

    // ── 覆盖 bootstrap shard_partition_map ─────────
    rt::publish_shard_partitions(build_harness_shard_map(records));

    // ── 起 per-core advance threads ────────────────
    std::vector<std::jthread> workers;
    for (auto core : kCores) {
        workers.emplace_back([rt, core] { rt::run(rt, core); });
    }

    // ── submit 用的 context（主线程） ──────────────
    pump::core::this_core_id = 0;                          // 任意 valid core id
    auto ctx = pump::core::make_root_context();

    // ── persist values → build gen → flush → readback ─
    auto persisted = persist_all_values(records, ctx);
    auto gen       = build_sealed_gen(/*gen_id=*/ 1,
                                      /*front_owner_index=*/ 0,
                                      /*lsn_start=*/ 1,
                                      records, persisted.durables);

    auto empty_guard = make_empty_base_guard();
    auto outcome     = run_one_flush_round(gen, empty_guard, ctx);

    validate_flush_result(outcome.result);                 // §5
    verify_readback_samples(outcome.manifest_for_readback.get(),
                            pick_samples(records), ctx);

    // ── stop runtime ───────────────────────────────
    for (auto core : kCores) rt->is_running_by_core[core].store(false);
    workers.clear();   // jthread 析构 join
    runtime::destroy_runtime(rt);

    return 0;
}
```

**重点 extensibility 点**：
- `persist_all_values` / `build_sealed_gen` / `run_one_flush_round` 都是可重复调的；
- 多轮 flush 就是把 persist + build + run 包成一个 for loop；
- 大 N 就是 `--num-keys` 传大值，key_digits / fence / sample 都跟着 derive；
- 多 gen 单轮就是 `sealed_gens` 里多塞几个 build_sealed_gen 的产物。

### 3.10 `format_options` 构造

来源 `format::kBootstrapFormatProfile`（runtime builder 用同一个，避免 `validate_build_inputs` tier 3 失败）。但 `format_options` 还有 WAL 参数（`wal_segment_size` / `wal_segment_count`），profile 里没有——按 step 032 §2.5 的"测试 fixture 自己传"规则硬编：

```cpp
format::format_options {
    .lba_size               = 4096,
    .tree_page_size         = 16384,
    .shadow_slots_per_range = 4,
    .value_class_count      = profile.class_sizes().size(),
    .value_class_sizes      = profile.class_sizes(),       // 抄 profile
    .wal_segment_size       = 1 MiB,
    .wal_segment_count      = 8,
}
```

`namespace_size` 按 1000 keys + value area headroom 估，先定 64 MiB 足够；后续 100K+ 再放大。

---

## 4. 主线程与 advance 线程的分工

### 4.1 模型

- 5 个 `std::jthread`，每个跑 `apps::inconel::rt::run(rt, core)`——step 031 §12.3 的 inconel 专属 advance loop，会在 `is_running_by_core[core]` 变 false 时退出。
- **主线程**不 run 任何 advance loop。只做：
  1. Submit pipeline + block 在 `std::future::get()` 等 scheduler 回调 resolve promise；
  2. 所有 flush / lookup / persist 完了之后，遍历 `kCores` 把 `is_running_by_core[c].store(false)`；
  3. `workers.clear()` 触发 jthread 析构 join。
- 主线程 `this_core_id` 随便设一个 valid core id（0 够用）——`per_core::queue` 只要 `this_core_id` 在合法范围就能 `try_enqueue`。

### 4.2 为什么不用 `pump::env::runtime::start`

`share_nothing::start` 的语义是 "主线程 = 某个 advance core，start 阻塞直到所有核都停"。harness 要在主线程同步等 flush 结果，不能让主线程陷入 `start` 的 advance loop——所以 roll 自己的。

### 4.3 Stop 顺序

1. 主线程看到 flush future fire + readback 完成；
2. **先** `is_running_by_core[core].store(false)` 所有核，**再** workers.clear()；
3. 切勿在 advance 还在跑的情况下直接 `destroy_runtime`——scheduler 析构顺序和 advance 读顺序会冲突。

---

## 5. Validation

harness 只做下列最小验证。任何不满足就打印详细上下文 + `std::abort()`，返回非 0 给 ctest。

| 断言 | 失败时打印 |
|---|---|
| `outcome.result.st == flush_stage_status::ok` | `st` 枚举值 + `fold_result.partitions.size()` + `worker_proposals.size()` + `new_manifest==nullptr` 的标志位 |
| `outcome.result.new_manifest != nullptr` | 无（和上一条一起） |
| `outcome.result.new_manifest->has_root()` | `root_slot.lba` + `root_range_base.lba` + `leaf_order.size()` |
| `outcome.result.new_manifest->leaf_order.size() >= 1` | `leaf_order.size()` + `slot_map.size()` + `reverse_topology.leaf_parent_idx.size()` |
| `outcome.result.flushed_gens_by_front` 包含 gen.front_owner_index=0 且含 gen_id=1 | 每个 front 的 flushed gens 列表 |
| `outcome.result.flushed_max_lsn == gen.max_lsn` | 两者值 |
| Readback sample 每个 key 返回 `lookup_leaf_value` 且 value bytes 对得上 | key / expected / got |

**没有** worker 计数 / per-core NVMe write 分布 / proposal vector 长度检查——由 key_range 分区保证（Q5 决议）。

Validation 失败 → 就是 production bug（大概率 Phase 9 的空树 bootstrap 路径没闭环），新会话修。

---

## 6. 文件与 CMake

| 项 | 值 |
|---|---|
| 文件 | `apps/inconel/test/test_flush_e2e.cc` |
| CMake target | `inconel_test_flush_e2e` |
| `add_executable` | `apps/inconel/test/test_flush_e2e.cc` |
| `target_link_libraries` | 和 `inconel_test_runtime_topology` 同套（pump / absl / spdk-free fully mock_nvme 链） |
| ctest | `add_test(NAME inconel_test_flush_e2e COMMAND inconel_test_flush_e2e)` |
| 默认 argv | 无（`num_keys` 用默认 1000），ctest 跑时不传参 |

**不加 `-DNUM_KEYS=…` 之类 cmake 选项**——都走 CLI arg。

---

## 7. 假设 production 层已工作（跑 harness 时才验）

以下路径 harness 假设能跑通；跑不通 → production bug，新会话修。033 不预先改任何一条。

| 假设 | 风险点 | 会 break 的症状 |
|---|---|---|
| `tree::tree_local_flush` 从空 `base_manifest` 起能产非空 `new_manifest` | `build_key_partitions` 对空树返回 `unsupported_shape_change`（`tree/memtable_fold.hh:234`，Phase 9 territory 未关） | `fold_result.st != ok` → `collect_worker_proposals` 空 iter → merge 空输入 → `tree_flush_result.st==ok` 但 `new_manifest.has_root()==false` |
| `owner_scheduler::run_merge` 对 `worker_proposals` 都产 `replaces_old_paddrs.empty()` 的 fresh leaves 能正确分配新 range + 造 root | 031 merge 协程的 `walk_stack` / `assign_and_emit_node` 对空 base_manifest 可能 panic 或 produce 错误 slot 分配 | merge 阶段 panic，或 tree_flush_result 内 manifest 字段不自洽 |
| `value::persist_values` 在非对称拓扑下（value_core=0，其它核是 read_domain / owner）工作正常 | value scheduler 的 drain 逻辑依赖 `this_core_id`；主线程 submit 时设 `this_core_id=0` 要和 value_core 匹配 | persist 阶段 hang 或 CRC 错 |
| `rt::publish_shard_partitions` 在 advance 线程起之前调用安全 | facade 内 iterate `tree_read_domains.list` 改 `rd->partitions`，如果 advance 线程还没起，不会 race | 如果 race，flush fold 看到旧 single-shard map，所有 key → shard 0 |
| `tree::lookup` 通过 `node_cache` 从 `mock_nvme` 读回 leaf page 并解析 leaf_record 返回 value | readback 端要求 tree page bytes + slot_map + leaf_order + value scheduler cache 全部正确 | readback sample 拿到 `absent` 或字节不对 |

**Harness 策略**：任何一条假设 break，打印相关上下文 `abort()`，让用户开新会话定位 production bug。

---

## 8. Checklist

### 8.1 开工

- [ ] plan 被用户 review 通过
- [ ] 核对 `inconel_test_runtime_topology` 的 runtime bring-up 和 teardown pattern（**由设计/测试维护者角色打开 test 文件时需先显式声明**，CLAUDE.md 最高规则）
- [ ] 确认 `design_overview.md` / `flush_and_frontier_switch.md` 已经定义了空树 bootstrap 的期望行为——如果**设计本身没定**，按 CLAUDE.md 约束 C 停下报 spec gap，不自补
- [ ] 确认 `num_keys` CLI 的默认 1000 在 1 GiB RAM 内能跑完（粗估 value area 4 KiB × 1000 ≈ 4 MiB，tree area 几个 page，完全够）

### 8.2 实现

- [ ] **不读 production 之外任何 test 文件**反推 harness 结构（最高规则；只允许在"我要读测试文件"显式声明后读 `test_runtime_topology.cc` 等已有 bring-up pattern）
- [ ] 不给 production 代码加字段 / 改常量 / 改接口
- [ ] `num_keys` CLI parse 失败 → `std::exit(2)` 打 usage，不 silent fallback
- [ ] 任何 assertion break → 打印详细上下文 + `std::abort`；不打 `return 1` 让 ctest 看见 crash 信号
- [ ] Harness 内 scheduler 调用全走 PUMP sender，不直 enqueue（`feedback_cross_sched_pump_only`）
- [ ] key / fence / sample 所有 "按 `num_keys` derive" 的函数都独立测（手跑 `num_keys = 10 / 1000 / 100000` 看输出合理）
- [ ] 代码命名不带 `step033` / `phase9` 数字（`feedback_no_step_names_in_production_code`，harness 是 test 但命名规则一致）

### 8.3 完成

- [ ] `cmake --build build --target inconel_test_flush_e2e` 通过
- [ ] `ctest -R inconel_test_flush_e2e` 有明确 pass / fail 信号
- [ ] 失败时的错误信息足够下个会话不用再跑一遍就能知道是哪条 §7 假设 break
- [ ] `ai_context/inconel/plan/033_e2e_flush_harness.md` 在 ship 前追加 "实现后" 部分，记录 harness 跑起来后的实际观察（哪些 production bug 浮出水面，各自转成 known_issues 的哪条 / 后续哪个 step 修）
- [ ] INDEX.md / flush_development_plan.md §5.1 状态更新：Phase 9 → "landed + e2e harness 033 接通（初轮验证通过 / 发现 X 个 bug 已建 issue）"

---

## 9. 决策日志

| Q | 决策 | 理由 |
|---|---|---|
| Q1 — production 改动范围 | **不动 production**，全部 bug 走新会话修 | 用户明确。避免 033 scope 失控；harness 先暴露面，再定点修，比 "预测 + 预修" 落到错的方向代价低。 |
| Q2 — key 分布策略 | 均分写死；`num_keys` CLI knob；无 strategy 枚举 | YAGNI。偏斜 / 混合长度等后续 step 真需要时再加。 |
| Q3 — 拓扑 | 硬编码 `cores={0,2,4,6,8}` / `read_domain={2,4,6}` / `value=0` / `owner=8` | 对齐 step 031 §12.1 目标拓扑；WAL / coord 落地会再改，到时改 harness。 |
| Q4 — 硬件核数保护 | 不做 skip / fail 逻辑 | 用户确认机器够，未来走配置文件。 |
| Q5 — 3-worker 验证方式 | 不加显式计数；用 3-shard map + 均分 key 让 pipeline 自然展开 | 3-shard map 路由 + fold `build_key_partitions` + `collect_worker_proposals` fan-out 已经保证每 shard 有非空 partition；无需额外观测点。 |
| 文件命名 | `test_flush_e2e.cc` / `inconel_test_flush_e2e`（不带 `_1000`） | 用户明确：harness 会持续扩展（多轮 flush / 大 N / value 集成），名字不能锁死在首轮 1000 keys。 |
| 扩展性 | `num_keys` / fence / sample 全 derive；`persist_all_values` / `build_sealed_gen` / `run_one_flush_round` 全函数化 | 用户明确：留未来多轮 flush + 大量 key + 保证 3 worker 被触发的扩展空间。核心 pipeline helper 和单轮 flush 入口独立，多轮就是 for 循环调它们。 |

---

## 10. 预期改动面估算

| 文件 | 类型 | 行数估计 |
|---|---|---|
| `apps/inconel/test/test_flush_e2e.cc` | new | ~500 |
| `CMakeLists.txt`（`apps/inconel/test/CMakeLists.txt` 或对应位置） | 改（add_executable + add_test） | ~5 |
| 合计 | | ~505 |

**零 production 改动**。所有动面局限在 test 目录 + CMake 注册。

---

## 11. 下一步（033 外，不在本 step）

按优先级：

1. **Production bug 修 step**（新会话一个 or 多个）——跑 harness 暴露出的 Phase 9 未闭路径（预期至少 1 个：空树 bootstrap）。
2. **多轮 flush harness 扩展**（`test_flush_e2e` 加 multi-round flow）——前提是空树 bootstrap 修完。
3. **100K+ key + value 集成 + sub-LBA / size class 混合**（`test_flush_e2e` 继续扩）。
4. **Standalone profile binary**（`feedback_perf_profile_on_module_e2e` 规则；本 step 不做）。
5. **Dead code sweep**（`flush_development_plan.md` §3.Phase 8 已取消，延到端到端测试 step 后做——即本 step 完成后）——清 `test_candidate_build` / `test_flush_carriers` / `test_leaf_mapping` 等 broken-by-phase-7 测试，依据是端到端 harness 是否仍 pass。
6. **WAL / coord integration**（先落 WAL / coord 模块本体，再在 harness 里替换掉手搓 sealed gen 的部分）。

---

## 12. 实现后观察（first run, 2026-04-19）

Harness 按 plan 实装完成，`cmake --build build --target inconel_test_flush_e2e` 通过（只剩 Phase 9 step 031 未清的 `-Wsubobject-linkage` warning，和本 step 无关）。默认 `--num-keys=1000` 跑出来如下：

```
test_flush_e2e: num_keys=1000, topology cores=[0,2,4,6,8] rd=[2,4,6] value=0 owner=8
  generated 1000 kv records (key_digits=3, first=key_000, last=key_999)
  formatted mock device: 32768000 bytes (8000 LBAs @ 4096 B)
  runtime built: bootstrap shard_count=1 (single-shard placeholder)
  published 3-shard map: shard_count=3
  advance threads up (5 cores)
  phase 1: persist 1000 values
    persisted 1000 value_refs
  phase 2: build sealed gen_id=1 (1000 entries, lsn 1..1000)
  phase 3: tree_local_flush
    flush returned: st=2, leaf_order.size=0, root_slot.lba=0,
                    flushed_max_lsn=1000, flushed_front_count=1
FAIL: flush_stage_status != ok (= 2)
```

### 12.1 Harness 责任链自身工作情况

| 阶段 | 结果 |
|---|---|
| formatted_storage → mock_device adopt | OK（device 32 MiB, LBA 4 KiB） |
| build_runtime 非对称拓扑 | OK（bootstrap shard_count=1 如预期，之后被 publish 覆盖） |
| `rt::publish_shard_partitions(custom_map)` 覆盖为 3-shard | OK（shard_count 从 1 → 3） |
| 5 个 jthread 起 `rt::run(rt, core)` + 主线程 submit 模型 | OK（phase 1/2/3 全跑到，无 deadlock / data race） |
| `value::persist_values` 1000 条 | OK（value_core=0 独立跑；durables 1000 条合法返回） |
| `build_sealed_gen` + `make_empty_base_guard` | OK |
| `tree::tree_local_flush(req) >> then(promise) >> submit(ctx)` 的 submit_and_wait | OK（flush 在 owner_core=8 被调度、结果回到主线程 promise） |
| validate_flush_result 打印上下文 → abort | OK（exit=134 = SIGABRT，cedar-clear failure signal） |

Harness 本体这一层**没有 bug**——它精确停在了它该停的位置（production unsupported 路径），并把所有诊断字段打了出来。

### 12.2 Production 侧暴露的 Phase 9 bug

**信号**：
- `st == unsupported_shape_change (= 2)` → `build_key_partitions` 的空树 narrowing（`tree/memtable_fold.hh:234`，注释明确 "Phase 9 territory"）触发。
- `flushed_max_lsn == 1000`（= gen.max_lsn）已正确填入 → 说明 Gap 2 规则在 `allocate_fold_round` 阶段的 `flushed_max_lsn = max(pinned_gens.max_lsn)` 逻辑是对的；bug 纯粹在 partition → worker fan-out 这段。
- `leaf_order.size == 0`, `root_slot.lba == 0` → merge 没跑（因为 partitions 空），new_manifest 还是空 manifest 的原 shape。
- `flushed_front_count == 1` → fold 已经把 gen 映射回 `flushed_gens_by_front[0]`，说明 fold 本身不依赖 `build_key_partitions` 成功。

**定位**：`apps/inconel/tree/memtable_fold.hh` `build_key_partitions` 第 234 行：

```cpp
if (!base_manifest->has_root() || base_manifest->leaf_order.empty()) {
    return flush_stage_status::unsupported_shape_change;
}
```

注释写明 "Phase 9 territory"——Phase 9 step 031 实装 owner merge 协程时没同步把这里的 narrowing 拿掉，也没在 `owner_scheduler::run_merge` / `assign_and_emit_node` 里补对应的空 base_manifest 处理路径（全新 range 分配 / 首次构造 `slot_map` / `leaf_order` / `reverse_topology` / 造 root internal page）。

**补齐工作**：见新 `known_issues.md` 条目（2026-04-19 新增）。建议独立 step：
1. 删 `build_key_partitions` 的 narrowing；
2. `owner_scheduler` merge coro 处理 `has_root() == false` + worker proposals 全 `replaces_old_paddrs.empty()` 的情况，分配 fresh ranges；
3. `new_manifest` 首次构造（不是 deep_copy from old）；
4. shard_partition_map rebuild（本 harness 手动覆盖；production 路径需要 tree_sched 每次 flush 后基于新 leaf_order rebuild）；
5. 跑 `inconel_test_flush_e2e` 直到 exit=0，phase 4 readback sample 全部 match。

### 12.3 Harness 如何在 Phase 9 bootstrap 修好后验证

修完上面 1-4 步之后，harness 不需要改——`inconel_test_flush_e2e` 直接验证：
- `st == ok`；
- `new_manifest->has_root() == true`, `leaf_order.size() >= 1`；
- phase 4 readback 样本（§12.6 说明的 ~50 均匀步长）通过 `tree::lookup` + `value::read_value` 读回 value bytes 对等。

如果读回出错，很可能是 shard_partition_map 的 route 方向（read path 用它决定去哪个 `tree_read_domain`）和 flush 时用它决定去哪个 worker 不一致——这是 INC-040 的约束"一张 leaf page 在系统里只 cache 一次"在 v1 严格对齐的口径，harness 的 sample 横跨 3 个 shard 所以能直接看出来。

### 12.4 INC-042 闭环（2026-04-19，同日修复）

后续会话按 §12.2 handoff 改四处 production，harness 在本机 9 核上首次 exit=0：

```
test_flush_e2e: num_keys=1000, topology cores=[0,2,4,6,8] rd=[2,4,6] value=0 owner=8
  ...
  phase 3: tree_local_flush
    flush returned: st=0, leaf_order.size=12, root_slot.lba=4015,
                    flushed_max_lsn=1000, flushed_front_count=1
  phase 4: readback 5 sample keys (first=key_000 .. last=key_999)
    readback OK for 5 samples
all passed
```

修的产生四处：

| 位置 | 改动 |
|---|---|
| `tree/memtable_fold.hh::build_key_partitions` | 拿掉空 `base_manifest` narrowing，routing 统一依赖全局 `shard_partition_map`（builder 预装的单 shard `+∞` 占位 map 对任何 key 都返合法 `shard_idx`） |
| `tree/candidate_build.hh` | 新增 `initialize_worker_bootstrap` + `build_leaves_from_sorted_keys_impl`：空 `leaf_order` 下 worker 直接 chunk sorted key_groups 成 fresh leaf pages（每个 `replaces_old_paddrs.empty()`），`all_leaves_merged / pairwise_done / cascade_initialized / all_internals_built` 预置 true 跳过 NVMe 读 / cascade 爬升；`process_flush_round` 开头加 `s.all_done` 早退，顺带修了驱动协程二次 poll 对 moved-out `built_leaves` 再 wrap 导致空 children panic 的 latent 问题 |
| `tree/owner_scheduler.hh::_owner::run_merge` | 按 `is_bootstrap = !has_root()` 分支：`make_merge_context_bootstrap` 跳过 `index_root_group`（避免把 `{0,0}` 当 contrib 让 pre-scan 去 LBA 0 读假 tree page）；`combine_worker_roots_for_bootstrap` 按 `first_key_of_worker_root` 稳序后 `build_internal_pages_owner(is_new_layer=true)` 包 fresh internal 层（必要时循环套多层直到塞进单 page）；Phase 3 post-order walk 原样复用（bootstrap 下合并树里没 paddr-ref 子节点，unchanged-subtree 分支天然不走）；`is_root_change` 天然返 true 走 root-change 异步 superblock 更新 |
| `tree/sender.hh::tree_local_flush` | 末尾追加 `then(rebuild_and_publish_shard_partitions)`：用 `new_manifest->leaf_order + tree_read_domain_count()` 调 `build_initial_shard_partition_map` 后 `rt::publish_shard_partitions` 刷全局 + 每个 read_domain 的 partitions snapshot；production 路径不再依赖 harness 手动预 publish |

**关键不变量**：merge 协程绝不直接 `schedule()` 到 NVMe scheduler，所有 IO 通过 `merge_step_decision` variant 交接给外层 PUMP sender 组合。整条异步链在 `tree_local_flush` 的 `rt::owner()->submit_flush_fold >> flat_map(collect_worker_proposals) >> flat_map(drive_merge_loop) >> flat_map(nvme->flush) >> flat_map(submit_finalize_merge) >> visit() >> continue_after_finalize_merge() >> then(rebuild_and_publish_shard_partitions)` 里词法可见——"一切异步都必须词法内显式组合，绝不允许隐式 spawn" 规则被严格遵守。

**回归检查**：`inconel_test_{runtime_topology, runtime, tree_lookup, tree_lookup_multicore, value, flush_e2e, candidate_build, flush_carriers, leaf_mapping, page_cache, superblock_format, tree_page_format, tree_pipeline_compile, tree_value, wal_format}` 全部 15 个 PASS。之前 step 031 列为 broken-by-phase-7 的 `test_candidate_build` / `test_flush_carriers` / `test_leaf_mapping` 也跟着修了。

**文档同步**：`known_issues.md` INC-042 从 Open 挪到 Resolved；`design_doc/runtime_state_machine.md` §4.7 同步了"每次成功 flush round 提交新 manifest 后 `tree_sched` 必须 rebuild `shard_partition_map`"的权威语义（明确 bootstrap builder placeholder 仅覆盖首 flush 前窗口；空 round / fold-unsupported / flush_ok=false 三种 short-circuit 下 `new_manifest==nullptr` 跳过 rebuild；future heat-driven rebuild 仍留给 coord_sched / 专门 rebuild seam）。

### 12.5 Follow-up（超出 INC-042 scope，归入 033 外）

`rt::publish_shard_partitions` 里 `for (auto* rd : tree_read_domains.list) rd->partitions = m;` 是 `tree_sched` 核跨 core 写 read_domain 的 shared_ptr 成员，与各 read_domain 核上的非 atomic 读存在理论竞争；现有 API 行为（bootstrap builder 与测试 harness 同 pattern），INC-042 只是复用，要收窄需走 `std::atomic<shared_ptr<...>>` 或 RCU 式发布。记入 known_issues 待后续 step。

### 12.6 Readback 样本扩展（2026-04-19，同日）

首版 harness 的 `pick_samples` 只选 5 个 boundary 点（`0 / N/4 / N/2 / 3N/4 / N-1`）——足够验证 shard 间 route 方向正确，但 **12 张 leaf page 中会有 7 张不被实际读回**（样本正好落在 5 张 leaf 上），tree_read_domain 的 `node_cache` miss/fill 在那 7 张 leaf 上不被执行，等于把一大半 leaf 的 CRC / slot header / decode 路径留成未验证。

改法：把 `pick_samples` 改成**按 `kTargetReadbackSamples = 50` 目标量动态 derive 均匀步长**（`stride = max(1, n / 50)`），首尾兜底（若最后一个步长样本不是 `records.back()` 就补上 `n-1`）。`num_keys=1000` 实际出 51 个样本，`num_keys=100000` 仍然约 50 个（量级稳定，单次 `tree::lookup` 批量开销不会炸）。小 N（< 50 keys）降级为"每条都读"。

`verify_readback_samples` 本身不动——它已经对任意 `std::span<const readback_sample>` 都适用：一次 `tree::lookup(keys, manifest)` 批量拿 `lookup_result[]`，然后对每个 `lookup_value` 用 `value::read_value(vr)` 拿字节再 `==` 对比 `expected_value`。所以扩样本只改 `pick_samples`，不改验证路径。

**覆盖面**（`num_keys=1000`）：
- 51 / 12 leaf ≈ 4.25 sample/leaf，每张 leaf page **都**被至少 1 个 sample 触达，node_cache 的 miss/fill 路径完整走一遍；
- 3 个 shard 各 ~17 sample，shard boundary 附近两侧都有样本；
- 每个 `lookup_value.vr` 都实际送进 `value::read_value` 反解码 → 字节对等，等价于"每个 sample key 的完整写→读字节一致性"都被验证一次。

**没做的事（下次扩展）**：
- `verify_readback_samples` 里对每个 sample 独立 `submit_and_wait<std::string>(read_value(vr))`，51 个 sample 下 mock_nvme 单线程够快，但 N 上到 100K+ 会拖慢。要改成 `for_each(lookup_results) >> concurrent(K) >> flat_map(value::read_value) >> all()` 并发版本。扩到大 N 那一轮再做。
- 全量 readback（N 个 sample = N keys）目前不做（用户决策）；51 均匀步长是"够验证写入一致性 + 不拖 N=100K 的合理量级"。
