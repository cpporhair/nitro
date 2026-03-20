# 使用 Claude Code 完成 AiSAQ 项目的心得报告

## 一、项目概览

**AiSAQ** 是一个基于 PUMP 异步 pipeline 框架的 NVMe 向量搜索引擎（DiskANN 变体），从零开始用 Claude Code 辅助完成了完整的工程实现：

| 维度 | 数据 |
|------|------|
| 开发周期 | 7 天（2026-03-13 → 2026-03-19） |
| 代码规模 | ~4,900 行 C++ |
| commit 数 | 31 个（其中 28 个 Claude co-authored） |
| 阶段 | Phase 1-4 全部完成（搜索、优化、多核多盘、索引构建） |
| 性能 | 单核 4.1x、8核3盘 19.3x 对比原版 aisaq-diskann |
| 涉及技术 | SPDK NVMe DMA、无锁 pipeline、AVX2 SIMD、CUDA PQ 训练、share-nothing 多核 |

PUMP 框架本身不是 Claude Code 写的——它是用户手写的底层异步管道库，有自己独特的 sender/operator 语义。Claude Code 的工作是**在这个框架上构建应用**，同时在过程中帮助优化框架本身。

---

## 二、最核心的心得：CLAUDE.md 是一切的基石

### 2.1 不写好 CLAUDE.md，Claude Code 就是个高级补全器

PUMP 框架有自己独创的编程范式——`just() >> then() >> flat() >> on(sched) >> submit(ctx)` 这套 sender pipeline 语法在任何主流项目中都不存在。没有 CLAUDE.md，Claude 对这套 API 的理解基本为零。

这个项目的 CLAUDE.md 体系经过多次迭代，最终形成了 **1,359 行、4 个文件** 的规范：

| 文件 | 行数 | 内容 |
|------|------|------|
| `CLAUDE.md` | 338 | 核心规则、禁止项、代码模式、sender 速查表、scheduler 模板 |
| `ai_spec/RUNTIME_MODEL.md` | 165 | 运行时执行流、Context/Scope/Op 核心类型 |
| `ai_spec/SENDERS_DETAIL.md` | 150 | 各 sender 的详细语义（flat、visit、concurrent 等） |
| `ai_spec/CODING_GUIDE.md` | 386 | 反模式、并发模型、复杂功能实现指南 |

**关键教训**：第一版 CLAUDE.md 只有寥寥几行 API 列表，Claude 写出的代码充满反模式（then 中阻塞、忘记 flat、shared_ptr 到处飞）。逐步补充「禁止项」「代码模式」之后，代码质量阶梯式提升。

### 2.2 CLAUDE.md 的写法原则

**写约束，不写教程**。Claude 不需要理解"为什么 flat() 要这样设计"，它需要知道"then 返回 sender 时必须 >> flat()"。最有效的格式是：

```
### 禁止
1. then 中阻塞 → 用 on(scheduler) 切换后异步操作
2. 捕获局部引用 → 值捕获或 Context 传递
```

**写模式，不写文档**。比起解释 `concurrent` 的内部实现原理，直接给出代码模板更有效：

```cpp
// 并发（三要素：concurrent + on(scheduler) + reduce/all）
for_each(items) >> concurrent(N) >> on(sched.as_task()) >> then(process) >> reduce();
```

**补充详细规范时用单独文件**。`CLAUDE.md` 保持精炼（速查表 + 核心规则），细节放 `ai_spec/` 下。Claude 会在需要时引用这些文件。

### 2.3 CLAUDE.md 应该随项目演进

整个开发过程中，CLAUDE.md 至少经历了 5 次重大更新：
- 加入 `visit()` 的详细分支规则（Claude 之前不知道 bool 会变成 true_type/false_type）
- 加入 Leader/Follower 合并模式（KV 项目的经验总结）
- 加入异常处理分层策略（实践中发现 Claude 总是吞掉异常）
- 加入 RPC session-only API 模式（网络模块重构后更新）
- 加入 `when_any` 语义（新增算子后同步文档）

---

## 三、协作模式：人类做架构决策，Claude 做实现

