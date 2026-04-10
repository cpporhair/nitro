# 017 — Runtime Format Profile Hardening

> 实现第十七步。把当前 runtime 暴露给配置层的 disk-format 字段收回到代码常量，去掉“传空就禁用 value scheduler”这类半运行时路径，为后续 superblock / recovery 预留单一来源。

## 本 step 覆盖的 issue

| Issue | 说明 |
|---|---|
| `INC-034` | `build_options` 把 `value_class_sizes` / `lba_size` / `value_data_area_base` / `value_data_area_end` 暴露成 runtime config，且存在 “传空 → silent disable” 等 fail path |

## 文件结构

```text
format/
└── format_profile.hh                 — 当前 dev/bootstrap format profile 常量

runtime/
├── builder.hh                        — 移除 4 个外部输入字段，value scheduler 固定按 profile 构造
└── start.hh                          — 移除对应 start_options 字段

value/
└── allocator.hh / scheduler.hh       — 从 profile 接受固定字段，不再依赖调用方 span

test/
├── apps/inconel/test/test_value.cc   — 改成走 profile，不再手传 4 个 format 字段
└── apps/inconel/test/test_runtime.cc — 验证标准 runtime 一定带 value scheduler
```

## 设计目标

1. 把当前 dev/runtime 使用的 disk-format truth 收敛到一处，避免 format 语义散落在 `start_options/build_options/test harness`。
2. 让标准 `build_runtime()` 不再支持“有 tree / nvme、没 value scheduler”的半运行时形态。
3. 为后续 `INC-031` superblock POD 和 `INC-035` format/recovery 留出单一接入点。

## 设计决策

| # | 决策点 | 结果 | 说明 |
|---|---|---|---|
| `D1` | 当前 carrier | **新增代码内的 `format_profile` 常量** | v1 先用 code-owned profile，之后再切到 superblock |
| `D2` | profile 类型 | **dev/bootstrap profile，不冒充最终恢复来源** | 明确这是“当前 bring-up 的单一来源”，不是永久 runtime 配置面 |
| `D3` | value scheduler 可选性 | **标准 runtime 一律构造 value scheduler** | 去掉 empty-span disable；需要 tree-only harness 时走专门测试装配 |
| `D4` | 失败语义 | **profile 不合法 / 设备太小直接 fail-fast** | 不再把错误推迟到 `registry::value_sched()` 首次使用时 |

## 详细设计

### 新增 `format/format_profile.hh`

引入一个集中 carrier，例如：

```cpp
struct format_profile {
    uint32_t lba_size;
    paddr    value_data_area_base;
    paddr    value_data_area_end;
    std::array<uint32_t, N> value_class_sizes;
};

inline constexpr format_profile kBootstrapFormatProfile = { ... };
```

约束：

1. 当前 4 个字段只允许从这里读，不再出现在 runtime 配置面。
2. profile 的内容必须和现有 mock/dev bring-up 兼容；测试若需要不同盘面，改测试专用 runtime harness，不改公共 `start_options/build_options`。
3. 后续 `INC-031` / recovery 落地后，这个 carrier 退化成“新盘 format 默认值”；标准启动路径改为从 superblock 读。

### `runtime/start.hh`

删除：

- `start_options.value_class_sizes`
- `start_options.lba_size`
- `start_options.value_data_area_base`
- `start_options.value_data_area_end`

保留：

- cache policy / cache capacity
- `cores / main_core / device`

这样 `start_runtime()` 不再承担 disk-format 配置职责，只负责“用哪个 runtime profile 启动”。

### `runtime/builder.hh`

删除 `build_options` 中对应 4 字段，并把 value scheduler 构造改成：

1. 总是读取 `kBootstrapFormatProfile`
2. 总是构造 `value_alloc_sched`
3. 对 profile 自洽性和设备容量做一次 upfront 校验

建议的 fail-fast 点：

- `device == nullptr`
- `value_data_area_base >= value_data_area_end`
- `device namespace` 小于 profile 需要的最大 LBA
- `value_class_sizes` 不满足 `value_object` 对齐约束

### 标准 runtime 不再支持“禁用 value”

本 step 明确拍板：

- `runtime::build_runtime()` / `runtime::start_runtime()` 是“完整 Inconel runtime bring-up”
- 如果只想测 tree path，不应通过“传空 profile 字段”获得半功能 runtime
- tree-only / unit-level 场景继续使用现有 registry 直装、mock harness 或专门测试环境

这能直接收掉 `INC-034` 里最危险的部分：silent disable + delayed assert

## 与 `INC-031` / step 016 的关系

这一步不实现 superblock。

本 step 只做两件事：

1. 把当前 code-owned truth 收到一个文件里
2. 把 runtime 配置面收窄成“不再可任意改盘格式”

后续 `INC-031` 的 superblock POD 落地后：

- format 新盘时，把同一份 profile 写入 superblock
- 启动恢复时，从 superblock 读回 profile
- `kBootstrapFormatProfile` 只作为“格式化新盘时的默认模板”

## 实施顺序

1. 新建 `format/format_profile.hh`。
2. `runtime/start.hh` / `runtime/builder.hh` 删除 4 个配置字段。
3. value scheduler 改成从 profile 取构造参数。
4. 补 fail-fast 校验，删掉 empty disable 路径。
5. 更新 `test_value.cc` / `test_runtime.cc`。

## 验证

至少回归：

- `inconel_test_value`
- `inconel_test_runtime`

重点观察：

- 标准 runtime 构建后 `core::registry::value_sched()` 一定非空
- 不再存在“空 `value_class_sizes` 也能 build 成功”的路径
- 设备容量小于 profile 需求时，build 阶段直接失败

本 step 不需要新建测试文件，直接扩现有 `test_value.cc` / `test_runtime.cc` 即可。
