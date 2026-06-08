# 038 — PUMP NVMe LBA Page Adapter Landing Status

## Direction

Reuse PUMP NVMe as the bottom LBA-page scheduler. Inconel owns the memory and
logical frame model above it:

1. `memory/lba_dma_page_pool` allocates and recycles LBA-sized DMA pages.
2. `memory/segmented_page_frame` models a contiguous logical LBA range as N
   independent LBA DMA pages.
3. `nvme/lba_page` maps each logical LBA segment to PUMP
   `page_concept`, so reads/writes use PUMP `get_pages` / `put_pages`.

This deliberately does not require contiguous DMA memory for multi-LBA frames.
It also does not introduce SGL support and does not change PUMP read merge.

## Landing Order

1. **Done.** Add the isolated L1 carriers and adapter headers:
   `memory/lba_dma_page_pool`, `memory/segmented_page_frame`,
   `nvme/lba_page`, and `nvme/frame_io`.
2. **Done.** Add real-NVMe runtime plumbing:
   `nvme::real_device`, `nvme::real_scheduler`, `nvme::runtime_scheduler`,
   per-core SPDK qpair
   ownership, per-core LBA DMA mempool, FUA/flush/trim wiring, and a
   raw-compat bridge that copies legacy contiguous callers through
   segmented LBA DMA pages before reaching PUMP NVMe.
3. **Done.** Move tree lookup / worker miss frames, owner merge old-page
   reads, owner writeback frames, non-leaf cache entries, and superblock
   read/mutate/write to segmented frames and `memory::frame_{read,write}_desc`.
4. **Done.** Move value resident/round/prefill/read-miss frames to segmented
   LBA DMA frames. `value_alloc_sched` remains the only value-page writable
   owner; `value_space_manager` still owns logical placement metadata only.

The old `format::{read,write}_desc` surface remains only as a compatibility
bridge for isolated legacy callers. The production tree/value/superblock
runtime path builds `memory::frame_read_desc` / `memory::frame_write_desc` and
uses `nvme::read_frame` / `nvme::write_frame`, so real mode reaches PUMP NVMe
through LBA DMA pages without a heap staging buffer.

Legacy `core::clock_cache` / `core::slru_cache` aliases still point at the
old contiguous `page_frame` for standalone cache compatibility. Runtime
startup uses `segmented_clock_cache` / `segmented_slru_cache`; direct old
policy instantiations of tree/value schedulers are wrapper specializations
whose base implementation is segmented.

## Adjacent Items

`INC-002` and `INC-016` are now unblocked at the L1/runtime seam. The next
local follow-up is not SGL or PUMP read-merge work; it is tightening higher
level frame accounting and reclaim quotas now that long-lived tree/value
frames already use LBA DMA pages. `INC-054` and `INC-053` remain
allocator/reclaim issues and should stay separate from the NVMe adapter step.

## Capacity Note

For 16 KiB tree pages on 4 KiB LBA, one logical tree frame carries four page
descriptors. The frame-level metadata is O(span_lbas), not O(records). At
10^9 KV scale, the resident set remains bounded by cache/dirty-frame quotas,
not by total tree page count.

## Real NVMe Validation

The destructive real-NVMe e2e path now runs through the same local SPDK/DPDK
stack used by the existing Sider/AiSAQ NVMe tests. The setup script only binds
devices to VFIO and allocates hugepages; it does not pin the runtime library
search path. On the current test host, `/usr/lib` provides DPDK 25.11.1 while
the checked-out SPDK tree under `/home/null/work/kv/spdk` carries DPDK 25.11.0.
SPDK v26.01 accepts 25.11.0 but rejects 25.11.1 at `spdk_env_init`, so real
tests must run with the local SPDK/DPDK libraries first in `LD_LIBRARY_PATH`.

Reference command used for the 2026-06-08 validation:

```bash
sudo -n env PCI_ALLOWED="0000:04:00.0" HUGEMEM=512 /usr/bin/spdk-setup config

sudo -n env \
  LD_LIBRARY_PATH=/home/null/work/kv/spdk/build/lib:/home/null/work/kv/spdk/dpdk/build/lib \
  timeout 180s ./build/inconel_test_flush_e2e \
    --pci-addr 0000:04:00.0 \
    --force-format \
    --num-keys 1000 \
    --rounds 3 \
    --readback-samples 50
```

Observed result: all three rounds completed on `0000:04:00.0`, including
bootstrap superblock writes, value/tree writes, NVMe flushes, and 51 sampled
readbacks (`43 value / 8 tombstone`), ending with `all passed`.
