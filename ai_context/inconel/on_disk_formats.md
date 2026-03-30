# Inconel 详细设计：盘面格式

> 依据：`ai_context/inconel/design_overview.md`（唯一概要规范）
>
> 本文细化概要 §2.5 持久化白名单中四类对象的字节级格式。所有格式定义使用 `__attribute__((packed))` 或等价手段保证布局确定。

## 1. 通用约定

### 1.1 字节序

所有多字节字段使用 **little-endian**（与 NVMe / x86 原生字节序一致），零额外转换开销。

### 1.2 对齐

| 层级 | 对齐粒度 | 说明 |
|------|---------|------|
| Superblock | `lba_size` | 占两个完整 LBA |
| WAL segment | `wal_segment_size`（LBA 对齐） | 每个 segment 起始地址按 segment size 对齐 |
| WAL entry | 无对齐要求 | 紧密排列，可跨 LBA 边界 |
| Tree page / slot | `tree_page_size`（LBA 对齐） | 每个 slot 占一个完整 tree page |
| Shadow range | `tree_page_size * shadow_slots_per_range` | 自然对齐 |
| Value object | `value_size_class` 对齐 | 按 slab 粒度分配 |

### 1.3 CRC

所有盘上对象使用 **CRC-32C**（SSE 4.2 硬件加速）。CRC 覆盖范围在每个对象格式中单独定义。

### 1.4 地址类型

```cpp
struct __attribute__((packed)) paddr {
    uint16_t device_id;                              // v1 = 0
    uint64_t lba;                                    // 逻辑块地址
};
// sizeof(paddr) = 10

struct __attribute__((packed)) range_ref {
    paddr base;                                      // shadow range 起始地址
    uint32_t slot_count;                             // shadow_slots_per_range
};
// sizeof(range_ref) = 14

struct __attribute__((packed)) value_ref {
    paddr    base;                                   // value object 所在 LBA
    uint16_t byte_offset;                            // LBA 内字节偏移（sub-LBA 寻址）
    uint32_t len;                                    // value body 字节数（不含 header）
    uint16_t flags;                                  // 预留扩展（v1 = 0）
};
// sizeof(value_ref) = 18
//
// 完整物理地址 = base.lba * lba_size + byte_offset
// 当 class_size >= lba_size 且对齐时，byte_offset 恒为 0
```

## 2. Superblock A/B

### 2.1 布局

Superblock 占盘面最前端两个 LBA，分别为 slot A 和 slot B：

```text
LBA 0: superblock A
LBA 1: superblock B
LBA 2 .. LBA (reserved_metadata_pages - 1): reserved（v1 未使用）
```

### 2.2 格式

```cpp
constexpr uint64_t SUPERBLOCK_MAGIC = 0x494E434F4E454C31;  // "INCONEL1"

struct __attribute__((packed)) superblock {
    // ── 魔数与版本 ──
    uint64_t magic;                                  // SUPERBLOCK_MAGIC
    uint32_t format_version;                         // 盘格式版本，v1 = 1

    // ── 设备参数 ──
    uint64_t namespace_size;                         // NVMe namespace 总大小（字节）
    uint32_t lba_size;                               // 逻辑块大小（字节，通常 4096）

    // ── Tree 参数 ──
    uint32_t tree_page_size;                         // 树节点页大小（字节，如 16384）
    uint32_t shadow_slots_per_range;                 // 每个 shadow range 的 slot 数

    // ── WAL 参数 ──
    paddr    wal_base_paddr;                         // WAL area 起始地址
    uint32_t wal_segment_size;                       // 单个 segment 大小（字节）
    uint32_t wal_segment_count;                      // segment 总数

    // ── Data Area 参数 ──
    paddr    data_area_base_paddr;                   // Data Area 起始地址
    paddr    data_area_end_paddr;                    // Data Area 结束地址（不含）

    // ── Value 参数 ──
    uint8_t  value_size_class_count;                 // size class 数量
    uint32_t value_size_classes[16];                 // 每个 size class 的字节数（含 header）
                                                     // v1 最多 16 个 class

    // ── 当前状态 ──
    paddr    root_base_paddr;                        // 最新 clean root 的 range base
    uint64_t generation;                             // superblock 写入代数，单调递增

    // ── 校验 ──
    uint32_t crc;                                    // CRC-32C，覆盖 magic 到 generation（含）
};
```

