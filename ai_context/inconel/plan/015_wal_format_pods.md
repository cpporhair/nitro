# 015 — WAL Format PODs

> 实现第十五步。把 ODF §3 已经冻结的 WAL 盘格式真正落成 `format/` 里的 POD 与编解码 helper，避免 front/WAL 写侧首次实现时再在调用点手搓 header 长度、CRC 和 payload layout。

## 本 step 覆盖的 issue

| Issue | 说明 |
|---|---|
| `INC-030` | `format/` 缺 WAL 三个 POD type：`wal_segment_header` / `wal_entry_header` / `wal_sealed_trailer` |

## 文件结构

```text
format/
└── wal.hh                            — WAL POD、常量、CRC/编解码 helper

ai_context/inconel/design_doc/
└── on_disk_formats.md                — 如 helper 命名或 status 分类需补充，和源码一并收口

test/
└── apps/inconel/test/test_wal_format.cc   — 新增 WAL format 定向测试
```

## 设计目标

1. 把 WAL 的字节级 truth 收敛到 `format/`，不让 future front/WAL owner 在 sender 或 scheduler 里重复写 layout 算术。
2. 让 entry 编码/解码、CRC 和 total_len 校验都有统一 helper，而不是散落成多份手写 memcpy。
3. 暂不引入任何 owner 状态、segment append 指针或 front/WAL sender；本 step 只做格式层。

## 设计决策

| # | 决策点 | 结果 | 说明 |
|---|---|---|---|
| `D1` | 文件归属 | **单头文件 `format/wal.hh`** | 这是纯 disk format，不放 `front/` 或 `wal/` |
| `D2` | helper 粒度 | **header/trailer 校验 + entry 编解码都提供** | 未来调用点不再自己算 `total_len`、payload 长度和 CRC |
| `D3` | 失败表达 | **reason-aware status，不返回裸 bool** | 未来 recovery/front 错误报告要能区分 bad_crc / bad_total_len / bad_op_type |
| `D4` | scope 边界 | **不做 segment allocator / append state** | 本 step 不碰 `front/`、`wal/`、pipeline |

## 详细设计

### `format/wal.hh`

新增并冻结：

```cpp
constexpr uint32_t WAL_SEGMENT_MAGIC = 0x57414C53;
constexpr uint32_t WAL_SEAL_MAGIC    = 0x5345414C;

enum class wal_op_type : uint8_t {
    put    = 0x01,
    del    = 0x02,
};

struct __attribute__((packed)) wal_segment_header { ... };
struct __attribute__((packed)) wal_entry_header   { ... };
struct __attribute__((packed)) wal_sealed_trailer { ... };
```

同时加：

- `static_assert(sizeof(wal_segment_header) == 26)`
- `static_assert(sizeof(wal_entry_header) == 25)`
- `static_assert(sizeof(wal_sealed_trailer) == 33)`

### Header / Trailer helper

建议提供：

```cpp
uint32_t wal_segment_header_crc(const wal_segment_header&);
uint32_t wal_sealed_trailer_crc(const wal_sealed_trailer&);

enum class wal_segment_status : uint8_t { ok, bad_magic, bad_crc, bad_version };
enum class wal_trailer_status : uint8_t { ok, bad_magic, bad_crc, bad_sealed_flag };
```

用途：

- future front owner 初始化 segment 时直接填 header
- future recovery 扫描时直接拿 status，而不是把所有失败压成 `false`

### Entry 编码 helper

需要把 “PUT / DELETE payload 不同” 收口成统一 helper，而不是让 future caller 自己拼：

```cpp
constexpr uint32_t wal_put_entry_size(uint32_t key_len);
constexpr uint32_t wal_delete_entry_size(uint32_t key_len);

enum class wal_entry_encode_status : uint8_t {
    ok,
    dst_too_small,
    key_too_large,
    bad_op_type,
};

wal_entry_encode_status encode_wal_put_entry(
    std::span<char> dst,
    uint32_t segment_gen,
    uint64_t lsn,
    uint32_t entry_count,
    std::string_view key,
    const value_ref& vr,
    uint32_t* out_total_len);

wal_entry_encode_status encode_wal_delete_entry(...);
```

要求：

1. helper 统一计算 `total_len`
2. helper 统一写尾部 CRC
3. 调用方不再自己管理 header/payload/crc 的偏移算术

### Entry 解码 helper

建议提供：

```cpp
enum class wal_entry_decode_status : uint8_t {
    ok,
    truncated,
    bad_total_len,
    bad_segment_gen,
    bad_op_type,
    bad_crc,
};

struct decoded_wal_entry {
    wal_op_type      op_type;
    uint64_t         lsn;
    uint32_t         entry_count;
    std::string_view key;
    std::optional<value_ref> vr;
};

wal_entry_decode_status decode_wal_entry(
    std::span<const char> src,
    uint32_t expected_segment_gen,
    decoded_wal_entry* out,
    uint32_t* out_total_len);
```

约束：

1. `src` 可以是“segment 剩余字节”的前缀；helper 必须用 `total_len` 判定是否截断
2. `decoded_wal_entry.key` 是 view，不做 owning copy
3. PUT 才有 `vr`，DELETE 的 `vr == nullopt`

## 实施顺序

1. 新建 `format/wal.hh`，把 POD / 常量 / size helper 一次补齐。
2. 再补 header/trailer 校验 helper。
3. 最后补 entry encode/decode helper 与 status enum。
4. 用单独的 format test 把 byte layout 锁住。

## 验证

本 step 需要新增：

- `apps/inconel/test/test_wal_format.cc`

至少覆盖：

- `sizeof(...)` / packed layout / magic 常量
- PUT / DELETE entry 的 `total_len` 计算
- encode → decode round-trip
- bad CRC / truncated / bad segment_gen / bad op_type 的 status 分类
- sealed trailer CRC 校验

本 step 不要求跑 front/WAL integration test，因为 owner/pipeline 还没落地。
