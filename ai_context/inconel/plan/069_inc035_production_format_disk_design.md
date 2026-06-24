# 069: INC-035 Production Format Disk Design

## 状态

`INC-035` 的当前问题不是缺少 superblock builder 或 layout arithmetic，而是
runtime destructive format 仍停在 bootstrap helper 形态：

- `format::compute_layout()` / `format::validate_layout()` 已能从
  `format_options + namespace_size` 推导动态 layout。
- `format::build_superblock()` 已能从 `layout_plan` 构造 A/B superblock。
- `runtime::force_format_device()` 仍调用 `bootstrap_layout_for_device()`，把
  `kBootstrapFormatProfile` 的固定 `value_data_area_end=100000` 写进真实盘。
- `force_format_device()` 只 zero WAL 区，再写 superblock A/B；没有执行 ODF §7
  要求的整盘 DEALLOCATE，也没有确认 deallocated LBA 读零语义。
- `runtime::db_session::format_or_recover()` 在 force-format 后
  `recovered_boot.reset()`，后续 `build_runtime()` 会在 `disk_profile == nullptr`
  时回退到 `kBootstrapFormatProfile`。因此只改写盘 layout 还不够，必须把
  刚格式化出的 profile / clean runtime state 传给 builder。

本设计定义 `INC-035` 的生产闭环：`--force-format` 在真实 NVMe 上按 namespace
使用整盘，建立 ODF §7 的 clean disk invariant，并让 runtime 立即以同一份
superblock profile 启动。

## 优先级背景

当前首要目标是长时间跑 YCSB `load + a`，并和 RocksDB 做可比数据。

这个目标要求 Inconel 至少满足三件事：

1. force-format 不能把大盘裁成 bring-up profile 小盘，否则容量和 RocksDB 对比都失真。
2. fresh disk 上未写 tree/value/WAL 的区域必须读回全零，否则 shadow slot 空洞、
   recovery 空页判断和 WAL zero-tail 扫描没有可靠前提。
3. force-format 后首次 runtime 必须使用刚写入 superblock 的动态 profile，
   不能继续用编译期 bootstrap profile。

`MultiGet` / `Range Scan` 不阻塞 YCSB A；`load-a` 的当前路径只需要
`write_batch` 和 `point_get`。因此 035 是长跑数据前最直接的实现项。

## 设计依据

- `on_disk_formats.md` §6：format 参数关系和 layout 约束。
- `on_disk_formats.md` §7：格式化流程，整盘 DEALLOCATE、读零语义、A/B superblock。
- `design_overview.md` §4.1 / §4.2：superblock format fields 和 A/B 选择规则。
- `runtime_state_machine.md` §7.3：TRIM 使用 NVMe Dataset Management DEALLOCATE。
- 现有代码：
  - `apps/inconel/format/layout_plan.hh`
  - `apps/inconel/format/format_options.hh`
  - `apps/inconel/format/superblock_builder.hh`
  - `apps/inconel/recovery/boot.hh`
  - `apps/inconel/runtime/format_device.hh`
  - `apps/inconel/runtime/session.hh`
  - `apps/inconel/runtime/builder.hh`

## 目标

1. 新增 production `runtime::format_disk(...)`，替代 public force-format 路径上的
   bootstrap layout。
2. `format_disk` 使用真实 namespace size 调用
   `format::compute_layout(opts, device.size_bytes())` 和 `validate_layout()`。
3. 默认 format 参数沿用当前 bootstrap profile 的 format-time knobs，但
   `data_area_base/end` 必须由 layout 计算，不从 profile 复制。
4. 对整盘执行 NVMe Dataset Management DEALLOCATE；默认要求设备声明
   deallocated logical block read value 为全零。
5. 在写 superblock 前做 read-zero 集成验证；验证失败时 fail-fast。
6. FUA 写 superblock A/B：A generation=1，B generation=0，root 为 null。
7. force-format 完成后返回 clean boot state，`build_runtime()` 直接使用该
   dynamic profile 和 clean recovered state。
