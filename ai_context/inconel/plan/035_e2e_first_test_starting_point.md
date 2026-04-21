# Step 035 — E2E 为测试新起点 + 大规模 e2e 跑通

> 状态：**草案**，等用户 review 通过再进实现会话。
>
> 用户指示（2026-04-20）：
>
> 1. 新起 step 035，**继续完成端到端测试**（接 033 的 `inconel_test_flush_e2e` 往下推）。
> 2. **过去的回归测试都不用管了——测试的新起点就是从 e2e 开始。**
>    即：老的 `inconel_test_tree_lookup` / `test_tree_page_format` / `test_wal_format` /
>    `test_value` / `test_runtime*` / `test_inc046_regression` / `test_leaf_mapping` /
>    `test_superblock_format` / `test_page_cache` / `test_tree_pipeline_compile` /
>    `test_tree_value` 等**不再作为 production 改动的门槛**。它们在本 step 里不是必须通过项，
>    如果和本 step 的 production 改动（如 profile 可调）冲突，允许 retire。
>
> 定位：**这是"测试规模基线"step**。目标不是再修 bug，而是让端到端测试自身跑到
> CLAUDE.md 容量校准表能覆盖的量级（~10^5–10^6 keys），具备"发现新问题"的能力。
> 033 跑 1000 keys 只能证明 happy path 通；035 之后才能声称"e2e 作为唯一验收口径"站得住脚。

---

## 1. 目标与非目标

### 1.1 目标

035 分 **Phase 1（规模化正确性）** 和 **Phase 2（并发读写）** 两段实现，两段都在本 step 内 land。

| 目标 | Phase | 说明 |
|---|---|---|
| **e2e 作为唯一测试入口** | 1 | 把 `inconel_test_flush_e2e` 正式升格为"flush/tree/value/runtime 四层的唯一 CI 验收 target"。老的 per-module regression 测试降级为"开发期手工 fixture"，不再进必跑集。 |
| **N 能开到 10^5–10^6 级别** | 1 | 当前 harness 在 N ≥ ~95K 时 tree allocator 把 value area 挤爆 panic。目标：在同一 harness binary 下 `--num-keys 1000000` 能稳定跑完三轮 flush + 样本读回全对。 |
| **profile 不再锁死 32 MiB** | 1 | `kBootstrapFormatProfile` 的 `value_data_area_end` 硬编码常量允许提升到能容纳 10^6 级 keys 的水位。仅改常量，不新增 runtime override 入口。 |
| **readback 抽样内部并发化** | 1 | 当前 `verify_against_expected` 每个 sample 独立 `submit_and_wait<value::read_value>`，N=100 万下会串行化成瓶颈。本 step 把 sample 侧的 `value::read_value` 改成并发。这里的"并发"指最后 verify 阶段**单线程 orchestrator + pipeline 内部 `concurrent()`**，和 Phase 2 的多线程 reader 是两件事。 |
| **跑 10^6 仍在合理时间内**（< 5 分钟） | 1 | 不做正式 perf 目标，但要保证 CI 不爆。 |
| **2 个外部 reader 线程跑并发读写口径** | 2 | 主线程跑连续多轮 flush，2 个独立 reader 线程（`this_core_id=10 / 11`）持续 `tree::lookup` 并对比已发布 snapshot。这是 035 把 e2e 正式定性为"并发读写测试"的关键一步。 |
| **Snapshot isolation 松口径 + 1 次重试** | 2 | reader 同时持 `(current, previous)` 两份 snapshot；lookup 结果 match 任一即 OK；首次 mismatch 重 load 一次 snapshot 再 lookup 一次（吸 benign race window）；仍 mismatch 才 abort。Phase 2 land 后 concurrent r/w 默认开启（`--concurrent-readers` 默认 2）。 |

### 1.2 非目标

| 非目标 | 原因 |
|---|---|
| 达到 CLAUDE.md 的 10 亿 KV | 10 亿 × 单 KV ~100 字节 ≈ 100 GB，mock_nvme 内存实现装不下。10 亿是 production SPDK nvme 路径的目标，不是 mock 测试路径。本 step 只到 memory-feasible 的上限（10^6–10^7）。 |
| **改 `tree::lookup` API / 任何跑 pipeline 的 production 签名** | 035 是**纯 test step**，不动 production pipeline。scale 目标由 profile 常量独立支撑。 |
| **修 `rt::publish_shard_partitions` 的 shared_ptr race**（033 §12.5 已登 known_issues） | 该 race 在并发读写下**预期会被 Phase 2 touch 到**——可能表现为 SIGSEGV（torn shared_ptr 读） / 偶发 lookup miss / 偶发错误 data_ver。**本 step 的职责是暴露它，不是修它**。见 §7 风险表。 |
| **修 `std::atomic<std::shared_ptr<T>>` 包装 / RCU 发布** | 同上。属于独立 step（追踪为 known_issues "publish race" 条目）。 |
| Standalone profile binary（§12.8#5 of 033） | 独立 step。本 step 先把功能性 e2e 推到大 N，profile binary 等 e2e 稳定再做。 |
| WAL / coord integration（§12.8#6） | 两个模块未落；不是 e2e 规模化的前置。 |
| INC-047 delete-to-empty structural collapse | 单独 tree 侧 step，不和规模化绑。 |
| INC-041 PUMP `concurrent()+reduce()` contract | blocked on pump/，不是本 step 能动的。 |
| 偏斜 key 分布、热点 workload、GC 压测 | 先把均匀大 N 跑通，偏斜留给后续 step。 |
| value size-class mix / sub-LBA 值 | 可选子目标（§4.4），如果大 N 同步推不顺就单独拆 step。 |
| 新模块（front / coord / recovery） | 本 step 不拉新代码面；目标是把已有实现跑到大 N。 |

### 1.3 测试策略转折（重点）

033 之前的口径：每个模块一个 test，e2e 是补强。
033 之后的口径（035 固化）：**e2e 是唯一入口**，per-module test 允许被 retire、删除、或降级成开发期调试 fixture。

实际约束：

- 本 step 允许 **删除、停用、改签** 任何和 production 改动冲突的 per-module test。
  例：`test_runtime.cc` 里 `static_assert(!has_value_data_area_end_option<build_options>)`
  如果挡路就删。
- 删除时在 `CMakeLists.txt` 同步移除 target；ctest 不再 list 它。
- 仍保留 per-module test 源文件作为参考，**除非**该文件在编译或链接时主动 break production
  改动，这种情况下允许删文件。
- `inconel_test_flush_e2e` 是从本 step 起唯一的 CI 必跑 target。
- 跑 e2e 暴露出的 production bug 走新会话 / 新 step 修，不在本 step 内预先改。

---

## 2. 阻塞点分析

### 2.1 当前 N 的硬上限在哪

033 harness 实测：

| N | 结果 | 触发 |
|---|---|---|
| 1000–90 000 | `all passed` | happy path |
| ~95 000–100 000 | `panic: tree_allocator::allocate: data area exhausted` | tree page 填到 LBA 5501，值区从 LBA 4000 向上长到 5500，相遇 |

根因是 `kBootstrapFormatProfile`：

```cpp
inline constexpr format_profile kBootstrapFormatProfile = {
    .lba_size               = 4096,
    .value_data_area_base   = paddr{0, 4000},
    .value_data_area_end    = paddr{0, 8000},   // ← 32 MiB 上限
    .tree_page_size         = 4096,             // ← 每 leaf 只 ~64 条 (32B key)
    ...
};
```

harness 的 `kNamespaceBytes = 8000 × 4096 = 32 MiB` 就是照这个 profile 派生的。

