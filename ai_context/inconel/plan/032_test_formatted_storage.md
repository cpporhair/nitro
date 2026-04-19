# Step 032 — 测试用初始化 helper (`make_formatted_storage`)

> 状态：**pre-implementation design**，所有决策点已闭环（§9），可进入实现。
>
> **定位：测试代码专用的 setup helper，不是生产 mkfs 命令。**
>
> 本 step 提供一个初始化一段连续内存、让其看起来像一块"刚格式化过的 Inconel 盘"的 helper。这段内存之后会交给 `mock_nvme::mock_device` 作为模拟 SSD 的 backing storage，测试即可在此基础上跑 flush、recovery 等真路径。
>
> 设计依据：`design_overview.md` §2.5 / §4 / §11 / §12、`on_disk_formats.md` §2 / §7、`code_modules.md`、`code_quality_standard.md` §3.9。

---

## 1. 目标与非目标

### 1.1 目标

提供函数：

```cpp
std::unique_ptr<char[]>
format::make_formatted_storage(const format_options& opts,
                               uint64_t              namespace_size);
```

返回 `namespace_size` 字节的 buffer，满足：

1. **LBA 0 / LBA 1 双槽 superblock 有效**：magic 正确，CRC 正确，`format_version == v1`，字段与 `opts` 一致，`root_base_paddr == paddr{0, 0}`（空树），A 的 `generation == 1`，B 的 `generation == 0`——保证 `choose_newer_superblock` 选 A。
2. **其余字节为零**：`new char[ns_size]()` 的默认零初始化即可——reserved metadata / WAL area / Data Area 都是零。recovery 未来读到零 magic 的 WAL segment 自然跳过；读到零 root_base_paddr 即识别为空树。
3. **格式约束自洽**：`superblock_area + reserved_metadata + wal_area + data_area == namespace_size`；各区域边界与 `on_disk_formats.md` §6 参数关系一致；value class 大小满足 sub-LBA / LBA-aligned / multi-LBA 规则。

同步提供：

```cpp
// mock_nvme/device.hh — 接管外部已初始化 buffer 的新 ctor
mock_device(std::unique_ptr<char[]> adopted_bytes,
            uint64_t                namespace_size,
            uint32_t                lba_size);
```

典型测试用法：

```cpp
auto buf = format::make_formatted_storage(opts, /*ns_size=*/ 64 * MiB);
mock_nvme::mock_device dev(std::move(buf), /*ns_size=*/ 64 * MiB, opts.lba_size);
// dev 代表一块刚格式化过的 Inconel 盘，测试可以从这里起跑。
```

### 1.2 非目标

明确不做的事：

1. **不是生产 mkfs 命令**。不提供 CLI binary、不解析参数、不在 ops 流程里使用。若未来需要生产命令，走另一个 step 重新设计（可能基于本 step 的 `format/` helper 复用）。
2. **不经过 scheduler / PUMP pipeline**。本 helper 是纯 CPU 内存构造，不 new runtime、不 new scheduler、不走 sender chain。
3. **不调 `mock_device::do_*`**。helper 只 build buffer，不 mutate 已有 `mock_device`；mutate 的责任由 mock_device 的 adopting ctor 承担（其实就是 move buffer in）。
4. **不做 `force_overwrite` / 覆写保护**。测试每次都构造新 mock_device，不存在"覆盖已有格式"的场景。
5. **不处理 I/O 失败**。memcpy 不会失败，该函数只有**输入校验失败**一类错误（throw `std::invalid_argument`）。
6. **不含 recovery 本体**。本 step 产出的 buffer 只**满足** recovery 的输入假设，recovery 实现作为后续 step 单独推进。
7. **不含 real `nvme` 支持**。当前只 target mock。real nvme 的"格式化"是生产命令，不属于本 step 范围。

---

## 2. 输入参数

### 2.1 `namespace_size`（调用方传入）

测试决定"我这块模拟 SSD 多大"——由调用方显式传给 `make_formatted_storage`。不从 `format_profile` 读，不从任何 runtime state 派生。

