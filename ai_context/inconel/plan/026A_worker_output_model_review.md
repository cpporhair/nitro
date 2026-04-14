# 026A — Worker 输出模型 Review

> Review 对象：当前 worktree 上的 `026` / `026A` production 代码与已更新测试。
>
> Review 口径：
>
> 1. 按当前讨论后的边界评审：`026` / `026A` 只负责 worker-local candidate/overlay producer，不负责 owner merge / new manifest materialization / full flush pipeline 接线。
> 2. `027` 负责把整条 flush pipeline 补全，包括 owner 消费 `worker_results`、构造 `new_manifest`、写 NVMe、维护 `root_range_base`。
> 3. 本 review 重点额外检查两条硬要求：
>    - 容量 / 效率要符合 Inconel 的规模目标，不能只在小量级自洽。
>    - worker 与 lookup 同核配对时，后台 flush 不能严重影响前台 lookup；当前口径按 **soft best-effort** 评估，即“后台尽量别太猛”，是否需要严格 CPU yielding / 强让路，留待 v1 完成后结合 YCSB 毛刺结果再定。

---

## 1. 结论

**方向正确，但按当前边界还不能算完全收口。**

已经完成的部分：

1. worker 输出模型已经从 leaf-only 改成 `base_manifest + changed_nodes` 的 sparse overlay，`flush_worker_result` / `flush_round_state.worker_results` 这条 carrier 方向正确。
2. `process_candidate_groups()` 已经把 leaf merge、slot exhaustion 检查、internal/root cascade、以及同一套 multi-round async read 协议拼到一起。
3. 与旧接口残留有关的测试已经更新；当前 `inconel_test_candidate_build` 和 `inconel_test_flush_carriers` 都能 build + pass。

仍然需要明确记账的部分：

1. 同核 fairness 目前只做了 **I/O read budget throttle**，没有做 **CPU work yielding**。在当前 soft best-effort 口径下，这更像“已知风险/待 benchmark 验证”，不再视为当前 step 的硬阻塞。
2. `process_candidate_groups()` 现在是“大 partition 全量扫”的形状；在大 flush round 下，单次 worker op 的 CPU 成本无上限，不符合“前台优先、后台必要时让路”的要求。
3. 026A 新语义的测试覆盖仍偏窄，主要锁住 leaf overlay，未锁住 root/internal cascade 和 lookup busy 下的让路行为。

因此当前状态更准确的判断是：

- **026 / 026A 的“输出模型改向”可以接受**
- **026 / 026A 的“效率 / 同核前后台相处方式”在 soft best-effort 口径下基本可继续推进，但需要在 v1 benchmark 阶段重点看毛刺**

---

## 2. 验证执行

本次 review 实际执行：

1. `cmake --build build --target inconel_test_candidate_build inconel_test_flush_carriers -j4`
2. `./build/inconel_test_candidate_build`
3. `./build/inconel_test_flush_carriers`
4. `rg -n "root_range_base" apps/inconel --glob '!apps/inconel/test/**' -S`
5. 逐文件静态检查：
   - `apps/inconel/tree/candidate_build.hh`
   - `apps/inconel/tree/worker_scheduler.hh`
   - `apps/inconel/tree/lookup_scheduler.hh`
   - `apps/inconel/tree/flush_types.hh`
   - `apps/inconel/tree/flush_round_state.hh`
   - `apps/inconel/tree/memtable_fold.hh`
   - `apps/inconel/test/test_candidate_build.cc`

验证结果：

1. 两个定向目标均编译通过。
2. 两个可执行测试均通过。
3. 生产代码里 `root_range_base` 仍只有字段声明、`empty()` 默认值、以及 worker 侧消费点；没有生产侧 manifest builder 赋值来源。这一点按当前分工记到 `027`，不记为 `026A` 失败。

---

## 3. 已完成部分

### 3.1 输出模型方向已切到 overlay

代码现状：

1. `flush_worker_result` 已定义为 `base + changed_nodes + retired_old_values`，见 `apps/inconel/tree/flush_types.hh:241-252`。
2. `flush_round_state` 已改成存 `worker_results`，见 `apps/inconel/tree/flush_round_state.hh:125-130`。
3. `build_candidates_for_partition()` 最终返回 `flush_worker_result`，见 `apps/inconel/tree/sender.hh:156-194`。

这说明 026A 已经不再把 worker 结果建模成 leaf-only batch，而是建模成 owner 可整合的 sparse overlay。这和“每个 worker 返回自己视角下的一棵修改后树的局部表示”是一致的。