### 2.2 为什么 profile 当前不可调

- `format_profile.hh:14-20` 注释明确 profile 不是 runtime config，目的是让
  `build_options` / `start_options` 保持"不承诺可跨 run 漂移的字段"。
- `test_runtime.cc` 有 `static_assert(!has_value_data_area_end_option<runtime::build_options>)`
  强制锁定这条。
- `build_runtime()` 直接 `const auto& profile = format::kBootstrapFormatProfile;`
  没有入口。

**这些都是合理的生产 API 设计**，目的是不让 profile 变成 per-run 可调参数；但现在
e2e 被规定为唯一测试入口，harness 必须能配置 disk 尺寸，否则 e2e 永远只能验 32 MiB。

### 2.3 Readback 串行化（次要但必须）

033 §12.6 + §12.8#2 的已知限制：

```cpp
// current: each sample triggers its own submit_and_wait<string>
auto got = submit_and_wait<std::string>([vr = lv.vr]() {
    return value::read_value(vr);
});
```

N=1000 下 51 sample 串行读足够快；N=10^6 下 51 sample 仍然 OK（sample 数由
stride 控制，N 大 stride 也大），但如果本 step 想扩 sample 数（§4.2）到每个 shard
上千样本，串行化会爆。改并发版本是配套动作。

---

## 3. 修改面（production / test / doc）

### 3.1 Production 改动（唯一允许项）

| 文件 | 改动 | 动机 |
|---|---|---|
| `apps/inconel/format/format_profile.hh` | 把 `kBootstrapFormatProfile.value_data_area_end.lba` 从 8000 提到 ~100000（~400 MiB namespace，10^6 keys 容量 + 2× headroom）。**不动** `value_data_area_base` / `lba_size` / `tree_page_size` / `value_class_sizes`。 | profile 常量本来就是 bring-up-era disk layout 的 single source，规模化场景正常该调的地方；保持单 instance 设计不破坏 INC-034 的 "profile 不进 build_options" 决定 |

**显式不做**的 production 改动：

- 不给 `tree::lookup` / `tree::tree_local_flush` / `build_runtime` 等任何 sender / scheduler API 加参数
- 不把 `current_shard_partitions()` / `rd->partitions` 改成 `std::atomic<std::shared_ptr<T>>`
- 不改 `test_runtime.cc` 里的 `static_assert(!has_value_data_area_end_option<...>)`——该 assert 只保 `build_options` 不增字段，和本 step 只改 profile 常量不冲突
- 不加新的 test-only runtime 入口（`build_runtime_for_harness` 之类）

### 3.1.1 老 per-module test 的冲突处理

profile 常量提升后，某些 per-module test 里 CHECK 具体 LBA 值或常量会挂：

| 文件 | 预期影响 |
|---|---|
| `apps/inconel/test/test_runtime.cc` | `DATA_AREA_END_LBA = 8000` / CHECK 具体 LBA 数 → 改成 `profile.value_data_area_end.lba` 动态 CHECK（或按 §1.3 直接删对应 CHECK / 整个 target） |
| `apps/inconel/test/test_value.cc` | 同上 |
| `apps/inconel/test/test_runtime_topology.cc` | 同上 |

**处理原则**（035 §1.3 授权）：

1. 如果挂的 CHECK 本身 e2e 覆盖了，删
2. 如果挂的 CHECK e2e 没覆盖，改成读 `kBootstrapFormatProfile` 的动态值（不硬编 8000）
3. 如果整个 target 只剩挂的 CHECK，连 target 从 `CMakeLists.txt` 一起删

删前在 `known_issues.md` 登记清单："035 移除的 per-module test: ..."

### 3.2 Harness 改动（`apps/inconel/test/test_flush_e2e.cc`）

**Phase 1 改动：**

| 改动 | 动机 |
|---|---|
| CLI `--num-keys` 上限从 10^8 保留，默认从 1000 不变 | 小 N 默认让 dev 侧跑得快；大 N 按命令显式请求 |
| `kNamespaceBytes` 跟着 profile 变大（用 `kBootstrapFormatProfile.value_data_area_end.lba * lba_size`） | harness 已经这么写了，profile 改了自动跟 |
| `verify_against_expected`：把 per-sample `value::read_value` 改并发（单线程 orchestrator，pipeline 内 `concurrent()`） | 大 N 下避免串行化；与 Phase 2 的多线程 reader 不同路径 |
| 保留样本策略（stride ~50）但暴露 `--readback-samples` CLI | 大 N 下需要手动扩样本数 |
| 新增 `--rounds` CLI（默认 3） | 长序列压力；多轮 allocator / shadow CoW cascade 累积效应 |
| 输出加 timing（per-round wall-clock + 总 wall-clock + total KV throughput） | 规模化后肉眼判断是否有回归 |
| 每轮 flush 前后打印 `manifest.leaf_order.size()`、`slot_map.size()`、tree area LBA 用量、value area LBA 用量 | allocator 压力可观测 |

**Phase 2 改动（见 §4.5 详细 spec）：**

| 改动 | 动机 |
|---|---|
| 新增 `published_snapshot_t` 结构 + `std::atomic<std::shared_ptr<published_snapshot_t>> g_snap` | reader 和 writer 之间的"已发布 (manifest, expected_state) 对"交换点 |
| 主线程每轮 flush 完后原子 publish 新 snapshot（保留 previous） | 让 reader 持有 `(current, previous)` 两份做松口径匹配 |
| 新增 2 个 reader `jthread`，`this_core_id=10 / 11` | 不和 advance 线程（id 0/2/4/6/8）/ 主线程（id 0）冲突 |
| Reader loop：load snapshot → random batch K keys → `tree::lookup(keys, snap.manifest)` → validate → 重试一次 → mismatch abort | 松 + 1 retry 吸 benign race window；逻辑 mismatch abort 但 torn shared_ptr crash 按预期发生不拦 |
| 新增 CLI `--concurrent-readers N`（默认 2），`--reader-batch K`（默认 16） | Phase 2 land 后 `N=2` 是默认；Phase 1 land 期间可通过 `--concurrent-readers 0` 临时关掉 |
| 主线程收尾：设置 `reader_stop = true` → 所有 readers `join` → 最终静态 verify（和今天一致） | 统一 stop 顺序；最终 verify 是 benchmark 式的"安静世界下再抽一遍"，强制通过 |

### 3.3 CMakeLists.txt

| 改动 | 动机 |
|---|---|
| 本 step 里先不删任何 target | 避免一步拉太多。删除交给后续小 step 或本 step 尾部清理。 |
| `inconel_test_flush_e2e` 已有，无需新增 | — |

### 3.4 文档

| 文件 | 改动 |
|---|---|
| `ai_context/inconel/plan/035_e2e_first_test_starting_point.md` | 本文；实现完成后追加 §"实现后观察" |
| `CLAUDE.md` 的 "Inconel 实现规范" §"v1 语义" 或 INDEX | 追加一句 "e2e 测试 harness 是 v1 唯一验收口径；per-module test 为开发期辅助"（只有本 step 完成后再落） |
| `known_issues.md` | 035 完成后登记"删除/停用的老 test"清单（如果真删了东西） |

---

## 4. Sub-goals 细则

### 4.1 N = 10^6 目标

| 指标 | 目标 |
|---|---|
| 默认 `--num-keys 1000` 仍 ~30 ms 完成 | 不退化 |
| `--num-keys 1000000` 完成时间 | < 5 min |
| 3 轮 flush 成功，readback 样本全对 | 必须 |
| tree area 和 value area 都不 exhaust | 必须 |
| 无 SIGSEGV / 无 UAF（jthread 析构顺序） | 必须 |