### 2.2 `format_options`（调用方传入）

```cpp
// apps/inconel/format/format_options.hh（新文件）
namespace apps::inconel::format {

    // v1 硬编码：不保留任何 metadata 页。LBA 2 紧接 WAL area。
    // 未来真 nvme step 若需要 reserved page（多盘冗余、诊断等）再开字段。
    constexpr uint32_t kReservedMetadataPagesV1 = 0;

    // v1 硬编码：只防 0 / 负值；WAL rotation / backpressure 的"运行期合理下限"
    // 不是格式合法性问题，由测试 fixture 自己传足量 segment 数。
    constexpr uint32_t kMinWalSegmentCount = 1;

    struct format_options {
        // 与 superblock 格式字段一一对应
        uint32_t lba_size;
        uint32_t tree_page_size;
        uint32_t shadow_slots_per_range;

        uint8_t  value_class_count;
        uint32_t value_class_sizes[kMaxValueClassCount];

        uint32_t wal_segment_size;
        uint32_t wal_segment_count;       // >= kMinWalSegmentCount
    };
}
```

**不接受 `format_profile&`**——profile 里的 `value_data_area_base/end` 是 runtime 运行期占位值，不是 format-time 边界，避免"从 profile 构造 options 时忘记清零这两个字段"导致的错配。

**不提供 `default_format_options` 工厂**——测试 fixture 对参数的诉求分化很大（有要大 segment 的、有要小 segment 的、有刻意错配的），一个"默认值"会让大半 fixture 照搬，掩盖每个 fixture 真正在意的参数组合。aggregate init 本身够简洁。

### 2.3 派生字段（helper 内部计算）

```text
wal_base_paddr.lba        = 2 + kReservedMetadataPagesV1        // v1 = 2
wal_end_lba               = wal_base_paddr.lba + wal_segment_count * (wal_segment_size / lba_size)
data_area_base_paddr.lba  = wal_end_lba
data_area_end_paddr.lba   = namespace_size / lba_size
```

派生字段装进中间结构 `layout_plan`（§5.1 文件清单），superblock_builder 拿着它填 SB 字段。

---

## 3. Buffer 后置状态（规范）

`make_formatted_storage` 返回的 `std::unique_ptr<char[]> buf`，大小等于 `namespace_size` 字节，满足：

### 3.1 字节地图

```text
buf + 0 * lba_size  ..  buf + 1 * lba_size - 1   : superblock A (gen=1, CRC ok)
buf + 1 * lba_size  ..  buf + 2 * lba_size - 1   : superblock B (gen=0, CRC ok)
buf + 2 * lba_size  ..  buf + namespace_size - 1 : 全零
```

Note: SB 结构只占 sizeof(superblock) 字节，剩余的 `lba_size - sizeof(superblock)` 字节也是零（未被显式覆盖，默认零初始化状态）。

### 3.2 Superblock 字段

A 和 B 除 `generation` 外完全一致：

| 字段 | 值 |
|------|----|
| `magic` | `SUPERBLOCK_MAGIC` |
| `format_version` | `SUPERBLOCK_FORMAT_VERSION_V1` |
| `namespace_size` | 入参 |
| `lba_size` | `opts.lba_size` |
| `tree_page_size`, `shadow_slots_per_range` | `opts` |
| `wal_base_paddr`, `wal_segment_size`, `wal_segment_count` | 派生 / `opts` |
| `data_area_base_paddr`, `data_area_end_paddr` | 派生 |
| `value_size_class_count`, `value_size_classes[]` | `opts` |
| `root_base_paddr` | `paddr{device_id=0, lba=0}` = 空树 |
| `generation` | A = 1, B = 0 |
| `crc` | `superblock_compute_crc()` |

### 3.3 与未来 recovery 的接口点

