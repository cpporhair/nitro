#ifndef APPS_INCONEL_CORE_BATCH_CARRIER_HH
#define APPS_INCONEL_CORE_BATCH_CARRIER_HH

// Shared write-batch carrier for Front/WAL Phase A (M01).
//
// This file owns only L2/L3 boundary data shapes:
//   - client ingress bytes and validated non-owning op views
//   - canonical entries as views into the batch-owned byte buffer
//   - front fragments by stable canonical-entry indices
//   - PUT-entry indices for later value persistence
//
// It deliberately does not define WAL append, coord publish, front owner
// methods, or write_batch sender state.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/btree_map.h>
#include <absl/container/inlined_vector.h>

#include "../format/types.hh"

namespace apps::inconel::core {

    enum class write_op_type : uint8_t {
        put = 0x01,
        del = 0x02,
    };

    struct raw_batch_op {
        write_op_type op;
        std::string   key;
        std::string   value;
    };

    struct client_batch_op_view {
        write_op_type   op;
        std::string_view key;
        std::string_view value;
        uint32_t         original_position;
    };

    class client_batch_view {
      public:
        explicit client_batch_view(std::span<const std::byte> bytes)
            : bytes_(bytes) {
            parse();
        }

        [[nodiscard]] uint32_t
        op_count() const noexcept {
            return static_cast<uint32_t>(ops_.size());
        }

        [[nodiscard]] std::span<const client_batch_op_view>
        ops() const noexcept {
            return {ops_.data(), ops_.size()};
        }

      private:
        static constexpr std::size_t kRecordHeaderBytes =
            sizeof(uint8_t) + sizeof(uint32_t) * 2;

        static bool
        read_u8(std::span<const std::byte> bytes,
                std::size_t& off,
                uint8_t& out) noexcept {
            if (off > bytes.size() || bytes.size() - off < sizeof(out)) {
                return false;
            }
            std::memcpy(&out, bytes.data() + off, sizeof(out));
            off += sizeof(out);
            return true;
        }

        static bool
        read_u32(std::span<const std::byte> bytes,
                 std::size_t& off,
                 uint32_t& out) noexcept {
            if (off > bytes.size() || bytes.size() - off < sizeof(out)) {
                return false;
            }
            std::memcpy(&out, bytes.data() + off, sizeof(out));
            off += sizeof(out);
            return true;
        }

        static std::string_view
        to_sv(std::span<const std::byte> bytes) noexcept {
            return {
                reinterpret_cast<const char*>(bytes.data()),
                bytes.size(),
            };
        }

        void
        parse() {
            std::size_t off = 0;
            uint32_t count = 0;
            if (!read_u32(bytes_, off, count)) {
                throw std::invalid_argument("client_batch_view: truncated record count");
            }

            if (count > (bytes_.size() - off) / kRecordHeaderBytes) {
                throw std::invalid_argument("client_batch_view: record count exceeds payload");
            }

            ops_.clear();
            ops_.reserve(count);

            for (uint32_t i = 0; i < count; ++i) {
                uint8_t  op_byte = 0;
                uint32_t key_len = 0;
                uint32_t value_len = 0;
                if (!read_u8(bytes_, off, op_byte) ||
                    !read_u32(bytes_, off, key_len) ||
                    !read_u32(bytes_, off, value_len)) {
                    throw std::invalid_argument("client_batch_view: truncated op header");
                }

                write_op_type op{};
                switch (op_byte) {
                case static_cast<uint8_t>(write_op_type::put):
                    op = write_op_type::put;
                    break;
                case static_cast<uint8_t>(write_op_type::del):
                    if (value_len != 0) {
                        throw std::invalid_argument(
                            "client_batch_view: DELETE must carry zero value bytes");
                    }
                    op = write_op_type::del;
                    break;
                default:
                    throw std::invalid_argument("client_batch_view: unknown op code");
                }

                if (off > bytes_.size() || bytes_.size() - off < key_len) {
                    throw std::invalid_argument("client_batch_view: truncated key bytes");
                }
                auto key = to_sv(bytes_.subspan(off, key_len));
                off += key_len;

                if (off > bytes_.size() || bytes_.size() - off < value_len) {
                    throw std::invalid_argument("client_batch_view: truncated value bytes");
                }
                auto value = to_sv(bytes_.subspan(off, value_len));
                off += value_len;

                ops_.push_back(client_batch_op_view{
                    .op                = op,
                    .key               = key,
                    .value             = value,
                    .original_position = i,
                });
            }

            if (off != bytes_.size()) {
                throw std::invalid_argument("client_batch_view: trailing bytes");
            }
        }

        std::span<const std::byte>       bytes_;
        std::vector<client_batch_op_view> ops_;
    };

    struct client_batch_buffer {
        std::vector<std::byte> bytes;

