# 002 — Shadow CoW B+ Tree 页数据结构

> 实现第二步。定义磁盘格式类型和 tree page 构建/解析操作。

## 文件结构

```
format/
├── types.hh        — paddr, range_ref, value_ref（全局地址类型）
├── crc.hh          — CRC-32C（SSE 4.2 硬件加速，noinline）
└── tree_page.hh    — tree page 磁盘格式 + shadow range 地址计算

tree/
├── page_builder.hh — 构建 leaf / internal 页
└── page_reader.hh  — 解析 / 搜索 leaf / internal 页
```

## format/types.hh

- `paddr { device_id, lba }` — 10B packed，支持 hash/比较
- `range_ref { base, slot_count }` — 14B packed
- `value_ref { base, byte_offset, len, flags }` — 18B packed

## format/crc.hh

- CRC-32C，SSE 4.2 `_mm_crc32_u64/u32/u8` 硬件加速
- 标记 `noinline` 避免 GCC `-O2` 下 intrinsic 内联导致寄存器状态错误复用

## format/tree_page.hh

- 常量：`TREE_PAGE_MAGIC = 0x54524545`
- 枚举：`node_type { internal, leaf }`，`record_kind { value, tombstone }`
- `tree_slot_header` — 19B packed（magic, format_version, type, record_count, free_space_offset, page_crc）
- `leaf_record_header` — 11B packed（data_ver, kind, key_len）
- `tree_page_compute_crc()` — CRC 覆盖整页除 page_crc 字段
- `tree_page_validate()` — magic + CRC 校验
- `slot_paddr()` / `range_size_lbas()` — shadow range 地址计算

## tree/page_builder.hh

- `leaf_page_builder` — 顺序写入 sorted key→{data_ver, value_ref/tombstone} 记录，finalize 写 header + CRC
- `internal_page_builder` — 顺序写入 sorted separator→child_base 记录 + rightmost child，finalize 写 header + CRC
- 调用方确保 key 有序

## tree/page_reader.hh

- `leaf_page_reader` — validate + 按 index 访问 / key 线性搜索 / lower_bound
- `internal_page_reader` — validate + find_child（遍历 separator 找第一个 > key 的 child）

## 验证

- leaf page：3 条记录 build → read → find → lower_bound → CRC 篡改检测 — 通过
- internal page：2 separator + rightmost → find_child 四种边界 — 通过
- shadow range：slot 地址计算 — 通过
- leaf 容量 4K/32B key：66 条 — 通过
- leaf 容量 16K/32B key：268 条（设计文档 §4.6 = ~268）— 吻合
- internal 容量 16K/32B key：372 children（设计文档 §4.7 = ~371）— 吻合
- mock_nvme round-trip：build → write → read → validate → find — 通过
