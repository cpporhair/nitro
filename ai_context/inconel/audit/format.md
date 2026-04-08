# Format 模块 Phase 0 审计

> Spec-only audit。**未读** test 文件 / plan 文件。

## 1. 范围

### 1.1 已读源码

| 文件 | 行数 | 备注 |
|------|------|------|
| `format/types.hh` | 70 | tree audit 已读 |
| `format/tree_page.hh` | 74 | tree audit 已读（INC-008 来源） |
| `format/value_object.hh` | 162 | value audit 已读 |
| `format/crc.hh` | 32 | **本轮新读** |

### 1.2 已读 spec

`on_disk_formats.md` 全文（tree audit 已读，覆盖 §1-§8）

## 2. 已经在前两轮 audit 覆盖的部分

- `tree_slot_header` POD ✓（INC-008 已追踪 internal_record POD 缺失）
- `leaf_record_header` POD ✓
- `value_object_header` POD ✓
- `paddr` / `range_ref` / `value_ref` POD ✓
- 各种 CRC / size class helper ✓

## 3. 本轮新发现

### F_format_1 — Superblock POD 完全缺失

**Spec**：ODF §2.2 定义 `struct __attribute__((packed)) superblock { magic, format_version, namespace_size, lba_size, tree_page_size, shadow_slots_per_range, wal_base_paddr, wal_segment_size, wal_segment_count, data_area_base_paddr, data_area_end_paddr, value_size_class_count, value_size_classes[16], root_base_paddr, generation, crc }`，约 120 字节。

**现状**：format/ 没有 `superblock.hh`，没有任何 superblock 类型定义。

**Tier 2**：未来 boot recovery / format / superblock update 等代码无 type 可用，会各自 roll own，扩散不一致。

### F_format_2 — `wal_segment_header` POD 缺失

**Spec**：ODF §3.2 定义 26 字节 packed struct（magic / format_version / segment_index / device_id / stream_id / segment_gen / crc）。

**现状**：缺失。

**Tier 2**：front_sched / wal_space_sched 落地时会自己定义，可能跟 spec 漂。

### F_format_3 — `wal_entry_header` POD 缺失

**Spec**：ODF §3.3 定义 25 字节 packed struct（total_len / segment_gen / lsn / entry_count / op_type / key_len），加 PUT/DELETE 的 entry 编解码规则。

**现状**：缺失。

**Tier 2**：同 F_format_2。

### F_format_4 — `wal_sealed_trailer` POD 缺失

**Spec**：ODF §3.4 定义 33 字节 packed struct（magic / segment_gen / write_end / min_lsn / max_lsn / sealed / crc）。

**现状**：缺失。

**Tier 2**：同上。

### F_format_5 — `crc32c()` 用 raw Intel SSE4.2 intrinsics，无 standard CRC-32C 的 init/xor 处理

**位置**：`format/crc.hh:11-28`

```cpp
inline uint32_t
crc32c(const void* data, size_t len, uint32_t crc = 0) {
    // ... _mm_crc32_u32 / _mm_crc32_u8 ...
}
```

**问题**：标准 CRC-32C（RFC 3720, iSCSI, btrfs, ext4 metadata 等）规定：

- 初始值 = `0xFFFFFFFF`（不是 0）
- 输出 XOR `0xFFFFFFFF`（不是不变）
- 多项式 = Castagnoli 0x1EDC6F41 ✓（这个 SSE4.2 intrinsics 是对的）

代码用默认 `crc = 0` 起始，没有 XOR-out 处理，**produced value ≠ 标准 CRC-32C**。

**自洽性 OK**：inconel 自己写自己读，CRC 函数对称，verify 时算出来一样，不影响正确性。

**外部互操作不 OK**：
- 写 fsck/diagnostic 工具的人按 spec "CRC-32C" 实现 → 算出来跟 inconel 不一样
- 跟其他 strong-consistency 系统对照（btrfs / iSCSI / SPDK NVMe ZNS metadata 等）也不匹配
- 任何 cross-tool 的 CRC 验证都会失败

**Spec 依据**：ODF §1.3 写 "所有盘上对象使用 CRC-32C（SSE 4.2 硬件加速）"。这句话的语义解读：
- 紧解读："CRC-32C 标准（含初始/XOR 处理）"——code 不符
- 宽解读："Castagnoli 多项式 + SSE4.2 硬件加速"——code 符

spec 没写明初始/XOR 是否要按标准——这是一个**spec 缺口**。

**Tier 2 weak**：是 spec 缺口被 "最容易实现的方式" 填上了（默认参数 0，没有 XOR-out）。Constraint C 命中。

**修复方向有 3 种**：

A. **改 code**：加初始 0xFFFFFFFF 和最终 XOR，跟标准对齐
```cpp
inline uint32_t crc32c(const void* data, size_t len, uint32_t crc = 0) {
    crc = ~crc;  // 等价于初始 0xFFFFFFFF
    // ...
    return ~crc;  // XOR-out 0xFFFFFFFF
}
```

B. **改 spec**：明确写 "Castagnoli polynomial, no init/XOR conditioning, self-consistent within inconel only" —— 接受当前实现作为 inconel 的私有约定

C. **保留现状 + 加注释**：spec 不动，code 不动，crc.hh 加注释解释 "raw Castagnoli, not standard CRC-32C; self-consistent only"

## 4. 跟现有 INC 的关系

| 新 finding | 处置建议 |
|---|---|
| F_format_1 (superblock POD) | **新 INC**，blocked on Phase 2 (reboot)，但 POD 定义本身可以现在加 |
| F_format_2/3/4 (WAL POD × 3) | **新 INC（合并 1 条）**，urgent（front_sched / batch PUT 是 Phase 1） |
| F_format_5 (CRC variant) | **新 INC**，等 user 拍板 A/B/C |

## 5. 审计纪律

| 项目 | 状态 |
|------|------|
| 打开 test 文件 | 否 |
| 打开 plan 文件 | 否 |
| 想读测试的冲动 | 0 次 |