容量粗估（按 032/033 方法，假设 32B key + 8B value，16 KiB tree page）：

- round 1 1M PUT → ~3900 leaves × 16 KiB ≈ 64 MiB tree + ~8 MiB value（value ~8B 进最小 class）
- round 2/3 再 +2 × 400K 混合 → tree area 涨到 ~80 MiB，value area 涨到 ~12 MiB
- 加 2× headroom → **namespace ≥ 200 MiB 即可，profile 把 end 提到 LBA 50 000（200 MiB）就够**

若 key 更大（64B / 256B）则相应扩；harness 的 key 始终按 `key_%0*u` 生成，
10^6 下 `max_key_index ≤ 1 700 000`, key 长度 9-10 字节 + value 长 ~7 字节（`val1_%u`），
单条 entry ~20 字节，和上面估算接近。

**结论**：profile `value_data_area_end.lba = 50 000`（200 MiB namespace）足够 10^6 keys。
可直接按 100 000（400 MiB）给 2× 余量。

### 4.2 Readback 并发化

当前：

```cpp
for each sample:
    got = submit_and_wait<string>([](){ return value::read_value(vr); });
    compare with expected;
```

改成（严格遵循 `feedback_pipeline_start_vs_operator`：一条长 pipeline，不嵌套起点）：

```cpp
auto results = submit_and_wait<vector<pair<size_t, string>>>([&]() {
    return for_each(indexed_live_value_samples)
        >> concurrent(K)                     // K ~ value_core 数（= 1）* 16 或常数 32
        >> on(value_sched.as_task())         // 如果 value scheduler 支持，否则 task_scheduler
        >> flat_map([](auto&& i_and_vr) {
            return just() >> value::read_value(i_and_vr.second)
                >> then([i = i_and_vr.first](auto&& s) {
                    return std::pair{i, std::move(s)};
                });
        })
        >> to_vector<pair<size_t, string>>();
});
for each result: compare with expected[result.first];
```

⚠️ INC-041 警告：PUMP 的 `concurrent() + reduce() / to_vector()` 对共享 accumulator
contract 不明确。先用 `to_vector()` 试；若见 data race / move-only 报错，退化成
per-sample 串行 + 总耗时控制（N=10^6 下 51 sample 串行 ~1 秒可接受）。

### 4.3 Timing / observability

每轮 flush 前后打印：

```
round 1: 1000000 ops (1000000 put, 0 tombstone, gen_id=1, lsn 1..1000000)
  flush START wall=...
  persist_values phase wall=...ms
  build_sealed_gen phase wall=...ms
  tree_local_flush phase wall=...ms
  flush returned: st=0, leaf_order=..., tree_lba_used=...
  flush END wall=...ms
```

目的：规模化后任何一轮慢异常能直接肉眼看出来。

### 4.4 Value size-class mix（可选子目标）

只有 §4.1/§4.2/§4.3 都通之后才碰。加 `--value-size-mix <uniform|mix>`：

- `uniform`: 当前行为（val 固定 ~8 字节，全进最小 class）
- `mix`: 每条 value 按 key index 取 [~10, 250, 1000, 3500, 16000] 五档之一，落进
  不同 size class；sub-LBA 的 10/250 进 class 0/1，LBA-size 的 1000/3500/16000 走
  class 2/3/4

目的：让 value allocator 的 class pool / bump head 路径在大 N 下也被 exercise。

若 §4.1-4.3 紧，§4.4 拆到下个 step。

### 4.5 Concurrent reader（Phase 2）

这是 035 的主要 e2e 扩展——让 e2e 从"顺序 writer + 静态 verify"扩成"持续 writer + 并发 reader"。

#### 4.5.1 数据结构

```cpp
struct published_snapshot_t {
    std::shared_ptr<const core::tree_manifest> manifest;
    std::shared_ptr<const expected_state>      expected;
    uint64_t                                   round_id;   // 调试用，非必要
};

// 单一全局 publish 点
std::atomic<std::shared_ptr<published_snapshot_t>> g_current_snap;

// reader 侧持"当前 + 前一个"做松口径 match
struct reader_held_snapshots {
    std::shared_ptr<const published_snapshot_t> current;
    std::shared_ptr<const published_snapshot_t> previous;
};
```

`expected_state` 从当前 `std::map<std::string, expected_entry>` 保持不变（035 范围内不换 flat_hash_map，§7 风险表有兜底）。publish 一次要**整份 copy**（拷贝 expected_state 作为新 shared_ptr 的内容，再原子 store）——copy 成本在主线程摊销，reader 侧只 load shared_ptr，O(1)。

#### 4.5.2 主线程 publish 顺序

```
round N:
  persist_values(round N puts)
  build_sealed_gen
  tree_local_flush → wait → new_manifest
  apply_round_to_expected(round N) → new_expected (copy prior + delta)
  new_snap = make_shared<published_snapshot_t>{new_manifest, new_expected, N}
  prev_snap = g_current_snap.load()   // 保留前一个，防 reader 看空
  g_current_snap.store(new_snap)      // 原子 publish
  // prev_snap 继续由 reader refcount 持；主线程不显式 drop
```

初始（round 1 之前）先 publish 一个空 snapshot（empty manifest + empty expected），避免 reader load 到 null。

#### 4.5.3 Reader 主循环

```
reader_main(reader_idx):
  pump::core::this_core_id = 10 + reader_idx   // 10 or 11
  held.current  = g_current_snap.load()
  held.previous = held.current                 // 起始两份等同
  local_rng = seed_from(reader_idx)

  while (!reader_stop.load(std::memory_order_relaxed)):
    new_snap = g_current_snap.load()
    if (new_snap != held.current):
      held.previous = held.current
      held.current  = new_snap

    // 从 held.current->expected 里随机抽 K = reader_batch_size 个 key
    keys = pick_random_keys(held.current->expected, K, local_rng)
    if (keys.empty()): continue   // 空 snapshot 边界

    // 第一次尝试（对齐 held.current）
    results = submit_and_wait<vector<lookup_result>>(
        just() >> tree::lookup(keys, held.current->manifest.get()));

    if (match_all(keys, results, held.current->expected)): ++ok; continue

    // 松口径：match previous
    if (match_all(keys, results, held.previous->expected)): ++ok_stale; continue

    // 重 load 一次（吸 benign race），用最新 snapshot 再 lookup
    reload = g_current_snap.load()
    results2 = submit_and_wait<vector<lookup_result>>(
        just() >> tree::lookup(keys, reload->manifest.get()));
    if (match_all(keys, results2, reload->expected)): ++ok_retry; continue

    // 还是不 match —— 真的 mismatch
    report_and_abort(reader_idx, keys, results, held, reload);
```

#### 4.5.4 `match_all` 的语义

```
match_all(keys, results, expected):
  for each (key_i, result_i):
    if (expected.contains(key_i)):
      e = expected[key_i]
      if (e.kind == value  &&  result_i is lookup_value   &&  data_ver match):  OK
      if (e.kind == tomb   &&  result_i is lookup_tombstone && data_ver match):  OK
      else: return false
    else:
      // expected 里没这个 key，result 必须是 absent
      // （pick_random_keys 保证不出现，但边界留作 defensive）
      if (result_i is absent): OK
      else: return false
  return true
```

不做 value bytes 对比（太慢）。data_ver 对齐即认为逻辑一致。

#### 4.5.5 停止顺序

```
主线程收尾：
  reader_stop.store(true, relaxed)
  for each reader thread: join (jthread 析构自动 join)
  final verify（安静世界）：沿用 033 的 51-sample stride 全量对比（含 value bytes）
  停 advance loops
  destroy runtime
```