recovery（033 单独实现）未来从 superblock A 读出参数、读到 `root_base_paddr.lba == 0` → leaf record pool 为空；扫 WAL area 看到零 magic → 跳过；无 complete batch → logical winners 为空；clean tree 无需写入；allocator head 重建到 `data_area_base_paddr` / `data_area_end_paddr`；`recovered_max_lsn = 0`，`next_lsn = 1`。

本 step **不**实现 recovery，仅保证 buffer 内容符合其未来输入假设。

---

## 4. 实现

### 4.1 主流程（3 步）

```cpp
std::unique_ptr<char[]>
make_formatted_storage(const format_options& opts,
                       uint64_t              namespace_size)
{
    // 1. 派生 layout 并校验
    const layout_plan L = compute_layout(opts, namespace_size);
    validate_layout(L);   // throws std::invalid_argument on region overflow
                          // / lba misalignment / class_size rules violation

    // 2. 分配零初始化 buffer（new char[N]() 语义）
    auto buf = std::make_unique<char[]>(namespace_size);

    // 3. 构造并 memcpy 双 superblock
    const superblock sb_a = build_superblock(L, /*generation=*/1);
    const superblock sb_b = build_superblock(L, /*generation=*/0);

    std::memcpy(buf.get() + 0 * opts.lba_size, &sb_a, sizeof(sb_a));
    std::memcpy(buf.get() + 1 * opts.lba_size, &sb_b, sizeof(sb_b));

    return buf;
}
```

仅此而已——没有 I/O、没有异步、没有 scheduler、没有 TRIM 循环、没有 FLUSH。

### 4.2 `compute_layout` + `validate_layout`

```cpp
struct layout_plan {
    uint32_t lba_size;
    uint64_t namespace_size;
    uint64_t total_lbas;

    paddr    wal_base_paddr;          // v1 恒 = {0, 2}
    uint32_t wal_segment_size;
    uint32_t wal_segment_count;
    uint32_t wal_segment_lbas;        // wal_segment_size / lba_size

    paddr    data_area_base_paddr;
    paddr    data_area_end_paddr;

    uint32_t tree_page_size;
    uint32_t shadow_slots_per_range;

    uint8_t  value_class_count;
    uint32_t value_class_sizes[kMaxValueClassCount];
};

layout_plan
compute_layout(const format_options& opts, uint64_t namespace_size);

void validate_layout(const layout_plan& L);
// throws std::invalid_argument on：
//   - lba_size == 0 或 namespace_size % lba_size != 0
//   - wal_segment_size % lba_size != 0
//   - wal_segment_size - HEADER_SIZE - TRAILER_RESERVED <= max_entry_size
//   - wal_segment_count < kMinWalSegmentCount   // v1 = 1
//   - wal_base_paddr.lba + wal_total_lbas > total_lbas （没空间给 data area）
//   - data_area_base.lba >= data_area_end.lba
//   - 任一 value_class_size 不满足 sub-LBA / LBA-aligned / multi-LBA 规则
//   - value_class_sizes 非严格递增 / 有 0
```

所有检查都在 buffer 分配之前完成——不合法的 options 不会留任何 side effect。

### 4.3 `build_superblock`

纯函数：

```cpp
superblock
build_superblock(const layout_plan& L, uint32_t generation);
// 填所有字段（magic / version / params / root_base_paddr={0,0} / generation）
// 然后 sb.crc = superblock_compute_crc(sb); return sb;
```

不调 `make_formatted_storage`，也不接触 buffer——只是 "layout + generation → 带 CRC 的 superblock POD"。这让 `build_superblock` 可以在后续 step（flush 的 root-change superblock 更新路径）直接复用，成为 SB 构造的**单一入口**。

### 4.4 `mock_device` 改动