        client_batch_buffer() = default;
        client_batch_buffer(client_batch_buffer&&) noexcept = default;
        client_batch_buffer&
        operator=(client_batch_buffer&&) noexcept = default;
        client_batch_buffer(const client_batch_buffer&) = delete;
        client_batch_buffer&
        operator=(const client_batch_buffer&) = delete;

        [[nodiscard]] client_batch_view
        view() const {
            return client_batch_view{
                std::span<const std::byte>(bytes.data(), bytes.size()),
            };
        }
    };

    struct canonical_entry {
        write_op_type     op;
        std::string_view  key;
        std::string_view  value;
        format::value_ref allocated_vr{};
    };

    struct front_fragment {
        uint32_t owner = 0;
        uint64_t batch_lsn = 0;
        uint32_t entry_count = 0;
        absl::InlinedVector<uint32_t, 32> entry_indices;
    };

    struct batch_ctx {
        client_batch_buffer input;
        uint64_t batch_lsn = 0;
        uint32_t entry_count = 0;
        std::vector<canonical_entry> canonical_entries;
        std::vector<front_fragment> fragments;
        std::vector<uint32_t> put_entry_indices;

        batch_ctx() = default;
        batch_ctx(batch_ctx&&) noexcept = default;
        batch_ctx&
        operator=(batch_ctx&&) noexcept = default;
        batch_ctx(const batch_ctx&) = delete;
        batch_ctx&
        operator=(const batch_ctx&) = delete;
    };