Final verify 必须在 reader join 之后、runtime 停之前，口径和 033 一致——**静态 verify 不允许 mismatch**。

#### 4.5.6 Reporter & counters

每个 reader 本地维护：

```
uint64_t ok              // 第一次就 match current
uint64_t ok_stale        // match previous
uint64_t ok_retry        // 重 load 后 match
uint64_t batches         // total batches issued
```

结尾主线程聚合打印：

```
[reader 0] batches=12345 ok=12300 ok_stale=40 ok_retry=5 mismatch=0
[reader 1] batches=12400 ok=12370 ok_stale=25 ok_retry=5 mismatch=0
```

`ok_stale` / `ok_retry` 非零是预期的（benign race window 的证据）；`mismatch` 非零会 abort。

#### 4.5.7 this_core_id 选择依据

advance 线程：`0, 2, 4, 6, 8`。主线程（submit flush）：`0`。

Reader 用 `10, 11`：

- 不与 advance 冲突（core 0/2/4/6/8 的 advance 线程拿的就是自己的 core id 做 `this_core_id`）
- 不与主线程冲突
- `per_core::queue` SPSC 的 "from this source core" 假设在 reader 这侧得到维持：两个 reader 分别走 list[10] / list[11]，互不竞争，且它们 producer 到下游 scheduler（`tree_read_domain[*].lookup_sched.req_q`）的 `list[10] / list[11]` 也互不冲突

#### 4.5.8 为什么两 reader 够

现在 tree_read_domain 有 3 个核（2/4/6）。两个 reader 用 2 shard-independent batching 已经能让 3 个 read_domain 轮流被打中（每 batch 的 16 keys 按 shard_partition 分流，两 reader 的 fan-out 叠加起来几乎一定覆盖所有 3 个 shard）。不上 3 reader 是因为：

- `this_core_id` 复用成本高（更多 per_core queue 维度）
- 打 3 个 reader 反而把 race window 拉得更分散，debug 成本增大
- 验证 A2 要的是"并发读写能跑"，不是"拉满 reader"

Phase 2 land 后若真要加到 3，一行 CLI 改默认即可。

---

## 5. 工作顺序

1. **先落 plan（本文）** → 用户 review 通过
2. **新会话实现**（不在本次对话）—— 严格按 Phase 1 → Phase 2 顺序，Phase 1 过了才碰 Phase 2

### 5.1 Phase 1（规模化正确性）

1. profile 常量提升（§3.1），`value_data_area_end.lba` 从 8000 改 ~100000
2. build 过
3. 跑 `inconel_test_flush_e2e --num-keys 1000` 确认小 N 仍过
4. 处理挂掉的 per-module test：按 §3.1.1 原则删或改
5. `inconel_test_flush_e2e` Phase 1 改动（§3.2 上半）：`--rounds N`、timing 打印、allocator 用量打印、readback 内部并发化（§4.2）
6. 逐级跑 `--num-keys 10000 / 100000 / 1000000` 确认
7. 预期可能触发的失败面（**都不在 Phase 1 修**，触发即报 issue 转新 step）：
   - allocator 压力新路径
   - 多轮 cascade 累积效应
   - INC-043 fail-fast panic 的 follow-up 触发路径（033 §12.7 已标注"displaced base leaf / 未剪空 leaf"）
   - INC-047 delete-to-empty 保守表示的累积
8. Phase 1 完成口径：`--num-keys 1000000 --rounds 3` 和 `--num-keys 1000 --rounds 10` 都 PASS 且 wall-clock 合理

### 5.2 Phase 2（并发读写）

1. 实装 §4.5 的 `published_snapshot_t` / `g_current_snap` / reader jthread / match_all / counters
2. 默认 `--concurrent-readers 2` 开
3. 跑 `inconel_test_flush_e2e --num-keys 1000`：短跑确认基础 plumbing 通（reader 能起、能读到 snapshot、主线程能 publish、final verify 仍过）
4. 跑 `inconel_test_flush_e2e --num-keys 100000 --rounds 3`：观察 reader counters（ok / ok_stale / ok_retry / mismatch）
5. 跑 `inconel_test_flush_e2e --num-keys 1000000 --rounds 3`：正式规模化并发读写
6. 预期 outcome 分三类，都**不在 Phase 2 修**：
   - **PASS**：counters 全 0 mismatch，race window 没撞上 → 035 land
   - **逻辑 mismatch abort**：带上下文的 reader-side abort → 真的 correctness bug，转新 step
   - **SIGSEGV / 不可解释 crash**：极大概率是 `rt::publish_shard_partitions` 的 torn shared_ptr race 实证 → 登 known_issues，转新 step 修（通常是 `std::atomic<std::shared_ptr<T>>` 包装）
7. Phase 2 完成口径：**在足够长的一轮（`--rounds 10` 左右）下能跑完 + 附带 counter 打印（不要求 mismatch=0，但要求 crash-free；crash 视为 production bug，035 ship 时必须同步把相关 issue 登在 known_issues）**
   - 若 Phase 2 因为 race 持续崩 → 035 在 Phase 2 入口处 land（`--concurrent-readers 0` 默认），并把 concurrent reader 代码路径完整保留；相关 race issue 开新 step 修完后再切 `--concurrent-readers 2` 默认

---

## 6. 验证清单

### 6.1 Phase 1 硬指标

- [ ] `./build/inconel_test_flush_e2e`（默认 N=1000, rounds=3, concurrent-readers=0）仍然 PASS
- [ ] `./build/inconel_test_flush_e2e --num-keys 100000` PASS
- [ ] `./build/inconel_test_flush_e2e --num-keys 1000000` PASS 且 wall-clock < 5 min
- [ ] `./build/inconel_test_flush_e2e --num-keys 1000 --rounds 10` PASS，多轮累积下 allocator 压力稳定
- [ ] 3 次重跑同 N 结果一致（Phase 1 无 flaky）
- [ ] 挂掉的 per-module test 处理方案写进 `known_issues.md`（或同步删除）

### 6.2 Phase 2 硬指标

- [ ] `./build/inconel_test_flush_e2e` 默认跑（含 2 reader）PASS，reader counters 打印
- [ ] `--num-keys 100000 --rounds 3 --concurrent-readers 2` 跑完（mismatch=0 或登新 issue）
- [ ] `--num-keys 1000000 --rounds 3 --concurrent-readers 2` 跑完（同上）
- [ ] 若出现 SIGSEGV / 逻辑 mismatch：`known_issues.md` 登记新条目 + 给出 reproducer 命令
- [ ] `035_e2e_first_test_starting_point.md` 追加 "实现后观察" 段，记录 counter 分布、触发 crash / mismatch 的 workload、新登的 issue

### 6.3 本 step 不承诺的指标

- CLAUDE.md 的 10 亿 KV（mock 内存放不下，生产路径的事）
- RocksDB × 5 性能锚点（当前阶段不是 benchmark step）
- 老 per-module test 继续 pass
- 并发读写下 mismatch / crash 在 035 内修复
- shard_partition_map 发布原子化（独立新 step）

---

## 7. 风险与对冲

**关键原则**（贯穿全表）：**035 是测试 step，不修 production bug**。下表的"对冲"策略全部遵循"暴露 → 登 issue → 新 step 修"的节奏；035 内只负责让测试 harness 自身稳定运行到能 trigger 问题，不做 defensive 修复。

