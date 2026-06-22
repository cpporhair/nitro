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

## Build

Rebuild real-NVMe targets from `build_real/`, not `build/`:

```bash
cmake --build build_real --target \
  inconel_test_steady_e2e \
  inconel_test_concurrent_runtime_e2e \
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

Pass `--pci-addr 0000:04:00.0` explicitly. Do not rely on a shell-local env var
unless it is passed through `sudo env`.

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
