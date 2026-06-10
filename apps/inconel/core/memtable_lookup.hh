#ifndef APPS_INCONEL_CORE_MEMTABLE_LOOKUP_HH
#define APPS_INCONEL_CORE_MEMTABLE_LOOKUP_HH

// Read-side CPU helpers for M02. These functions search only the
// caller-supplied front_read_set snapshot; they never consult a front
// scheduler's current active/imms state.

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "./memtable.hh"

namespace apps::inconel::core {

    struct memtable_scan_item {
        std::string_view     key;
        uint64_t             data_ver = 0;
        memtable_entry::kind kind     = memtable_entry::kind::tombstone;
        value_handle         vh{};
    };

    using memtable_scan_result = std::vector<memtable_scan_item>;

    namespace detail {

        inline const memtable_entry*
        better_visible_entry(const memtable_entry* current,
                             const memtable_entry* candidate) noexcept {
            if (candidate == nullptr) return current;
            if (current == nullptr || candidate->data_ver > current->data_ver) {
                return candidate;
            }
            return current;
        }

        inline memtable_lookup_result
        lookup_result_from_entry(const memtable_entry* entry) {
            if (entry == nullptr) return memtable_miss{};
            if (entry->k == memtable_entry::kind::value) {
                return memtable_value_hit{.durable = entry->vh.durable};
            }
            return memtable_tombstone{};
        }

    }  // namespace detail

    [[nodiscard]] inline memtable_lookup_result
    lookup_memtable(std::string_view key,
                    uint64_t         read_lsn,
                    const front_read_set& frs) {
        const memtable_entry* best = nullptr;

        if (frs.active) {
            best = detail::better_visible_entry(
                best, find_visible_entry(*frs.active, key, read_lsn));
        }

        for (const auto& imm : frs.imms) {
            if (!imm) continue;
            best = detail::better_visible_entry(
                best, find_visible_entry(*imm, key, read_lsn));
        }

        return detail::lookup_result_from_entry(best);
    }

    [[nodiscard]] inline memtable_scan_result
    scan_memtable(std::string_view begin,
                  std::string_view end,
                  uint64_t         read_lsn,
                  const front_read_set& frs) {
        if (!std::less<>{}(begin, end)) return {};

        using map_type = decltype(memtable_gen::table);
        using iter_type = map_type::const_iterator;

        struct gen_range {
            iter_type cur;
            iter_type stop;
        };

        std::vector<gen_range> ranges;

        auto add_gen = [&](const std::shared_ptr<memtable_gen>& gen) {
            if (!gen) return;
            auto lo = gen->table.lower_bound(begin);
            auto hi = gen->table.lower_bound(end);
            if (lo != hi) ranges.push_back(gen_range{.cur = lo, .stop = hi});
        };

        add_gen(frs.active);
        for (const auto& imm : frs.imms) add_gen(imm);

        memtable_scan_result result;

        for (;;) {
            std::string_view min_key;
            bool             found = false;

            for (const auto& range : ranges) {
                if (range.cur == range.stop) continue;
                const auto key = range.cur->first;
                if (!found || std::less<>{}(key, min_key)) {
                    min_key = key;
                    found = true;
                }
            }

            if (!found) break;

            const memtable_entry* best = nullptr;
            std::string_view      best_key;

            for (auto& range : ranges) {
                if (range.cur == range.stop) continue;
                if (range.cur->first != min_key) continue;

                for (const auto& entry : range.cur->second) {
                    if (entry.data_ver <= read_lsn &&
                        (best == nullptr || entry.data_ver > best->data_ver)) {
                        best = &entry;
                        best_key = range.cur->first;
                    }
                }
                ++range.cur;
            }

            if (best != nullptr) {
                result.push_back(memtable_scan_item{
                    .key      = best_key,
                    .data_ver = best->data_ver,
                    .kind     = best->k,
                    .vh       = best->vh,
                });
            }
        }

        return result;
    }

}  // namespace apps::inconel::core

#endif  // APPS_INCONEL_CORE_MEMTABLE_LOOKUP_HH