### 3.1 有效的分工

| 人类负责 | Claude 负责 |
|---------|-----------|
| 确定 Phase 目标和优先级 | 将目标翻译成具体代码 |
| 选择算法和数据结构（如 inline PQ、BFS 重排列） | 实现算法细节 |
| 设计 Scheduler 划分（哪些状态需要保护） | 写 scheduler 的 handle/op/sender 模板代码 |
| 判断性能瓶颈在哪（perf profile 分析） | 尝试优化方案（SIMD、batch、预计算） |
| 做出 revert 决策（PQ scheduler 放弃） | 快速生成替代方案 |
| 调试环境问题（SPDK 配置、SSD format） | 生成调试代码和日志 |

### 3.2 最成功的协作方式：需求文档驱动

`ai_context/aisaq_requirements.md`（需求文档）贯穿整个开发过程，起到了"活的 spec"的作用：

1. **开发前**：人类写好需求文档的核心设计约束（磁盘布局、Inline PQ、DRAM-free）
2. **每个 Phase 开始**：在文档中更新当前阶段的目标和已完成状态
3. **遇到关键决策**：在文档中记录决策和理由（如 PQ scheduler 为什么放弃）
4. **开发中**：Claude 参考文档确认实现是否偏离

**失败教训**：Phase 1 初期没有严格按需求文档实现，Claude 把 AiSAQ 的核心特征（inline PQ）换成了标准 DiskANN 的全量 PQ 码常驻内存模型。这是根本性的设计错误，浪费了一个完整会话的时间。此后在 memory 中记录了「严格按需求文档实现，基础需求优先」的 feedback。

### 3.3 最有效的交互模式

**短迭代，频繁验证**：不要让 Claude 一次写完 500 行然后编译不过。更好的模式是：
- 写 50-100 行核心逻辑 → 编译 → 修复 → 跑测试 → 看结果 → 调整

**用数据说话**：每次优化后立即跑 benchmark，用具体数字确认效果。这比让 Claude 猜测"这个优化应该有效"要靠谱得多。AiSAQ 的 commit message 就记录了这种模式：

```
recall@10从5%提升到43%
recall@10从30%提升到92%
QPS超越aisaq-diskann
消除25%空轮询开销
QPS提升7%
```

**明确说"不要"**：Claude 有过度工程的倾向。最有效的纠正是直接说"不要做 X"，然后这条 feedback 会被记录到 memory 中，影响后续所有会话。

---

## 四、Memory 系统的实战价值

### 4.1 Feedback Memory 是最重要的 memory 类型

12 个 memory 文件中，**feedback 类型占 7 个**，它们直接改变了 Claude 的行为：

| Feedback | 改变了什么 |
|----------|-----------|
| `flat_map 使用场景` | 停止在不需要 flat_map 的地方用 flat_map |
| `push_context 在循环中只能用一次` | 避免了反复出现的 moved-from 崩溃 |
| `不要轻易怀疑框架 bug` | 停止把时间浪费在框架层排查上 |
| `对框架性能有信心` | 停止提出不必要的"框架优化"建议 |
| `NVMe OS 盘不能用` | 避免了可能毁掉系统的危险操作 |
| `配置文件必须匹配` | 避免了反复出现的 Recall 异常 |
| `严格按需求文档实现` | 避免了设计偏离 |

**关键洞察**：每个 feedback 都对应一个真实的时间浪费事件。`feedback_config_mismatch.md` 的 "Why" 明确写着："这个 session 中多次因为 NVMe 上是单盘数据但用 3 盘 config 搜索导致 Recall 异常（9.99%、77%），浪费了大量时间排查。" 把痛苦经历变成 memory，是 Claude Code 最强大的复利机制。

### 4.2 Project Memory 用来记录"为什么"

代码和 git log 能告诉你"做了什么"，但不能告诉你"为什么做这个决策"和"当前状态是什么"。Project memory 填补了这个空白：

- `project_aisaq_status.md`：Phase 1-3 完成状态、架构概览、性能基线、NVMe 设备列表
- `project_aisaq_phase4_pq_bug.md`：PQ Recall 问题的排查过程和结论——"PQ codebook 质量完全没问题，问题在搜索实现"

