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
        uint16_t len = 0;
        bool found() const { return data != nullptr; }
    };

    struct hot_result { const char* data; uint16_t len; };
    struct cold_result {
        uint32_t page_id;
        uint8_t  slot_index;
        uint8_t  size_class;
        uint16_t value_len;
        uint32_t version;
        uint64_t nvme_lba;
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
        std::vector<uint64_t> pending_nvme_frees_;

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
                                   e->value_len, e->version, pe.nvme_lba};

            // IN_MEMORY or EVICTING: page is in RAM, read directly.
            pe.hotness = ++access_clock_;
            return hot_result{slab.slot_ptr(e->page_id, e->slot_index), e->value_len};
        }

        // Promote a cold key to hot after NVMe read. Best-effort: version mismatch → skip.
        void promote(const char* key, uint16_t key_len, uint32_t version,
                     const char* value, uint16_t value_len) {
            auto* e = ht.lookup(key, key_len);
            if (!e || e->version != version) return;

            auto& pe = pt[e->page_id];
            if (pe.state != page_entry::ON_NVME) return;

            uint32_t old_page_id = e->page_id;
            auto sc = size_class_for(value_len);
            auto ar = slab.allocate(sc);
            std::memcpy(ar.slot_ptr, value, value_len);
            e->page_id    = ar.page_id;
            e->slot_index = ar.slot_index;
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
                 const char* value, uint16_t value_len,
                 int64_t expire_at = -1) {
            // Sync eviction at hard limit — discard only (no NVMe from here).
            if (evict_cfg_.memory_limit > 0 &&
                memory_used_bytes() >= evict_cfg_.max_bytes()) {
                while (memory_used_bytes() >= evict_cfg_.urgent_bytes()) {
                    if (discard_one_page() == 0) break;
                }
            }

            auto new_sc = size_class_for(value_len);
            auto* e = ht.lookup(key, key_len);

            if (e) {
                auto& old_pe = pt[e->page_id];

                if (old_pe.state == page_entry::ON_NVME) {
                    // Key is on NVMe — allocate fresh hot slot.
                    uint32_t old_page_id = e->page_id;
                    auto ar = slab.allocate(new_sc);
                    std::memcpy(ar.slot_ptr, value, value_len);
                    e->page_id    = ar.page_id;
                    e->slot_index = ar.slot_index;
                    e->value_len  = value_len;
                    e->expire_at  = expire_at;
                    e->version++;
                    pt[e->page_id].hotness = ++access_clock_;
                    nvme_page_dec_live(old_page_id);
                    return;
                }

                // IN_MEMORY or EVICTING: page is in RAM.
                auto old_sc = static_cast<size_class_t>(old_pe.size_class);

                if (old_sc == new_sc && old_pe.state == page_entry::IN_MEMORY) {
                    // Same size class, non-evicting: overwrite in place.
                    std::memcpy(slab.slot_ptr(e->page_id, e->slot_index), value, value_len);
                } else {
                    // Different size class, or EVICTING: migrate to new page.
                    slab.free_slot(e->page_id, e->slot_index);
                    auto ar = slab.allocate(new_sc);
                    std::memcpy(ar.slot_ptr, value, value_len);
                    e->page_id    = ar.page_id;
                    e->slot_index = ar.slot_index;
                }
                e->value_len  = value_len;
                e->expire_at  = expire_at;
                e->version++;
            } else {
                auto ar = slab.allocate(new_sc);
                std::memcpy(ar.slot_ptr, value, value_len);

                e = ht.insert(key, key_len);
                e->page_id    = ar.page_id;
                e->slot_index = ar.slot_index;
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

        // Sample random IN_MEMORY pages, return the coldest page_id.
        // Returns UINT32_MAX if no pages to evict.
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
                found++;
                if (pe.hotness < best_hotness) {
                    best_hotness = pe.hotness;
                    best_id = idx;
                }
            }
            return best_id;
        }

        // Discard a page: erase all entries, free memory. No NVMe.
        // Used as fallback when NVMe is full or for sync eviction at hard limit.
        int discard_one_page() {
            uint32_t victim = sample_coldest_page();
            if (victim == UINT32_MAX) return 0;

            discard_page_entries(victim);
            slab.evict_page(victim);
            return 1;
        }

        // Mark a page for async NVMe eviction.
        // Returns the victim page_id, or UINT32_MAX if nothing to evict.
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

        // Called when NVMe write completes (success or failure).
        // Returns true if the NVMe LBA should be freed (write useless).
        bool complete_eviction(uint32_t page_id, bool success) {
            auto& pe = pt[page_id];

            if (pe.state != page_entry::EVICTING) {
                return true;  // shouldn't happen — free LBA to be safe
            }

            if (pe.live_count == 0) {
                // All entries were deleted during the write.
                slab.release_page_memory(page_id);
                pt.free_page_id(page_id);
                return true;  // NVMe LBA is useless
            }

            if (!success) {
                // Write failed — revert to IN_MEMORY.
                pe.state = page_entry::IN_MEMORY;
                return true;  // NVMe LBA was never valid
            }

            // Success — transition to ON_NVME, release memory.
            pe.state = page_entry::ON_NVME;
            slab.release_page_memory(page_id);
            return false;  // NVMe LBA is in use
        }

        uint64_t memory_used_bytes() const { return slab.memory_used_bytes(); }
        uint32_t key_count()         const { return ht.count(); }

    private:
        // Free the slot for an entry, handling ON_NVME pages.
        void free_entry_slot(entry* e) {
            auto& pe = pt[e->page_id];
            if (pe.state == page_entry::ON_NVME) {
                nvme_page_dec_live(e->page_id);
            } else {
                slab.free_slot(e->page_id, e->slot_index);
            }
        }

        // Decrement live_count on an ON_NVME page.
        // If zero, queue NVMe LBA for freeing and release page_id.
        void nvme_page_dec_live(uint32_t page_id) {
            auto& pe = pt[page_id];
            pe.live_count--;
            if (pe.live_count == 0) {
                pending_nvme_frees_.push_back(pe.nvme_lba);
                pt.free_page_id(page_id);
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