8. 保持现有 YCSB CLI 兼容：`--force-format` 仍是唯一触发 destructive format 的入口。
9. 为后续 benchmark 输出保留 layout summary，但本步不定义 RocksDB benchmark 标准。

## 非目标

1. 不实现在线重新格式化，也不保留旧数据。
2. 不新增多盘、namespace partition、metadata redundancy 或格式版本迁移。
3. 不把 runtime topology、front count、cache policy 写入 superblock。
4. 不改变 WAL/tree/value on-disk object 格式。
5. 不实现 RocksDB 对比 harness；035 只清掉 Inconel 侧真实盘 format 阻塞。
6. 不为 YCSB 增加新的 workload distribution。
7. 不用 zero-write fallback 作为默认生产路径；fallback 只能显式 opt-in。

## 当前差距

| 领域 | 当前实现 | 问题 |
|---|---|---|
| Layout | `bootstrap_layout_for_device()` 复制 `kBootstrapFormatProfile` boundaries | 真实 namespace 剩余空间不可用 |
| Format I/O | 只 zero WAL 区 | Data Area 旧数据可能残留，ODF §7 读零前提未建立 |
| TRIM | sync recovery I/O 只有 read/write/flush/zero | 缺同步 Dataset Management DEALLOCATE helper |
| DLFEAT | `real_device` 不暴露 deallocate read value | 无法证明 trim 后读零 |
| Build handoff | force-format 后 `recovered_boot.reset()` | builder 回退 bootstrap profile，和新 superblock 不一致 |
| Diagnostics | force-format 不输出 layout 结果 | 长跑前难确认使用了整盘 |

## 目标 API

### Runtime format options

不要在 `format/` 层新增全局 `default_format_options()`。`format_options.hh` 已明确
format 层不提供共享默认 factory，避免测试 fixture 误用默认值掩盖 intent。

runtime 层可以新增自己的 production default：

```cpp
namespace apps::inconel::runtime {

struct format_disk_policy {
    bool require_deallocate = true;
    bool require_deallocate_read_zero = true;
    bool allow_zero_write_fallback = false;
    bool verify_zero_after_deallocate = true;
    uint64_t zero_verify_sample_lbas = 8;
    uint64_t trim_chunk_lbas = 0; // 0 = implementation default
};

struct format_disk_request {
    format::format_options options;
    format_disk_policy policy = {};
};

format::format_options default_format_options_for_runtime();

struct formatted_disk_state {
    format::layout_plan layout;
    format::format_profile profile;
    recovery::recovered_runtime_state clean_runtime;
    format::superblock_choice::source active_superblock_source =
        format::superblock_choice::source::a;
};

formatted_disk_state format_disk(nvme::real_device& device,
                                 uint32_t core,
                                 const format_disk_request& req);

}  // namespace apps::inconel::runtime
```

`default_format_options_for_runtime()` 只复制 format-time knobs：

- `lba_size`
- `tree_page_size`
- `shadow_slots_per_range`
- `value_class_count`
- `value_class_sizes`
- `wal_segment_size`
- `wal_segment_count`
- `value_space_quantum_bytes`
- `value_space_group_size_lbas`

它不得复制 `value_data_area_base/end`。这两个字段必须来自
`compute_layout()`.

### Layout to profile

新增一个小 helper，把已验证的 `layout_plan` 转成 runtime/recovery profile：

```cpp
format::format_profile profile_from_layout(const format::layout_plan& L);
```

该 helper 应放在 runtime format helper 或 recovery bootstrap helper 附近，避免
`format_profile` 重新变成 format 层的 layout source of truth。

### Clean boot state

fresh format 的 runtime state 应显式构造，而不是再跑完整 recovery：

