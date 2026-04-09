# 012 — Bounded Advance Fairness

> 实现第十二步。把 tree/value 两侧 scheduler 的 `advance()` 从“单次吃空整条 queue”改成“每 queue 每轮只处理有限预算”，让单次 advance 的工作量可控，避免一个热队列把其它队列长期饿死。

## 本 step 覆盖的 issue

| Issue | 说明 |
|---|---|
| `INC-029` | tree + value 的 `advance()` 用 `.drain()` 吃空 queue，单轮延迟不可控，缺公平性 |

## 文件结构

```text
tree/
└── scheduler.hh                      — cache_queue / lookup_queue 改成 bounded processing

value/
└── scheduler.hh                      — finalize / persist / read / fill 四条队列改成 bounded processing
```

## 设计目标

1. 限制单次 `advance()` 的最长工作量，避免 tail latency 随队列深度线性增长。
2. 让多个 queue 间都有机会获得服务，而不是“谁先热谁一直吃满 CPU”。
3. 不改变现有 sender API 和 scheduler 外部可见语义。

## 设计决策

| # | 决策点 | 结果 | 说明 |
|---|---|---|---|
| `D1` | 预算形态 | **私有常量预算，不新增 runtime 配置** | 先把公平性机制立住，不把 tuning 面扩散到配置层 |
| `D2` | value 队列顺序 | **仍保持 finalize → persist → read → fill** | 顺序有现成语义依赖，不在本步改 |
| `D3` | tree 队列顺序 | **仍保持 cache completion → lookup request** | 先让已完成 read 回填 cache，再推进 lookup |
| `D4` | persist 预算单位 | **按 leader round 计数** | 单个 round 内 follower 数由 step 010 控制，本步不重复细分 |

## 详细设计

### 预算常量

两个 scheduler 都用私有常量，不上升到 runtime/build options。

建议形态：

```cpp
static constexpr uint32_t kMaxCacheOpsPerAdvance = 64;
static constexpr uint32_t kMaxLookupOpsPerAdvance = 64;
```

```cpp
static constexpr uint32_t kMaxFinalizePerAdvance = 64;
static constexpr uint32_t kMaxPersistRoundsPerAdvance = 32;
static constexpr uint32_t kMaxReadPerAdvance = 64;
static constexpr uint32_t kMaxFillPerAdvance = 64;
```

这里不要求树和值完全共用同一个数字；value persist 比单次 read / fill 更重，预算可以更小。

### `tree/scheduler.hh`

当前：

```cpp
cache_queue_.drain(...)
lookup_queue_.drain(...)
```

改为：

```cpp
for (uint32_t i = 0; i < kMaxCacheOpsPerAdvance; ++i) {
    auto item = cache_queue_.try_dequeue();
    if (!item) break;
    ...
}

for (uint32_t i = 0; i < kMaxLookupOpsPerAdvance; ++i) {
    auto item = lookup_queue_.try_dequeue();
    if (!item) break;
    ...
}
```

语义保持：

- 仍然先处理 cache completion
- 仍然在 completion 后唤醒 lookup waiter
- `progress` 只要任一队列处理了至少一个 item 就返回 `true`

### `value/scheduler.hh`

当前：

- `finalize_q_.drain(...)`
- persist 用 `while (try_dequeue())`
- `read_q_.drain(...)`
- `fill_q_.drain(...)`

改为 4 段 bounded loop：

1. finalize：最多 `kMaxFinalizePerAdvance`
2. persist：最多 `kMaxPersistRoundsPerAdvance`
3. read：最多 `kMaxReadPerAdvance`
4. fill：最多 `kMaxFillPerAdvance`

persist 特别说明：

- 每次 `handle_persist(leader)` 仍然会在内部合并一批 follower
- 但 `advance()` 外层只把它算作“处理了 1 个 leader round”
- 剩余 leader 继续留在 `persist_q_`，等待下一轮 `advance()`

### progress 语义

本步不改 `advance()` 的返回契约：

- 处理了任何一个 queue item → `true`
- 本轮四个/两个 queue 都没拿到 item → `false`

也不引入“预算耗尽但队列未空”的额外状态。

## 实施顺序

1. `tree/scheduler.hh` 改 bounded cache / lookup loop。
2. `value/scheduler.hh` 改 bounded finalize / persist / read / fill loop。
3. 核对 `progress` 聚合语义没有回归。

## 验证

实现本 step 时至少回归：

- `inconel_test_tree_lookup`
- `inconel_test_tree_lookup_multicore`
- `inconel_test_value`
- `inconel_test_tree_value`
- `inconel_test_runtime`

重点观察：

- 正常功能不变
- 多队列同时有活时不会再被某一条 queue 长时间独占
- bounded loop 不会漏处理 item 或让进度判定失真
