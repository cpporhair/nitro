# 016 — Superblock POD

> 实现第十六步。把 ODF §2 的 superblock A/B 布局真正落成 `format/` 里的 packed POD 与 helper，为后续 format/recovery/boot 读取建立稳定字节级接口。

## 本 step 覆盖的 issue

| Issue | 说明 |
|---|---|
| `INC-031` | `format/` 缺 `superblock` POD |

## 文件结构

```text
format/
└── superblock.hh                     — superblock POD、CRC/status helper、A/B 选择 helper

ai_context/inconel/design_doc/
└── on_disk_formats.md                — 如 helper 约束需要补注，和源码一并收口

test/
└── apps/inconel/test/test_superblock_format.cc   — 新增 superblock format 定向测试
```

## 设计目标

1. 把 superblock 的字节级 layout 从文档变成编译期可验证的 POD。
2. 提供足够的 helper，让 future format/recovery 代码不再自己手搓 CRC、generation 选择和数组字段拷贝。
3. 保持 scope 纯 format：本 step 不写 `format_disk()`，也不接启动恢复流程。

## 设计决策

| # | 决策点 | 结果 | 说明 |
|---|---|---|---|
| `D1` | 归属 | **`format/superblock.hh`** | superblock 是纯盘面对象，不放 `runtime/` 或 `recovery/` |
| `D2` | helper 形态 | **POD + CRC/status + A/B 选择 helper** | 先把 future caller 最容易抄错的逻辑收口 |
| `D3` | 与 profile 的关系 | **可提供 profile 转换 helper，但不在本 step 接 runtime** | 与 step 017 对齐，避免两份字段语义漂移 |
| `D4` | scope 边界 | **不做设备 I/O** | 读 LBA / 写 inactive slot / FUA 仍留给后续 format/recovery step |

## 详细设计

### `format/superblock.hh`

新增并冻结：

```cpp
constexpr uint64_t SUPERBLOCK_MAGIC = 0x494E434F4E454C31;

struct __attribute__((packed)) superblock {
    uint64_t magic;
    uint32_t format_version;
    uint64_t namespace_size;
    uint32_t lba_size;
    uint32_t tree_page_size;
    uint32_t shadow_slots_per_range;
    paddr    wal_base_paddr;
    uint32_t wal_segment_size;
    uint32_t wal_segment_count;
    paddr    data_area_base_paddr;
    paddr    data_area_end_paddr;
    uint8_t  value_size_class_count;
    uint32_t value_size_classes[16];
    paddr    root_base_paddr;
    uint64_t generation;
    uint32_t crc;
};
```

需要的编译期保证：

- `static_assert(std::is_trivially_copyable_v<superblock>)`
- `static_assert(sizeof(superblock) <= 4096)`  
  v1 当前 bootstrap profile 的 `lba_size` 固定为 4096，因此这条上界必须显式锁住

### Helper

建议同时提供：

```cpp
uint32_t superblock_compute_crc(const superblock&);

enum class superblock_status : uint8_t {
    ok,
    bad_magic,
    bad_crc,
    bad_format_version,
};

struct superblock_choice {
    const superblock* chosen;
    enum class source : uint8_t { a, b, none } which;
};

superblock_status inspect_superblock(const superblock&);
superblock_choice choose_newer_superblock(const superblock& a, const superblock& b);
```

规则：

1. `choose_newer_superblock` 只在“都可解析”时比较 `generation`
2. 一份坏、一份好时直接选好的
3. 两份都坏返回 `none`
4. “两份 generation 相同但内容不同” 视为格式损坏，helper 直接返回 `none` 或显式 status，不做 silent tie-break

### 与 step 017 的衔接

如果 `format_profile` 已存在，可新增纯转换 helper：

```cpp
superblock build_superblock_from_profile(...);
format_profile profile_from_superblock(const superblock&);
```

但本 step 不让 runtime 直接依赖这些 helper；真正接线留给 format/recovery step。

## 实施顺序

1. 新建 `format/superblock.hh`，补 struct 与 `static_assert`。
2. 再补 CRC / inspect helper。
3. 最后补 A/B 选择 helper（以及可选的 profile 转换 helper）。
4. 用单独 format test 锁定 layout 与选择逻辑。

## 验证

本 step 需要新增：

- `apps/inconel/test/test_superblock_format.cc`

至少覆盖：

- `sizeof(superblock)` 与 packed layout
- CRC 正确 / 篡改后 bad_crc
- magic / format_version 错误分类
- A/B 选择：`(good newer, good older)`, `(good, bad)`, `(bad, bad)`, `(same generation but different payload)`

本 step 不需要跑 recovery integration test，因为 boot/recovery 还没接上 superblock 读写。