```cpp
recovery::recovered_runtime_state clean_recovered_state_from_layout(
    const format::layout_plan& L,
    format::superblock_choice::source source);
```

字段语义：

| 字段 | fresh format 值 |
|---|---|
| `tree` | empty tree snapshot |
| `live_value_extents` | empty |
| `tree_free_ranges` | empty |
| `recovered_durable_lsn` | 0 |
| `next_lsn` | 1 |
| `tree_alloc_head_lba` | `layout.data_area_base_paddr.lba` |
| `active_superblock_source` | A |

这样 `install_recovered_runtime_state()` 会把 tree allocator head、
value space manager 和 superblock slot 都初始化到和盘面一致的 clean state。

## 设备能力 API

`nvme::real_device` 需要暴露两个只读 capability：

```cpp
bool namespace_supports_deallocate() const noexcept;
bool deallocate_reads_zero() const noexcept;
```

建议实现：

- `namespace_supports_deallocate()` 使用 `spdk_nvme_ns_get_flags(ns)` 检查
  `SPDK_NVME_NS_DEALLOCATE_SUPPORTED`。
- `deallocate_reads_zero()` 使用
  `spdk_nvme_ns_get_dealloc_logical_block_read_value(ns)`，要求返回
  `SPDK_NVME_DEALLOC_READ_00`。

如果 SPDK / device 报告不支持 deallocate 或不保证读零：

- 默认 fail-fast，错误信息必须包含 BDF、namespace size、sector size 和失败 capability。
- 只有 `allow_zero_write_fallback=true` 时才允许整盘 zero-write fallback。
- fallback 必须在日志/输出里显式标记为慢路径，不能静默冒充 trim。

## 同步 TRIM helper

在 `recovery/sync_io.hh` 增加 real-device 同步 trim：

```cpp
void sync_trim_logical_lbas(nvme::real_device& device,
                            uint32_t core,
                            uint64_t logical_lba,
                            uint64_t logical_lba_count,
                            uint32_t logical_lba_size);
```

映射规则与现有 read/write 相同：

```text
sectors_per_lba = logical_lba_size / device.sector_size()
ns_lba          = logical_lba * sectors_per_lba
sector_count    = logical_lba_count * sectors_per_lba
```

实现应按 `uint32_t` DSM range length 分块，必要时每次提交一个
`spdk_nvme_dsm_range`。后续可以批量多 range，但 035 不需要为了整盘 format
先做复杂 range packing。

非 real-device 的 template fake 可以暂不提供通用 trim；测试需要 fake 时再按
现有 `sync_read/write/flush` 模式补。

## 格式化流程

`format_disk()` 的顺序固定如下：

1. 构造 `format_options`。
2. `layout = compute_layout(options, device.size_bytes())`。
3. `validate_layout(layout)`。
4. 校验 `layout.lba_size % device.sector_size() == 0`，并确认
   `device.total_logical_lbas(layout.lba_size) == layout.total_lbas`。
5. 根据 policy 检查 deallocate/read-zero capability。
6. 清盘：
   - 默认：`sync_trim_logical_lbas(device, core, 0, layout.total_lbas, layout.lba_size)`。
   - 显式 fallback：`sync_zero_logical_lbas(... whole namespace ...)`。
7. read-zero 验证：
   - 在写 superblock 前执行。
   - 至少覆盖 LBA 0、LBA 1、WAL base、WAL tail、Data Area base、Data Area tail。
   - 大盘额外用 deterministic stride 采样，采样数由 policy 控制。
   - 验证失败直接抛错，不继续写 superblock。
8. 构造 superblock：
   - A：`build_superblock(layout, 1)`
   - B：`build_superblock(layout, 0)`
9. FUA 写 A 到 LBA 0，FUA 写 B 到 LBA 1。
10. `sync_flush(device, core)`。
11. 可选读回 A/B，使用 `choose_newer_superblock()` 确认 A 被选中。
12. 返回 `formatted_disk_state{layout, profile, clean_runtime}`。