### 4.3 Memory 应该是精练的判断，不是日志

一个好的 memory 文件不超过 30 行，包含：rule/fact → Why → How to apply。如果需要详细的技术文档，放 `ai_context/` 下（不是 memory），因为那些内容会随代码变化。

---

## 五、踩过的坑和对应的最佳实践

### 5.1 环境问题 > 代码问题

**现象**：性能只有预期的 60%，Claude 花了大量时间分析 hot path 分配、CPU governor、大小核差异。

**根因**：`CMakeLists.txt` 中 `add_compile_options(-O0)` 出现在 cmake 的 per-config flags 之后，覆盖了 Release 的 `-O3`。

**最佳实践**：建立环境检查清单（已记录在 memory 中），在分析代码之前先检查：
- 残留进程？
- 实际编译 flags？（`ninja -t commands`）
- CPU governor？
- Debug vs Release？

### 5.2 不要让 Claude 猜测框架行为

Claude 多次猜测框架存在 bug（lock_free_queue 内存序问题、concurrent 状态累积等），每次最终都是应用层错误。唯一真正的框架 bug 是 `assert()` 内的副作用——这是经典的 C++ 陷阱，不是设计问题。

**最佳实践**：在 CLAUDE.md 的「禁止」项中明确写：

> 给 src/env 或 src/pump 加锁 → 错误是使用方式问题，追溯调用链找根因

### 5.3 先做对，再做快

AiSAQ 的 Recall 从 5% → 43% → 92% → 99.75% 的提升路径：

| Commit | Recall | 关键改动 |
|--------|--------|---------|
| Phase 1 初版 | 5% | PQ 距离用于一切 |
| 精确 L2 替代 PQ | 43% | 展开节点用精确距离 |
| PQ 候选 + 精确 L2 结果 | ~95% | 候选集用 PQ 排序，最终结果用精确 L2 |
| 多入口点搜索 | 92% → 99.75% | num_probes 解决单入口覆盖不足 |

每一步都是先让算法正确，验证 Recall 达标，然后才做性能优化。尝试在 Recall 不够的时候做性能优化是完全浪费的。

### 5.4 勇于 Revert

PQ scheduler 是一个典型案例：

```
1bd1f6c aisaq: PQ scheduler Phase 1——batch sender 异步调度 process_node
4f248fa Revert "aisaq: PQ scheduler Phase 1——batch sender 异步调度 process_node"
443020a aisaq: §9.5 PQ 合并 scheduler 标记放弃，记录评估结论
```

实现了、测量了、发现收益不足、revert了、在需求文档中记录了放弃的原因。整个过程只花了一个 session 的一小部分。**让 Claude 快速实现一个方案用来验证可行性，验证失败就果断放弃**——这是 Claude Code 最大的价值之一。

### 5.5 环状猜疑链：性能调优中最危险的陷阱

3月14日晚到3月16日，调试单核 QPS 时陷入了一个经典的**环状猜疑链**。时间线如下：

**Phase 2 Sprint 1 起步（3/14 21:00）**：SIFT1M 格式转换完成，首次跑通搜索。单核 QPS 只有 397——而 aisaq-diskann 原版单线程是 3,604。差了将近 10 倍。

Claude 开始了一轮又一轮的"优化"尝试，每一轮都把问题归咎于不同的层面，形成了一个闭环：

```
  ┌─→ "SIMD 优化不够，PQ 距离计算太慢"
  │       ↓ 加了 AVX2，提升 7%，还差得远
  │   "不是 SIMD 的问题，是框架 pipeline 带来了损耗"
  │       ↓ 分析 op_pusher 占 8%，不是主因
  │   "不是框架问题，是 NVMe 读取不是顺序写，随机读太慢"
  │       ↓ 试了页面合并读取，没有显著提升
  │   "不是 IO 问题，是 scheduler 空轮询消耗 CPU"
  │       ↓ 移除 task_scheduler，消除 25%，但还不够
  └── "那肯定还是 PQ 计算本身太慢，SIMD 还不够……"  ←─┘
```

