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

        // Drain pending NVMe LBA frees (from SET/DEL on ON_NVME keys).
        if (nvme_alloc_ && !store.pending_nvme_frees_.empty()) {
            for (auto lba : store.pending_nvme_frees_)
                nvme_alloc_->free(lba);
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
            // Effective usage: subtract in-flight evictions (committed to be freed).
            auto used = store.memory_used_bytes();
            auto pending = static_cast<uint64_t>(in_flight_evictions_) * PAGE_SIZE;
            auto effective = (used > pending) ? used - pending : 0ULL;

            if (effective >= cfg.max_bytes()) {
                // At hard limit: sync discard until below urgent.
                while (store.memory_used_bytes() >= cfg.urgent_bytes()) {
                    if (store.discard_one_page() == 0) break;
                }
            } else if (effective >= cfg.urgent_bytes()) {
                // High water: evict several pages per advance.
                for (int i = 0; i < 8; i++) {
                    auto eff = store.memory_used_bytes()
                        - static_cast<uint64_t>(in_flight_evictions_) * PAGE_SIZE;
                    if (eff < cfg.begin_bytes()) break;
                    if (!try_start_eviction()) break;
                }
            } else if (effective >= cfg.begin_bytes() && !progress) {
                // Low water + idle: evict one page.
                try_start_eviction();
            }
        }

        return progress;
    }

    // Start an async NVMe eviction. Returns true if started.
    inline bool store_scheduler::try_start_eviction() {
        if (!nvme_sched_ || !nvme_alloc_) {
            // No NVMe — fall back to discard.
            return store.discard_one_page() > 0;
        }

        if (nvme_alloc_->full()) {
            // NVMe full — discard.
            return store.discard_one_page() > 0;
        }

        uint32_t victim = store.begin_eviction();
        if (victim == UINT32_MAX) return false;

        auto lba = nvme_alloc_->allocate();
        if (lba == nvme::nvme_allocator::INVALID_LBA) {
            // Race: became full between check and allocate. Revert.
            store.pt[victim].state = page_entry::IN_MEMORY;
            return store.discard_one_page() > 0;
        }

        auto& pe = store.pt[victim];
        pe.nvme_lba = lba;

        auto* page = new nvme::sider_page{lba, pe.mem_ptr, ssd_info_};
        in_flight_evictions_++;

        // Fire-and-forget: NVMe write → completion callback.
        nvme_sched_->put(page)
            >> pump::sender::then(
                [this, victim, page](pump::scheduler::nvme::put::res<nvme::sider_page>&& res) {
                    bool free_lba = store.complete_eviction(victim, res.is_success());
                    if (free_lba)
                        nvme_alloc_->free(page->lba);
                    in_flight_evictions_--;
                    delete page;
                })
            >> pump::sender::submit(pump::core::make_root_context());

        return true;
    }

} // namespace sider::store
