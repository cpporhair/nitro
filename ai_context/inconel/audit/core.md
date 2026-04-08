# Core 模块 Phase 0 审计

> Spec-only audit。**未读** test 文件 / plan 文件。

## 1. 范围

### 1.1 已读源码

| 文件 | 行数 | 备注 |
|------|------|------|
| `core/page_cache.hh` | 56 | 本轮新读 |
| `core/clock_cache.hh` | 148 | 本轮新读 |
| `core/slru_cache.hh` | 279 | 本轮新读 |
| `core/registry.hh` | 163 | tree audit 已读 |
| `core/tree_manifest.hh` | 45 | tree audit 已读 |

总 ~700 行。

### 1.2 已读 spec

| 文档 | 章节 |
|------|------|
| `runtime_memory_and_cache.md` | §6 Cache/Pool 视图、§7 Owner / Sharding、§8 Placement 耦合（lines 500-790，value audit 已读） |

### 1.3 显式未读

| 文件 | 原因 |
|------|------|
| `apps/inconel/test/test_*.cc` | 规则禁止 |
| `ai_context/inconel/plan/*.md` | 审计纪律 |

---

## 2. 已被现有 INC 覆盖的部分

| 文件 / 现象 | 已追踪 |
|---|---|
| `tree_manifest::resolve` 用 assert，Release 退化 UB | INC-005 |
| `tree_manifest::has_root` 用 `lba == 0` 当 sentinel | INC-011 |
| `tree_manifest` 模块归属 core/ vs tree/ 不明 | INC-007 |
| `clock_cache` / `slru_cache` 内部 std::unordered_*、std::vector | INC-009/010 |
| `cache_concept` 的 raw `char*` + caller-must-free 模型 (use site workaround) | INC-016 |
| `registry::value_sched()` assert(value_alloc_sched) 在 Release 退化为 nullptr → 段错误 | INC-034 (silent disable + delayed assert 是 R2 的子症状) |

`registry.hh` 顶部的 future scheduler placeholders（coord/tree/wal/front 等）已显式标注 "not implemented yet"，结构 OK，本轮无新 finding。

---

## 3. Critical Findings

### C1 — `cache_concept` 模型化 buffer cache 而非 spec 的 frame cache

**Spec**：RMC §6.1 `readonly_frame_cache` 期望的概念结构：

```cpp
struct readonly_frame_cache {
    // key = frame_id, value = cache-owned raw page_frame*
    // 消费者通过 frame_pin 保活
    lru_or_clock<frame_id, page_frame*> entries;
};
```

约束：

1. 只收纳 `clean_readonly` frame（state 区分）
2. `pin_count > 0` 时不可驱逐（pin support）
3. 驱逐后只丢 page image，不改 allocator/reclaim/correctness 状态

**现状**：`page_cache.hh:42-49` 的 `cache_concept`：

```cpp
template <typename C>
concept cache_concept = requires(C c, const C cc, paddr k, char* b) {
    { c.get(k) }       -> std::same_as<const char*>;
    { c.put(k, b) }    -> std::same_as<std::optional<evicted_entry>>;
    { cc.contains(k) } -> std::same_as<bool>;
    { cc.size() }      -> std::same_as<uint32_t>;
    { cc.capacity() }  -> std::same_as<uint32_t>;
    { c.evict_one() }  -> std::same_as<std::optional<evicted_entry>>;
};
```

差异表：

| Spec 期望 | 当前 | 影响 |
|---|---|---|
| key = `frame_id`（含 domain：tree_node/value_page/etc.） | key = `paddr` | v1 OK，每实例只服务一个 domain |
| value = `page_frame*`（含 pin_count + state） | value = raw `char*` | **缺 pin + 缺 state** |
| `pin_count > 0` 不可驱逐 | 无 pin 概念 | 当前 use site 同步消费，OK |
| 驱逐时只丢 image，allocator 状态不动 | 缓存不持有 buffer 所有权，caller 必须 free | INC-016 已追踪 |
| dirty / clean 状态分离 | 只有"是否在 cache 内" | dirty open frame 完全绕过 cache（value 自己用 `writable_pages_`）|

**为什么 v1 跑得通**：当前两个 use site 都是只读：

- `tree::lookup_scheduler` 缓存 tree node page，纯读，pin 由 caller-side coroutine 同步控制
- `value::scheduler::readonly_cache_` 缓存 clean value page，只服务读路径

dirty / writeback 状态由各 owner 自己用 ad-hoc 数据结构管（value 用 `writable_pages_`，tree flush 写侧未实现）。

**Tier 2 — concept drift 扩散风险**：