```cpp
// apps/inconel/mock_nvme/device.hh
struct mock_device {
    std::unique_ptr<char[]> storage;   // was: char* storage
    uint64_t                namespace_size;
    uint32_t                lba_size;
    uint64_t                total_lbas;
    std::vector<bool>       trimmed;
    std::mutex*             shared_mtx = nullptr;
    std::atomic<uint64_t>   read_count_{0};
    std::atomic<uint64_t>   write_count_{0};

    // 现有 ctor — 语义不变，内部换成 make_unique<char[]>
    mock_device(uint64_t ns_size, uint32_t lba_sz)
        : namespace_size(ns_size)
        , lba_size(lba_sz)
        , total_lbas(ns_size / lba_sz)
        , trimmed(total_lbas, false)
        , storage(std::make_unique<char[]>(ns_size)) {}

    // 新 ctor — 接管外部已初始化的 buffer
    mock_device(std::unique_ptr<char[]> adopted_bytes,
                uint64_t                ns_size,
                uint32_t                lba_sz)
        : namespace_size(ns_size)
        , lba_size(lba_sz)
        , total_lbas(ns_size / lba_sz)
        , trimmed(total_lbas, false)
        , storage(std::move(adopted_bytes)) {}

    // ~mock_device 删除：unique_ptr 自动管理
    // copy ctor / assign 继续 = delete（unique_ptr 默认就是 move-only）

    // do_write_impl / do_read_impl / do_trim_impl 内部
    //   storage + lba * lba_size   →   storage.get() + lba * lba_size
    // （机械替换，每处 + .get()）
};
```

**关键性质**：

- `storage` 由 `char*` 改 `std::unique_ptr<char[]>`——现有 `new char[ns_size]()` → `std::make_unique<char[]>(ns_size)` 语义等价（后者也是 value-init 零）。
- **所有原 `storage + expr`** 的使用点改为 `storage.get() + expr`（`do_write_impl` / `do_read_impl` / `do_trim_impl` / `test_read_raw` / `test_write_raw` 等）——纯机械改动。
- **新 ctor 的 `trimmed` 向量默认 all false**：`test_is_trimmed` 语义是"用户调过 `do_trim` 的 LBA"，不是"当前是否为零"。外部初始化的 buffer 没走过 `do_trim` 路径，保持 false 是正确的语义。如果后续某个测试需要"所有非 SB 区域被标记为 trimmed"，可以考虑再加 overload，但不在本 step。
- **没有 copy**：adopting ctor 用 `std::move(adopted_bytes)` 转移所有权。调用方移交 buffer 后 `unique_ptr` 为空。

### 4.5 `make_formatted_storage` 不调 mock_nvme 任何符号

```text
依赖方向：
  format/make_formatted_storage.hh
    → format/format_options.hh
    → format/layout_plan.hh
    → format/superblock_builder.hh
    → format/superblock.hh（已有，复用 superblock_compute_crc）
    → format/types.hh（已有，paddr 等）

  不依赖 mock_nvme/。
```

`mock_nvme/device.hh` 的 adopting ctor 只依赖标准库 `std::unique_ptr<char[]>`——不依赖 format。

**两个模块零互依**：用户把 `format::make_formatted_storage` 的返回值 `std::move` 进 `mock_nvme::mock_device` ctor，这个胶水在**调用方**一行代码就完成。

---

## 5. 代码归属

### 5.1 新增 / 改动文件

```text
新增：
  apps/inconel/format/
    format_options.hh           ← format_options 结构 + k* 常量（kReservedMetadataPagesV1 / kMinWalSegmentCount）
    layout_plan.hh              ← layout_plan 结构 + compute_layout() + validate_layout()
    superblock_builder.hh       ← build_superblock(layout, gen) -> superblock
    formatted_storage.hh        ← make_formatted_storage() 主 helper

改动：
  apps/inconel/mock_nvme/device.hh
    - storage: char*   →   std::unique_ptr<char[]>
    - 新增 adopting ctor (std::unique_ptr<char[]>, ns_size, lba_size)
    - 删除手写 ~mock_device（unique_ptr 自管）
    - 所有 "storage + expr" 机械替换为 "storage.get() + expr"
    （grep 过整个 repo 确认 .storage / ->storage 没有任何外部使用，改动面局限在本文件）
```

### 5.2 为什么不扩 `format_profile`

