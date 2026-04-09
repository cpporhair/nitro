# 014 — Tree Page Slot Directory Review

> reviewer 视角结论。基于实际 production diff 和本地动态回归，不采信实现者自述作为结论。

## 结论

当前 **不能收**。有 1 个高优先级问题和 1 个中优先级问题需要先修，再补一条更贴近 slot-directory 的定向测试，才适合更新 `known_issues.md`。

## Findings

### 1. 高优先级：slot directory 用 `uint16_t*` 直接取址，落在 19B header 后面，会产生未对齐的 16-bit 读写

- 位置：
  - `apps/inconel/format/tree_page.hh`
  - `apps/inconel/tree/page_builder.hh`
  - `apps/inconel/tree/page_reader.hh`
- 具体问题：
  - `tree_slot_header` 是 19 字节。
  - 新 layout 把 `uint16_t offsets[record_count]` 紧跟在 header 后面。
  - 这意味着 directory 起始地址是 `page + 19`，天然不是 2 字节对齐。
  - 当前实现把它强转成 `uint16_t*` / `const uint16_t*`，后续 `dir[i] = ...`、`buf + dir[i]` 都是在做未对齐的 16-bit load/store。
- 为什么这是问题：
  - 在 x86 上大概率“能跑”，所以普通回归测试可能全部通过。
  - 但在 C++ 语义上这仍然是未定义行为，不应该拿“x86 没炸”当正确性证据。
  - 在更严格的架构、对齐检查、UBSan/ASan 组合下，这类访问可能直接报错或 fault。
- 修复方向：
  - 不要暴露 typed directory pointer。
  - 改成 `memcpy` 风格的 helper，例如：
    - `load_tree_slot_offset(hdr, index)`
    - `store_tree_slot_offset(hdr, index, value)`
  - builder / reader 全部通过 helper 读写 offset，避免未对齐类型访问。

### 2. 中优先级：`internal_page_builder` 记录了 `rightmost_set_`，但 `finalize()` 不校验，新的 O(1) `rightmost_child()` 会把坏页静默当成有效页

- 位置：
  - `apps/inconel/tree/page_builder.hh`
  - `apps/inconel/tree/page_reader.hh`
- 具体问题：
  - 这次实现新增了 `rightmost_set_` 字段，说明实现者已经意识到 internal page 有“必须先设 rightmost child”这个前提。
  - 但 `finalize()` 没有 `assert(rightmost_set_)` 或 fail-fast。
  - 同时，reader 的 `rightmost_child()` 已经从“线性扫到尾”改成“直接读 `free_space_offset - sizeof(paddr)`”。
- 为什么这是问题：
  - 如果调用方忘了 `set_rightmost_child()` 就 `finalize()`，当前页仍可能带着合法 CRC。
  - 这时 `rightmost_child()` 读到的不是“显式缺失”，而是 payload 尾部某段字节。
  - 表现不是显式崩溃，而更像 lookup 走错子树，属于 silent wrong-result 风险，比直接 assert 更难排查。
- 修复方向：
  - 在 `internal_page_builder::finalize()` 里对 `rightmost_set_` 做 `assert` 或 `panic_inconsistency(...)`。
  - 保持 API contract 明确：internal page 未设置 rightmost child 不允许 finalize。

## 已做验证

我实际运行了当前 build 里的二进制，不是只看编译是否通过：

- `./build/inconel_test_tree_lookup`
- `./build/inconel_test_tree_lookup_multicore`
- `./build/inconel_test_runtime`
- `./build/inconel_test_tree_value`
- `./build/inconel_step_02_tests`

以上都通过，说明：

1. 新 layout 没把现有 tree lookup / runtime / tree+value 集成路径直接跑坏。
2. binary search 和 finalize-time materialize 在当前 x86 Release build 下至少能通过现有回归。

## 仍然缺的验证

即使修掉上面两个问题，也还缺一条更贴近本 step 的定向测试，否则 coverage 还是偏集成、不够白盒。

建议补：

- 新增 `apps/inconel/test/test_tree_page_format.cc`

至少覆盖：

1. leaf/internal build → parse → `get()` / `find()` / `lower_bound()` / `find_child()`
2. directory offset 单调递增、页内合法、指向 record 起点
3. `rightmost_child()` 直接读尾部位置的行为
4. 4K/16K 容量边界
5. CRC 篡改仍然拒绝

## 当前建议

先修上面两条，再补 page-format 定向测试；修完后我再做第二轮 review，确认 step 014 是否可以收。
