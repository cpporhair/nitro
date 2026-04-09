# 014 — Tree Page Slot Directory

> 实现第十四步。把 tree page 从“`tree_slot_header` 后直接顺序 records”的临时布局，收敛成带完整 slot directory 的最终盘格式，并把 leaf/internal reader 的查找从 linear scan 改成 binary search。

## 本 step 覆盖的 issue

| Issue | 说明 |
|---|---|
| `INC-012` | leaf/internal page reader 全 linear scan，热路径每页 ~134/185 次比较 |

## 文件结构

```text
format/
└── tree_page.hh                      — 增加 slot directory helper / 常量

tree/
├── page_builder.hh                   — finalize 写 full slot directory
└── page_reader.hh                    — get/find/lower_bound/find_child 改 binary search

ai_context/inconel/design_doc/
├── on_disk_formats.md                — 更新 tree page 物理布局与容量估算
└── INDEX.md                          — 更新 leaf/internal 典型容量数字

test/
├── apps/inconel/test/test_tree_page_format.cc   — 新增 page-format 定向测试
└── apps/inconel/test/test_tree_lookup*.cc        — 仅回归现有 lookup/runtime 集成路径
```

## 设计目标

1. 冻结 tree page 的最终盘格式，避免 tree 写侧 / flush 先搭在 linear-scan 布局上。
2. 让 `leaf_page_reader::find/lower_bound` 与 `internal_page_reader::find_child` 收敛到 O(log N) 比较次数。
3. 不改变 shadow CoW、`tree_manifest`、slot 解析和 CRC 语义；变化只发生在单页内部 layout。

## 设计决策

| # | 决策点 | 结果 | 说明 |
|---|---|---|---|
| `D1` | directory 形态 | **full slot directory** | 每页都存 `uint16_t offsets[record_count]`，不做稀疏目录 |
| `D2` | directory 位置 | **紧跟 `tree_slot_header`** | layout 固定为 `[header][offsets][payload]`，避免 reader 额外推导 |
| `D3` | offset 语义 | **record 起始位置的页内绝对偏移** | reader 直接 `buf + offset[i]` 即可，不再顺扫前缀 records |
| `D4` | builder 写法 | **append payload + finalize 一次性 materialize directory** | 避免每次 `add_*()` 都搬移已有 payload；单次 `memmove` 成本留在冷写侧 |

## 详细设计

### 盘面 layout

Leaf / internal page 统一改成：

```text
[ tree_slot_header ]
[ uint16_t offsets[record_count] ]
[ records payload ... ]
```

约束：

1. `offsets[i]` 按逻辑 key 顺序排列。
2. `offsets[i]` 指向第 `i` 条 record 的起始字节。
3. internal page 的 `rightmost_child_base` 仍放在 payload 尾部，不进入 directory。
4. `free_space_offset` 继续表示“已用字节的尾端偏移”，即 directory + payload 的结尾。

### `format/tree_page.hh`

新增 page-local helper，把 layout 算术收口到一处：

```cpp
inline uint32_t tree_slot_directory_bytes(uint16_t record_count);
inline const uint16_t* tree_slot_directory(const tree_slot_header*);
inline uint16_t* tree_slot_directory(tree_slot_header*);
inline const char* tree_payload_begin(const tree_slot_header*);
inline char* tree_payload_begin(tree_slot_header*);
```

这里不引入新的 header 字段；directory 大小由 `record_count` 唯一决定。

### `tree/page_builder.hh`

builder 仍维持“调用方按 key 有序 add records”的外部 API，但内部补两点：

1. 维护一份 owner-local 的 `record_offsets_` scratch（建议 `absl::InlinedVector<uint16_t, 384>`）。
2. `add_value/add_tombstone/add_child/set_rightmost_child` 先按“无 directory”布局顺序写 payload。
3. `finalize()` 时：
   - 计算 `dir_bytes = sizeof(uint16_t) * count`
   - 将整个 payload（leaf records，或 internal records + rightmost child）整体右移 `dir_bytes`
   - 把右移后的 record 起始 offset 写入 header 后的 directory
   - 回写 `record_count/free_space_offset/page_crc`

理由：

- 这是写侧 page-build 的冷路径，不是 point lookup 热路径。
- 单页一次 `memmove` 的复杂度稳定、实现可审计，不需要把“未来 directory 大小”扩散进每个 `add_*()` 的空间计算。
- 不引入额外 payload owning copy，只新增一份 offset scratch。

### `tree/page_reader.hh`

reader 改为“directory 定位 + binary search”：

- `get(index)`：`offset = offsets[index]`，直接解码该 record
- `find(key)`：对 leaf 的 key 做标准 lower_bound，命中后再判等
- `lower_bound(target)`：返回第一个 `key >= target` 的逻辑 index
- `find_child(lookup_key)`：对 internal separator key 做 `first > lookup_key` 的 binary search
- `rightmost_child()`：直接从 `hdr->free_space_offset - sizeof(paddr)` 取尾部 child，不再 O(N) 顺扫到页尾

注意：

1. 查找比较的逻辑顺序仍然是 key 升序，不因 payload 物理位置改变而改变。
2. internal node 的 `rightmost_child` 语义完全不变，仍对应“没找到 `separator > key` 时走最右子节点”。

### 容量估算更新

新增 directory 后，每条 record 固定多出 2B offset 开销。

在 `tree_page_size = 16384`、平均 key 长 32B 时：

- leaf：`16384 - 19` 可用空间改为 `16384 - 19 - 2*N`
  近似容量从 `~268` 下降到 `~259 records`
- internal：每个 separator 额外 2B offset
  近似扇出从 `~371 children` 下降到 `~356 children`

4K leaf 的 `32B key` 容量也会从 `66` 下降到 `64`。

这些数字要同步更新：

- `design_doc/on_disk_formats.md`
- `design_doc/INDEX.md`
- 旧 `plan/002_tree_page_data_structure.md` 中的验证数字只保留历史背景，不再作为当前 truth

## 实施顺序

1. 先改 `on_disk_formats.md` / `INDEX.md`，把最终 layout 和容量数字拍板。
2. `format/tree_page.hh` 加 directory helper。
3. `page_builder.hh` 改 finalize materialize。
4. `page_reader.hh` 改成 directory + binary search。
5. 补 page-format 定向测试，再回归现有 tree lookup/runtime 测试。

## 验证

本 step 至少需要：

1. 新增 `apps/inconel/test/test_tree_page_format.cc`
2. 回归：
   - `inconel_test_tree_lookup`
   - `inconel_test_tree_lookup_multicore`
   - `inconel_test_runtime`

`test_tree_page_format` 重点覆盖：

- leaf/internal build → parse → `get()` → `find()` / `lower_bound()` / `find_child()`
- internal `rightmost_child()` 为 O(1) 尾部读取
- directory offset 单调递增且都落在页内合法范围
- CRC 篡改仍会被拒绝
- 4K / 16K 容量数字更新为 64 / ~259 / ~356 的新期望
