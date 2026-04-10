# 018 — Read-Only Frame Cache Core Review

> reviewer 视角结论。基于 production diff、设计文档交叉核对和本地动态验证；不采信实现者自述作为结论。
> 当前 review 只记录阻塞问题和已验证范围，不把测试输出反推成 spec。

## 结论

当前 **可以收**。
step 018 的主体迁移方向正确：`frame_id / page_frame / frame_pin` 已引入，`cache_concept` 已从 raw `char*` 转为 frame cache，tree/value readonly cache 也已迁到 `page_frame*` + `pin()`。
首轮 review 发现的 cache same-key replacement correctness blocker 已修复：旧 frame 仍 pinned 时，`put(new_frame_with_same_id)` 现在会拒绝替换并返回输入 frame，cache 继续保留旧 pinned frame。

## Findings

### 1. 已修复：same-key `put()` 没有保护 pinned old frame

- 位置：
  - `apps/inconel/core/clock_cache.hh`：same-key update 分支
  - `apps/inconel/core/slru_cache.hh`：same-key update 分支
  - `apps/inconel/core/page_cache.hh`：`put()` ownership contract 注释
- 具体问题：
  - 当前 `clock_cache::put(f)` / `slru_cache::put(f)` 对同一个 `frame_id` 的已有 entry 直接替换：
    - cache entry 指向新 `f`
    - 返回旧 `page_frame*`
    - caller 按 contract 释放返回的 frame + backing buffer
  - 如果旧 frame 此时 `pin_count > 0`，仍然存在活跃 `frame_pin` 指向旧 frame。
  - caller 释放旧 frame 后，活跃 pin 就变成 dangling pointer；后续访问或析构 `frame_pin` 都可能 use-after-free。
- 为什么这是问题：
  - `runtime_memory_and_cache.md` §5.4 的 canonical ownership 明确要求：
    - `pin_count > 0` 的 frame 不可驱逐
    - frame 生命周期由 cache / pool / dirty set 决定，不由引用计数释放
  - same-key replacement 本质上也是 eviction / displacement，不能绕过 `pin_count`。
  - `page_cache.hh` 当前注释只写了“cache full + all pinned 返回输入 frame”，没有覆盖“same-key old frame pinned”这个拒绝插入 case，future cache 实现会继续复制这个 bug。
- 期望行为：
  - same-key `put(f)` 时，如果旧 frame `pin_count == 0`：
    - 允许替换
    - 返回旧 frame 给 caller 释放
  - same-key `put(f)` 时，如果旧 frame `pin_count > 0`：
    - 必须拒绝替换
    - cache 继续保留旧 frame
    - 返回输入 frame `f` 给 caller 释放，语义与“无法驱逐所以拒绝插入”一致
    - `size()` 不变，后续 `pin(id)` 仍返回旧 frame
  - `page_cache.hh` contract 注释需要同步写明这个 same-key pinned rejection。
- 修复结果：
  - `clock_cache::put()` 的 same-key 分支已在旧 frame pinned 时直接返回输入 frame，不替换 cache entry。
  - `slru_cache::put()` 的 same-key 分支已在旧 frame pinned 时直接返回输入 frame，并且不做 promote / move。
  - `core/page_cache.hh` 的 `put()` contract 已补明 “key present + old pinned → return f (replacement rejected)”。

## Reviewer-Owned Test

reviewer 已在 `apps/inconel/test/test_page_cache.cc` 增加回归测试：

- `test_update_existing_rejects_when_old_frame_pinned<clock_cache>`
- `test_update_existing_rejects_when_old_frame_pinned<slru_cache>`

该测试约束：

1. 先插入 old frame。
2. pin 住 old frame，使 `pin_count == 1`。
3. 对同一个 `frame_id` 调 `put(replacement)`。
4. 期望返回的是 `replacement` 本身，表示插入被拒绝。
5. cache 中仍然是 old frame，且 old frame pin 计数正常回落。

这条测试是 reviewer-owned；production 实现 agent 不允许打开或修改测试文件。

## 已做验证

修复前的实现曾通过基础目标，但这些目标不能覆盖 same-key pinned replacement 场景；新增 reviewer red test 后，该问题已能稳定暴露。

修复后已重新执行完整 step 018 验证：

- `cmake --build build --target inconel_test_page_cache inconel_test_tree_lookup inconel_test_tree_lookup_multicore inconel_test_value inconel_test_runtime inconel_test_tree_value`
- `./build/inconel_test_page_cache`
- `./build/inconel_test_tree_lookup`
- `./build/inconel_test_tree_lookup_multicore`
- `./build/inconel_test_value`
- `./build/inconel_test_runtime`
- `./build/inconel_test_tree_value`

结果：全部通过。`inconel_test_page_cache` 包含 20 个 page cache section，其中包括 2 个 same-key pinned replacement reviewer red tests。

## 收口判断

本 step 可以作为 “INC-036 phase A + INC-038” 收口，原因：

1. cache concept 已不再暴露 raw `char*` / `get()` / `evict_one()`。
2. `pin()` / `frame_pin` / `pin_count` 的基本生命周期已被 tests 锁住。
3. runtime eviction 和 same-key replacement 都不会驱逐 pinned frame。
4. tree/value readonly cache 已迁到 `page_frame*`，multi-LBA value bypass 保持原 D1 行为。
5. `drain_one()` 已明确为 teardown-only drain，不再冒充 policy eviction API。
