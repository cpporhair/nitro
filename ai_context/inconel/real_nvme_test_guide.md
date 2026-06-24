# Inconel Real NVMe Test Guide

This is the runbook for destructive Inconel e2e tests on the local scratch
NVMe device. Read this before running any `apps/inconel/test/*_e2e.cc`
binary on real hardware.

## Do Not Use `build/` For Real NVMe

`build/` may link against system SPDK/DPDK under `/usr/lib`. On this host that
path has already failed with:

- `DPDK version 25.11.1 is not supported`
- `Cannot use IOVA as 'PA' since physical addresses are not available`
- `Error creating '/run/user/1000/dpdk': Read-only file system`

Use `build_real/`. It is configured for the vendored SPDK tree under
`/home/null/work/kv/spdk`.

## Current Scratch Device

Known-good scratch BDF on this machine:

```bash
0000:04:00.0
```

Do not use `0000:03:00.0`; it has mounted system partitions. Before any
destructive test, confirm the target is bound to `vfio-pci` and is not mounted:

```bash
sudo -n /home/null/work/kv/spdk/scripts/setup.sh status
```

Expected relevant shape:

```text
0000:03:00.0 ... Active devices: mount@...
0000:04:00.0 ... vfio-pci
```

If the scratch device is not bound, bind only the intended BDF:

```bash
sudo -n env PCI_ALLOWED="0000:04:00.0" HUGEMEM=512 /usr/bin/spdk-setup config
```

The setup script only handles VFIO and hugepages. It does not select the
runtime SPDK/DPDK libraries for the test process.

The 066C YCSB concurrency checker has an additional positive allowlist. It
allows `0000:04:00.0` by default; if a different scratch BDF is intentionally
used, pass `INCONEL_ALLOWED_SCRATCH_BDF=<BDF>` through `sudo env` only after
the status check above confirms that the BDF is not mounted and not
`0000:03:00.0`.

## Build

Rebuild real-NVMe targets from `build_real/`, not `build/`:

```bash
cmake --build build_real --target \
  inconel_ycsb \
  inconel_test_steady_e2e \
  inconel_test_concurrent_runtime_e2e \
  inconel_test_ycsb_concurrency_checker_e2e \
  inconel_test_flush_e2e \
  inconel_test_value_placement_e2e \
  inconel_test_write_backpressure_e2e \
  inconel_test_multishard_split_e2e \
  inconel_test_seal_inflight_race_e2e \
  -j2
```

If `build_real/` is missing or has stale cache, configure it with the vendored
pkg-config paths:

```bash
PKG_CONFIG_PATH=/home/null/work/kv/spdk/build/lib/pkgconfig:/home/null/work/kv/spdk/dpdk/build/lib/pkgconfig \
cmake -B build_real -DCMAKE_BUILD_TYPE=Release
```

Then rebuild the target set above.

## Verify Linkage

Before running, check that the binary resolves SPDK and DPDK from
`/home/null/work/kv/spdk`, not `/usr/lib`:

```bash
ldd build_real/inconel_test_steady_e2e | rg "spdk|rte|not found"
```

Expected:

```text
libspdk_*.so => /home/null/work/kv/spdk/build/lib/...
librte_*.so  => /home/null/work/kv/spdk/dpdk/build/lib/...
```

Wrong:

```text
libspdk_*.so => /usr/lib/...
librte_*.so  => /usr/lib/...
```

## Runtime Environment

Always run real-NVMe e2e with `sudo` and preserve the vendored library path:

```bash
export INCONEL_REAL_NVME_LIBS=/home/null/work/kv/spdk/build/lib:/home/null/work/kv/spdk/dpdk/build/lib
```

Use `XDG_RUNTIME_DIR=/tmp` to avoid inheriting a desktop user runtime dir into
DPDK:

```bash
sudo -n env \
  XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_test_steady_e2e \
  --pci-addr 0000:04:00.0
```

`sudo` matters. Running as the desktop user can fail before Inconel starts
because DPDK cannot access physical addresses for default `IOVA=PA`.

## Common Test Commands

Steady e2e:

```bash
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_test_steady_e2e \
  --pci-addr 0000:04:00.0
```

Concurrent runtime e2e:

```bash
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_test_concurrent_runtime_e2e \
  --pci-addr 0000:04:00.0
```

YCSB concurrency checker e2e:

```bash
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_test_ycsb_concurrency_checker_e2e \
  --pci-addr 0000:04:00.0 \
  --scenario c1

sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_test_ycsb_concurrency_checker_e2e \
  --pci-addr 0000:04:00.0 \
  --scenario c2

sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_test_ycsb_concurrency_checker_e2e \
  --pci-addr 0000:04:00.0 \
  --scenario c3

sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_test_ycsb_concurrency_checker_e2e \
  --pci-addr 0000:04:00.0 \
  --scenario c4

sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_test_ycsb_concurrency_checker_e2e \
  --pci-addr 0000:04:00.0 \
  --scenario c5

sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_test_ycsb_concurrency_checker_e2e \
  --pci-addr 0000:04:00.0 \
  --scenario c6
```

Flush e2e:

```bash
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_test_flush_e2e \
  --pci-addr 0000:04:00.0 \
  --force-format \
  --num-keys 1000 \
  --rounds 3 \
  --readback-samples 50
```

Other Gap-A e2e binaries follow the same pattern:

```bash
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/<binary> \
  --pci-addr 0000:04:00.0
```

YCSB config-based commands:

`apps/inconel/ycsb/config.sample.json` is the default YCSB real-NVMe sample.
It names the scratch BDF `0000:04:00.0` and keeps `device.force_format=false`
so opening the config is not destructive by itself. Use CLI overrides for
per-run workload size and explicit destructive format requests; CLI values win
over the JSON file. If overriding the BDF with `--pci`, only use the scratch
device and never `0000:03:00.0`.

As of 069 / INC-035, `--force-format` and `device.force_format=true` run the
production full-device format path. It clears the whole namespace, derives the
Inconel layout from the live namespace capacity, and writes fresh superblock
A/B before the runtime starts. Treat it as destructive for every LBA on the
target namespace. The current YCSB config output exposes whether force-format
is enabled, but the per-run result does not yet print a format layout summary
such as `data_area_end_lba` or `clear_method`.

YCSB config dry-run:

```bash
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_ycsb \
  --config apps/inconel/ycsb/config.sample.json \
  --dry-run \
  --records 1000 \
  --operations 1000 \
  --verify-samples 32
```

YCSB execution entry smoke:

```bash
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_ycsb \
  --config apps/inconel/ycsb/config.sample.json \
  --force-format \
  --records 1000 \
  --operations 1000 \
  --verify-samples 32
```

Mixed read/update smoke:

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

Long `load + workload a` run:

```bash
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 12h build_real/inconel_ycsb \
  --config apps/inconel/ycsb/config.sample.json \
  --force-format \
  --workload load-a \
  --records 1000000 \
  --operations 1000000 \
  --value-size 256 \
  --batch-size 1 \
  --inflight 64 \
  --seed 1 \
  --verify-samples 1024
```

If the next run is meant to verify recovery over the just-written data, omit
`--force-format`; otherwise it will intentionally clear the device again.

For RocksDB comparison, keep more than the throughput line. Record the exact
Inconel commit, build directory, BDF, namespace capacity, full
`inconel_ycsb --print-config` output, workload kind, records, operations,
value size, batch size, inflight, seed, load/run throughput, error counters,
and maintenance stats. The RocksDB run must use the same key/value generator
settings and the same client pressure sweep, otherwise the numbers are not
comparable.

YCSB expected-state oracle smoke:

```bash
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_ycsb \
  --config apps/inconel/ycsb/config.sample.json \
  --force-format \
  --workload load-a \
  --records 1000 \
  --operations 1000 \
  --inflight 1 \
  --expect-all \
  --write-expect-file /tmp/inconel_ycsb_expected.json
```

Then restart without formatting and verify the persisted expected state:

```bash
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_ycsb \
  --config apps/inconel/ycsb/config.sample.json \
  --workload c \
  --records 1000 \
  --operations 1000 \
  --expect-file /tmp/inconel_ycsb_expected.json \
  --expect-all
```

For mutating workloads, expected-state oracle mode intentionally requires
`--inflight 1`. Concurrent mutation consistency belongs to the 066C interval
checker, not this phase-level YCSB oracle.

YCSB consistency suite:

`apps/inconel/scripts/ycsb_consistency.sh` runs the 066 real-NVMe consistency
suite serially, checks the scratch BDF safety precondition, rejects the known
system disk BDF, and parses YCSB counters. Use individual scenarios while
debugging and `all` only when the device can be destructively reused for the
full sequence:

```bash
apps/inconel/scripts/ycsb_consistency.sh a0
apps/inconel/scripts/ycsb_consistency.sh a5
apps/inconel/scripts/ycsb_consistency.sh a10
apps/inconel/scripts/ycsb_consistency.sh c1
apps/inconel/scripts/ycsb_consistency.sh c2
apps/inconel/scripts/ycsb_consistency.sh c3
apps/inconel/scripts/ycsb_consistency.sh c4
apps/inconel/scripts/ycsb_consistency.sh c5
apps/inconel/scripts/ycsb_consistency.sh c6
apps/inconel/scripts/ycsb_consistency.sh c7
apps/inconel/scripts/ycsb_consistency.sh c8
apps/inconel/scripts/ycsb_consistency.sh all
```

The default scratch BDF is `0000:04:00.0`. To override it, set
`INCONEL_YCSB_BDF`, but never set it to `0000:03:00.0`.
The script also takes a per-BDF `flock`, checks `maintenance.failed=0` for
YCSB real runs, checks `checker_maintenance.failed=0` for C2/C3/C5/C6, requires
`checker_maintenance.seal > 0` for C2, and requires
`checker_maintenance.flush/non_noop_flush > 0` for C3/C5/C6. C5 also requires
`checker_barrier.reads=4096`; C6 requires
ACK-immediate `checker_barrier.reads=64`,
`checker_frontier_barrier.reads=64`, `generation=2`, and
`checker_frontier_window.reads>0`. C7/C8 are scripted recovery-continuation
scenarios with exact expected-state oracle files: C7 verifies update winners
after existing-tree + WAL-delta recovery, then a post-recovery full PUT and
second restart; C8 verifies tombstone recovery, post-tombstone PUT, and a
second restart.

## Maintenance Cadence Tests

061 enables production maintenance by default. When a test uses the default
runtime options, it must not also drive `rt::reclaim_once()` manually for the
same runtime. Let the production maintenance scheduler own reclaim/trim cadence
and assert its `maintenance_stats_snapshot` counters instead.

For older deterministic tests that manually sequence `seal_once()` /
`flush_once()` / `reclaim_once()` through `runtime::build_runtime`, explicitly
disable automatic maintenance in the runtime build options:

```cpp
.maintenance = {
    .enabled = false,
},
```

Some older e2e tests, such as `inconel_test_steady_e2e`, hand-build their
topology instead of calling `runtime::build_runtime`; those tests do not install
the 061 scheduler unless they add it explicitly. Keep their manual drain helper
local to the test.

Do not work around reclaim races by adding another production root `submit(...)`
path in a test or helper. That recreates the async fragmentation 061 is
intended to remove.

## Troubleshooting

`DPDK version 25.11.1 is not supported`

The process loaded system DPDK from `/usr/lib`. Use `build_real/` and pass
`LD_LIBRARY_PATH` through `sudo env`.

`Cannot use IOVA as 'PA' since physical addresses are not available`

The process is not running with the privileges DPDK expects. Run with
`sudo -n env ...`.

`Error creating '/run/user/1000/dpdk': Read-only file system`

The process inherited the desktop user `XDG_RUNTIME_DIR`. Run with
`XDG_RUNTIME_DIR=/tmp`.

`--pci-addr BDF or INCONEL_NVME_PCI_ADDR is required`

For YCSB, pass `--config apps/inconel/ycsb/config.sample.json` or override the
sample with `--pci 0000:04:00.0`. For older e2e binaries, pass
`--pci-addr 0000:04:00.0` explicitly. Do not rely on a shell-local env var
unless it is passed through `sudo env`, and do not use `0000:03:00.0`.

SPDK cannot find the NVMe controller

Run `sudo -n /home/null/work/kv/spdk/scripts/setup.sh status` and confirm the
BDF is on `vfio-pci`. Re-run the `PCI_ALLOWED=... spdk-setup config` command
if needed.

Dynamic library `not found`

Check `ldd` and rebuild `build_real/`. Do not patch around missing libraries
with symlinks.

## Historical Notes

Previous successful real-NVMe validations used:

- scratch device `0000:04:00.0`
- `build_real/`
- vendored SPDK/DPDK under `/home/null/work/kv/spdk`
- `sudo -n env LD_LIBRARY_PATH=/home/null/work/kv/spdk/build/lib:/home/null/work/kv/spdk/dpdk/build/lib ...`

See also:

- `ai_context/inconel/known_issues.md`, resolved `INC-058`
- `ai_context/inconel/plan/059_gap_a_e2e_coverage_plan.md`, section 3 and 5
- `ai_context/inconel/plan/038_pump_nvme_lba_page_adapter.md`, Real NVMe Validation