**真正的根因不在环上**。实际上有三个独立的根因，它们都不是"计算/框架/IO/轮询"这个环上的问题：

| 时间 | 真正根因 | 表现 | 修复 |
|------|---------|------|------|
| 3/14 21:27 | `assert(q.dequeue(r))` 副作用 | Release 下 dequeue 不执行，请求丢失 | 提取 assert 外 |
| 3/14 21:49 | local queue 溢出 | 并发>128 时请求静默丢失，pipeline 挂死 | bounded try_dequeue |
| 3/14 22:38 | **`-O0` 覆盖 `-O3`** | 所有"Release"构建实际以零优化运行 | 移除 add_compile_options(-O0) |

特别是第三个——**编译 flags 错误**。`CMakeLists.txt` 中 `add_compile_options(-O0)` 出现在 cmake per-config flags 之后，静默覆盖了 Release 的 `-O3`。这意味着所有的性能分析、所有的"SIMD 不够快"、"框架开销太大"的结论全部基于**零优化的二进制**。修复这一行之后，QPS 从 ~400 直接跳到 ~3,800。

```
3/14 22:38  fix(build): 移除add_compile_options(-O0) → QPS 397 → ~3,800（9.5x）
3/14 23:01  concurrent(64) 并发查询              → QPS ~3,800 → 3,882
3/15 08:57  移除 task_scheduler 空轮询            → 小幅提升
3/15 13:57  AVX2 SIMD                            → QPS → 4,030
```

**这个故事的教训不是"要检查编译 flags"（虽然确实要检查），而是一个更深层的认知问题：**

**Claude（以及人类）在面对"性能不达预期"时，倾向于在熟悉的技术维度上兜圈子**。SIMD、框架开销、IO 模式、CPU 轮询——这些都是 Claude 知识范围内的优化方向，所以它会在这些方向上反复尝试。但真正的 root cause 是一个环境配置问题，在 Claude 的思维模型之外。

#### 如何打破环状猜疑链

**规则 1：先做 10x 量级的排除**。如果预期 3,000 QPS 但实测 400，差距是 ~10x。SIMD 优化通常带来 1.2-2x，框架开销通常 <10%，IO 优化通常 1.5-3x。**没有任何单一的代码级优化能解释 10x 差距**。此时应该立刻怀疑环境问题（编译、配置、硬件），而不是在代码层面找原因。

```
if (实测/预期 < 0.3):
    检查环境（编译flags、残留进程、硬件配置）
elif (实测/预期 在 0.5-0.8):
    profile 找热点
else:
    可能已接近理论值
```

**规则 2：每轮猜测必须有可证伪的预测**。"SIMD 不够"→"加了 AVX2 应该提升 30%"——如果实测只有 7%，说明假设错误，应该**立刻放弃这个方向**而不是继续在 SIMD 上深挖。Claude 的问题是：看到 7% 提升后，不是放弃 SIMD 假设，而是转向下一个猜测，然后在几轮之后又回到 SIMD。

**规则 3：超过 3 轮没有突破就暂停，回到最基础的检查**。如果连续尝试了 3 个不同方向的优化（SIMD / 框架 / IO / 轮询）都没有带来显著提升，大概率根因不在你正在看的地方。此时应该：
1. `ninja -t commands <target>` 检查实际编译命令
2. `ps aux | grep` 检查残留进程
3. 跑一个最简 benchmark（如空循环）验证基础性能
4. 对比 Debug vs Release 的实际行为差异

**规则 4：建立性能基准的信封计算**。在优化之前先做一次理论天花板估算（后来在 benchmark_single_core.md 中做了这个工作）：
- 每查询 ~114 节点 × 30μs/node = 3.4ms CPU → 串行上限 294 QPS
- concurrent(40) 重叠 → 理论 ~1,760 QPS

有了这个信封计算，397 QPS（串行模式）vs 294 QPS 理论串行上限，就能立刻看出：串行模式下 QPS 和理论值相近（考虑 IO 等待），问题不在计算速度——在并发模式下才需要提升。而如果连串行模式下的理论值都达不到，才需要去找代码级瓶颈。