`format_profile` 的定位是 "runtime 消费的稳定子集，INC-031/035 之后从 superblock 回灌"。WAL 参数（`wal_segment_size` / `wal_segment_count`）runtime 当前不消费，塞进 profile 会让 constexpr 常量承载 format-time 决策。

更干净的划分：

- `format_profile` —— runtime 消费的稳定子集
- `format_options` —— format helper 的完整入参（含 WAL 参数），只给 format 用

### 5.3 `runtime/` 不出现新文件

所有新代码都在 `format/` 模块，`runtime/` 不动：

- helper 不构造 scheduler、不跑 pipeline、不和其他 scheduler 协作
- 本质就是"layout + CRC 计算 + memcpy"，属于 `format/` 模块 L0 "POD + 序列化 helper" 定位
- `mock_nvme/device.hh` 的小改（ctor 新增 + storage 类型替换）也不涉及 runtime 层

---

## 6. 错误处理

**只有一类错误：输入校验失败**。

| 阶段 | 错误 | 错误类 |
|------|------|--------|
| `validate_layout` | lba_size == 0 / 不能整除 ns_size | `std::invalid_argument` |
| `validate_layout` | WAL segment 参数违规（大小不对齐、太小装不下 entry、数量低于 min） | `std::invalid_argument` |
| `validate_layout` | Data Area 空间不足 | `std::invalid_argument` |
| `validate_layout` | value class 大小规则违反 | `std::invalid_argument` |

所有检查在 `make_formatted_storage` 内 buffer 分配**之前**完成——不合法 options 不产生任何 side effect（不分配 buffer）。异常直接传给调用方。

没有 `io_error`（不做 I/O）、没有 `format_refused`（不覆盖现有格式）。

---

## 7. 测试（本步**不**包含）

根据 CLAUDE.md 最高优先级规则，实现阶段禁止读测试。测试在单独的 step（032 实现完成后的 test-writing step）补齐，由设计者/测试维护角色推进。

大致验收面：

1. `make_formatted_storage(valid_opts, ns_size)` 返回 buffer，LBA 0/1 的 superblock 通过 `inspect_superblock` = ok
2. `choose_newer_superblock(A, B)` 选出 A（gen=1 > gen=0）
3. A.root_base_paddr == {0, 0}
4. LBA 2 到 buffer 末尾全零
5. `mock_device(std::move(buf), ns_size, lba_size)` 构造后 `test_read_raw(0)` 读出 SB_A 原字节
6. 非法 options（各种违规组合）→ `std::invalid_argument`
7. 现有 mock_device 测试（用无参 ctor）行为保持不变，`storage` 类型改动对外透明

---

## 8. 与后续步骤的契合点

- **Recovery (033)** 会读本 step 产出的 buffer；`make_formatted_storage` 必须保证 SB 字段完整、CRC 正确，让 recovery 在零 WAL / 零 data area 场景下收敛到 clean runtime。
- **Root-change flush 的 superblock 更新**：复用 `build_superblock(layout, gen)`——传入新的 `root_base_paddr`（塞 layout）和 `generation+1`。这个 builder 成为未来所有 SB 构造路径的单一入口。
- **未来的生产 mkfs 命令**（如果需要）：会以 `format/` 下的 `build_superblock` + `layout_plan` 为底层复用层，上层改成通过 nvme scheduler 的同步 wrapper 调 TRIM / FUA write。本 step 不规划，但把可复用点抽干净了。

---

## 9. 已决策记录

所有问题均已定案。下面是每条的最终结论 + 简短理由，便于 review 时直接对照。

### ✅ Q1 — `format_profile&` 作入参

**不允许**。profile 的 `value_data_area_base/end` 是 runtime 占位值而非 format-time 边界，会诱导错配。`format_options` 拷贝需要的字段。

### ✅ Q2 — 独立二进制 `inconel-mkfs`

**不做**。本 step 非目标，test-only helper 在进程内调用就够。