| 风险 | 对冲 |
|---|---|
| profile 常量改大后，部分 per-module test 不只是 CHECK 改动，而是在语义上依赖 32 MiB（比如 allocator 边界 test） | §3.1.1 授权：改动态 CHECK 或删 target。优先保 e2e |
| 10^6 key 暴露 INC-043 fail-fast 的"两条已知触发路径"（033 §12.7 标注 displaced base leaf / 未剪空 leaf） | panic 即是真实 production bug；登新 issue，不在 035 内修 |
| tree allocator 在大 N 下 fragmentation 累积（多轮 retire 老 range → 新 range 又连续分配） | 观测 + 文档化；真触发新 bug 开新 step |
| Phase 1 readback 并发触碰 INC-041 pump contract 问题 | 退化成串行版本；不阻塞 Phase 1 主线 |
| `jthread` 析构 join 在大 N 下延迟长 | 目前就是 `workers.clear()` 顺序析构；若慢就加显式 `stop` CV broadcast。先不提前优化 |
| 10^6 的 `expected_state` `std::map<string, entry>` 本身慢 | 若 verify 阶段 map 构造本身 >1s，改 `flat_hash_map` 或 sort+binary-search。先不提前换 |
| N 越大 harness 内存占用越大，超宿主机 RAM | 本机足够做 10^6；不够则降级目标到 10^5；不在 harness 做 OOM 保护 |
| **Phase 2 触发 `rt::publish_shard_partitions` 的 torn shared_ptr race 导致 SIGSEGV / 偶发 lookup miss** | **预期会发生**。这正是 A2 的目的——把 033 §12.5 登记的理论 race 打实。登 known_issues，开新 step 修（预期方向：`std::atomic<std::shared_ptr<T>>` 或 RCU-style 发布）。035 内**不包任何 retry-on-crash / signal handler**——UB crash 就让它 crash，abort exit 给 ctest 明确信号 |
| **Phase 2 reader 逻辑 mismatch abort** | 带上下文的 reader-side abort 是真实 correctness bug 信号（比 race 严重）——不在 race window 内还 match 不上 = 某处 visibility 实际不对。登新 issue，不在 035 内修 |
| Phase 2 并发 publish 和 persist_values 的 value allocator 并发冲突 | 主线程才 submit persist / flush（reader 不 submit 写路径），所以 persist 侧不会被 reader 踩；reader 只读 → 不 touch value allocator 写状态。若真触发 race 另算 |
| Phase 2 `g_current_snap` 自身的 `atomic<shared_ptr<T>>` 依赖 C++20 的 libstdc++ 实现（当前 binary 用 -std=c++26） | 直接用 `std::atomic<std::shared_ptr<T>>`；若 libstdc++ 版本不支持就 fallback 到手写 `mutex + shared_ptr` 版本（035 不追求 lockfree publish 效率） |
| Phase 2 两 reader 和主线程 publish 同时跑，publish 的 std::map copy 本身慢（10^6 entries） | publish 成本在 **主线程**，reader load 是 O(1) shared_ptr atomic load；若 publish copy 时间 dominate 写路径，可接受（benchmark 不是本 step 目标）。若真爆就改成 struct-of-arrays / 增量版本号 + delta patch，但这不在本 step |

---

## 8. 设计依据

| 约束 / 文档 | 用法 |
|---|---|
| CLAUDE.md 最高规则 "实现期禁读测试" | 下游实现会话里严格执行；设计/测试维护角色（本 step）允许读 test，但决策以 spec/production 为准 |
| CLAUDE.md 约束 A "收窄实现必须显式声明并 fail-fast" | 本 step 改 profile 是"放宽"容量上限，不是收窄，不触发 A；但如果新发现 shape-specific 限制要补 panic |
| CLAUDE.md 约束 B "通用命名必须对应通用语义" | harness 的新 CLI / helper 必须起通用名 |
| `feedback_match_codebase_patterns` | readback 并发化沿用 `for_each >> concurrent >> flat_map >> to_vector` 这套 codebase 已有 pattern，不发明新 shape |
| `feedback_no_step_names_in_production_code` | 代码标识符不带 "step035" / "scale_up" 等 |
| `feedback_layered_complete_not_prototype` | profile 调大一次到位，别做 "先翻倍看看再翻倍" 的 prototype |
| `feedback_perf_over_simplicity` | 并发 readback 优先选快的，哪怕代码多几行 |
| `feedback_wait_before_commit` | 本 step 实现完了等用户明确 ack 再 git commit |
| `feedback_communicate_progress` | 每 sub-goal 完一个就用中文汇报 |
| 033 §11 / §12.8 follow-up 列表 | 035 scope = 选 §12.8 #2 + #4 + 小量 §12.8 #3（subobject-linkage 视情况），其他不入 |

---

## 9. 决策日志

| Q | 决策 | 理由 |
|---|---|---|
| Q1 profile 常量提升 vs 可注入入口 | 方案 A（提升常量） | 不拉新 API 面；后续真需要多 profile 再做方案 B。符合 `feedback_layered_complete_not_prototype` |
| Q2 老 per-module test 保留 vs 删 | 冲突即删，不冲突保留 | 用户明确 "老回归不用管"；保留是为了开发期手工 fixture，不挡 production 改动 |
| Q3 N 目标上限 | 10^6（≥ 10^5 必须） | mock 内存可承受；和 CLAUDE.md 容量校准表 "10 亿下 15-62 GB tree" 相比是缩 1000×，但能证明结构能放大 3 个数量级 |
| Q4 tree_page_size 是否同时从 4096 提到 16384 | 二选一都行，先试保持 4096 | 改一个变量先；如果 leaves 数量太多导致 manifest 或 shard_partition_map 压力大再换 |
| Q5 Phase 1 readback 触发 INC-041 如何办 | 退串行，不阻塞本 step | INC-041 是 pump/ 问题，035 不修 |
| Q6 value size-class mix（§4.4） | 可选子目标，紧则拆单独 step | 不阻塞规模化主线 |
| Q7 timing 输出格式 | 按 §4.3 走，不做 JSON | 人肉判断优先；未来 profile binary step 再做结构化输出 |
| Q8 默认 rounds 保留 3 | 保留 | 和 033 一致；新增 `--rounds N` CLI 做长序列压测 |
| Q9 harness binary 一个还是拆多个 | 一个（`inconel_test_flush_e2e`） | 033 §9 已决策 "单 binary 持续扩"，035 不拆 |
| Q10 是否同时推进 INC-047 覆盖 | 不推进 | 延后；INC-047 需要 tree 写侧改动，035 只负责规模化 |
| Q11 035 范围内是否修 `rt::publish_shard_partitions` race | **不修** | 用户明确 "你只管测试，崩溃了是代码实现的问题，需要测出来修改"。Phase 2 作用就是把 race 实证打出来；035 职责到登 issue 为止 |
| Q12 Reader snapshot 严格 vs 松 | 松（current + previous + 1 retry） | 严格在 benign race window 下会误报；松口径更能隔离"真 bug" vs "临时窗口" |
| Q13 Reader 数量 | 2（`this_core_id = 10, 11`） | 两个能覆盖 3 shard 的 fan-out；3 个 reader 反而把 race 窗口摊开不利于 debug |
| Q14 `--concurrent-readers` 默认值 | Phase 2 land 后默认 2；Phase 1 期间默认 0 | 和"e2e 是唯一入口"的精神对齐——e2e 的正常跑就是并发口径，不开反而失去覆盖 |
| Q15 Phase 2 SIGSEGV 是否视为"035 失败" | 不视为失败 | 是暴露 race 的**预期结果**，035 的 ship gate 是"登 issue 清楚 + reproducer 能重放"，不是"跑不崩" |
| Q16 并发 publish 数据结构 | `std::atomic<std::shared_ptr<published_snapshot_t>>` 首选，libstdc++ 若不支持退 `mutex + shared_ptr` | 不自造 lockfree；035 不追求 publish 路径微秒级延迟 |
| Q17 value bytes 对比在 reader 主循环是否做 | 不做，只 match data_ver | 避免把 reader 拖成 value_scheduler IO bound；final verify 阶段保留字节级对比（和 033 一致） |
| Q18 reader 停止信号使用 `std::atomic<bool>` 还是 CV | atomic<bool> + relaxed load | reader 主循环每 batch 检查一次，relaxed load 足够；stop 延迟最多一个 batch 可接受 |

