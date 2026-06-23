# 065: Inconel YCSB 配置系统设计

## 背景

当前 `inconel_ycsb` 只有手写 CLI 参数解析。随着 recovery boot、runtime builder、cache policy、maintenance 与真实 NVMe smoke 逐步接入，单次命令行已经承载了太多静态拓扑和运行参数：

- 设备 BDF、SPDK core mask、qpair depth
- runtime core 拓扑与各 owner core
- cache policy 与容量
- maintenance seal / memtable / WAL 阈值
- workload、verify、flush-after-load 等测试语义

这导致真实设备验证很容易依赖临时长命令，复现实验和审查运行参数都困难。064 recovery 完成后，下一步需要把 YCSB 入口补成可复现、可校验、可 dry-run 的配置系统。

## 参考实现结论

### Sider

相关文件：

- `apps/sider/config.hh`
- `apps/sider/main.cc`
- `apps/sider/sider.json`
- `apps/sider/scripts/bench.sh`

可复用的设计点：

- 使用 `nlohmann::json` 读取配置文件。
- 支持 `--config`，同时保留 CLI-only fallback，方便短 smoke 命令。
- 运行前打印 effective config，避免真实设备验证时不知道实际用了哪些参数。
- 支持 `512M`、`8G` 这类内存字符串。
- benchmark 脚本把常用场景固化下来，避免依赖口头约定和临时命令。

不直接照搬的点：

- Sider 的配置较小，直接平铺字段可以接受；Inconel YCSB 需要按 `device`、`runtime`、`maintenance`、`workload`、`verification` 分组。

### KV

相关文件：

- `apps/kv/runtime/config.hh`
- `apps/kv/senders/start_db.hh`
- `apps/kv/config.json`

可复用的设计点：

- 配置文件是启动入口的必需输入，拓扑和 IO 参数在初始化前一次性确定。
- core role、NVMe qpair、FUA 等运行环境参数都属于配置，而不是散落在启动代码里。
- 初始化阶段根据配置派生 core mask 和环境参数。

不直接照搬的点：

- KV 使用 `boost::property_tree`，Inconel 应统一采用 Sider/AiSAQ 已经使用的 `nlohmann::json`。
- KV 的全局 mutable config 风格不适合继续扩大，Inconel YCSB 应保留现有 `ycsb::config` 值对象，再传入 runner/runtime。

### AiSAQ

相关文件：

- `apps/aisaq/runtime/config.hh`
- `apps/aisaq/main.cc`
- `apps/aisaq/config.json`
- `apps/aisaq/config_build.json`

可复用的设计点：

- 配置文件描述静态 topology，命令行描述 mode 和少量运行期输入。
- JSON 内有 named NVMe device，并对 core 和 device 引用做早期校验。
- `main_core` 默认到第一颗 core，但要求必须在 core 列表内。
- build/search/cache 分组清晰，避免把所有参数堆到顶层。

不直接照搬的点：

- AiSAQ 是多 mode 二级命令。Inconel YCSB 当前 workload 已在参数中表达，本阶段不引入新的子命令 API。

## 目标

1. 为 `inconel_ycsb` 增加 JSON config 文件入口。
2. 保留当前 CLI 参数兼容性，现有 smoke 命令不需要重写。
3. 明确 defaults、config file、environment、CLI 的优先级。
4. 提供 effective config 打印和 dry-run，真实设备验证前可以不打开 NVMe 先检查配置。
5. 所有配置错误 fail-fast，错误信息带字段路径。
6. 禁止把磁盘格式内部参数伪装成普通运行配置。

## 非目标

- 不扩展 public KV API，不增加 MultiGet、scan、RMW 或事务接口。
- 不实现新的 workload distribution，例如 Zipfian、range scan 或读写混合脚本。
- 不把 `format::profile`、tree page size、WAL layout、value class sizes 暴露成 JSON 字段。
- 不在配置系统里重建 existing tree，不改变 064 recovery 语义。
- 不把 dead hints、Value Area 扫描、scratch rebuild 作为 recovery correctness 的输入。

## 配置入口

### CLI 兼容

新增：

- `--config <path>`：读取 JSON 配置。
- `--print-config`：打印 human-readable effective config。
- `--dump-config`：打印 canonical JSON effective config。
- `--dry-run`：解析、合并、校验并打印配置后退出，不打开 NVMe。

保留当前全部 CLI 参数：

