#pragma once

// This file provides store_scheduler::advance() which depends on NVMe types.
// Include AFTER nvme headers in main.cc. Not included by handler.hh.

#include "store/scheduler.hh"
#include "nvme/page.hh"
#include "nvme/allocator.hh"
#include "env/scheduler/nvme/scheduler.hh"
#include "env/scheduler/nvme/put_page.hh"

namespace sider::store {

    inline bool store_scheduler::advance() {
        bool progress = false;
        store_req* r;
        while (req_q.try_dequeue(r)) {
            r->fn(store);
            delete r;
            progress = true;
        }

        // Drain pending NVMe LBA frees — route to correct disk's allocator.
        if (!disks_.empty() && !store.pending_nvme_frees_.empty()) {
            for (auto& pf : store.pending_nvme_frees_)
                disks_[pf.disk_id].allocator->free(pf.lba);
            store.pending_nvme_frees_.clear();
        }

        // Active expiry: sample every 64 advance() calls.
        if (++advance_count_ >= 64) {
            advance_count_ = 0;
            store.expire_scan(20);
        }

        // Eviction: water-level based.
        auto& cfg = store.evict_cfg_;
        if (cfg.memory_limit > 0) {
            auto used = store.memory_used_bytes();
            auto effective = (used > in_flight_eviction_bytes_)
                ? used - in_flight_eviction_bytes_ : 0ULL;

            if (effective >= cfg.max_bytes()) {
                while (store.memory_used_bytes() >= cfg.urgent_bytes()) {
                    if (store.discard_one_large() > 0) continue;
                    if (store.discard_one_page() == 0) break;
                }
            } else if (effective >= cfg.urgent_bytes()) {
                for (int i = 0; i < 8; i++) {
                    auto eff = store.memory_used_bytes()
                        - std::min(store.memory_used_bytes(), in_flight_eviction_bytes_);
                    if (eff < cfg.begin_bytes()) break;
                    if (!try_start_large_eviction()) {
                        if (!try_start_eviction()) break;
                    }
                }
            } else if (effective >= cfg.begin_bytes() && !progress) {
                if (!try_start_large_eviction())
                    try_start_eviction();
            }
        }

        return progress;
    }

    // Pick a non-full disk via round-robin. Returns index or UINT8_MAX if all full.
    inline uint8_t store_scheduler_pick_disk(
            std::vector<store_scheduler::nvme_disk>& disks, uint8_t& next) {
        auto n = static_cast<uint8_t>(disks.size());
        for (uint8_t i = 0; i < n; i++) {
            uint8_t d = next++ % n;
            if (!disks[d].allocator->full()) return d;
        }
        return UINT8_MAX;
    }

    inline bool store_scheduler::try_start_eviction() {
        if (disks_.empty()) {
            return store.discard_one_page() > 0;
        }

        uint8_t disk_id = store_scheduler_pick_disk(disks_, next_disk_);
        if (disk_id == UINT8_MAX) {
            return store.discard_one_page() > 0;
        }

        uint32_t victim = store.begin_eviction();
        if (victim == UINT32_MAX) return false;

        auto& disk = disks_[disk_id];
        auto lba = disk.allocator->allocate();
        if (lba == nvme::nvme_allocator::INVALID_LBA) {
            store.pt[victim].state = page_entry::IN_MEMORY;
            return store.discard_one_page() > 0;
        }

        auto& pe = store.pt[victim];
        pe.nvme_lba = lba;
        pe.disk_id = disk_id;

        auto* page = nvme::page_obj_pool.get();
        *page = nvme::sider_page{lba, pe.mem_ptr, disk.ssd_info, PAGE_SIZE};
        in_flight_eviction_bytes_ += PAGE_SIZE;

        disk.scheduler->put(page)
            >> pump::sender::then(
                [this, victim, disk_id, page]
                (pump::scheduler::nvme::put::res<nvme::sider_page>&& res) {
                    bool free_lba = store.complete_eviction(victim, res.is_success());
                    if (free_lba)
                        disks_[disk_id].allocator->free(page->lba);
                    in_flight_eviction_bytes_ -= PAGE_SIZE;
                    nvme::page_obj_pool.put(page);
                })
            >> pump::sender::submit(pump::core::make_root_context());

        return true;
    }

    inline bool store_scheduler::try_start_large_eviction() {
        if (disks_.empty()) {
            return store.discard_one_large() > 0;
        }

        if (store.large_page_ids_.empty()) return false;

        // Large values need contiguous LBAs on one disk.
        // If no disk has contiguous space, discard.
        uint32_t victim = store.begin_large_eviction();
        if (victim == UINT32_MAX) return false;

        auto& pe = store.pt[victim];
        uint32_t pc = pe.page_count;
        uint64_t evict_bytes = static_cast<uint64_t>(pc) * PAGE_SIZE;

        // Try each disk for contiguous allocation.
        uint64_t lba = nvme::nvme_allocator::INVALID_LBA;
        uint8_t disk_id = UINT8_MAX;
        auto n = static_cast<uint8_t>(disks_.size());
        for (uint8_t i = 0; i < n; i++) {
            uint8_t d = next_disk_++ % n;
            lba = disks_[d].allocator->allocate_contiguous(pc);
            if (lba != nvme::nvme_allocator::INVALID_LBA) {
                disk_id = d;
                break;
            }
        }

        if (disk_id == UINT8_MAX) {
            // No disk has contiguous space — revert and discard.
            pe.state = page_entry::IN_MEMORY;
            store.large_page_ids_.push_back(victim);
            return store.discard_one_large() > 0;
        }

        pe.nvme_lba = lba;
        pe.disk_id = disk_id;

        auto& disk = disks_[disk_id];
        auto* page = nvme::page_obj_pool.get();
        *page = nvme::sider_page{lba, pe.mem_ptr, disk.ssd_info, evict_bytes};
        in_flight_eviction_bytes_ += evict_bytes;

        disk.scheduler->put(page)
            >> pump::sender::then(
                [this, victim, disk_id, pc, evict_bytes, page]
                (pump::scheduler::nvme::put::res<nvme::sider_page>&& res) {
                    bool free_lba = store.complete_eviction(victim, res.is_success());
                    if (free_lba)
                        disks_[disk_id].allocator->free_contiguous(page->lba, pc);
                    in_flight_eviction_bytes_ -= evict_bytes;
                    nvme::page_obj_pool.put(page);
                })
            >> pump::sender::submit(pump::core::make_root_context());

        return true;
    }

} // namespace sider::store