---

## 10. 预期改动面估算

### Phase 1

| 文件 | 类型 | 行数估计 |
|---|---|---|
| `apps/inconel/format/format_profile.hh` | 改（提升常量 1 行） | ~3 |
| `apps/inconel/test/test_flush_e2e.cc` | 改（timing + `--rounds` + 并发 readback + allocator 用量打印） | ~120 |
| `apps/inconel/test/test_runtime.cc` / `test_value.cc` / `test_runtime_topology.cc` | 改或删（视冲突） | ~50–150 |
| `CMakeLists.txt` | 可能删 1-2 个 target | ~5 |

### Phase 2

| 文件 | 类型 | 行数估计 |
|---|---|---|
| `apps/inconel/test/test_flush_e2e.cc` | 改（§4.5 的 `published_snapshot_t` + reader jthread + match_all + counters + CLI） | ~200 |

### 通用

| 文件 | 类型 | 行数估计 |
|---|---|---|
| `ai_context/inconel/plan/035_*.md` | 本文 + "实现后观察" | ~150（追加） |
| `ai_context/inconel/known_issues.md` | 增"删除的老 test" + Phase 2 实证的 race / mismatch issue | ~50 |

**合计**：~600 行（其中 Phase 2 专有 ~200 行）。

**零 production pipeline 改动**。唯一 production 改动是 profile 常量（§3.1）。

---

## 11. 下一步（035 外）

1. **Standalone profile binary**（`feedback_perf_profile_on_module_e2e`）——035 规模通过后做
2. **INC-047 delete-to-empty 收敛** —— 有独立 step 价值
3. **`rt::publish_shard_partitions` 原子发布**（033 §12.5 + 035 可能在 10^6 下触发） —— 可独立 step
4. **`-Wsubobject-linkage` warning cleanup** —— 独立清理 step
5. **偏斜 key / 热点 workload** —— 性能方向，可拆 perf step
6. **WAL / coord integration** —— 先落 WAL/coord 模块本体
7. **10 亿 KV 目标** —— 产生在 production SPDK path 上，mock 路径不追
8. **INC-048 `value::persist_values` 无界 concurrent 分块**（Phase 1 实现后新增，见 §12） —— production 侧解决后可把 harness 的 `kPersistChunkPuts = 1024` 兜底撤掉

---

## 12. 实现后观察（Phase 1，2026-04-20）

> 实际 land Phase 1 后回填。Phase 2 完成后再在本节之后另起一段。
>
> 机器：dev 主机本地 mock_nvme 路径；Release 构建（`cmake -B build -DCMAKE_BUILD_TYPE=Release` + `cmake --build build`）；OS/CPU 不锁频，但冷启动跑第一次、没重启机器中间跑 4 档，作为 smoke 级观察。

### 12.1 Production 改动

只动了 plan §3.1 授权的一条：`apps/inconel/format/format_profile.hh::kBootstrapFormatProfile.value_data_area_end.lba` 从 `8000` → `100000`（~400 MiB namespace）。其它 production pipeline 一行没改。

### 12.2 Per-module test 冲突处理

全部按 §3.1.1 option 2（"改动态 CHECK，不硬编 8000"）处理，没有整 target 删除：

| 文件 | 改动 | 动机 |
|---|---|---|
| `apps/inconel/test/test_value.cc` | 删掉 `DATA_AREA_END_LBA = 8000` 常量；`profile.value_data_area_end.lba == DATA_AREA_END_LBA` 改成 `> base.lba`（`static_assert profile_is_self_consistent` 已经保证整体一致性）；`TOTAL_LBAS` 从 `8192` 改成 `profile.value_data_area_end.lba`，让 mock device 覆盖 profile 的 value area | 老 CHECK 是"profile 形状跟 test 期望常量一致"，profile 形状已由 `format_profile.hh` 的 `static_assert profile_is_self_consistent` 保护，不再需要 test 侧硬编数字 |
| `apps/inconel/test/test_inc046_regression.cc` | `kNamespaceBytes` 从 `8000 * lba_size` 改成 `profile.value_data_area_end.lba * lba_size` | 同上；device 必须覆盖 profile，动态读取即可 |
| `apps/inconel/test/test_flush_e2e.cc` | 同步动态化 `kNamespaceBytes` | 同上 |

`test_runtime.cc` / `test_runtime_topology.cc` 原本就读动态的 `profile.value_data_area_end.lba`，无需改动，重跑 PASS。`test_tree_value.cc` 的 `DATA_AREA_END_LBA = 8000` 是该测试自己的本地 mock 设备常量（不跟 profile 对比），不受影响。

### 12.3 Harness 改动

| 改动 | 说明 |
|---|---|
| 新增 `--rounds R` / `--readback-samples S` CLI | `R >= 2`（默认 3），`S >= 1`（默认 50） |
| Round 3+ 复用 round-3 op pattern，`round_tag` 升到 round_id | 重复 overwrite/tombstone/newcomer 模式，强制 shadow-CoW slot cycling |
| Per-phase timing：`persist_values` / `build_sealed_gen` / `tree_local_flush` + 单轮 total + 全部 rounds total + ops/sec | 用 `std::chrono::steady_clock`，单位 ms |
| Allocator snapshot 打印：`tree_head_lba` / `tree_used_lbas` / `value_head_lba` / `value_used_lbas` / `free_lbas` | 数据源是 `core::registry::data_area_heads_ptr` 上的 atomics |
| `manifest.slot_map.size()` 进 per-round 输出 | 配合 `leaf_order.size()` 一起看 shadow 分布 |
| `verify_against_expected` 的 value bytes 校验改为 pipeline 内 `for_each(items) >> concurrent(32) >> flat_map(value::read_value) >> then(slot write) >> all()` | 单线程 orchestrator + pipeline 内部并发；tombstone 校验仍同步（不需要 value I/O）|
| `persist_put_values` 拆成 1024 个 put 一批顺序 submit | 绕开 INC-048（`value::persist_values` 无界 `concurrent()` 打爆 `mock_nvme::per_core::queue` capacity 2048）|

### 12.4 四档验收实测

| 命令 | 结果 | 端到端 wall | 备注 |
|---|---|---|---|
| `./build/inconel_test_flush_e2e` (N=1000, R=3) | PASS | 1.2–1.3 ms (3 次重跑一致) | 稳定，无 flaky |
| `./build/inconel_test_flush_e2e --num-keys 100000` | PASS | 122 ms | round 1 占 44ms，round 2/3 各 10ms |
| `./build/inconel_test_flush_e2e --num-keys 1000000` | PASS | 1619 ms | 远低于 5 min cap；round 1 514 ms（persist 34 + build_sealed 187 + flush 293），round 2/3 合计 ~247 ms |
| `./build/inconel_test_flush_e2e --num-keys 1000 --rounds 10` | PASS | 2.6 ms | 10 轮 allocator 压力稳定增长（见 §12.5） |

### 12.5 Allocator 用量曲线