### 为什么 A generation=1 / B generation=0

这沿用 ODF §7，恢复时 A/B helper 会选择 generation 更大的 A。
B 仍写入有效 superblock，目的是让两个槽都处于可解析状态；后续 root-change flush
按 inactive slot 轮换即可。

### 为什么不在 format 后跑完整 recovery

fresh format 后盘面语义完全由刚写入的 superblock 和 all-zero data/WAL area 决定。
再跑完整 recovery 会扫描大量空 WAL/tree 区，对 YCSB 长跑启动没有价值；更重要的是
format 已经持有 `layout_plan`，直接构造 clean runtime state 可以避免
“superblock 是动态的、builder 却拿 nullptr profile 回退 bootstrap”的当前错误。

## Session 集成

`db_session::format_or_recover()` 改成两条路径都填充 `recovered_boot`：

```cpp
if (opts.boot_mode == db_boot_mode::force_format) {
    auto formatted = format_disk(*device, opts.topology.main_core, req);
    recovered_boot.emplace(recovery::recovered_boot_state{
        .profile = formatted.profile,
        .tree_geometry = recovery::tree_geometry_from_profile(formatted.profile),
        .superblock_source = formatted.active_superblock_source,
        .superblock_generation = 1,
        .runtime_state = std::move(formatted.clean_runtime),
    });
    return;
}

recovered_boot.emplace(recover_empty_clean_boot(...));
```

`make_build_options()` 保持现有逻辑：只要 `recovered_boot.has_value()`，
`disk_profile` 和 `recovered_state` 都传给 builder。

这会让 force-format 和 recover 两条启动路径在 builder 视角收敛：

```text
open_device
  -> force_format: format_disk -> clean recovered_boot
  -> recover:      recover_empty_clean_boot -> recovered_boot
  -> build_runtime(profile, recovered_state)
```

## YCSB / CLI 行为

本步保持现有 CLI / JSON schema：

- `--force-format` / `device.force_format=true` 触发 production `format_disk()`。
- 未指定 format knobs 时，使用 runtime default options。
- `--dry-run` 不触发 destructive format。
- `--no-force-format` 仍走 recovery。

建议同时补充输出字段，供长跑前确认：

```text
format.lba_size
format.total_lbas
format.wal_base_lba
format.wal_segment_size
format.wal_segment_count
format.data_area_base_lba
format.data_area_end_lba
format.data_area_bytes
format.clear_method = deallocate|zero-write-fallback
```

这些字段可以先只在 force-format 成功后打印；后续 benchmark harness 再把它们纳入
机器可读结果。

## 验证计划

### Static / deterministic

1. `default_format_options_for_runtime()` 不复制 data-area boundaries。
2. 小 fake namespace 下 `compute_layout` 得到 `data_area_end == total_lbas`。
3. `profile_from_layout` 的所有字段与 superblock builder 一致。
4. fresh `clean_recovered_state_from_layout` 满足 `build_runtime` 的 recovered_state
   preconditions。
5. negative cases：
   - namespace size 不是 lba_size 整数倍。
   - WAL 区超过 namespace。
   - deallocate unsupported 且 fallback disabled。
   - deallocate read value 不是 zero 且 fallback disabled。

### Real NVMe smoke

使用 `build_real` 和 scratch BDF：

```bash
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_ycsb \
  --config apps/inconel/ycsb/config.sample.json \
  --force-format \
  --workload load-a \
  --records 1000 \
  --operations 1000 \
  --verify-samples 32
```

然后不 force-format 重启：

```bash
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_ycsb \
  --config apps/inconel/ycsb/config.sample.json \
  --workload c \
  --records 1000 \
  --operations 1000 \
  --verify-samples 32
```

### Capacity guard

至少跑一个 records/value footprint 会超过旧 bootstrap Data Area 的场景，确认：

