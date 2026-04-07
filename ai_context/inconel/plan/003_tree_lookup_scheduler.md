# 003 — Tree Lookup Scheduler

> 实现第三步。基于 002 的 tree page 格式，实现 batch tree lookup scheduler + pipeline。

## 文件结构

```
tree/
├── lookup.hh        — lookup 结果类型 + scheduler 决策类型
├── page_builder.hh  — leaf / internal 页构建（002 已实现）
├── page_reader.hh   — 页解析 / 搜索（002 已实现）
├── scheduler.hh     — lookup_scheduler（PUMP 6 组件模式）
└── sender.hh        — 对外唯一接口：lookup() pipeline
```

## tree/lookup.hh — 结果与决策类型

lookup 结果（variant）：
- `lookup_value { data_ver, value_ref }` — 找到值
- `lookup_tombstone { data_ver }` — 找到墓碑
- `lookup_absent {}` — 不存在

scheduler 每轮决策（variant）：
- `decision_done {}` — 所有 key 已解析完毕
- `decision_need_read { read_descs[], page_map[] }` — 需要读取 NVMe 页
- `decision_need_cache {}` — 预留（等待 cache 回填）

## tree/scheduler.hh — lookup_scheduler

**设计原则**：scheduler 只做 CPU 工作（查 cache、遍历 tree、准备 read descriptors），不持有 NVMe 依赖。NVMe 读取由 sender.hh 的 pipeline 层完成。

**PUMP 6 组件**：

1. **_tree_lookup::req** — `lookup_state*` + `cb(batch_decision&&)` + 侵入式 waiter 链表指针
2. **_tree_lookup::op** — `tree_lookup_op` 类型标记 + `start<pos>()` 构造 req
3. **_tree_lookup::sender** — `lookup_scheduler*` + `lookup_state*`
4. **_cache_pages::req/op/sender** — 同上模式，用于页 cache 回填，输出 `bool`
5. **op_pusher 特化** — `requires tree_lookup_op` / `requires cache_pages_op`
6. **compute_sender_type 特化** — process 输出 `batch_decision`，submit_cache 输出 `bool`

**op::start() 延迟定义**：op/sender 中引用 `lookup_scheduler*`（前置声明），`start()` 方法体定义在 `lookup_scheduler` 完整定义之后，避免不完整类型错误。

**Scheduler 内部状态**：

| 状态 | 类型 | 说明 |
|------|------|------|
| `page_cache_` | `unordered_map<paddr, const char*>` | 已就绪页 |
| `loading_pages_` | `unordered_set<paddr>` | 正在读取的页（去重） |
| `cache_bufs_` | `vector<unique_ptr<char[]>>` | 页 buffer 所有权 |
| `waiters_head_` | `_tree_lookup::req*` | 侵入式等待链表头 |

**handle 逻辑**（`advance()` 中执行）：

1. 先 drain cache_queue — 将读完的页放入 `page_cache_`，唤醒可推进的 waiters
2. 再 drain lookup_queue — 对每个 req 调用 `handle()`：
   - `process_entries()` — 沿 cache 遍历 tree（internal → find_child → resolve → 继续，leaf → find → 结果）
   - 全部解析完 → `cb(decision_done{})`
   - 有未缓存页 → `prepare_reads()` 收集去重后的 read descriptors → `cb(decision_need_read{})`
   - 需要的页全在 loading 中 → 挂 waiter 链表等待

**Waiter 模式**：当 req 需要的所有未解析页都在 `loading_pages_` 中（被其他 pipeline 发起的读覆盖），该 req 不重复发读，而是挂到 `waiters_head_` 链表。cache 回填后逐个检查 waiter 是否可推进，可推进者重新入 lookup_queue。

**Loading 去重**：`prepare_reads()` 对每个未解析 entry 的 `next_page`，只在 `page_cache_` 和 `loading_pages_` 都不存在时才分配 buffer 并加入 `read_descs`。

## tree/sender.hh — lookup pipeline（对外唯一接口）

```
lookup(tree_sched, nvme_sched, keys, manifest)
```

Pipeline 结构：

```
with_context(lookup_state)(
    loop:
        for_each(check_not_done 协程)
        >> tree_sched->process(state)           // → batch_decision
        >> visit()                              // → variant 展开
        >> flat_map:
            decision_need_read → on_decision_need_read()
            decision_done     → just(true)
        >> all()
    >> 收集 results
)
```

`on_decision_need_read(nvme_sched, tree_sched, dec)`：
```
with_context(dec)(
    loop(n) >> concurrent()
        >> nvme_sched->read(...)                // 并发读页
        >> all()
        >> tree_sched->submit_cache(page_map)   // 回填 cache
)
```

**职责分离**：`nvme_sched_t` 只出现在 sender.hh 的模板参数中，scheduler.hh 完全不知道 NVMe 的存在。

## lookup_state（lives on PUMP context stack）

```cpp
struct lookup_state {
    struct entry {
        string_view key;
        bool resolved;
        lookup_result result;
        paddr next_page;        // 当前下降到的节点地址
    };
    vector<entry> entries;
    bool all_done;
    bool first_call;
    uint32_t page_size, page_lbas;
    const tree_manifest* manifest;
};
```

初始化：所有 entry 的 `next_page` 指向 root。空树直接全部标记 resolved + absent。

## 验证

- 单核 3 层树（root → 4 internal → 8 leaf，400 keys）：
  - 全命中、全 miss、混合、单 key、空树 — 通过
- 多核并发（core 0 发起，core 2/4 各跑一套 scheduler）：
  - 400 个独立 pipeline 并发 in-flight — 通过
  - 100 个 miss key 并发 — 通过
