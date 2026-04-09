# 009 — Read-Side / Format / Core Hardening

> 实现第九步。把当前代码里最紧急、且可以在同一 patch 系列里收敛的 read-side / format / core hardening 项一起做掉：统一 corruption 处理为 panic，纠正 CRC 语义，补齐 tree page format POD，移除 tree lookup 的死 variant，并修复 SLRU 的 latent 状态 bug。

## 本 step 覆盖的 issue

| Issue | 说明 |
|---|---|
| `INC-032` | CRC-32C 语义纠正，范围小且依赖已由 step 008 接好 |
| `INC-004` | tree/value corruption 统一 fail-fast，不再 silent fallback / exception path |
| `INC-005` | `tree_manifest::resolve()` 的 Release UB 与 `INC-004` 共用同一 panic helper |
| `INC-008` | `internal_record` POD 与 helper 收敛到 format 层，避免继续散文 memcpy |
| `INC-015` | `decision_need_cache` 是纯死分支，适合在 read-side hardening 里顺手清掉 |
| `INC-037` | `slru_cache` stale `in_protected` 是独立、小范围、确定性的 latent bug |

## 文件结构

```text
core/
├── panic.hh                          — 新增统一 panic helper
├── tree_manifest.hh                  — resolve miss 改为 panic
└── slru_cache.hh                     — 修 stale in_protected 状态

format/
├── tree_page.hh                      — 新增 tree_page_status / internal_record helper
├── value_object.hh                   — 改用标准 CRC-32C
└── crc.hh                            — 删除

tree/
├── lookup.hh                         — 删除 decision_need_cache
├── page_builder.hh                   — internal_record 写入收敛
├── page_reader.hh                    — internal_record 解析收敛
├── scheduler.hh                      — corruption 改 panic
└── sender.hh                         — visit 简化

value/
└── scheduler.hh                      — corruption / invariant break 改 panic，保留 recoverable fail()

build/
└── CMakeLists.txt                    — INCONEL_ABSL_LIBS 增加 absl::crc32c

spec/
└── design_doc/on_disk_formats.md     — ODF §1.3 明确标准 CRC-32C 语义
```

## 设计目标

1. 把当前 read-side / format / core 中最危险的 silent fallback、Release UB、诊断丢失路径收掉。
2. 保持变更边界集中在已有读路径和 format helper，形成一个单一、可连续实现的 hardening step。
3. 让 corruption 与 invariant break 的处理策略统一为 fail-fast，避免 tree/value 各走各的错误语义。
4. 让 spec 文案和代码实现重新对齐，尤其是 CRC-32C 和 `internal_record` 这两处。

## 设计决策

| # | 决策点 | 结果 | 说明 |
|---|---|---|---|
| `D1` | corruption 处理策略 | **统一 panic，不走 silent fallback / exception path** | 已经检测到 disk/page/value corruption 时继续运行没有语义价值，只会扩散错误 |
| `D2` | panic helper 形态 | **新增 `core/panic.hh`，用 `fprintf + abort` 的轻量 helper** | 复用 `value::handle_finalize` 现有风格，不引入额外 formatting/runtime 依赖 |
| `D3` | CRC 修复方式 | **切到 `absl::ComputeCrc32c`，不保留本地 raw SSE helper** | 直接对齐标准 CRC-32C，避免继续维护“看起来叫 CRC-32C，实际上是私有变体”的实现 |
| `D4` | tree page 状态表示 | **`tree_page.hh` 增加 `tree_page_status`** | scheduler 需要 reason-aware panic，不能再只有 `bool tree_page_validate()` |
| `D5` | `internal_record` 落点 | **定义在 `format/tree_page.hh`，builder/reader 全部走 helper** | 和 `leaf_record_header` 保持同层，减少 4 处散文 memcpy |
| `D6` | read-side 死分支处理 | **删除 `decision_need_cache`，不保留“预留 variant”** | 当前实现永不产生该分支，继续暴露在 sender 类型上只会误导后续代码 |

## 详细设计

### `core/panic.hh`

新增统一 helper：

```cpp
namespace apps::inconel::core {

[[noreturn]] void
panic_inconsistency(const char* site, const char* fmt, ...);

}
```

要求：

- 输出统一前缀，例如 `inconel panic: <site>: ...`
- 目标是 **检测到 corruption / invariant break 后立即终止进程**
- 不做异常封装，不返回 status，不引入自定义 error hierarchy
- 实现保持轻量，只依赖 `std::fprintf` / `std::vfprintf` / `std::abort`