### 5.6 框架优化来自应用驱动

AiSAQ 开发过程中发现并修复了多个框架问题，这些都不是"为了优化而优化"，而是应用 profile 驱动的：

| 应用发现的问题  | 框架修复                            |
|----------------|-------------------------------------|
| scope 频繁 malloc/free | scope slab 池化（per-thread free list） |
| context shared_ptr 原子操作 | pushed_context 裸指针化        |
| submit 重复 connect() | 消除双倍 op_tuple 构建            |
| per_core::queue 同核心开销 | local fast path（零原子操作）   |
| assert 副作用   | assert 外提取有副作用表达式          |

**最佳实践**：用真实应用的 perf profile 驱动框架优化，而不是臆测瓶颈。

---

## 六、Claude Code 特有的工作流建议

### 6.1 `ai_context/` 目录：给 Claude 看的技术文档

项目中维护了 12 个 `ai_context/` 文件（benchmark 报告、设计文档、瓶颈分析等）。这些文件**不是给人看的文档**，而是**给 Claude 看的上下文**。它们的特点：
- 高度结构化（表格、代码块、清晰的标题层级）
- 包含定量数据（QPS、Recall、时间戳）
- 记录决策过程而不仅是结果

### 6.2 Commit Message 即文档

每个 commit message 都包含了变更的核心语义：

```
aisaq: 展开节点用精确L2距离替代PQ距离，recall@10从5%提升到43%
aisaq: concurrent(64)并发查询，QPS超越aisaq-diskann
aisaq: 多盘条带化 + 去除 preemptive bounce 性能退化
```

这让 Claude 在新会话中通过 `git log` 就能快速理解项目演进历史。

### 6.3 利用 Claude 的并行能力

Claude Code 可以同时发起多个 agent 做并行研究。在 AiSAQ 开发中，这在以下场景很有价值：
- 同时读取多个参考实现文件（DiskANN 源码 + PUMP KV 源码）
- 同时搜索多个代码模式
- 构建阶段同时研究 GPU PQ 训练和 CPU Vamana 图构建

### 6.4 让 Claude 做重复性工作

Claude 最擅长的场景：
- **样板代码**：scheduler 的六组件模板（op、sender、req、scheduler、op_pusher 特化、compute_sender_type 特化）每次都是相同模式
- **格式转换工具**：`convert_index.cc`（296 行）几乎是纯机械翻译
- **配置解析**：JSON 配置 → C++ struct 的映射代码
- **SIMD 优化**：AVX2 intrinsics 的编写（PQ 距离计算）

Claude 最不擅长的场景：
- **架构决策**：选哪些 scheduler、怎么分片、什么时候需要 Leader/Follower
- **性能诊断**：看 perf 火焰图找瓶颈（虽然它能分析你给它看的 profile 数据）
- **硬件交互**：SPDK 配置、NVMe 设备管理、核心绑定

---

## 七、总结

用 Claude Code 在 7 天内完成一个 ~5000 行的高性能 NVMe 向量搜索引擎，核心经验：

1. **CLAUDE.md 是 10x 效率放大器**。花一天写好 1000 行 spec，比每次会话花 10 分钟解释 API 用法高效 100 倍。
2. **Memory 系统把痛苦变成资产**。每个浪费时间的 bug 都应该变成一条 feedback memory，确保不再重蹈覆辙。
3. **人做判断，Claude 做实现**。架构、算法选择、性能诊断是人的工作；模板代码、格式转换、优化尝试是 Claude 的工作。
4. **用数据驱动而非直觉**。每次改动后立即跑 benchmark，用数字验证效果。Claude 很擅长"试一试"，人类负责判断"值不值得"。
5. **环境 > 代码**。在深入分析代码之前先检查编译 flags、残留进程、硬件配置。这个教训在 memory 中记录了两次。
6. **勇于让 Claude 快速原型 + 快速放弃**。PQ scheduler 花了一个小时实现和验证，然后 revert。这比花两天"先在纸上论证可行性"高效得多。