### 2.3 读写规则

1. 恢复时读取 A 和 B，选择 `CRC 正确且 generation 更大` 的那份。
2. 更新时写入当前 **inactive** 的那个 slot（generation 较小的），generation + 1，FUA 写入。
3. 只有当 `root_base_paddr` 变化时才需要更新（概要 §4.2 规则 3）。
4. 两份都 CRC 错误 → 格式化损坏，需要人工介入。
5. 两份 generation 相同但内容不同 → 不应出现（写入序列保证），作为格式化损坏处理。

### 2.4 Reserved Metadata Pages

`LBA 2 .. (wal_base_paddr.lba - 1)` 为预留空间。v1 不使用，保留给未来多盘冗余、格式扩展或诊断信息。

## 3. WAL Segment 格式

### 3.1 Segment 物理布局

```text
segment_base_lba                                        segment_end_lba
├─ segment_header ─┼── entries ──────────────────┼─ sealed_trailer ─┤
│   HEADER_SIZE    │  紧密排列，可跨 LBA 边界    │  TRAILER_SIZE    │
```

segment 起始地址计算：
```
segment_base_lba = wal_base_paddr.lba + index * (wal_segment_size / lba_size)
```

### 3.2 Segment Header

```cpp
constexpr uint32_t WAL_SEGMENT_MAGIC = 0x57414C53;  // "WALS"

struct __attribute__((packed)) wal_segment_header {
    uint32_t magic;                                  // WAL_SEGMENT_MAGIC
    uint32_t format_version;                         // 与 superblock.format_version 一致
    uint32_t segment_index;                          // 该 segment 在 WAL area 中的序号
    uint16_t device_id;                              // v1 = 0
    uint32_t stream_id;                              // 产生该 segment 的前台 owner 编号
    uint32_t segment_gen;                            // 复用代数（首次 = 1，每次复用 +1）
    uint32_t crc;                                    // CRC-32C，覆盖 magic 到 segment_gen（含）
};
// sizeof = 26

constexpr uint32_t HEADER_SIZE = sizeof(wal_segment_header);  // = 26
```

### 3.3 WAL Entry

```cpp
constexpr uint8_t WAL_OP_PUT    = 0x01;
constexpr uint8_t WAL_OP_DELETE = 0x02;
// 0x03..0xFF 预留给 future large-value / multi-extent 等

struct __attribute__((packed)) wal_entry_header {
    uint32_t total_len;                              // 整条 entry 字节数（header + payload + crc）
    uint32_t segment_gen;                            // 必须与 segment header 的 gen 一致
    uint64_t lsn;                                    // batch_lsn
    uint32_t entry_count;                            // 该 batch_lsn 的 canonical record 总数
    uint8_t  op_type;                                // WAL_OP_PUT / WAL_OP_DELETE
    uint32_t key_len;                                // key 字节数
};
// sizeof = 25

// PUT entry 布局：
// [ wal_entry_header | value_ref(18 bytes) | key_bytes(key_len) | crc32(4 bytes) ]
// total_len = sizeof(wal_entry_header) + sizeof(value_ref) + key_len + 4

// DELETE entry 布局：
// [ wal_entry_header | key_bytes(key_len) | crc32(4 bytes) ]
// total_len = sizeof(wal_entry_header) + key_len + 4
```

CRC 覆盖范围：从 `wal_entry_header.total_len` 开始到 `key_bytes` 末尾（不含 crc 本身的 4 字节）。

#### entry 编解码

```text
编码（前台 append）：
  1. 计算 total_len = sizeof(header) + [sizeof(value_ref) if PUT] + key_len + 4
  2. 填写 wal_entry_header（含 total_len）
  3. if PUT: 写入 value_ref（18 bytes）
  4. 写入 key_bytes
  5. 计算 CRC-32C（覆盖从 total_len 到 key_bytes 末尾），写入尾部 4 bytes

解码（recovery 扫描）：
  1. 读取 wal_entry_header
  2. 检查 segment_gen 与 segment header 一致
  3. 计算 expected_payload_len = total_len - sizeof(header) - 4
  4. 读取 payload + crc
  5. 校验 CRC
  6. 按 op_type 解析 payload
```