本 step 内所有以下场景统一改用它：

- tree page CRC / magic / zero-page 异常
- value object decode 异常
- `tree_manifest::resolve()` miss
- `value::handle_finalize()` unknown round id

### CRC 路径：`format/crc.hh`、`format/tree_page.hh`、`format/value_object.hh`、`CMakeLists.txt`

当前 `format/crc.hh::crc32c()` 是 raw SSE4.2 Castagnoli 累加，缺标准 CRC-32C 的 init/xor conditioning。自洽但不标准。

本步改法：

1. 删除 `format/crc.hh`
2. `format/tree_page.hh` 与 `format/value_object.hh` 直接改用 `absl::ComputeCrc32c`
3. `CMakeLists.txt` 中 `INCONEL_ABSL_LIBS` 增加 `absl::crc32c`
4. `design_doc/on_disk_formats.md` §1.3 追加一行，明确：
   - 盘上对象使用 **标准 CRC-32C**
   - 当前实现由 `absl::ComputeCrc32c` 提供
   - 包含标准 init/xor conditioning

这里不引入本地 wrapper，也不保留旧函数名。step 008 已经把 Abseil 依赖接进 Inconel test target，这一步只是在同一依赖集中多加 `absl::crc32c`。

### `format/tree_page.hh`：`tree_page_status` + `internal_record`

#### `tree_page_status`

新增 page 状态枚举，替代 scheduler 只拿 `bool` 的旧模型：

```cpp
enum class tree_page_status : uint8_t {
    ok = 0,
    zero_page,
    bad_magic,
    bad_crc,
};
```

并新增一个 reason-aware helper，例如：

```cpp
tree_page_status inspect_tree_page(const void* page, uint32_t page_size);
```

语义：

- 前 4 bytes 全零 → `zero_page`
- magic 错 → `bad_magic`
- magic 对但 CRC 错 → `bad_crc`
- 全部合法 → `ok`

`tree_page_validate()` 可以保留为薄 wrapper：

```cpp
inline bool tree_page_validate(...) {
    return inspect_tree_page(...) == tree_page_status::ok;
}
```

这样 builder/reader 的现有 bool 风格不必一次性全改，但 scheduler 能拿到具体 reason 做 panic。

#### `internal_record`

按 ODF §4.2 把 internal node record 的固定前缀导入 format 层：

```cpp
struct __attribute__((packed)) internal_record {
    uint16_t key_len;
};
static_assert(sizeof(internal_record) == 2);

inline uint32_t internal_record_size(uint16_t key_len);
inline const char* internal_record_key(const internal_record* rec);
inline const paddr* internal_record_child_base(const internal_record* rec);
inline paddr* internal_record_child_base(internal_record* rec);
```

这里的 `internal_record` 只表示变长 record 的固定头部；key bytes 与 `child_base` 仍然按 helper 计算偏移访问。这比把 layout 继续散在 builder/reader 的 `sizeof(uint16_t) + key_len + sizeof(paddr)` 更稳，也与 `leaf_record_header` 的层级一致。

### `tree/page_builder.hh` 与 `tree/page_reader.hh`

全部 internal node record 访问改走 `internal_record` / `internal_record_size()` helper：

- `internal_page_builder::record_size()` 不再手写 `sizeof(uint16_t) + key_len + sizeof(paddr)`
- `add_child()` 写入时先构造 `internal_record` header，再写 key 和 child_base
- `internal_page_reader::read_internal_record()` / `skip_internal_record()` 不再直接从裸 `uint16_t key_len` 开始散文 `memcpy`
- `rightmost_child()` 保持独立尾字段语义，不混进 record helper

这一步只做 format 收敛，internal page 的物理 layout 保持不变。

### `tree/scheduler.hh` 与 `core/tree_manifest.hh`

#### corruption 改 panic

`process_entries()` 当前在 cache hit 后直接：

- CRC / magic 失败 → `lookup_absent{}`
- manifest miss → `assert`，Release 变 UB

本步改成：

1. 用 `inspect_tree_page()` 取状态
2. 若状态不是 `ok`，直接 `panic_inconsistency(...)`
3. panic 信息必须带上：
   - 当前 `paddr`
   - `tree_page_status`
   - lookup 所在逻辑位置（如 `tree::lookup_scheduler::process_entries`）