### ✅ Q3 — `reserved_metadata_pages` 默认值

**写死 0**（`kReservedMetadataPagesV1 = 0`），不进 `format_options` 字段。未来真 nvme step 若需要 reserved page 再开字段。

### ✅ Q4 — `kMinWalSegmentCount`

**1**。helper 只防 0 / 负值；"rotation 需要至少 2 个 segment" 这种运行期 sanity 不是格式合法性问题，测试 fixture 需要高段数自己传。

### ✅ Q5 — `wal_segment_size` 默认值 / 工厂

**不提供 `default_format_options`**。测试 fixture 对 WAL 参数诉求分化大（有大 / 小 / 错配各种），一个默认值会让大半 fixture 照搬、掩盖真正在意的参数组合。aggregate init 本身够简洁。

### ✅ Q6 — Data Area 预写占位

**不预写任何内容**。`root_base_paddr == {0,0}` 作为空树标记是合法状态，代码库里的 null-root 分支（`manifest->has_root()` / recovery §12.2 step 2）因为 post-delete / consolidation 场景本来就要存在——pre-init 一棵空树既不能消除这些分支、又会新增"format 的空 leaf builder 和 runtime leaf builder 字节一致"的同步点。

### ✅ Q7 — API 形状（§1.1 / §4.1）

同步、无 I/O、返回 `std::unique_ptr<char[]>`。不构造 runtime、不构造 scheduler。

### ✅ Q8 — real nvme 支持

非目标，本 step 不做。未来生产 mkfs 另走新 step。

### ✅ Q9 — recovery empty-disk 路径

**不含在本 step**，留给 033。本 step 交付后，buffer 只能被纯 format 层测试和 `test_read_raw` 类测试消费，runtime 整合要等 033。这个分步保证 032 scope 不失控、033 的 input case 也清楚（本 step 的产物正好是 recovery 最简 input）。

### ✅ Q10 — `mock_device::storage` 改 unique_ptr 影响面

**完全局部**。grep 过整个 repo，`.storage` / `->storage` 无任何外部使用。改动全部在 `mock_nvme/device.hh` 一个文件内 8 处 touch point（1 declaration + 1 dtor 删除 + 现有 ctor + 新增 adopting ctor + 5 处 `storage + expr` → `storage.get() + expr`）。

### ✅ Q11 — 主 helper 文件名

**`format/formatted_storage.hh`**，名词风格跟 `format/` 其它文件（`format_profile.hh` / `superblock.hh` / `tree_page.hh` / `value_object.hh` / `wal.hh`）一致；文件承载 `make_formatted_storage()` 主函数，未来如果需要 inspect/probe helper 可以顺势放进来。

---

## 10. 实现前检查清单

所有 Q1–Q11 已 ✅ 见 §9。剩余 housekeeping：

- [ ] 实施后在 INDEX 的"快速定位表"加 "测试 setup / formatted_storage"

---

## 11. 预期改动面估算

| 文件 | 类型 | 行数估计 |
|------|------|---------|
| `format/format_options.hh` | new（struct + `kReservedMetadataPagesV1` / `kMinWalSegmentCount` 常量） | ~60 |
| `format/layout_plan.hh` | new（含 compute_layout + validate_layout） | ~120 |
| `format/superblock_builder.hh` | new（build_superblock 纯函数） | ~50 |
| `format/formatted_storage.hh` | new（主 helper） | ~40 |
| `mock_nvme/device.hh` | 改（storage 类型 + 新 ctor + dtor 删除 + 访问点 .get()） | ~15 增量 |
| `format/superblock.hh` | 无需改 | 0 |
| 合计 | | ~285 |

**比原版（~400）更小**——无 I/O 代码、无异常类、无分块 TRIM、无 runtime/format.hh。只剩纯 POD + 一个 memcpy helper + mock_device 的机械局部改动。

无任何现有 production 路径被删改，只是新增 + 一处类型替换。符合 `feedback_incremental_refactor`。

---

所有决策点已闭环，可进入实现。