1. INC-016 已追踪 buffer ownership 这一层，但**没追踪 frame 抽象（pin + state）这一层**——cache_concept 重写其实有两层缺口
2. AI 在 v2 加新 cache（e.g. value_page domain hot tier、tree_node NUMA-local shard）会照 cache_concept 抄，把"无 pin / 无 state / 无 domain"模式扩散到新代码
3. 等 frame management 真落地（pin_count + state machine）时，cache_concept 必须做第二次大改

**处置**：扩 INC-016 范围，从"buffer ownership 模型"扩到"buffer ownership + frame 抽象（pin_count + state）"。明确两层都要重做，不能只修 ownership 那层就以为完事。

---

### C2 — `slru_cache::evict_one` 不重置 `in_protected`，留下 latent stale state

**位置**：`slru_cache.hh:172-181` (`evict_one` 的 protected 分支) + `slru_cache.hh:198-203` (`free_node`)

```cpp
// evict_one 的 protected 分支
if (prot_tail_ != NIL) {
    uint32_t idx = prot_tail_;
    auto& n = nodes_[idx];
    evicted_entry victim{ n.key, n.buf };
    unlink_protected(idx);   // 不重置 n.in_protected
    index_.erase(n.key);
    free_node(idx);          // 不重置 n.in_protected
    return victim;
}

void
free_node(uint32_t idx) {
    nodes_[idx].occupied = false;
    nodes_[idx].next = free_head_;
    free_head_ = idx;
    // 不重置 in_protected
}
```

**触发链**：

1. node k1 经过 hit 被 promote 到 protected → `in_protected = true`
2. cache 在某轮 drain 中通过 evict_one 拉走 k1（probation 已空 → 走 protected 分支）
3. 同一 cache 实例继续被复用（put k2）→ `alloc_node` 拿到这个 stale 节点
4. `link_probation_head` 把 node 链入 probation 列表，但 `in_protected` 仍为 true
5. `get(k2)` 走 `if (n.in_protected)` 分支 → `move_to_protected_head` → `unlink_protected` 操作的是 *probation* 列表里的节点 → 破坏 protected 列表指针 + 错误递减 `prot_size_`

**当前不可达**：`page_cache.hh:37-39` 注释明确 `evict_one` 是"teardown drain"——析构时调用，析构后不会再 put。

**latent 风险**：

1. 任何把 evict_one 用作 runtime 内存压力响应的修改 → 立即 silent corruption
2. 任何在 `put()` 中加 protected eviction 路径（spec 没要求但合理）→ 立即触发
3. AI 给新 cache 实现照抄 slru_cache 的 evict_one 模式 → 把同样的不完全状态重置抄走

**Tier 2 — latent corruption + AI pattern propagation**：当前测试不到，未来改 cache 的人不会怀疑 evict_one。

**修复方向**：`free_node` 加 `nodes_[idx].in_protected = false;`，统一覆盖所有 free path。同时 `alloc_node` 加 `assert(!nodes_[idx].in_protected)` 让不变量显式化。

---

### C3 — `cache_concept::evict_one` 含义跨实现漂移

**位置**：

- `clock_cache.hh:127-140`：`evict_one` 用 `index_.begin()`（unordered_map 迭代器序，**非确定性**，**不按 clock policy**）
- `slru_cache.hh:161-182`：`evict_one` 先 probation tail 后 protected tail（接近 LRU policy）

**page_cache.hh:37-39 注释**：

> `evict_one` is the teardown drain: pulls one entry out and returns its (key, buf), letting the caller free every admitted buf via a loop.

但是名字 `evict_one` 暗示"按策略 evict 一个"。后果：

1. 如果未来某段代码以为 evict_one 按策略选 victim（e.g. 内存压力下要 evict 最冷的），会发现 clock_cache 选的不是最冷的（甚至可能反复选到刚被 hit 的）
2. AI 给新 cache 实现 evict_one 时，看 slru_cache 会以为"按 LRU 序"，看 clock_cache 会以为"任意一个"，cache_concept 没规范，两边各自漂

**Tier 2 weak — naming drift + concept under-specification**

**修复方向**：

A. 重命名 `evict_one` → `drain_one` 或 `teardown_pop_any`，文档明确"非 policy eviction"
B. 拆 concept → `evict_one`（按 policy）+ `drain_one`（teardown）两个 method，clock_cache.evict_one 走真正 clock sweep
C. 不动名字，page_cache.hh 文档加更强警告

---

## 4. 其他 Findings (Tier 3)

### C4 — `clock_cache::slot::occupied` / `slru_cache::node::occupied` 死字段

