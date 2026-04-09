# 016 — Superblock POD Review

> reviewer 视角结论。基于实际 production diff、设计文档交叉核对和本地动态验证，不采信实现者自述作为结论。

## 结论

当前 **可以收**。  
`apps/inconel/format/superblock.hh` 本身没有发现 production 正确性问题；我本地重跑 reviewer-owned `inconel_test_superblock_format` 也通过。  
本轮 review 唯一发现的是 `on_disk_formats.md` 里 same-generation 特例的举例自相矛盾；该文档问题已由 reviewer 直接修正，因此不再构成阻塞。

## Findings

### 1. 已修复：`same generation + identical bytes` 的新增示例与格式化流程矛盾

- 位置：
  - `ai_context/inconel/design_doc/on_disk_formats.md` §2.3 规则 5
  - `ai_context/inconel/design_doc/on_disk_formats.md` §7 格式化流程
- 具体问题：
  - 本次在 §2.3 新增了这样一句：
    - “两份 generation 相同且字节完全一致（例如刚格式化但 B 槽尚未写入）则不算冲突，按 A 处理。”
  - 但同一份文档的格式化流程明确写的是：
    - 先写 superblock A，`generation = 1`
    - 再写 superblock B，`generation = 0`
  - 因此：
    - “B 槽尚未写入”时，B 应该还是全零或无效页，不会形成“两份都 valid 且 generation 相同”
    - “B 槽已写入”时，A/B generation 又明确不同，也不是这个 case
- 为什么这是问题：
  - 这不是措辞小瑕疵，而是同一设计文档内部对同一盘面状态给出了互相冲突的描述。
  - future recovery / format 作者如果只看 §2.3，可能会误以为“fresh format 后出现 same-generation-identical 是标准常态”，这和 §7 的真实写入序列不一致。
- 处理结果：
  - reviewer 已把这个括号里的错误示例删掉，只保留语义本身：
    - “两份 generation 相同且字节完全一致则不算冲突，按 A 处理。”
  - 修完后，§2.3 与同文档 §7 的格式化流程不再互相打架。

## 对代码实现的判断

这次 `apps/inconel/format/superblock.hh` 我没有发现需要退回的 production 问题，理由如下：

1. POD 字段顺序与 ODF §2.2 一致，`superblock_compute_crc()` 的覆盖范围也对齐 “magic 到 generation（含）”。
2. `inspect_superblock()` 的状态分类与本 step 设计目标一致：`ok / bad_magic / bad_crc / bad_format_version`。
3. `choose_newer_superblock()` 对三类主路径的行为合理：
   - 两份都好时按 generation 选新
   - 一好一坏时选好的
   - 两坏返回 `none`
4. “same generation but different payload → none” 的 fail-fast 处理符合 plan 016 与 ODF §2.3 的约束，没有 silent tie-break。
5. “same generation + identical bytes → 返回 A” 这个实现选择本身我认为可以接受；真正的问题不在代码，而在文档给的例子错了。

## 已做验证

我本地实际执行了 reviewer test target，不是只看静态 diff：

- `cmake --build build --target inconel_test_superblock_format`
- `./build/inconel_test_superblock_format`

结果：

- `constants/layout/golden bytes: OK`
- `CRC helper + inspect status classification: OK`
- `A/B choice helper: newer/good-vs-bad/bad-vs-bad/same-gen-conflict: OK`

说明这一步新增的 POD / CRC / inspect / A/B choice helper 已经满足 reviewer 先前写下的 format contract。

## 当前建议

step 016 可以继续按“代码 + reviewer 测试 + 文档收口已完成”来处理。  
下一个最自然的相邻项仍然是 step 017 / `INC-034`，不要把 `INC-036` 混进这一步。