    [[nodiscard]] inline uint64_t
    key_hash(std::string_view key) noexcept {
        uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : key) {
            h ^= static_cast<uint64_t>(c);
            h *= 1099511628211ULL;
        }
        return h;
    }

    namespace detail {

        inline void
        append_bytes(std::vector<std::byte>& out,
                     const void* data,
                     std::size_t len) {
            if (len == 0) return;
            const auto* first = static_cast<const std::byte*>(data);
            out.insert(out.end(), first, first + len);
        }

        inline void
        append_u8(std::vector<std::byte>& out, uint8_t value) {
            append_bytes(out, &value, sizeof(value));
        }

        inline void
        append_u32(std::vector<std::byte>& out, uint32_t value) {
            append_bytes(out, &value, sizeof(value));
        }

        inline uint32_t
        checked_u32_size(std::size_t value, const char* what) {
            if (value > std::numeric_limits<uint32_t>::max()) {
                throw std::invalid_argument(what);
            }
            return static_cast<uint32_t>(value);
        }

        inline std::vector<uint32_t>
        build_canonical_positions(std::span<const client_batch_op_view> ops) {
            absl::btree_map<std::string_view, uint32_t> last;
            for (uint32_t i = 0; i < ops.size(); ++i) {
                last[ops[i].key] = i;
            }

            std::vector<uint32_t> keep;
            keep.reserve(last.size());
            for (const auto& [key, pos] : last) {
                (void)key;
                keep.push_back(pos);
            }
            std::sort(keep.begin(), keep.end(),
                      [&](uint32_t lhs, uint32_t rhs) {
                          return ops[lhs].original_position <
                                 ops[rhs].original_position;
                      });
            return keep;
        }

    }  // namespace detail

    [[nodiscard]] inline client_batch_buffer
    encode_client_batch(std::span<const raw_batch_op> raw_ops) {
        client_batch_buffer encoded;

        std::size_t total_bytes = sizeof(uint32_t);
        for (const auto& op : raw_ops) {
            total_bytes += sizeof(uint8_t) + sizeof(uint32_t) * 2;
            total_bytes += op.key.size();
            switch (op.op) {
            case write_op_type::put:
                total_bytes += op.value.size();
                break;
            case write_op_type::del:
                if (!op.value.empty()) {
                    throw std::invalid_argument(
                        "encode_client_batch: DELETE must carry zero value bytes");
                }
                break;
            default:
                throw std::invalid_argument("encode_client_batch: unknown op type");
            }
            detail::checked_u32_size(op.key.size(),
                                     "encode_client_batch: key too large");
            detail::checked_u32_size(op.value.size(),
                                     "encode_client_batch: value too large");
        }

        encoded.bytes.reserve(total_bytes);
        detail::append_u32(
            encoded.bytes,
            detail::checked_u32_size(raw_ops.size(),
                                     "encode_client_batch: op count too large"));

        for (const auto& op : raw_ops) {
            const std::string_view key{op.key};
            const std::string_view value =
                (op.op == write_op_type::put) ? std::string_view{op.value}
                                              : std::string_view{};
            detail::append_u8(encoded.bytes, static_cast<uint8_t>(op.op));
            detail::append_u32(encoded.bytes, static_cast<uint32_t>(key.size()));
            detail::append_u32(encoded.bytes,
                               static_cast<uint32_t>(value.size()));
            detail::append_bytes(encoded.bytes, key.data(), key.size());
            detail::append_bytes(encoded.bytes, value.data(), value.size());
        }

        return encoded;
    }

    [[nodiscard]] inline batch_ctx
    build_batch_ctx(client_batch_buffer&& input,
                    client_batch_view&& view,
                    uint64_t batch_lsn,
                    uint32_t front_count) {
        // The parsed view must have been produced from this exact input buffer.
        // Do not rely on string_views into `input.bytes` surviving the vector
        // move below: compute canonical positions first, then rebase views by
        // byte offset into `ctx.input`.
        if (front_count == 0) {
            throw std::invalid_argument("build_batch_ctx: front_count must be nonzero");
        }

        const auto ops = view.ops();
        const auto keep = detail::build_canonical_positions(ops);
        const auto* old_base =
            reinterpret_cast<const char*>(input.bytes.data());
        const auto old_begin = reinterpret_cast<std::uintptr_t>(old_base);
        const std::size_t old_size = input.bytes.size();
        const auto old_end = old_begin + old_size;

        batch_ctx ctx;
        ctx.input = std::move(input);
        ctx.batch_lsn = batch_lsn;

        const auto* new_base =
            reinterpret_cast<const char*>(ctx.input.bytes.data());
        auto rebase_view = [&](std::string_view sv) -> std::string_view {
            if (sv.empty()) return {};
            const auto* p = sv.data();
            const auto begin = reinterpret_cast<std::uintptr_t>(p);
            if (begin < old_begin) {
                throw std::logic_error("build_batch_ctx: view precedes input buffer");
            }
            const auto end = begin + sv.size();
            if (end < begin || end > old_end) {
                throw std::logic_error("build_batch_ctx: view exceeds input buffer");
            }
            const auto off = static_cast<std::size_t>(begin - old_begin);
            return {new_base + off, sv.size()};
        };

        ctx.canonical_entries.reserve(keep.size());
        ctx.put_entry_indices.reserve(keep.size());

        for (uint32_t pos : keep) {
            const auto& op = ops[pos];
            const uint32_t canonical_index =
                static_cast<uint32_t>(ctx.canonical_entries.size());
            ctx.canonical_entries.push_back(canonical_entry{
                .op           = op.op,
                .key          = rebase_view(op.key),
                .value        = (op.op == write_op_type::del)
                                    ? std::string_view{}
                                    : rebase_view(op.value),
                .allocated_vr = {},
            });
            if (op.op == write_op_type::put) {
                ctx.put_entry_indices.push_back(canonical_index);
            }
        }

        ctx.entry_count = static_cast<uint32_t>(ctx.canonical_entries.size());
        if (ctx.entry_count == 0) return ctx;

        absl::InlinedVector<std::pair<uint32_t, uint32_t>, 64> route;
        route.reserve(ctx.canonical_entries.size());
        for (uint32_t i = 0; i < ctx.canonical_entries.size(); ++i) {
            const auto& entry = ctx.canonical_entries[i];
            const uint32_t owner =
                static_cast<uint32_t>(key_hash(entry.key) % front_count);
            route.push_back({owner, i});
        }
        std::stable_sort(route.begin(), route.end(), [](const auto& lhs,
                                                        const auto& rhs) {
            return lhs.first < rhs.first;
        });

        ctx.fragments.reserve(std::min<std::size_t>(front_count, route.size()));
        for (std::size_t i = 0; i < route.size();) {
            const uint32_t owner = route[i].first;
            absl::InlinedVector<uint32_t, 32> indices;
            do {
                indices.push_back(route[i].second);
                ++i;
            } while (i < route.size() && route[i].first == owner);
            ctx.fragments.push_back(front_fragment{
                .owner         = owner,
                .batch_lsn     = batch_lsn,
                .entry_count   = ctx.entry_count,
                .entry_indices = std::move(indices),
            });
        }

        return ctx;
    }

    [[nodiscard]] inline batch_ctx
    build_batch_ctx(client_batch_buffer&& input,
                    uint64_t batch_lsn,
                    uint32_t front_count) {
        client_batch_view view = input.view();
        return build_batch_ctx(
            std::move(input), std::move(view), batch_lsn, front_count);
    }

    [[nodiscard]] inline batch_ctx
    build_batch_ctx(std::span<const raw_batch_op> raw_ops,
                    uint64_t batch_lsn,
                    uint32_t front_count) {
        return build_batch_ctx(
            encode_client_batch(raw_ops), batch_lsn, front_count);
    }

    static_assert(!std::is_copy_constructible_v<client_batch_buffer>);
    static_assert(!std::is_copy_assignable_v<client_batch_buffer>);
    static_assert(std::is_nothrow_move_constructible_v<client_batch_buffer>);
    static_assert(std::is_nothrow_move_assignable_v<client_batch_buffer>);
    static_assert(!std::is_copy_constructible_v<batch_ctx>);
    static_assert(!std::is_copy_assignable_v<batch_ctx>);
    static_assert(std::is_nothrow_move_constructible_v<batch_ctx>);
    static_assert(std::is_nothrow_move_assignable_v<batch_ctx>);

}  // namespace apps::inconel::core

#endif  // APPS_INCONEL_CORE_BATCH_CARRIER_HH