N=1000, rounds=10（单位 LBA）：

| round | leaf_order | slot_map | tree_used | value_used | free |
|---|---|---|---|---|---|
| 1  | 12 | 13 | 13  | 16 | 95971 |
| 2  | 14 | 15 | 22  | 21 | 95957 |
| 3  | 16 | 17 | 32  | 25 | 95943 |
| 4  | 16 | 17 | 42  | 30 | 95928 |
| 5  | 16 | 17 | 52  | 35 | 95913 |
| 6  | 16 | 17 | 62  | 40 | 95898 |
| 7  | 16 | 17 | 72  | 44 | 95884 |
| 8  | 16 | 17 | 82  | 49 | 95869 |
| 9  | 16 | 17 | 92  | 54 | 95854 |
| 10 | 16 | 17 | 102 | 58 | 95840 |

leaf_order / slot_map 从 round 3 起稳定在 16 / 17（round 3 起 op 范围不再扩），两区各按 ~10 LBA/round (tree) 和 ~5 LBA/round (value) 线性增长——每轮的新 range 稳定分配、老 range 未被 recycle，这是 v1 预期行为（retire/reclaim pipeline 还没落）。`free_lbas` 占比 >95%，说明当前 10 轮远远没把 `value_data_area_end.lba = 100000` 填满。

N=10^6, rounds=3：
- round 1 结束：leaf_order=10311, slot_map=10376, tree_used=10376, value_used=15625, free=69999
- round 2 结束：leaf_order=12373, slot_map=12450, tree_used=14533, value_used=20313, free=61154
- round 3 结束：leaf_order=14435, slot_map=14525, tree_used=18692, value_used=25000, free=52308

10^6 keys → ~10.3k leaves（~97 keys/leaf，profile `tree_page_size=4096` 单 LBA 页，32B key + 8B value + slot header 下大致符合容量估算），tree_used ~18.7k LBA、value_used ~25k LBA，剩余 ~52k LBA、即本 profile 还有充足头寸走到 N ~ 3×10^6 量级（容量上限仍由 `value_data_area_end.lba` 决定，超出再调）。

### 12.6 本会话没碰到、也不处理的潜在问题

- INC-041（`concurrent() + reduce()/to_vector()` 共享 accumulator contract）：Phase 1 新 readback 用的是"pre-sized vector 按 slot_idx 写入 + `>> all()`"的模式，每个 slot 单元独立、没有共享 accumulator，因此绕开了 INC-041，没触到。INC-041 本身仍然挂在 pump/ 侧等 framework 修。
- `rt::publish_shard_partitions` 的 shared_ptr race（033 §12.5）：Phase 1 里 publish 只在 round 1 的 custom 3-shard map 一次 + 每轮 flush 尾自动 rebuild，reader 侧没有并发读（那是 Phase 2 的活）。四档跑完没撞上。
- INC-043 fail-fast panic 的 "displaced base leaf / 未剪空 leaf" 两条触发路径：N=10^6 rounds=3 没触，N=1000 rounds=10 也没触，`build_leaf_order_full` 的 panic 没响。
- INC-047 delete-to-empty 累积：N=1000 rounds=10 有反复 tombstone 老区间但没清空整 leaf；N=10^6 rounds=3 的 tombstone 分散度足够小，也没碰 leaf 全空。

### 12.7 新暴露的 issue

- **INC-048**（新）：`value::persist_values` 的 leader 路径用 `as_stream(writes) >> concurrent() >> flat_map(nvme::write)` 无界并发，单次传 ~1M entries 直接打爆 `mock_nvme::scheduler::req_queue`（per_core::queue，capacity 2048）。正式 nvme scheduler 的 queue depth 也是 2048，不是 mock 专属。Phase 1 在 harness 侧用 `kPersistChunkPuts = 1024` 兜底（每 1024 个 put 一次 `submit_and_wait`），production 侧修好后可撤。已登 known_issues `INC-048`。

### 12.8 Phase 2 的 Prereq / 风险状态

- `std::atomic<std::shared_ptr<T>>` 发布点尚未引入——Phase 2 要做的 `published_snapshot_t` 原子发布（plan §4.5.1）在 Phase 1 没有 touch。
- 两个 reader jthread 的 `this_core_id = 10, 11` 坐标在 Phase 1 没 register 到 registry；Phase 2 需要确认 `per_core::queue` 的 MAX_CORES=128 足够（是）、runtime 的 `is_running_by_core` 是否要兼容 core_id 10/11（需要 check）。
- Phase 1 的 concurrent readback 走的是 "主线程 submit 唯一 writer + 自己 verify"；Phase 2 要加 2 个独立 reader 线程并发跟 writer，这是 Phase 1 没覆盖的时序新面。

---

## 13. 实现后观察（Phase 2，2026-04-21）

> 实际 land Phase 2 后回填。
>
> 机器：dev 主机本地 mock_nvme 路径；Release 构建（`cmake -B build --fresh -DCMAKE_BUILD_TYPE=Release` + `cmake --build build --target inconel_test_flush_e2e -j`）；GCC 15.2.1。四档验收在同一 session 里按顺序跑。

### 13.1 Production 改动

**零改动。** Phase 2 是纯 test-only 扩展，production pipeline 一行没碰：
- `tree::lookup` API 签名不变
- `rt::publish_shard_partitions` / `current_shard_partitions()` / `rd->partitions` 的 shared_ptr 发布路径没改（033 §12.5 登记的 race 仍以原形保留——plan §1.2 明确"不修"）
- `runtime::build_options` / `start_options` 不变
- `kBootstrapFormatProfile` 不变（Phase 1 已经把 `value_data_area_end.lba` 从 8000 改到 100000）

### 13.2 Harness 改动

按 plan §4.5 / §5.2 实装，全部集中在 `apps/inconel/test/test_flush_e2e.cc`：

| 改动 | 说明 |
|---|---|
| 新增 `published_snapshot_t { manifest, expected, round_id }` | plan §4.5.1 定义 |
| 全局 `std::atomic<std::shared_ptr<published_snapshot_t>> g_current_snap` | GCC 15.2 + libstdc++ 直接支持 C++20 `atomic<shared_ptr<T>>`，没退 mutex fallback |
| 全局 `std::atomic<bool> g_reader_stop` | reader 退出信号，`relaxed` 一端 store、另一端 load |
| `publish_empty_snapshot()` + `publish_round_snapshot(manifest, state, round_id)` | round loop 外先 publish 空 snapshot；每轮末尾 publish 新 snapshot（整份 deep-copy `expected_state`） |
| `reader_counters { ok, ok_stale, ok_retry, batches, mismatch }` | plan §4.5.6 的 per-reader 计数 |
| `match_all_against(keys, results, expected)` | plan §4.5.4：kind + data_ver 比较；`expected` 里没的 key → result 必须是 `lookup_absent`。不比对 value bytes（留给 final static verify） |
| `rebuild_keys_cache` + `pick_random_keys` | reader 每次 snapshot 变更时重建 `vector<string_view>` 视图（views 的存活由 reader 持有的 snapshot shared_ptr 保证），然后用 `std::mt19937_64` 均匀抽 K 个 |
| `reader_main(reader_idx, batch_size, out_counters)` | plan §4.5.3 主循环：`this_core_id = 10 + reader_idx`，load snapshot → 维护 (current, previous) → 抽 K keys → `submit_and_wait` 跑 `just() >> tree::lookup(keys, current->manifest.get())` → match current → previous → reload → 仍不 match 才 abort（打印详细上下文） |
| CLI `--concurrent-readers N` (默认 2，范围 [0, 2]) | plan §4.5.7 固定 reader id = {10, 11}；放开到 3 另做 step |
| CLI `--reader-batch K` (默认 16，范围 [1, 1024]) | plan §4.5 默认值 |
| main: 初始空 snapshot publish → 启动 N 个 `std::jthread` reader → 进入 round loop（每轮尾 publish）→ stop reader 并 join → 最终静态 verify（和 033 / Phase 1 口径一致） | plan §4.5.5 停止顺序 |

