#pragma once

#include <chrono>
#include <cstring>
#include <variant>
#include <vector>

#include "store/types.hh"
#include "store/page_table.hh"
#include "store/slab.hh"
#include "store/hash_table.hh"

namespace sider::store {

    static inline int64_t now_ms() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count();
    }

    struct eviction_config {
        uint64_t memory_limit = 0;    // 0 = no eviction
        double begin_pct  = 0.60;
        double urgent_pct = 0.90;

        uint64_t begin_bytes()  const { return static_cast<uint64_t>(memory_limit * begin_pct); }
        uint64_t urgent_bytes() const { return static_cast<uint64_t>(memory_limit * urgent_pct); }
        uint64_t max_bytes()    const { return memory_limit; }
    };

    // Legacy helper for tests (no NVMe).
    struct get_result {
        const char* data = nullptr;
        uint32_t len = 0;
        bool found() const { return data != nullptr; }
    };

    struct hot_result { const char* data; uint32_t len; };
    struct cold_result {
        uint32_t page_id;
        uint8_t  slot_index;
        uint8_t  size_class;
        uint32_t value_len;
        uint32_t version;
        uint64_t nvme_lba;
        uint32_t page_count;   // 1 for slab, >1 for large values
        uint8_t  disk_id;      // which NVMe disk
    };
    struct nil_result {};

    using lookup_result = std::variant<hot_result, cold_result, nil_result>;

    struct kv_store {
        page_table     pt;
        slab_allocator slab{pt};
        hash_table     ht;
        uint32_t       scan_cursor_ = 0;
        uint32_t       access_clock_ = 0;
        uint32_t       rng_state_ = 12345;
        eviction_config evict_cfg_;

        // NVMe LBAs pending free — drained by scheduler after each advance.
        struct pending_free { uint8_t disk_id; uint64_t lba; };
        std::vector<pending_free> pending_nvme_frees_;

        // Large value tracking.
        uint64_t large_memory_bytes_ = 0;
        std::vector<uint32_t> large_page_ids_;   // IN_MEMORY large value page_ids

        lookup_result get(const char* key, uint16_t key_len) {
            auto* e = ht.lookup(key, key_len);
            if (!e) return nil_result{};

            // Lazy expiry.
            if (e->expire_at >= 0 && now_ms() >= e->expire_at) {
                free_entry_slot(e);
                ht.erase(key, key_len);
                return nil_result{};
            }

            auto& pe = pt[e->page_id];

            if (pe.state == page_entry::ON_NVME)
                return cold_result{e->page_id, e->slot_index, pe.size_class,
                                   e->value_len, e->version, pe.nvme_lba,
                                   pe.page_count, pe.disk_id};

            // IN_MEMORY or EVICTING: page is in RAM, read directly.
            pe.hotness = ++access_clock_;
            if (e->is_large())
                return hot_result{pe.mem_ptr, e->value_len};
            return hot_result{slab.slot_ptr(e->page_id, e->slot_index), e->value_len};
        }

        // Promote a cold key to hot after NVMe read. Best-effort: version mismatch → skip.
        void promote(const char* key, uint16_t key_len, uint32_t version,
                     const char* value, uint32_t value_len) {
            auto* e = ht.lookup(key, key_len);
            if (!e || e->version != version) return;

            auto& pe = pt[e->page_id];
            if (pe.state != page_entry::ON_NVME) return;

            uint32_t old_page_id = e->page_id;

            if (is_large_value(value_len)) {
                alloc_large_for_entry(e, value, value_len);
            } else {
                auto sc = size_class_for(value_len);
                auto ar = slab.allocate(sc);
                std::memcpy(ar.slot_ptr, value, value_len);
                e->page_id    = ar.page_id;
                e->slot_index = ar.slot_index;
                e->set_large(false);
            }
            pt[e->page_id].hotness = ++access_clock_;
            nvme_page_dec_live(old_page_id);
        }

        // Test helper: extract hot/nil as legacy get_result.
        static get_result as_get_result(const lookup_result& r) {
            if (auto* h = std::get_if<hot_result>(&r))
                return {h->data, h->len};
            return {};
        }

        void set(const char* key, uint16_t key_len,
                 const char* value, uint32_t value_len,
                 int64_t expire_at = -1) {
            // Sync eviction at hard limit — discard only (no NVMe from here).
            if (evict_cfg_.memory_limit > 0 &&
                memory_used_bytes() >= evict_cfg_.max_bytes()) {
                while (memory_used_bytes() >= evict_cfg_.urgent_bytes()) {
                    if (discard_one_large() > 0) continue;
                    if (discard_one_page() == 0) break;
                }
            }

            bool new_large = is_large_value(value_len);
            auto* e = ht.lookup(key, key_len);

            if (e) {
                auto& old_pe = pt[e->page_id];

                if (old_pe.state == page_entry::ON_NVME) {
                    // Key is on NVMe — allocate fresh hot storage.
                    uint32_t old_page_id = e->page_id;
                    if (new_large) {
                        alloc_large_for_entry(e, value, value_len);
                    } else {
                        auto new_sc = size_class_for(value_len);
                        auto ar = slab.allocate(new_sc);
                        std::memcpy(ar.slot_ptr, value, value_len);
                        e->page_id    = ar.page_id;
                        e->slot_index = ar.slot_index;
                        e->set_large(false);
                    }
                    e->value_len  = value_len;
                    e->expire_at  = expire_at;
                    e->version++;
                    pt[e->page_id].hotness = ++access_clock_;
                    nvme_page_dec_live(old_page_id);
                    return;
                }

                // IN_MEMORY or EVICTING: page is in RAM.
                bool old_large = e->is_large();

                if (!old_large && !new_large) {
                    // small → small (existing logic)
                    auto old_sc = static_cast<size_class_t>(old_pe.size_class);
                    auto new_sc = size_class_for(value_len);

                    if (old_sc == new_sc && old_pe.state == page_entry::IN_MEMORY) {
                        std::memcpy(slab.slot_ptr(e->page_id, e->slot_index), value, value_len);
                    } else {
                        slab.free_slot(e->page_id, e->slot_index);
                        auto ar = slab.allocate(new_sc);
                        std::memcpy(ar.slot_ptr, value, value_len);
                        e->page_id    = ar.page_id;
                        e->slot_index = ar.slot_index;
                    }
                } else if (old_large && new_large) {
                    // large → large
                    uint32_t new_pc = pages_for(value_len);
                    if (new_pc == old_pe.page_count && old_pe.state == page_entry::IN_MEMORY) {
                        // Same page_count, in memory: overwrite in place.
                        std::memcpy(old_pe.mem_ptr, value, value_len);
                    } else {
                        // Different page_count or EVICTING: free old, alloc new.
                        free_large_entry_mem(e);
                        alloc_large_for_entry(e, value, value_len);
                    }
                } else if (old_large) {
                    // large → small
                    free_large_entry_mem(e);
                    auto new_sc = size_class_for(value_len);
                    auto ar = slab.allocate(new_sc);
                    std::memcpy(ar.slot_ptr, value, value_len);
                    e->page_id    = ar.page_id;
                    e->slot_index = ar.slot_index;
                    e->set_large(false);
                } else {
                    // small → large
                    slab.free_slot(e->page_id, e->slot_index);
                    alloc_large_for_entry(e, value, value_len);
                }

                e->value_len  = value_len;
                e->expire_at  = expire_at;
                e->version++;
            } else {
                // New key.
                if (new_large) {
                    e = ht.insert(key, key_len);
                    alloc_large_for_entry(e, value, value_len);
                } else {
                    auto new_sc = size_class_for(value_len);
                    auto ar = slab.allocate(new_sc);
                    std::memcpy(ar.slot_ptr, value, value_len);

                    e = ht.insert(key, key_len);
                    e->page_id    = ar.page_id;
                    e->slot_index = ar.slot_index;
                    e->set_large(false);
                }
                e->value_len  = value_len;
                e->expire_at  = expire_at;
                e->version    = 0;
            }

            pt[e->page_id].hotness = ++access_clock_;
        }

        int del(const char* key, uint16_t key_len) {
            auto* e = ht.lookup(key, key_len);
            if (!e) return 0;

            // Lazy expiry on del: expired key counts as not found.
            if (e->expire_at >= 0 && now_ms() >= e->expire_at) {
                free_entry_slot(e);
                ht.erase(key, key_len);
                return 0;
            }

            free_entry_slot(e);
            ht.erase(key, key_len);
            return 1;
        }

        // Active expiry: sample up to `samples` slots, delete expired entries.
        int expire_scan(int samples = 20) {
            if (ht.count() == 0) return 0;
            auto cap = ht.capacity();
            auto ts = now_ms();
            int expired = 0;

            for (int i = 0; i < samples; i++) {
                if (scan_cursor_ >= cap) scan_cursor_ = 0;
                auto* e = ht.entry_at(scan_cursor_);
                scan_cursor_++;
                if (!e) continue;
                if (e->expire_at >= 0 && ts >= e->expire_at) {
                    free_entry_slot(e);
                    ht.erase(e->key_data, e->key_len);
                    expired++;
                }
            }
            return expired;
        }

        // ── Eviction ──

        uint32_t fast_rand() {
            rng_state_ ^= rng_state_ << 13;
            rng_state_ ^= rng_state_ >> 17;
            rng_state_ ^= rng_state_ << 5;
            return rng_state_;
        }

        // Sample random IN_MEMORY slab pages (skip large), return coldest page_id.
        uint32_t sample_coldest_page(int samples = 5) {
            auto page_count = pt.size();
            if (page_count == 0) return UINT32_MAX;

            uint32_t best_id = UINT32_MAX;
            uint32_t best_hotness = UINT32_MAX;
            int found = 0;

            for (int attempt = 0; attempt < samples * 4 && found < samples; attempt++) {
                uint32_t idx = fast_rand() % page_count;
                auto& pe = pt[idx];
                if (pe.state != page_entry::IN_MEMORY) continue;
                if (pe.page_count > 1) continue;   // skip large values
                found++;
                if (pe.hotness < best_hotness) {
                    best_hotness = pe.hotness;
                    best_id = idx;
                }
            }
            return best_id;
        }

        // Sample coldest large value from large_page_ids_.
        uint32_t sample_coldest_large(int samples = 3) {
            if (large_page_ids_.empty()) return UINT32_MAX;

            uint32_t best_id = UINT32_MAX;
            uint32_t best_hotness = UINT32_MAX;
            int n = static_cast<int>(large_page_ids_.size());

            for (int i = 0; i < std::min(samples, n); i++) {
                uint32_t idx = fast_rand() % static_cast<uint32_t>(n);
                uint32_t pid = large_page_ids_[idx];
                auto& pe = pt[pid];
                if (pe.state != page_entry::IN_MEMORY) continue;
                if (pe.hotness < best_hotness) {
                    best_hotness = pe.hotness;
                    best_id = pid;
                }
            }
            return best_id;
        }

        // Discard a slab page: erase all entries, free memory. No NVMe.
        int discard_one_page() {
            uint32_t victim = sample_coldest_page();
            if (victim == UINT32_MAX) return 0;

            discard_page_entries(victim);
            slab.evict_page(victim);
            return 1;
        }

        // Discard a large value: erase entry, free memory. No NVMe.
        int discard_one_large() {
            if (large_page_ids_.empty()) return 0;
            uint32_t victim = sample_coldest_large();
            if (victim == UINT32_MAX) return 0;

            auto& pe = pt[victim];
            discard_page_entries(victim);
            free_large(pe.mem_ptr);
            large_memory_bytes_ -= static_cast<uint64_t>(pe.page_count) * PAGE_SIZE;
            remove_from_large_ids(victim);
            pt.free_page_id(victim);
            return 1;
        }

        // Mark a slab page for async NVMe eviction.
        uint32_t begin_eviction() {
            uint32_t victim = sample_coldest_page();
            if (victim == UINT32_MAX) return UINT32_MAX;

            auto& pe = pt[victim];
            pe.state = page_entry::EVICTING;

            // Remove from slab partials — no new allocations on this page.
            auto sc = static_cast<size_class_t>(pe.size_class);
            auto& partials = slab.partial_pages_[sc];
            for (size_t i = 0; i < partials.size(); i++) {
                if (partials[i] == victim) {
                    partials[i] = partials.back();
                    partials.pop_back();
                    break;
                }
            }

            return victim;
        }

        // Mark a large value for async NVMe eviction.
        uint32_t begin_large_eviction() {
            uint32_t victim = sample_coldest_large();
            if (victim == UINT32_MAX) return UINT32_MAX;

            auto& pe = pt[victim];
            pe.state = page_entry::EVICTING;
            remove_from_large_ids(victim);
            return victim;
        }

        // Called when NVMe write completes (success or failure).
        // Returns true if the NVMe LBA(s) should be freed (write useless).
        // Works for both slab pages and large values.
        bool complete_eviction(uint32_t page_id, bool success) {
            auto& pe = pt[page_id];

            if (pe.state != page_entry::EVICTING) {
                return true;  // shouldn't happen — free LBA to be safe
            }

            bool is_large = (pe.page_count > 1);

            if (pe.live_count == 0) {
                // All entries were deleted during the write.
                if (is_large) {
                    free_large(pe.mem_ptr);
                    large_memory_bytes_ -= static_cast<uint64_t>(pe.page_count) * PAGE_SIZE;
                } else {
                    slab.release_page_memory(page_id);
                }
                pt.free_page_id(page_id);
                return true;  // NVMe LBA is useless
            }

            if (!success) {
                // Write failed — revert to IN_MEMORY.
                pe.state = page_entry::IN_MEMORY;
                if (is_large) large_page_ids_.push_back(page_id);
                return true;  // NVMe LBA was never valid
            }

            // Success — transition to ON_NVME, release memory.
            pe.state = page_entry::ON_NVME;
            if (is_large) {
                free_large(pe.mem_ptr);
                pe.mem_ptr = nullptr;
                large_memory_bytes_ -= static_cast<uint64_t>(pe.page_count) * PAGE_SIZE;
            } else {
                slab.release_page_memory(page_id);
            }
            return false;  // NVMe LBA is in use
        }

        uint64_t memory_used_bytes() const {
            return slab.memory_used_bytes() + large_memory_bytes_;
        }
        uint32_t key_count() const { return ht.count(); }

    private:
        // Free the slot for an entry, handling ON_NVME pages and large values.
        void free_entry_slot(entry* e) {
            auto& pe = pt[e->page_id];
            if (pe.state == page_entry::ON_NVME) {
                nvme_page_dec_live(e->page_id);
            } else if (e->is_large()) {
                free_large_entry_mem(e);
            } else {
                slab.free_slot(e->page_id, e->slot_index);
            }
        }

        // Decrement live_count on an ON_NVME page.
        // If zero, queue NVMe LBA(s) for freeing and release page_id.
        void nvme_page_dec_live(uint32_t page_id) {
            auto& pe = pt[page_id];
            pe.live_count--;
            if (pe.live_count == 0) {
                for (uint32_t i = 0; i < pe.page_count; i++)
                    pending_nvme_frees_.push_back({pe.disk_id, pe.nvme_lba + i});
                pt.free_page_id(page_id);
            }
        }

        // Allocate contiguous DMA memory for a large value, set up page_entry and entry.
        void alloc_large_for_entry(entry* e, const char* value, uint32_t value_len) {
            uint32_t pc = pages_for(value_len);
            auto* mem = alloc_large(value_len);
            std::memcpy(mem, value, value_len);

            uint32_t pid = pt.alloc_page_id();
            auto& pe = pt[pid];
            pe.state      = page_entry::IN_MEMORY;
            pe.size_class = 0;
            pe.live_count = 1;
            pe.page_count = pc;
            pe.hotness    = 0;
            pe.mem_ptr    = mem;
            pe.slot_bitmap = 0;

            e->page_id    = pid;
            e->slot_index = 0;
            e->set_large(true);

            large_memory_bytes_ += static_cast<uint64_t>(pc) * PAGE_SIZE;
            large_page_ids_.push_back(pid);
        }

        // Free large value memory for an IN_MEMORY or EVICTING entry.
        // For EVICTING: defers memory free to complete_eviction callback.
        void free_large_entry_mem(entry* e) {
            auto& pe = pt[e->page_id];
            pe.live_count--;

            if (pe.state == page_entry::EVICTING) {
                // DMA in flight — complete_eviction handles cleanup.
                return;
            }

            // IN_MEMORY: free immediately.
            free_large(pe.mem_ptr);
            large_memory_bytes_ -= static_cast<uint64_t>(pe.page_count) * PAGE_SIZE;
            remove_from_large_ids(e->page_id);
            pt.free_page_id(e->page_id);
        }

        void remove_from_large_ids(uint32_t page_id) {
            for (size_t i = 0; i < large_page_ids_.size(); i++) {
                if (large_page_ids_[i] == page_id) {
                    large_page_ids_[i] = large_page_ids_.back();
                    large_page_ids_.pop_back();
                    return;
                }
            }
        }

        // Erase all hash table entries pointing to a page.
        void discard_page_entries(uint32_t page_id) {
            struct key_copy { char* data; uint16_t len; };
            std::vector<key_copy> keys;

            auto cap = ht.capacity();
            for (uint32_t i = 0; i < cap; i++) {
                auto* e = ht.entry_at(i);
                if (!e || e->page_id != page_id) continue;
                auto* kd = new char[e->key_len];
                std::memcpy(kd, e->key_data, e->key_len);
                keys.push_back({kd, e->key_len});
            }

            for (auto& k : keys) {
                ht.erase(k.data, k.len);
                delete[] k.data;
            }
        }
    };

} // namespace sider::store