`core::tree_manifest::resolve()` 改为：

```cpp
if (it == slot_map.end()) {
    core::panic_inconsistency("tree_manifest::resolve",
                              "missing range_base dev=%u lba=%lu", ...);
}
```

即：

- 不再依赖 `assert`
- 不返回 sentinel
- 不吞成 `lookup_absent`

#### `INC-015`：删除 `decision_need_cache`

`tree/lookup.hh` 中：

- 删除 `decision_need_cache`
- `batch_decision` 收敛为 `variant<decision_done, decision_need_read>`

`tree/sender.hh` 中：

- `visit()` 后的分发只保留两路：
  - `decision_need_read` → `on_decision_need_read`
  - `decision_done` → `just(true)`

### `value/scheduler.hh`

#### decode corruption 改 panic

当前两个路径都会把 decode 失败折叠成 exception：

- `handle_fill()`：`value::read: corrupt value object on disk (post-NVMe)`
- `serve_hit_or_fail()`：`value::read: corrupt value object in <source>`

问题不是字符串，而是：

- decode reason 被折叠掉了
- corruption 进入 PUMP exception path，而不是 fail-fast

本步改法：

1. 保留 `format::value_decode_status`
2. 不再让 `try_decode_value()` 把所有错误折叠成 `nullopt`
3. `handle_fill()` / `serve_hit_or_fail()` 直接拿到具体 `value_decode_status`
4. 一旦状态不是 `ok`，调用 `panic_inconsistency(...)`

panic 信息至少带上：

- `value_ref.base`
- `value_ref.byte_offset`
- `value_ref.len`
- 数据来源（`post_nvme` / `writable_pages` / `readonly_cache`）
- `value_decode_status`

recoverable 错误保持原语义不变，例如：

- `class_for_len()` 找不到 class
- 业务层正常 miss
- 其他非 corruption 的 `fail(...)`

#### unknown round_id 也收敛到 helper

`handle_finalize()` 当前已有 `fprintf + abort`。本步把它切到 `core::panic_inconsistency(...)`，统一格式，但**语义不变**：unknown round id 仍然是 invariant break，仍然直接终止。

### `core/slru_cache.hh`

修 `INC-037`，范围保持最小：

1. `free_node(idx)` 统一写：

```cpp
nodes_[idx].in_protected = false;
```

2. `alloc_node()` 取出 free node 后加：

```cpp
assert(!nodes_[idx].in_protected);
```

目的不是改变淘汰策略，而是把 latent stale state 明确清零并把不变量显式化。

## 实施顺序

1. 新增 `core/panic.hh`，把 helper 接好。
2. `CMakeLists.txt` 增加 `absl::crc32c`，然后删除 `format/crc.hh`，切换 `tree_page.hh` / `value_object.hh` 到 `absl::ComputeCrc32c`。
3. `format/tree_page.hh` 新增 `tree_page_status` 与 `internal_record` helper。
4. `tree/page_builder.hh` / `tree/page_reader.hh` 收敛 internal record 访问。
5. `tree/scheduler.hh` / `core/tree_manifest.hh` 改 corruption / invariant 路径为 panic。
6. `tree/lookup.hh` / `tree/sender.hh` 删除 `decision_need_cache`。
7. `value/scheduler.hh` 改 decode corruption / unknown round_id 为 panic。
8. `core/slru_cache.hh` 修 stale `in_protected`。
9. 更新 `design_doc/on_disk_formats.md` 的 CRC 文案。

## 验证

实现本 step 时至少回归以下 target：

- `inconel_test_tree_lookup`
- `inconel_test_tree_lookup_multicore`
- `inconel_test_page_cache`
- `inconel_test_value`
- `inconel_test_tree_value`
- `inconel_test_runtime`

预期：

- tree/value corruption 不再被解释成正常 miss 或 exception path，而是直接 panic
- `tree_manifest::resolve()` 不再依赖 Release 被去掉的 `assert`
- `internal_record` layout 仍与 ODF §4.2 完全一致
- `decision_need_cache` 删除后，tree lookup sender 行为不变，只是类型面收窄
- `slru_cache` 的正常 get/put/teardown 行为不变，但 stale `in_protected` 不变量显式成立

实现阶段继续遵守 `CLAUDE.md`：可以跑这些 target，但**不要打开或搜索测试文件内容**。