### 3.4 Sealed Trailer

```cpp
struct __attribute__((packed)) wal_sealed_trailer {
    uint32_t magic;                                  // 0x5345414C ("SEAL")
    uint32_t segment_gen;                            // 与 segment header 一致
    uint32_t write_end;                              // 最后一条 entry 之后的字节偏移
    uint64_t min_lsn;                                // 该 segment 中最小 lsn
    uint64_t max_lsn;                                // 该 segment 中最大 lsn
    uint8_t  sealed;                                 // 1 = sealed
    uint32_t crc;                                    // CRC-32C，覆盖 magic 到 sealed（含）
};
// sizeof = 33

constexpr uint32_t TRAILER_SIZE = sizeof(wal_sealed_trailer);
// TRAILER_RESERVED = ceil(TRAILER_SIZE, lba_size) 向上取整到页
// 实际写 trailer 时追加到 segment 末尾空间
```

trailer 位置固定在 segment 的最后 `TRAILER_RESERVED` 字节处（概要 §11.2）。entries 的追加空间为 `[HEADER_SIZE, wal_segment_size - TRAILER_RESERVED)`。

**trailer 不是必须的**：它只是加速 recovery 扫描的 hint（概要 §11.6 规则 4）。如果 segment 在 seal 后来不及写 trailer 就崩溃，recovery 仍然可以通过逐条解析 entry 来定界。

### 3.5 Entry 跨 LBA 边界

WAL entry 可以跨 `lba_size` 页边界（概要 §11.2 额外约束 2），但不能跨 segment 边界。

当前台 `append_entry_fua` 写入时：

```text
如果 entry 完全在同一个 LBA page 内：
  → 只需 FUA 写该 page（tail_buf）

如果 entry 跨越 N 个 LBA pages：
  → 先非 FUA 写前 N-1 个完整 page
  → 最后一个 page 用 FUA 写
  → FUA 完成保证所有先提交的写也已 durable
```

### 3.6 Segment Size 约束

```text
wal_segment_size 必须满足：
  wal_segment_size - HEADER_SIZE - TRAILER_RESERVED > max_entry_size

max_entry_size = sizeof(wal_entry_header) + sizeof(value_ref) + MAX_KEY_LEN + 4
```

v1 建议 `wal_segment_size` >= 4 MiB，`MAX_KEY_LEN` = 1024 bytes。

## 4. Tree Page 格式

### 4.1 通用 Slot Header

每个 tree slot（无论 internal 还是 leaf）都以相同的通用 header 开头：

```cpp
constexpr uint32_t TREE_PAGE_MAGIC = 0x54524545;  // "TREE"

enum class node_type : uint8_t {
    internal = 1,
    leaf     = 2,
};

struct __attribute__((packed)) tree_slot_header {
    uint32_t magic;                                  // TREE_PAGE_MAGIC
    uint32_t format_version;                         // 与 superblock.format_version 一致
    node_type type;                                  // internal / leaf
    uint16_t record_count;                           // 该页中的记录数
    uint32_t free_space_offset;                      // 空闲空间起始偏移
    uint32_t page_crc;                               // CRC-32C
};
// sizeof = 19
// page_crc 校验范围 = [page_start, page_start + tree_page_size) 中除 page_crc 自身 4 bytes 外的所有内容
// 即：header（除 page_crc）+ 所有 records + 尾部空闲空间
```

### 4.2 Internal Node 格式

```text
[ tree_slot_header ]
[ separator_key_0 | child_base_0 ]
[ separator_key_1 | child_base_1 ]
...
[ separator_key_{n-1} | child_base_{n-1} ]
[ rightmost_child_base ]                             // 最右子节点
```

