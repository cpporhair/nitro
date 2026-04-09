# 010 — Value Persist Cleanup

> 实现第十步。把当前 `value::scheduler::handle_persist` 这条写路径收敛成一个可读、可回滚、可控尾延迟的 round builder：去掉 `goto`，修 fresh bump / whole page rollback，给单轮 follower 数加上界，并顺手清掉 `class_sizes_view_` 冗余字段，同时把 `writable_pages_` 与 spec 概念的对应关系写清楚。

## 本 step 覆盖的 issue

| Issue | 说明 |
|---|---|
| `INC-017` | rollback 不归还 `fresh_bump` / `whole_page`，out-of-space 也不应继续走 recoverable fail path |
| `INC-026` | `handle_persist` 的 `goto round_failed` + 嵌套循环可读性差 |
| `INC-028` | leader-follower 合并无上限，单 round 尾延迟不可控 |
| `INC-025` | `writable_pages_` 与 spec `hole_pages/open_frames` 的对应关系需要显式说明 |
| `INC-027` | `class_sizes_storage_` + `class_sizes_view_` 双字段冗余 |

## 文件结构

```text
value/
├── allocator.hh                      — 增加 bump rollback helper
└── scheduler.hh                      — persist round 重构 + follower 上限 + 去掉 class_sizes_view_

spec/
└── design_doc/runtime_memory_and_cache.md
                                     — 补一段说明当前 `writable_pages_` 与 spec 概念的对应关系
```

## 设计目标

1. 让 `handle_persist` 的控制流不再依赖 `goto` 和“隐藏在注释里的状态语义”。
2. 让 rollback 真正恢复 allocator / writable page 状态，而不是接受 silent degradation。
3. 给单 round 的 follower 聚合加上界，避免一次 `advance()` 吞光整个 `persist_q_`。
4. 删除 `class_sizes_view_` 这类无意义中间字段，减少 value scheduler 的局部复杂度。

## 设计决策

| # | 决策点 | 结果 | 说明 |
|---|---|---|---|
| `D1` | round 构建错误分类 | **显式区分 recoverable 与 fatal** | oversize value 仍可走 `fail()`；out-of-space / encode impossible 直接 panic |
| `D2` | bump rollback 形态 | **allocator 增加 `push_back_bump(page_base, span_lbas)`** | round rollback 逆序归还 fresh bump 页，保证 bump head 可恢复 |
| `D3` | follower 上限 | **私有常量上限，不新增 runtime 配置** | 先把单 round 工作量收住，避免本步引入新的配置面 |
| `D4` | `class_sizes_view_` | **直接删除** | `class_sizes_storage_` 可直接构造 `std::span`，不需要持久 view 字段 |
| `D5` | `writable_pages_` 文档化 | **保留现有实现，补清楚语义映射** | 本步不把它拆成 `hole_pages` / `open_frames` 两套结构，只把当前语义写明 |

## 详细设计

### `value/allocator.hh`

新增 fresh bump 逆向归还 helper：

```cpp
void push_back_bump(paddr page_base, uint32_t span_lbas) noexcept;
```

语义：

- 只用于回滚当前 round 内刚刚 bump 出来的页
- 必须满足 `page_base.device_id == dev_.device_id`
- 必须满足 `page_base.lba == dev_.bump_head_lba`
- 成功后执行：

```cpp
dev_.bump_head_lba += span_lbas;
```

这等价于“撤销最近一次 `bump_next_page(span_lbas)`”。  
rollback 时必须按 `rnd.pages` 的**逆序**处理 fresh bump 页，才能保证这个 helper 的前置条件成立。

### `value/scheduler.hh`：round build 控制流重构

#### 1. 给 entry 追加过程一个显式状态

把当前 `persist_one_entry(...) -> bool` 改成显式结果，例如：

```cpp
enum class persist_entry_status : uint8_t {
    ok = 0,
    value_too_large,
    out_of_space,
    encode_failure,
};
```

`append_entry_to_round(...)` 返回：

- `ok`
- `value_too_large`
  - `find_min_class(...)` 找不到 class
  - 这是 caller 输入超出当前 disk format 能力，仍属于 recoverable fail
- `out_of_space`
  - `acquire_round_page(...)` 返回空
  - 当前 v1 无 reclaim，继续运行只会产生“随机哪批先死”的不稳定行为，所以这是 fatal
- `encode_failure`
  - class 已经选好后仍 encode 失败，说明内部逻辑出错，应当直接 panic