- force-format 输出的 `data_area_end_lba` 等于真实 namespace logical LBA 数。
- workload 不因旧 `value_data_area_end=100000` 撞墙。
- restart recovery 读取到同一 dynamic profile。

具体大规模参数应放到后续 benchmark 标准文档里；035 只要求证明容量上限不再被
bootstrap profile 固定。

## 实现顺序

1. **Format state helpers**
   - runtime default format options
   - `profile_from_layout`
   - `clean_recovered_state_from_layout`

2. **Device capability + sync trim**
   - `real_device` deallocate/read-zero accessors
   - `recovery::sync_trim_logical_lbas`
   - read-zero sample verifier

3. **Production format**
   - 新 `format_disk()`
   - 旧 `force_format_device()` 改成 compatibility wrapper 或直接删除 public 使用点
   - wrapper 若保留，名字必须说明 destructive production format，不再叫 bootstrap

4. **Session handoff**
   - force-format 后填充 `recovered_boot`
   - build_runtime 使用 dynamic profile

5. **YCSB diagnostics**
   - force-format 成功输出 layout summary
   - 更新 real NVMe guide 中关于 `--force-format` 的说明

6. **Known issue closeout**
   - 验证通过后更新 `known_issues.md` 中 `INC-035` 状态。

## 并发推进方式

这一步可以并发做，但编辑边界要分开：

| Workstream | 范围 | 串行化点 |
|---|---|---|
| A: format/runtime helpers | `runtime/format_device.hh`, `format/layout_plan` adjacent helpers | 与 C 都会碰 `format_device.hh`，需串行合并 |
| B: device capability + sync trim | `nvme/real_device.hh`, `recovery/sync_io.hh` | 可与 A 并行 |
| C: session/YCSB handoff | `runtime/session.hh`, YCSB output | 等 A 的 return type 稳定 |
| D: docs/tests | real NVMe guide, deterministic tests | 等 A/B/C API 稳定 |

真实 NVMe smoke 必须串行，不能和其它 real-NVMe 测试抢同一个 scratch BDF。

## 风险与处理

### 设备不保证 deallocate read-zero

风险：ODF §7 的 all-zero invariant 无法由 TRIM 建立。

处理：默认 fail-fast；只有显式开启 zero-write fallback 才整盘写零。fallback 慢但语义
正确，适合临时开发，不适合作为 benchmark 默认路径。

### 整盘 zero fallback 太慢

风险：用户误以为格式化卡死。

处理：fallback 必须显式 opt-in，并输出 progress 或至少输出 fallback mode。035 首轮可以
不做 progress，但不能静默。

### force-format 和 recover profile 分叉

风险：写盘是 dynamic layout，runtime 仍用 bootstrap profile。

处理：force-format 后必须填 `recovered_boot`；把这个作为 035 的验收门槛。

### `format_profile` 继续承担 layout source

风险：重新引入 `value_data_area_end` 固定 profile 漂移。

处理：format 命令只把 profile 当 runtime/recovery carrier；layout source 永远是
`format_options + namespace_size -> layout_plan`。

## 完成标准

`INC-035` 可关闭当且仅当：

1. `--force-format` 写入的 superblock `data_area_end_paddr.lba == namespace_size / lba_size`。
2. force-format 后首次 runtime 使用 dynamic profile，而不是 `kBootstrapFormatProfile`。
3. 整盘清理默认走 Dataset Management DEALLOCATE，并检查 deallocated read-zero capability。
4. read-zero verifier 覆盖 superblock 写入前的 WAL/Data Area 关键点。
5. A/B superblock 可由 `choose_newer_superblock()` 选出 A。
6. YCSB `load-a` real NVMe smoke 通过，随后 no-force restart 的 read smoke 通过。
7. 文档更新说明 `kBootstrapFormatProfile` 只剩 default format knobs 模板，不再是 public
   force-format 容量上限。