```cpp
struct __attribute__((packed)) internal_record {
    uint16_t key_len;                                // separator key 长度
    // key_bytes[key_len]                            // separator key（变长）
    // paddr child_base                              // 子节点 range base
};
// 变长：sizeof = 2 + key_len + sizeof(paddr)
```

查找规则：
```text
给定 lookup_key，在 internal node 中找第一个 separator_key > lookup_key 的位置 i
→ 走 child_base[i]（没找到 → 走 rightmost_child_base）
// child_base[i] 是 separator_key[i] 的左子树，覆盖 [sep_{i-1}, sep_i)
// child_base[0] 天然就是最左子节点，覆盖 (-inf, sep_0)
```

Internal node 的记录按 separator key 升序排列。

### 4.3 Leaf Node 格式

```text
[ tree_slot_header ]
[ leaf_record_0 ]
[ leaf_record_1 ]
...
[ leaf_record_{n-1} ]
```

```cpp
enum class record_kind : uint8_t {
    value     = 1,
    tombstone = 2,
};

struct __attribute__((packed)) leaf_record_header {
    uint64_t data_ver;                               // 同 key 比较版本（语义 == batch_lsn）
    record_kind kind;                                // value / tombstone
    uint16_t key_len;                                // logical key 长度
};
// sizeof = 11

// value record 布局：
// [ leaf_record_header | key_bytes(key_len) | value_ref(18 bytes) ]
// total = 11 + key_len + 18

// tombstone record 布局：
// [ leaf_record_header | key_bytes(key_len) ]
// total = 11 + key_len
```

Leaf 记录按 logical key 升序排列。同一逻辑 key 在同一个 leaf slot 中最多出现一次（同一 tree snapshot 下同 key 只有一个 winner）。

#### `data_ver` 编码选择

概要 §1.9 提到 `data_ver` 的物理编码可以落在 versioned key 或 leaf payload 中。本设计选择放在 `leaf_record_header` 的独立字段中（而非编码进 key），原因：

1. key 的比较语义更清晰（纯 logical key 比较）。
2. `data_ver` 只在 fold / recovery / tombstone 判定时使用，不影响 B+ tree 的搜索路径。
3. 读路径不需要从 versioned key 中拆分出 logical key。

### 4.4 Shadow Range 物理布局

```text
range_base_lba
├── slot 0: [ tree_page_size bytes ]
├── slot 1: [ tree_page_size bytes ]
├── slot 2: [ tree_page_size bytes ]
...
└── slot (X-1): [ tree_page_size bytes ]

X = shadow_slots_per_range
range_size = tree_page_size * X
```

每个 slot 是一个独立的完整 tree page（internal 或 leaf）。同一 range 内的 slots 属于同一逻辑节点的不同版本。

slot 地址计算：
```
slot_paddr.lba = range_base.lba + slot_index * (tree_page_size / lba_size)
```

### 4.5 空 Slot 识别

新分配的 range 在写入第一个有效 slot 之前，其他 slot 处于 "未使用" 状态。TRIM 后 read 返回全零（已验证，见 memory: project_inconel_decisions.md）。

识别规则：
```text
读取 slot 的前 4 bytes：
  - 全零 → TRIM 后未使用（空 slot）
  - == TREE_PAGE_MAGIC → 解析 header，校验 payload_crc
    - CRC 通过 → 有效 slot
    - CRC 不通过 → torn write / 损坏 slot
  - 其他值 → 非法状态
```

Recovery 时使用此规则扫描 Data Area（详见 `recovery_and_wal_reclaim.md`）。

### 4.6 Leaf 页容量估算

假设 `tree_page_size = 16384`（16 KiB）：

```text
可用空间 = 16384 - sizeof(tree_slot_header) = 16384 - 19 = 16365 bytes

value record size = 11 + key_len + 18 = 29 + key_len
tombstone record size = 11 + key_len

假设 avg key_len = 32 bytes:
  value record avg = 61 bytes
  → ~268 records per leaf

假设 avg key_len = 128 bytes:
  value record avg = 157 bytes
  → ~104 records per leaf
```

### 4.7 Internal 页容量估算

