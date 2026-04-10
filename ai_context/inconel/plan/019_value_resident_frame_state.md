# 019 — Value Resident Frame State Alignment

> 实现第十九步。完成 `INC-036` 在 value owner 侧剩下的那半：把当前 `page_data + writable_pages_` 这套“把 resident partial page、open page、可分配页混成一个队列”的工程化简化，拆成显式 `value_page_frame` 与 resident state machine。

## 本 step 覆盖的 issue

| Issue | 说明 |
|---|---|
| `INC-036`（phase B） | value owner 缺 `value_page_frame` / `clean_allocatable` / `dirty_append` / `dirty_hole_fill` / `writeback_inflight` 的显式状态机 |

## 文件结构

```text
memory/
└── frame.hh                          — 增加 `value_page_frame` 及 value-only 字段

value/
└── scheduler.hh                      — resident page 状态从 `writable_pages_` 收敛到显式 frame state

test/
└── apps/inconel/test/test_value.cc   — 扩 resident/open-frame 状态转换与 read-path 命中测试
```

## 设计目标

1. 让 value owner 的 resident page 生命周期一眼可见，而不是继续靠 `page_data.free_mask + writable_pages_` 猜当前状态。
2. 把“正在填充的页”和“已 durable 但仍可继续分配的 resident 页”分开表示，为 future reclaim/recovery 接口留出稳定状态边界。
3. 不越界去做 `INC-018/019/020`：本 step 只整理 resident/open-frame 状态，不引入 non-resident hole metadata 和 recovery 安装接口。

## 设计决策

| # | 决策点 | 结果 | 说明 |
|---|---|---|---|
| `D1` | 本 step 的 scope | **只处理 resident state** | `hole_page_list` / `freed_slots` / `recycle_whole` 仍留给后续 value reclaim 系列 |
| `D2` | owner 结构 | **`open_frames[class_idx]` + `allocatable_frames[class_idx]`** | 把“当前写目标”与“可再次打开的 durable resident 页”分开 |
| `D3` | cache 关系 | **`clean_allocatable` 不进 readonly cache** | 它是 placement 资源，不是普通 read cache entry |
| `D4` | 写回完成后的状态 | **`free_count == 0 → clean_readonly`；`free_count > 0 → clean_allocatable`** | 让下一轮分配和读路径都能按状态分流 |

## 详细设计

### `memory/frame.hh`

在 step 018 的 `page_frame` 基础上新增：

```cpp
struct value_page_frame : page_frame {
    uint16_t class_idx;
    uint16_t slots_per_page;
    uint64_t free_mask;
    uint16_t free_count;

    enum class open_mode : uint8_t {
        none,
        append,
        hole_fill,
    } mode;
};
```

这里先用当前实现已经拥有的 `free_mask`/`free_count` 粒度，不强行引入更重的 bitmap 实现。

### `value/scheduler.hh` owner 状态

当前：

```cpp
std::vector<std::vector<page_data>> writable_pages_;
```

改成显式 resident state：

```cpp
std::vector<value_page_frame*> open_frames_;                 // 每 class 0/1 个
std::vector<std::vector<value_page_frame*>> allocatable_frames_;
```

语义：

1. `open_frames_[ci]`
   - `dirty_append` 或 `dirty_hole_fill`
   - 当前 round 继续往里填的唯一 active frame
2. `allocatable_frames_[ci]`
   - `clean_allocatable`
   - 已 durable、resident、仍有 free slot，但当前没有被 writer 打开

### 分配路径

`alloc_page(class_idx)` 的 resident 优先级收敛成：

1. `open_frames_[ci]` 且还有 free slot
2. `allocatable_frames_[ci]` 的 hottest frame，取出并转成：
   - fresh/顺写 continuation：`dirty_append`
   - hole reuse reopen：`dirty_hole_fill`
3. 若两者都没有，再走当前 allocator 的 fresh bump / whole-page 分配

这样以后 read path 与 write path 都不再扫同一个“混合队列”。

### finalize / writeback completion

当前 round 的 `round_page` 也改成持有 `value_page_frame*`，不再复制一套平行字段。

写回完成后：

1. `free_count == 0`
   - `st = clean_readonly`
   - `mode = none`
   - 进入 readonly cache（仅 1-LBA admit 规则保持不变）
2. `free_count > 0`
   - `st = clean_allocatable`
   - `mode = none`
   - 进入 `allocatable_frames_[ci]`
   - 不进入 readonly cache

rollback 时：

- fresh bump / whole page 仍按现有 allocator 路径归还
- 对于来自 resident frame reopen 的页，只把状态退回原先的 `clean_allocatable` 并放回 `allocatable_frames_[ci]`

### 读路径

`read_value()` 的 resident 命中顺序变成：

1. `open_frames_[ci]`（dirty frame，直接读）
2. `allocatable_frames_[ci]`（clean_allocatable，直接读）
3. readonly cache（clean_readonly）
4. NVMe miss → fill

这样 read path 不再把 `writable_pages_` 当成“既是 open page，又是 clean resident page”的混合结构。

## 明确不做的内容

本 step 不做：

- non-resident `hole_page_list`
- `freed_slots`
- `recycle_whole`
- `install_recovered_state`

也就是说，这一步只是把 resident/open state 收敛清楚；真正的 reclaim/recovery source 仍在后续 `INC-018/019/020` 系列。

## 实施顺序

1. `memory/frame.hh` 增加 `value_page_frame`。
2. `value/scheduler.hh` 删除 `page_data` / `writable_pages_` 的主路径地位。
3. 引入 `open_frames_` / `allocatable_frames_`。
4. `handle_persist` / `handle_finalize` / rollback / `handle_read` 同步迁移。
5. 扩 `test_value.cc` 覆盖 resident state transition。

## 验证

至少回归：

- `inconel_test_value`
- `inconel_test_runtime`

`test_value.cc` 需要新增/更新的重点：

- partially-free page 在 writeback completion 后进入 `clean_allocatable`
- 下一轮 persist 会优先 reopen resident allocatable frame，而不是重新 fresh bump
- read path 能命中 `open_frames_` 和 `allocatable_frames_`
- 写满后才进入 readonly cache，partial page 不会误入 readonly cache