### 3.2 cascade 已进入 worker primitive

代码现状：

1. `trace_root_to_parent()` 从 `base_manifest.root_slot / root_range_base` 出发收集 root→parent 路径，见 `apps/inconel/tree/candidate_build.hh:303-373`。
2. `cascade_one_leaf()` 会把 leaf parent 直到 root 的必要 internal node 放进 `changed_nodes`，见 `apps/inconel/tree/candidate_build.hh:383-436`。
3. `process_candidate_groups()` 已把 leaf merge（pass 1A）、cascade（pass 1B）、leaf/internal read 准备（pass 2）放在同一个多轮协议里，见 `apps/inconel/tree/candidate_build.hh:447-590`。

这说明“worker 负责完整 consolidation cascade，owner 负责后续 merge/materialize”这条分工，代码方向已经对上。

### 3.3 与旧接口残留相关的问题已消除

之前旧测试还在断言 `result.leaves` / `round_state.candidates`。当前这部分已经同步到 overlay 模型，相关 build/test 均通过，不再构成 review finding。

---

## 4. Findings

### M-1: 同核 fairness 只做了 I/O throttle，没有做 CPU yielding

**位置**：

- `apps/inconel/tree/candidate_build.hh:268-278`
- `apps/inconel/tree/candidate_build.hh:455-536`
- `apps/inconel/tree/worker_scheduler.hh:301-310`
- `apps/inconel/tree/lookup_scheduler.hh:154-163`

当前实现确实读取 `paired_lookup->pending_lookups`，并用 `flush_read_budget()` 收紧 pass 2 的 read 数量。这个设计只解决了“后台别把 NVMe read 打太猛”。

但同核竞争真正更危险的是 CPU：

1. pass 1A 每次调用都会扫描整个 `leaf_groups`
2. cache hit 的 leaf 会直接在 worker 当前 tick 里做 merge + page build
3. pass 1B 还会继续做 root→parent trace 和 cascade
4. 这些 CPU 工作在 lookup busy 时**不会自动缩短**

也就是说，只要 tree page 在 cache 里，后台 flush 反而更容易长时间占 core。lookup 的 pending 只会影响“这一轮发几个 read”，不会影响“这一轮做多少 merge / trace / cascade”。

按“严格让路”标准，这不够；但按当前确认的 soft best-effort 口径，这条更合适记为 **中优先级风险**：

1. worker 和 lookup 同核、共享 read-domain cache，这条风险是真实存在的；
2. 但当前实现至少已经做到：
   - cache 只查不插，避免 flush 驱逐 lookup 热页；
   - read 数量按 `pending_lookups` 线性退让；
   - worker queue drain 本身是 bounded 的。
3. 是否必须再上 CPU chunk / 强让路，应等 v1 完成后以 YCSB tail / 毛刺结果决定。

**建议方向**

如果这条要求仍记在 `026/026A` 边界内，应该至少把单次 `process_candidates` 的 CPU work 做成 bounded chunk，而不是“一个 op 扫完整个 partition”。

---

### M-2: `process_candidate_groups()` 的单次 CPU 成本对大 partition 无上界

**位置**：

- `apps/inconel/tree/memtable_fold.hh:206-225`
- `apps/inconel/tree/candidate_build.hh:453-590`

partition 是按 `workset.size() / worker_count` 等分得到的。对大 flush round，每个 worker 可能吃到非常大的 `leaf_groups` span。

但 `process_candidate_groups()` 当前没有 cursor / budgeted CPU slice：

1. pass 1A 扫全量 `leaf_groups`
2. pass 1B 再扫全量 `leaf_groups`
3. pass 2 再扫全量 `leaf_groups`

因此单次 worker op 的 CPU 代价是：

`O(partition_size + pending_cascade * tree_height)`

而不是一个 bounded 小片段。

`worker_scheduler` 虽然限制了每次 `advance()` 最多处理 8 个 op（`kMaxProcessCandidatesOpsPerAdvance = 8`），但这对单个巨大 op 没有保护作用；大 op 仍然会长时间占住线程。

**为什么这是中优先级**

这条在当前 soft best-effort 口径下，不必立即判成阻塞项，但它是后续 benchmark 时最值得盯的一条：

1. 规模要求：大 round 下可能退化成单次长尾 CPU 段
2. 同核公平：lookup 无法在 worker 的大 op 中间插队
3. 如果 YCSB tail 不达标，这一条会是最优先回头改的点

---

### M-3: cascade 路径存在重复 trace 成本，cache-hot 时会放大 CPU 压力