- `--pci`
- `--force-format`
- `--workload`
- `--records`
- `--operations`
- `--value-size`
- `--batch-size`
- `--inflight`
- `--seed`
- `--key-prefix`
- `--flush-after-load`
- `--verify-samples`
- `--verify-existing-updates`
- `--verify-existing-deletes`
- `--cores`
- `--front-cores`
- `--main-core`
- `--value-core`
- `--owner-core`
- `--coord-core`
- `--wal-space-core`
- `--maintenance-core`
- `--maintenance-seal-active-bytes`
- `--maintenance-total-memtable-bytes`
- `--maintenance-wal-seal-percent`
- `--maintenance-max-sealed-gens-per-front`
- `--tree-cache-policy`
- `--tree-cache-capacity`
- `--value-cache-policy`
- `--value-cache-capacity`
- `--spdk-core-mask`
- `--qpair-depth`

当前 help 漏列了 `--spdk-core-mask`、`--qpair-depth`、`--key-prefix`。实现配置系统时必须顺手修正 help，否则 operator 仍然无法发现完整配置面。

### 合并优先级

优先级从低到高：

1. hardcoded defaults
2. JSON config file
3. environment fallback
4. CLI overrides

环境变量本阶段只保留当前兼容项：

- `INCONEL_NVME_PCI_ADDR`

该环境变量只在 `device.pci_addr` 仍为空时生效。也就是说，如果 JSON 或 CLI 已经显式设置 BDF，环境变量不得覆盖它。

CLI 永远是最高优先级。典型用途是共用一份 config 文件，只临时覆盖 `--pci`、`--records` 或 `--force-format`。

## JSON schema

建议样例：

```json
{
  "device": {
    "pci_addr": "0000:04:00.0",
    "force_format": false,
    "spdk_core_mask": "",
    "qpair_depth": 128
  },
  "runtime": {
    "cores": [0, 1, 2, 3],
    "main_core": 0,
    "front_cores": [0, 1],
    "value_core": 0,
    "owner_core": 0,
    "coord_core": 0,
    "wal_space_core": 0,
    "maintenance_core": 0,
    "tree_cache": {
      "policy": "clock",
      "capacity": 1024
    },
    "value_cache": {
      "policy": "clock",
      "capacity": 4096
    }
  },
  "maintenance": {
    "seal_active_bytes": "256M",
    "total_memtable_bytes": "1G",
    "wal_seal_percent": 70,
    "max_sealed_gens_per_front": 4
  },
  "workload": {
    "kind": "load-c",
    "records": 10000,
    "operations": 10000,
    "value_size": 256,
    "batch_size": 1,
    "inflight": 64,
    "seed": 1,
    "key_prefix": "user",
    "flush_after_load": false
  },
  "verification": {
    "samples": 0,
    "existing": "none"
  },
  "output": {
    "print_config": true
  }
}
```

### Field mapping

`device`:

- `pci_addr` maps to `config::pci_addr`.
- `force_format` maps to `config::force_format`.
- `spdk_core_mask` maps to `config::spdk_core_mask`.
- `qpair_depth` maps to `config::qpair_depth`.

`runtime`:

- `cores` maps to `config::cores`.
- `front_cores` maps to `config::front_cores`.
- `main_core` maps to `config::main_core`.
- role core fields map one-to-one to existing optional role core fields.
- `tree_cache.policy` and `tree_cache.capacity` map to tree cache config.
- `value_cache.policy` and `value_cache.capacity` map to value cache config.

`maintenance`:

- `seal_active_bytes` maps to `maintenance_seal_active_bytes`.
- `total_memtable_bytes` maps to `maintenance_total_memtable_bytes`.
- `wal_seal_percent` maps to `maintenance_wal_seal_percent`.
- `max_sealed_gens_per_front` maps to `maintenance_max_sealed_gens_per_front`.

`workload`:

- `kind` accepts current workload names: `load`, `a`, `b`, `c`, `update`, `del`, `load-a`, `load-b`, `load-c`.
- underscore aliases such as `load_a` may be accepted for CLI backward compatibility, but canonical dumped JSON should use hyphen names.
- numeric workload fields map one-to-one to current CLI fields.

`verification`:

- `samples` maps to `verify_samples`.
- `existing` accepts `none`, `updates`, `deletes`.
- `existing=updates` maps to `verify_existing_updates=true`.
- `existing=deletes` maps to `verify_existing_deletes=true`.

`output`:

- `print_config` controls whether to print the human summary before opening NVMe.
- `dump_config` may be added later, but CLI `--dump-config` is enough for the first implementation.

## Validation rules

Parsing and validation happen before any SPDK setup or NVMe open.

Required effective fields:

- `device.pci_addr` must be non-empty after defaults, config, env and CLI are merged.
- `runtime.cores` must be non-empty.

Device validation:

- Reject `0000:03:00.0`, which is the known system disk BDF in this environment.
- `qpair_depth` must be positive.
- `spdk_core_mask` may be empty; empty means derive from core config as today.

Core validation:

- `runtime.cores` cannot contain duplicates.
- `runtime.front_cores`, when non-empty, must be a subset of `runtime.cores`.
- `main_core` and all non-negative role cores must be present in `runtime.cores`.
- If `runtime.front_cores` is empty, the runtime builder may keep the existing default front ownership behavior.