```text
可用空间 = 16365 bytes

internal record size = 2 + key_len + 10 = 12 + key_len
加上末尾 rightmost_child_base = 10 bytes

假设 avg separator_key_len = 32 bytes:
  record avg = 44 bytes
  → ~371 children per internal node

假设 avg separator_key_len = 128 bytes:
  record avg = 140 bytes
  → ~116 children per internal node
```

扇出度足够高，树深度在常见数据量下不超过 3-4 层。

## 5. Value Object 格式

### 5.1 布局

```cpp
struct __attribute__((packed)) value_object_header {
    uint32_t magic;                                  // 0x56414C55 ("VALU")
    uint32_t body_len;                               // value body 字节数
    uint32_t body_crc;                               // CRC-32C，覆盖 body（不含 header）
};
// sizeof = 12

// value object 布局：
// [ value_object_header | value_body(body_len) | padding ]
// padding 到 value_size_class 边界
```

### 5.2 Size Class 分配

Value Area 按 size class 分配（slab allocator 模式）。每个 size class 的 slab 大小在 superblock 中定义。

```text
value_size_classes[] 示例：
  class 0:   64 bytes  (header 12 + body <= 52)     ← sub-LBA, 每 LBA 64 个 slot
  class 1:  128 bytes  (header 12 + body <= 116)     ← sub-LBA, 每 LBA 32 个 slot
  class 2:  256 bytes  (header 12 + body <= 244)     ← sub-LBA, 每 LBA 16 个 slot
  class 3:  512 bytes  (header 12 + body <= 500)     ← sub-LBA, 每 LBA 8 个 slot
  class 4: 1024 bytes  (header 12 + body <= 1012)    ← sub-LBA, 每 LBA 4 个 slot
  class 5: 4096 bytes  (header 12 + body <= 4084)    ← LBA 对齐, byte_offset = 0
  class 6: 16384 bytes (header 12 + body <= 16372)   ← 多 LBA, byte_offset = 0
```

分配时选择最小的能容纳 `sizeof(value_object_header) + value_len` 的 class。

每个 class 的 slot 在 LBA 内的布局：

```text
sub-LBA class (class_size < lba_size):
  一个 LBA 包含 slots_per_page = lba_size / class_size 个 slot
  slot[i] 的 byte_offset = i * class_size

LBA-aligned class (class_size >= lba_size, class_size % lba_size == 0):
  每个 slot 独占 class_size / lba_size 个 LBA
  byte_offset 恒为 0

约束：class_size 必须能整除 lba_size，或 lba_size 能整除 class_size
```

### 5.3 Value Area 物理布局

```text
value allocator 从 data_area_end_paddr 向低地址分配：

data_area_end_paddr
  └── chunk N: [ LBA page | LBA page | ... ]  (某 class 的连续 LBA 组)
  └── chunk N-1: [ LBA page | LBA page | ... ]
  ...
  └── chunk 0: [ LBA page | ... ]
← 向低地址增长
```

每个 chunk 属于一个 size class。chunk 内部的 LBA 按顺序填充 value objects。

对于 sub-LBA class，一个 LBA page 内包含多个 slot（紧密排列，无内部碎片）。
对于 LBA-aligned class，一个 slot 占整数个 LBA。

### 5.4 `value_ref` 与 Value Object 的对应

```text
value_ref.base        → value object 所在 LBA
value_ref.byte_offset → LBA 内的字节偏移
value_ref.len         → value body 字节数（不含 header）
value_ref.flags       → v1 = 0

value object 在盘上的物理起始位置 = base.lba * lba_size + byte_offset

实际读取时：
  1. 读取 base.lba 对应的整个 LBA（或者对于多 LBA class，读取连续 LBAs）
  2. 在 byte_offset 处定位 value_object_header
  3. 校验 magic
  4. 校验 body_len == value_ref.len
  5. 从 byte_offset + sizeof(header) 处读取 body_len bytes
  6. 校验 body_crc
```

### 5.5 Sub-LBA 写入策略

v1 只承诺 page-based `nvme_sched` 写入能力。当前 write method 只有两条路径：