**位置**：

- `apps/inconel/tree/candidate_build.hh:397`
- `apps/inconel/tree/candidate_build.hh:571-577`

同一个 pending leaf：

1. pass 1B 先做一次 `trace_root_to_parent()`
2. 如果没完成，pass 2 又重新 trace 一次，只为拿到第一个 miss page

这在语义上是正确的，但在 cache-hot、树高固定的场景里，会把 CPU 代价按“pending leaves × rounds”重复支付。

当 flush 正好和前台 lookup 同核时，这类重复 trace 会直接放大 H-1 / H-2 的问题。

**建议方向**

不一定要在 026A 里马上改，但至少要明确这是已知成本，不要把当前形状误判成“已经足够高效”。

---

### M-4: zero-write skip 还没实现，会放大后续 owner merge / write 压力

**位置**：

- `apps/inconel/tree/candidate_build.hh:491-500`
- 对照设计：`ai_context/inconel/design_doc/runtime_state_machine.md:896-900`

当前 leaf merge 后无条件把结果写进 `changed_nodes`。没有 “merged image == old image” 的 skip。

对 026/026A 而言，这不是 correctness bug，但会放大：

1. `changed_nodes` 数量
2. 027 owner merge 需要处理的条目数
3. 后续 write plan 的压力

在大 flush round 下，这会直接增加后台 CPU 和写放大。

如果 026/026A 的目标包含“输出模型不能明显损伤后续规模与效率”，这条应该至少被显式记账。

---

### M-5: 新语义测试覆盖仍偏窄，没有锁住你真正关心的点

**位置**：

- `apps/inconel/test/test_candidate_build.cc:341-352`
- `apps/inconel/test/test_candidate_build.cc:401-410`
- `apps/inconel/test/test_candidate_build.cc:496-500`

当前测试已经同步到 `changed_nodes`，但主要覆盖的是：

1. leaf overlay
2. `needs_new_range == false`
3. bounded reads on leaf pages

还没有覆盖：

1. `needs_new_range == true`
2. internal page read / `internal_page_bufs`
3. root cascade
4. lookup busy 时 `pending_lookups` 对 worker 行为的实际影响

所以现在的“测试通过”只能说明 leaf overlay 没坏，不能说明 026A 在同核 fairness 和 root/internal cascade 上已经完成。

---

## 5. 明确不计入 026 / 026A 的项

以下问题存在，但按当前讨论后的边界，不记为 026 / 026A 未完成：

1. `root_range_base` 的生产者还没落地。它应该由 owner / manifest builder 在 `027` 构造 `base_manifest` / `new_manifest` 时维护，而不是 worker 单独接参数。
2. top-level `tree_local_flush()` 还没接 worker candidate fanout/fan-in。
3. `flush_merge_request` 还没从 `mapping_results` 切到 `worker_results`。
4. owner 还没做 overlay merge / slot-range allocation / internal page patch / `new_manifest` materialization。

这些都属于 `027` 的 flush pipeline 补全过程。

---

## 6. 建议讨论点

下面几条建议作为讨论提纲，不是本 review 强行指定的唯一实现：

1. `026/026A` 是否把“CPU 级让路”当作当前 step 必需项，而不是留到 `028` hardening。
2. 如果要在当前边界内解决，`candidate_build_state` 是否需要增加 cursor / ready-queue，把 `process_candidate_groups()` 从“全量扫”改成“bounded chunk”。
3. 是否接受当前 zero-write skip 缺失，记到 `027` 一起做；还是认为它属于 026 输出质量的一部分，应该前移。
4. 是否要在当前阶段补两类测试：
   - `needs_new_range=true` + internal/root cascade
   - synthetic `pending_lookups` busy 场景下的 worker budget / yielding 行为

---

## 7. 当前判断

如果只问“026A 有没有把 worker 输出从 leaf-only 改成 overlay”，答案是 **有**。  
如果问“026 / 026A 按当前讨论后的要求能不能继续往下推”，我当前判断是 **可以继续推进到 027**，但要明确带着下面这些已知风险：

1. 大 partition 下单次 worker op CPU 段无上界
2. 同核场景里后台 flush 目前只有 soft best-effort 的 I/O 退让，没有严格 CPU 让路
3. 这些问题是否需要前移修，取决于 v1 完成后的 YCSB tail / 毛刺结果

也就是说，当前更适合把它定义为：

- **026 / 026A：功能方向可收，性能公平性风险待 benchmark 关口决定是否回补**