编译路径要点：`reader_main` 的定义放在 `submit_and_wait` 之后的同一 anonymous namespace 里；对应的 snapshot types / counters / match helpers 和 publish helpers 在 `apply_round_to_expected` 之后、`submit_and_wait` 之前，这样不需要前置声明。

### 13.3 四档验收实测（`--concurrent-readers 2 --reader-batch 16` 默认生效）

| 命令 | 结果 | 端到端 wall | Reader 计数 |
|---|---|---|---|
| `./build/inconel_test_flush_e2e` (N=1000, R=3) | PASS + `all passed` | 1.6 ms | r0: batches=8 ok=8 ok_stale=0 ok_retry=0 mismatch=0; r1: batches=16 ok=16 ok_stale=0 ok_retry=0 mismatch=0 |
| `./build/inconel_test_flush_e2e --num-keys 100000 --rounds 3` | PASS | 159.8 ms | r0: batches=1180 ok=1180; r1: batches=1102 ok=1102；全部 ok_stale/ok_retry/mismatch = 0 |
| `./build/inconel_test_flush_e2e --num-keys 1000000 --rounds 3` | PASS | 1.74 s | r0: batches=10966 ok=10966; r1: batches=10902 ok=10902；全部 ok_stale/ok_retry/mismatch = 0 |
| `./build/inconel_test_flush_e2e --num-keys 1000 --rounds 10` | PASS | 4.5 ms | r0: batches=80 ok=80; r1: batches=44 ok=44；全部 0 |
| `./build/inconel_test_flush_e2e --num-keys 100000 --rounds 10`（补跑，拉高 publish 频次 + reader 总 runtime） | PASS | 356 ms | r0: batches=5040 ok=5040; r1: batches=4654 ok=4654；全部 0 |

静态 final verify（51-sample stride + value bytes）在每档都 PASS，和 Phase 1 口径一致。

### 13.4 Outcome 分类

按 plan §5.2 #6 的三类 outcome：

- **PASS** ✅：以上全部 5 档都属于此类；counters `mismatch=0`、无 SIGSEGV、静态 verify OK。
- **逻辑 mismatch abort**：**未触发**。
- **SIGSEGV / torn shared_ptr crash**：**未触发**。

这意味着 plan §5.2 #6 第三条 "若 Phase 2 因 race 持续崩就把 `--concurrent-readers` 默认改 0" 不需要启用——**默认 2 reader 的配置可以正常 ship**。

**为什么 race 没被实证**（分析，不是辩护）：
- `rt::publish_shard_partitions` 一轮只调一次（flush 尾部 `rebuild_and_publish_shard_partitions`）。在当前 workload 下，round 间隔 ≈ 10–500 ms，而 reader 每 batch 只需要微秒级的 "load snapshot + build route plan + enqueue" 时间——publish 打中 reader 正在 route 的窗口概率非常小。
- reader 这一侧 `tree::lookup(keys, current->manifest.get())` 调的 `core::registry::current_shard_partitions()` 确实会跨核读 `current_shard_partitions_ptr`（非 atomic shared_ptr），但一次 lookup 只读一次，然后在 local `auto partitions = …` 栈变量里用剩的全程，不再触达全局；race 只在这一次 load 的指令瞬间。
- 我们用的是 "build a fresh map + 整个 struct pointer swap"，非 partial mutation；即使读端正好夹在 writer 的 "先 install global，再刷每个 rd->partitions" 两步之间，`current_shard_partitions_ptr` 已经是新 map 了，lookup 用新 map 路由到 `tree_read_domain_at(shard_idx)`——新 shard_idx 在 rd->partitions 尚未更新到新版本前是老 partitions 视角下生成的，会路到老 read_domain 的 lookup_sched；但这**只有在 rebuild 后 shard 数量/id 映射发生重排时才可能路错**，而 `rebuild_and_publish_shard_partitions` 在同样 `tree_read_domain_count()=3` 下重建 3 shard（shard_idx 0/1/2 → read_domain list[0/1/2]），shard_idx 永远对得上 read_domain 下标，没有错路风险。
- `rd->partitions` 这个 member 本身在 Phase 2 的 lookup 热路径里**没被读**——`tree::lookup` 只依赖全局 `current_shard_partitions()`，`rd->partitions` 的消费者是 flush fold 路径（`memtable_fold`），而 flush 只在主线程内调用。所以即使 033 §12.5 担心的"跨 core 非 atomic 写 rd->partitions" 真的存在数据 race，Phase 2 的 reader 走的不是受影响的代码路径。

**结论**：不是 race 不存在，而是**当前 workload 和当前 lookup 路径对 race 的暴露面非常窄**。Plan §1.2 的"暴露 race"目的没有在 5 组实测里实现；但 plan §5.2 #6 的 ship gate 是 "crash-free + counter printed + rounds=10 completes"，五组全部满足。

### 13.5 本会话没碰到、也没主动探测的问题

- INC-041（PUMP `concurrent()+reduce()/to_vector()` 共享 accumulator）：Phase 2 的 reader 用的是 `for_each` 驱动 `tree::lookup` 自己的内部 `concurrent() + all()` 收集，没走 reader 本地 reduce，没触及。
- `rt::publish_shard_partitions` 的 shared_ptr race（033 §12.5）：如 §13.4 分析，reader 热路径避开，没触发。
- INC-043 fail-fast panic（033 §12.7）：最大 case `--num-keys 1000000 --rounds 3` 没触；`--num-keys 100000 --rounds 10` 也没触。
- INC-047 delete-to-empty：未触。

### 13.6 新登记的 issue

**无。** Phase 2 没产生新的 known_issues 条目。033 §12.5 的 publish race 依然悬挂作为后续独立 step 的候选——但 Phase 2 没有为它提供可重放崩溃的 reproducer，下一步想修 race 的 step 需要自己造更激进的 workload（比如 reader 多到把 route 热路径占满，或者主线程 publish 频率拉到 kHz 级别）。

### 13.7 Ship 状态

- **Plan §6.2 所有硬指标 ✅ 满足**（四档都跑通 + counter 打印 + mismatch=0 + 无 crash）
- **默认 `--concurrent-readers 2` 保留**（无需按 §5.2 #6 fallback 到 0）
- **`--reader-batch 16` 默认保留**
- Phase 2 完成口径达成：`--rounds 10` 长序列下跑完 + counter 打印 + crash-free
- 033 §12.5 的 publish race **仍是悬挂 known issue**，Phase 2 未能实证触发，后续独立 step 继续跟

### 13.8 后续建议（不在 035 scope）

- 要真触发 publish race，workload 需要比 035 更激进——例如 publish 侧手动触发 1kHz 频率、reader 数拉到 4+ 并且故意让 `current_shard_partitions()` 的 load 落在 publish 两步之间。这类激进 workload 属于 race-fix step 自己构造的 regression harness，不是 035 要做的事。
- 如果想把 `ok_stale` / `ok_retry` 正向打出去而不只是"兜底存在"，可以在独立 step 里把 reader 直接调成批次 submit 多份 lookup 挤压 publish 时间缝——但这改变了 035 的用意（035 是"把并发读写接入 e2e"，不是"对 race 做定向压测"）。