#### 2. `handle_persist()` 拆成三段

目标形态：

1. `collect_round_items(leader_item)`
2. `build_round(round&, items)`
3. `publish_round(round&, leader_item, followers)`

不再保留 `goto round_failed`。

更具体地：

```cpp
void handle_persist(_value_persist::req* leader_item) {
    auto items = collect_round_items(leader_item);
    auto rnd = std::make_unique<round>();
    rnd->id = next_round_id_++;

    auto status = build_round(*rnd, items);
    if (status == value_too_large) {
        rollback_pages(*rnd);
        fail_round_items(items, ...);
        return;
    }
    if (status == out_of_space) {
        core::panic_inconsistency(...);
    }
    if (status == encode_failure) {
        core::panic_inconsistency(...);
    }

    finalize_round_writes(*rnd);
    publish_round(std::move(rnd), items);
}
```

这样：

- recoverable 路径只剩 `value_too_large`
- fatal 路径不再借 `exception_ptr` 传内部控制流
- rollback 只服务真正还会返回 caller 的路径

### follower 上限：`INC-028`

`collect_round_items(leader_item)` 只收：

- 当前 leader
- 最多 `kMaxFollowersPerRound` 个额外 follower

建议用私有常量：

```cpp
static constexpr uint32_t kMaxFollowersPerRound = 64;
```

而不是新增 build/runtime 配置。理由：

- 本步目标是先收住单轮工作量
- 不需要把 tuning 面提前暴露到 runtime/start 配置层
- 如果未来需要调参，再单独提升成 runtime option

实现要求：

- `persist_q_` 中超过上限的请求保留到下一次 `advance()`
- `handle_persist()` 仍然只构造一个 leader round
- `rnd->followers` 不包含 leader 自己

### rollback 语义：`INC-017`

`rollback_pages(round& rnd)` 改成逆序遍历：

```cpp
for (auto it = rnd.pages.rbegin(); it != rnd.pages.rend(); ++it) { ... }
```

各 source 的处理：

- `value_page_source::writable`
  - 用 `original_free_mask` 恢复回 `writable_pages_[ci]`
- `value_page_source::whole_page`
  - 调 `alloc_.recycle_whole_page(class_idx, page_base)`
- `value_page_source::fresh_bump`
  - 调 `alloc_.push_back_bump(page_base, span_lbas)`

这一步后，注释里的“accept bump head leak”要删掉，不再保留。

### 去掉 `class_sizes_view_`：`INC-027`

保留：

```cpp
absl::InlinedVector<uint32_t, 16> class_sizes_storage_;
```

删除：

```cpp
std::span<const uint32_t> class_sizes_view_;
```

`find_min_class()` 的 call site 直接写：

```cpp
std::span<const uint32_t>(class_sizes_storage_.data(), class_sizes_storage_.size())
```

或包成一个临时 helper，但 helper 只返回临时 `span`，不再持有成员字段。

### `writable_pages_` 语义补充：`INC-025`

本步不改变实现结构，只补清楚语义：

- 当前 `writable_pages_[ci]` 表示“已 durable、仍有 free slot、由 scheduler 持有页像以便下轮继续复用”的页队列
- 它更接近 spec 里的“open frame 在 round 间继续保留”的工程化落地
- 它不是 future recovery/reclaim 语义下的完整 `hole_pages`

落地方式：

1. 在 `value/scheduler.hh` 的字段注释上明确这层对应关系
2. 在 `runtime_memory_and_cache.md` 增加一小段实现注记，说明当前代码把“durable partially-free page 的 resident continuation”统一实现为 `writable_pages_`

## 实施顺序

1. `value/allocator.hh` 增加 `push_back_bump(...)`。
2. `value/scheduler.hh` 重构 `persist_one_entry` / `handle_persist`，去掉 `goto`。
3. `rollback_pages()` 改成逆序恢复，并覆盖 `fresh_bump` / `whole_page`。
4. `collect_round_items()` 加 `kMaxFollowersPerRound` 上限。
5. 删除 `class_sizes_view_`。
6. 更新 `writable_pages_` 注释与 `runtime_memory_and_cache.md` 的实现说明。

## 验证

实现本 step 时至少回归：

- `inconel_test_value`
- `inconel_test_tree_value`
- `inconel_test_runtime`

重点观察：

- value persist 正常路径不回归
- oversize value 仍然走 recoverable fail，而不是误 panic
- out-of-space 不再试图继续运行
- follower 合并被明确限制，不再一次吞空整个 `persist_q_`