`occupied` 在两个 cache 里都是写入但从不读取：

- `clock_cache.hh:36`（声明）+ 行 96（写 true）+ 行 135（写 false）—— grep 找不到读
- `slru_cache.hh:41`（声明）+ 行 150（写 true）+ 行 200（写 false）—— grep 找不到读

clock_cache 中 `idx < size_` 已经隐含 occupied 状态（slots 顺序填）。slru_cache 中 free list 隶属已经隐含。

**处置**：不建条目，局部修复时直接删字段。

### C5 — `page_cache.hh` 文件名误导

文件叫 `page_cache.hh` 但只定义 `cache_concept`（不是 page cache 实现）。AI grep "page_cache" 找类型时会到这文件，找不到对应 class / 实现，要再次 grep。

文件实际作用 = "cache concept 定义 + clock/slru implementations 的 static_assert binding"。

**处置**：不建条目（rename / 文件重组时一起做）。

### C6 — `evicted_entry` 类型错位住在 `clock_cache.hh`

`evicted_entry`（`paddr key; char* buf;`）是 cache_concept 的输出类型，逻辑上属于 concept 层。当前住在 `clock_cache.hh:17-20`，导致 `slru_cache.hh:12` 必须 include `clock_cache.hh` "for evicted_entry"，slru_cache 因此对 clock_cache 有不必要的 build 依赖。

**处置**：不建条目（C5 一起处理 / 移动到 `page_cache.hh`）。

### C7 — `clock_cache::put` 清扫循环 `for(;;)` 无显式终止保证

逻辑上 ≤2N 迭代必终止（一遍清 ref，一遍 evict），但代码层面是 `for(;;)`。如果 cache 内部 invariant 被破坏（罕见），会 wedge 在 advance 里。

**处置**：不建条目（极低概率，且加 termination assertion 是 ≤3 行的事）。

### C8 — `cache_concept::size()` / `capacity()` 当前 use site 不调

dead concept members？还是为未来 metrics 预留？无法判断。如果未来确实不需要，可以从 concept 删掉减小契约面。

**处置**：不建条目（待后续 use site 长出来再决定）。

---

## 5. 跟现有 INC 的关系

| Finding | 处置 |
|---|---|
| C1 (cache_concept 缺 pin / state 抽象，跟 spec frame cache 不符) | **新 INC-036** (urgent) — 按 RMC §6.1 + §11 重做 cache_concept；INC-016 (DMA buf ownership) 是正交两层，不合并 |
| C2 (slru_cache evict_one stale in_protected) | **新 INC-037** (urgent) |
| C3 (evict_one 跨实现语义漂移) | **新 INC-038** (normal) — rename → drain_one，跟 INC-036 重做 cache_concept 时一起做 |
| C4-C8 | 不建条目 |
| tree_manifest.hh / registry.hh 部分 | 已被 INC-005 / INC-007 / INC-011 / INC-034 覆盖，无新条目 |

预计 2 条新 INC + 1 条 INC 扩展。

---

## 6. 跟前 4 个 audit 的对照

core/ 比 tree/value/format/runtime 状况**好**的地方：

- 两个 cache 实现（clock + slru）都做了 fail-fast 构造校验（`capacity == 0` 抛 invalid_argument，slru 校验 prot_cap/prob_cap 都 >= 1）。这是**正面例子**，跟 value::handle_finalize 的 fprintf+abort 一样是已经做对的 panic-on-init-error 模式
- `cache_concept` 的 ownership 注释（`page_cache.hh:23-39`）写得**非常详细**——明确说明"caller must free"、"为什么 same-key 不能 silent overwrite"、"evict_one 是 teardown drain"。这部分文档化做得比 tree/value 都好

core/ 比其他模块**差**的地方：

- 概念层面 cache_concept 跟 spec frame cache 有抽象差距（C1）——这是模块设计上的选择，不是单纯的 bug，但文档没声明这是简化版
- slru_cache 有真 latent bug（C2），且当前测不到——这是 tree/value 都没有的特殊形态（其他模块的 bug 都是当前可观察的）

**最重要的发现**：C1 + C2 都是 "AI 抄出去会扩散" 类型。INC-016 当前只追踪 ownership 那层，没追踪 frame 抽象那层——必须扩。

---

## 7. 审计纪律

| 项目 | 状态 |
|------|------|
| 打开过 test 文件 | 否 |
| 打开过 plan 文件 | 否 |
| 想读测试的冲动 | 0 次 |
| spec 章节作为信任源 | 是 (RMC §6-7) |
| 每条 finding 标 Tier | 是 |
| 每条 finding 标 spec 依据或 rationale | 是 |