```text
if class_size % lba_size == 0:
    → 直接写整 LBA(s)，无需 read-modify-write

else:  // sub-LBA class, v1 page-based realization
    → 顺序填充新 LBA page：dirty_append frame，memcpy 到 dma_buf（见 write_path_and_pipeline.md §5.7）
    → 复用 resident hole page：页像已在 DMA 内存中，无需额外 NVMe read，直接 memcpy 到空洞位置
    → 复用 non-resident hole：需要先把整页读成 value_page_frame，再进入 dirty_hole_fill
    → 以上三种最终都是 page-based writeback（整 LBA 写回 NVMe）
```

**非 v1 future capability**：未来若底层 I/O 层支持更细粒度 durable write，可在不改变 `value_ref` 格式、placement policy 或 recovery 规则的前提下扩展 write method。具体适配方式留待 write path / lower I/O 设计确定，不在盘格式文档中展开。

### 5.6 Value Object 的 CRC 使用场景

| 场景 | 是否校验 CRC |
|------|-------------|
| 前台 PUT 写入后 | 不需要（刚写的数据） |
| memtable hit 读 | 不需要（走 hot_blob，不读盘） |
| tree hit → value read | 需要（从 SSD 读回，校验完整性） |
| recovery | v1 不校验 value body（概要 §12.3 第 8 点） |

v1 boot recovery 直接复用 winner `value_ref`，不读取也不校验 value body。CRC 的主要作用是 runtime tree-path value read 的数据完整性保护。

## 6. 格式参数关系总结

```text
namespace_size            → 盘总大小
lba_size                  → NVMe 逻辑块大小（通常 4096）

superblock_area           = 2 * lba_size
reserved_metadata_area    = (wal_base_paddr.lba - 2) * lba_size

wal_area                  = wal_segment_size * wal_segment_count
data_area                 = (data_area_end_paddr.lba - data_area_base_paddr.lba) * lba_size

tree_range_size           = tree_page_size * shadow_slots_per_range
value_slab_granularity    = value_size_classes[i]

约束：
  superblock_area + reserved + wal_area + data_area <= namespace_size
  data_area_base_paddr.lba >= wal_base_paddr.lba + wal_segment_count * (wal_segment_size / lba_size)
  tree_page_size >= lba_size
  tree_page_size % lba_size == 0
  wal_segment_size % lba_size == 0
  value_size_classes[i] % lba_size == 0 或 lba_size % value_size_classes[i] == 0
  （class 可以是 sub-LBA 或整数个 LBA，但必须能整除或被整除）
```

## 7. 格式化流程

首次使用盘面时的格式化步骤：

```text
1. 计算各区域边界：
   wal_base_paddr.lba = 2 + reserved_metadata_pages
   wal_end_lba = wal_base_paddr.lba + wal_segment_count * (wal_segment_size / lba_size)
   data_area_base_paddr.lba = wal_end_lba
   data_area_end_paddr.lba = namespace_size / lba_size

2. TRIM 整盘（NVMe Dataset Management DEALLOCATE）
   → 所有 LBA 读回全零（依赖 DLFEAT 验证）

3. 写入 superblock A（generation = 1）:
   填充所有格式参数
   root_base_paddr = { 0, 0 }（空树）
   计算 CRC
   FUA 写入 LBA 0

4. 写入 superblock B（generation = 0）:
   同 A 的内容但 generation = 0
   FUA 写入 LBA 1

5. 格式化完成，可以启动运行时
```

## 8. 版本演进预留

### 8.1 `format_version` 策略

`format_version` 是盘格式的不兼容版本号：

- v1 = 当前版本
- 每次盘格式不兼容变更 → version + 1
- 运行时启动时检查 superblock.format_version 是否在支持范围内

### 8.2 `value_ref.flags` 预留

v1 所有 flags = 0。未来扩展方向（概要 §10.6）：

| bit | 含义 |
|-----|------|
| 0 | 0 = inline single extent, 1 = external extent list |
| 1-15 | 预留 |

### 8.3 WAL `op_type` 预留

v1 只使用 0x01 (PUT) 和 0x02 (DELETE)。预留 0x03+ 给 future large-value、multi-extent 等扩展操作。