Workload validation:

- `records`, `operations`, `value_size`, `batch_size`, `inflight` must be positive where applicable.
- `value_size` must fit the bootstrap value class profile currently used by `format_device.hh`.
- `verification.existing` cannot be both updates and deletes.
- `verification.existing != none` requires `verification.samples > 0`.
- Existing update/delete verification remains limited to workload `c`, matching current runner semantics.
- Existing delete verification requires enough operations to cover the sampled records, matching the current `operations >= records` check.

Maintenance validation:

- `wal_seal_percent` must be in `[1, 100]`.
- byte fields must be positive.
- byte fields accept either JSON unsigned integers or strings using Sider-style suffixes: `K`, `M`, `G`, `T`.

JSON validation:

- Unknown keys are errors.
- Wrong JSON types are errors.
- Error messages must include a dotted field path, for example `runtime.cores[2]` or `maintenance.seal_active_bytes`.
- Duplicated semantic fields through aliases should be rejected in JSON. For example, do not allow both `workload.kind` and `workload.workload`.

Exit codes:

- `0`: successful run or successful dry-run.
- `1`: runtime, IO, workload, or verification failure.
- `2`: configuration parse or validation failure.

## Disk format boundary

Configuration must not expose disk format internals.

Allowed:

- `device.force_format` can request the existing bootstrap formatting path.
- no-force boot uses the recovered disk profile and recovered runtime state from the device.

Forbidden in JSON:

- tree page size
- LBA size
- superblock layout values
- WAL layout or WAL region sizing
- value class sizes
- value area region boundaries
- recovered LSN overrides
- manual live/dead ref counters

Reason:

064 recovery relies on the durable disk profile and recovered frontier. Letting a runtime config override those values would make recovery non-reproducible and could accidentally pair a tree snapshot with incompatible WAL/value metadata.

## Implementation plan

### Phase B1: Config file loader

Files:

- `apps/inconel/ycsb/config.hh`

Work:

- Add `nlohmann::json` based loader.
- Add a first-pass CLI scan for `--config`, `--help`, `--dry-run`, `--print-config`, `--dump-config`.
- Load defaults, then JSON, then env fallback, then CLI overrides.
- Keep the existing `config` struct as the effective config value.
- Keep all existing CLI option names.

### Phase B2: Printing and dry-run

Files:

- `apps/inconel/ycsb/config.hh`
- `apps/inconel/ycsb/main.cc`

Work:

- Add human-readable effective config print, similar to Sider.
- Add canonical JSON dump.
- Add dry-run path that returns before `nvme::real_device` construction.
- Update usage text to list all supported options.

### Phase B3: Sample config and docs

Files:

- `apps/inconel/ycsb/config.sample.json`
- `ai_context/inconel/real_nvme_test_guide.md`

Work:

- Add a scratch-NVMe sample that uses `0000:04:00.0`.
- Update real NVMe guide examples to prefer `--config` plus small CLI overrides.
- Keep warnings against `0000:03:00.0`.

### Phase B4: Regression checks

Minimum checks:

- `git diff --check`
- build `inconel_ycsb`
- `inconel_ycsb --help`
- `inconel_ycsb --config <sample> --dry-run`
- config value overridden by CLI, for example JSON `records=100` plus CLI `--records 200`
- unknown JSON key fails with exit code `2`
- system disk BDF fails with exit code `2`

Existing review gates after implementation:

- hidden root submit gate
- virtual/override gate

Real device smoke after config implementation:

- Read `ai_context/inconel/real_nvme_test_guide.md`.
- Confirm scratch device state.
- Use only the scratch BDF, not `0000:03:00.0`.
- Run an empty clean boot smoke with config.
- Run WAL-only no-force fail-fast smoke with config.
- Run the smallest full load/read config smoke after recovery path is ready.

## Adjacent follow-ups

These are better after the config system lands:

- Add post-run verification controls to config. Current YCSB verifies before run for existing-data scenarios; full read/write correctness wants a post-run verification mode.
- Add workload presets or benchmark scripts for fixed real-NVMe scenarios, following Sider's `bench.sh` pattern.
- Add structured JSON metrics output if repeated benchmark comparison becomes a priority.
- Add more workload distributions only after current YCSB semantics are complete and reproducible.

## Open decisions

1. `--print-config` default should likely be enabled for real NVMe runs. If output noise becomes an issue, add `--quiet-config` later instead of hiding config by default.
2. Canonical dump should use hyphenated workload names even if CLI accepts underscore aliases.
3. Named NVMe devices, like AiSAQ, are not needed for single-device YCSB today. Add them only if Inconel grows multi-device YCSB configuration.
