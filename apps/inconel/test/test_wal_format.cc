//
// WAL format unit tests
//
// Reviewer-owned tests for step 015. These lock the byte-level format and
// failure classification promised by ODF §3 / plan 015 without reaching into
// any future front owner or recovery pipeline code.
//

#include "apps/inconel/test/check.hh"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include <absl/crc/crc32c.h>

#include "apps/inconel/format/types.hh"
#include "apps/inconel/format/wal.hh"

using namespace apps::inconel::format;

namespace {

value_ref
make_vr(uint16_t device_id, uint64_t lba, uint16_t byte_offset,
        uint32_t len, uint16_t flags) {
    return value_ref{
        .base        = paddr{device_id, lba},
        .byte_offset = byte_offset,
        .len         = len,
        .flags       = flags,
    };
}

void
append_u8(std::vector<char>& out, uint8_t v) {
    out.push_back(static_cast<char>(v));
}

void
append_u16_le(std::vector<char>& out, uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFFu));
    out.push_back(static_cast<char>((v >> 8) & 0xFFu));
}

void
append_u32_le(std::vector<char>& out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFFu));
    out.push_back(static_cast<char>((v >> 8) & 0xFFu));
    out.push_back(static_cast<char>((v >> 16) & 0xFFu));
    out.push_back(static_cast<char>((v >> 24) & 0xFFu));
}

void
append_u64_le(std::vector<char>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}

void
store_u8(std::vector<char>& out, size_t off, uint8_t v) {
    CHECK(off < out.size());
    out[off] = static_cast<char>(v);
}

void
store_u32_le(std::vector<char>& out, size_t off, uint32_t v) {
    CHECK(off + 4 <= out.size());
    out[off + 0] = static_cast<char>(v & 0xFFu);
    out[off + 1] = static_cast<char>((v >> 8) & 0xFFu);
    out[off + 2] = static_cast<char>((v >> 16) & 0xFFu);
    out[off + 3] = static_cast<char>((v >> 24) & 0xFFu);
}

void
append_crc32c(std::vector<char>& out) {
    const uint32_t crc = static_cast<uint32_t>(
        absl::ComputeCrc32c(absl::string_view(out.data(), out.size())));
    append_u32_le(out, crc);
}

std::vector<char>
make_expected_put_entry(uint32_t segment_gen, uint64_t lsn,
                        uint32_t entry_count, std::string_view key,
                        const value_ref& vr) {
    std::vector<char> out;
    out.reserve(wal_put_entry_size(static_cast<uint32_t>(key.size())));

    append_u32_le(out, wal_put_entry_size(static_cast<uint32_t>(key.size())));
    append_u32_le(out, segment_gen);
    append_u64_le(out, lsn);
    append_u32_le(out, entry_count);
    append_u8(out, static_cast<uint8_t>(wal_op_type::put));
    append_u32_le(out, static_cast<uint32_t>(key.size()));

    append_u16_le(out, vr.base.device_id);
    append_u64_le(out, vr.base.lba);
    append_u16_le(out, vr.byte_offset);
    append_u32_le(out, vr.len);
    append_u16_le(out, vr.flags);

    out.insert(out.end(), key.begin(), key.end());
    append_crc32c(out);
    return out;
}

std::vector<char>
make_expected_delete_entry(uint32_t segment_gen, uint64_t lsn,
                           uint32_t entry_count, std::string_view key) {
    std::vector<char> out;
    out.reserve(wal_delete_entry_size(static_cast<uint32_t>(key.size())));

    append_u32_le(out, wal_delete_entry_size(static_cast<uint32_t>(key.size())));
    append_u32_le(out, segment_gen);
    append_u64_le(out, lsn);
    append_u32_le(out, entry_count);
    append_u8(out, static_cast<uint8_t>(wal_op_type::del));
    append_u32_le(out, static_cast<uint32_t>(key.size()));

    out.insert(out.end(), key.begin(), key.end());
    append_crc32c(out);
    return out;
}

bool
bytes_eq(const std::vector<char>& a, const std::vector<char>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

void
test_constants_and_sizes() {
    CHECK(sizeof(wal_segment_header) == 26);
    CHECK(sizeof(wal_entry_header) == 25);
    CHECK(sizeof(wal_sealed_trailer) == 33);

    CHECK(WAL_SEGMENT_MAGIC == 0x57414C53u);
    CHECK(WAL_SEAL_MAGIC == 0x5345414Cu);
    CHECK(static_cast<uint8_t>(wal_op_type::put) == 0x01u);
    CHECK(static_cast<uint8_t>(wal_op_type::del) == 0x02u);

    CHECK(WAL_SEGMENT_HEADER_SIZE == 26u);
    CHECK(WAL_SEALED_TRAILER_SIZE == 33u);
    CHECK(WAL_ENTRY_TRAILER_CRC_BYTES == 4u);
    CHECK(WAL_PUT_FIXED_BYTES == 47u);
    CHECK(WAL_DELETE_FIXED_BYTES == 29u);
    CHECK(wal_put_entry_size(0) == 47u);
    CHECK(wal_put_entry_size(32) == 79u);
    CHECK(wal_delete_entry_size(0) == 29u);
    CHECK(wal_delete_entry_size(32) == 61u);

    printf("  constants/sizes: OK\n");
}

void
test_segment_header_inspect() {
    wal_segment_header h{
        .magic          = WAL_SEGMENT_MAGIC,
        .format_version = 7,
        .segment_index  = 9,
        .device_id      = 0,
        .stream_id      = 11,
        .segment_gen    = 13,
        .crc            = 0,
    };
    h.crc = wal_segment_header_crc(h);

    CHECK(inspect_wal_segment_header(h, 7) == wal_segment_status::ok);

    wal_segment_header bad_magic = h;
    bad_magic.magic = 0x12345678u;
    bad_magic.crc = wal_segment_header_crc(bad_magic);
    CHECK(inspect_wal_segment_header(bad_magic, 7) == wal_segment_status::bad_magic);

    CHECK(inspect_wal_segment_header(h, 8) == wal_segment_status::bad_version);

    wal_segment_header bad_crc = h;
    bad_crc.stream_id ^= 1u;
    CHECK(inspect_wal_segment_header(bad_crc, 7) == wal_segment_status::bad_crc);

    printf("  segment header inspect: OK\n");
}

void
test_sealed_trailer_inspect() {
    wal_sealed_trailer t{
        .magic       = WAL_SEAL_MAGIC,
        .segment_gen = 17,
        .write_end   = 4096,
        .min_lsn     = 1000,
        .max_lsn     = 2000,
        .sealed      = WAL_SEALED_FLAG_SEALED,
        .crc         = 0,
    };
    t.crc = wal_sealed_trailer_crc(t);

    CHECK(inspect_wal_sealed_trailer(t) == wal_trailer_status::ok);

    wal_sealed_trailer bad_magic = t;
    bad_magic.magic = 0x0BADCAFEu;
    bad_magic.crc = wal_sealed_trailer_crc(bad_magic);
    CHECK(inspect_wal_sealed_trailer(bad_magic) == wal_trailer_status::bad_magic);

    wal_sealed_trailer bad_crc = t;
    bad_crc.write_end += 128;
    CHECK(inspect_wal_sealed_trailer(bad_crc) == wal_trailer_status::bad_crc);

    wal_sealed_trailer bad_flag = t;
    bad_flag.sealed = 0;
    bad_flag.crc = wal_sealed_trailer_crc(bad_flag);
    CHECK(inspect_wal_sealed_trailer(bad_flag) == wal_trailer_status::bad_sealed_flag);

    printf("  sealed trailer inspect: OK\n");
}

void
test_put_round_trip_and_golden() {
    const char raw_key[] = { 'k', '\0', 'y', '!' };
    const std::string_view key(raw_key, sizeof(raw_key));
    const value_ref vr = make_vr(3, 0x0102030405060708ULL, 77, 1234, 9);
    const uint32_t expected_len = wal_put_entry_size(static_cast<uint32_t>(key.size()));

    std::vector<char> actual(expected_len);
    uint32_t out_len = 0;
    CHECK(encode_wal_put_entry(std::span<char>(actual.data(), actual.size()),
                               99, 0x1122334455667788ULL, 7, key, vr, &out_len)
          == wal_entry_encode_status::ok);
    CHECK(out_len == expected_len);

    const std::vector<char> expected =
        make_expected_put_entry(99, 0x1122334455667788ULL, 7, key, vr);
    CHECK(bytes_eq(actual, expected));

    decoded_wal_entry decoded{};
    uint32_t decoded_len = 0;
    CHECK(decode_wal_entry(std::span<const char>(actual.data(), actual.size()),
                           99, &decoded, &decoded_len)
          == wal_entry_decode_status::ok);
    CHECK(decoded_len == expected_len);
    CHECK(decoded.op_type == wal_op_type::put);
    CHECK(decoded.lsn == 0x1122334455667788ULL);
    CHECK(decoded.entry_count == 7);
    CHECK(decoded.key.size() == key.size());
    CHECK(std::memcmp(decoded.key.data(), key.data(), key.size()) == 0);
    CHECK(decoded.vr.has_value());
    CHECK(decoded.vr->base.device_id == vr.base.device_id);
    CHECK(decoded.vr->base.lba == vr.base.lba);
    CHECK(decoded.vr->byte_offset == vr.byte_offset);
    CHECK(decoded.vr->len == vr.len);
    CHECK(decoded.vr->flags == vr.flags);

    printf("  put encode/decode golden + NUL key: OK\n");
}

void
test_delete_round_trip_and_golden() {
    const std::string_view key("", 0);
    const uint32_t expected_len = wal_delete_entry_size(static_cast<uint32_t>(key.size()));

    std::vector<char> actual(expected_len);
    uint32_t out_len = 0;
    CHECK(encode_wal_delete_entry(std::span<char>(actual.data(), actual.size()),
                                  42, 0x0101010102020202ULL, 3, key, &out_len)
          == wal_entry_encode_status::ok);
    CHECK(out_len == expected_len);

    const std::vector<char> expected =
        make_expected_delete_entry(42, 0x0101010102020202ULL, 3, key);
    CHECK(bytes_eq(actual, expected));

    decoded_wal_entry decoded{};
    uint32_t decoded_len = 0;
    CHECK(decode_wal_entry(std::span<const char>(actual.data(), actual.size()),
                           42, &decoded, &decoded_len)
          == wal_entry_decode_status::ok);
    CHECK(decoded_len == expected_len);
    CHECK(decoded.op_type == wal_op_type::del);
    CHECK(decoded.lsn == 0x0101010102020202ULL);
    CHECK(decoded.entry_count == 3);
    CHECK(decoded.key.empty());
    CHECK(!decoded.vr.has_value());

    printf("  delete encode/decode golden + empty key: OK\n");
}

void
test_decode_failure_statuses() {
    constexpr size_t header_bytes = sizeof(wal_entry_header);
    const std::string_view key("del-key", 7);
    std::vector<char> good(wal_delete_entry_size(static_cast<uint32_t>(key.size())));
    uint32_t out_len = 0;
    CHECK(encode_wal_delete_entry(std::span<char>(good.data(), good.size()),
                                  123, 9001, 5, key, &out_len)
          == wal_entry_encode_status::ok);
    CHECK(out_len == good.size());

    decoded_wal_entry out{};
    uint32_t decoded_len = 0;

    CHECK(decode_wal_entry(std::span<const char>(good.data(), header_bytes - 1),
                           123, &out, &decoded_len)
          == wal_entry_decode_status::truncated);

    CHECK(decode_wal_entry(std::span<const char>(good.data(), good.size() - 1),
                           123, &out, &decoded_len)
          == wal_entry_decode_status::truncated);

    std::vector<char> bad_total_len_min = good;
    store_u32_le(bad_total_len_min, offsetof(wal_entry_header, total_len), 28u);
    CHECK(decode_wal_entry(std::span<const char>(bad_total_len_min.data(), bad_total_len_min.size()),
                           123, &out, &decoded_len)
          == wal_entry_decode_status::bad_total_len);

    std::vector<char> bad_segment_gen = good;
    store_u32_le(bad_segment_gen, offsetof(wal_entry_header, segment_gen), 999u);
    CHECK(decode_wal_entry(std::span<const char>(bad_segment_gen.data(), bad_segment_gen.size()),
                           123, &out, &decoded_len)
          == wal_entry_decode_status::bad_segment_gen);

    std::vector<char> bad_op_type = good;
    store_u8(bad_op_type, offsetof(wal_entry_header, op_type), 0x7Fu);
    CHECK(decode_wal_entry(std::span<const char>(bad_op_type.data(), bad_op_type.size()),
                           123, &out, &decoded_len)
          == wal_entry_decode_status::bad_op_type);

    std::vector<char> bad_total_len_key = good;
    store_u32_le(bad_total_len_key, offsetof(wal_entry_header, key_len), 0u);
    CHECK(decode_wal_entry(std::span<const char>(bad_total_len_key.data(), bad_total_len_key.size()),
                           123, &out, &decoded_len)
          == wal_entry_decode_status::bad_total_len);

    const value_ref vr = make_vr(0, 77, 2, 16, 0);
    const std::string_view put_key("xy", 2);
    std::vector<char> good_put(wal_put_entry_size(static_cast<uint32_t>(put_key.size())));
    CHECK(encode_wal_put_entry(std::span<char>(good_put.data(), good_put.size()),
                               123, 9002, 6, put_key, vr, &out_len)
          == wal_entry_encode_status::ok);
    std::vector<char> bad_total_len_put = good_put;
    store_u32_le(bad_total_len_put, offsetof(wal_entry_header, total_len),
                 static_cast<uint32_t>(header_bytes + WAL_ENTRY_TRAILER_CRC_BYTES
                                       + sizeof(value_ref) - 1));
    CHECK(decode_wal_entry(std::span<const char>(bad_total_len_put.data(), bad_total_len_put.size()),
                           123, &out, &decoded_len)
          == wal_entry_decode_status::bad_total_len);

    std::vector<char> bad_crc = good;
    bad_crc.back() ^= 0x01;
    CHECK(decode_wal_entry(std::span<const char>(bad_crc.data(), bad_crc.size()),
                           123, &out, &decoded_len)
          == wal_entry_decode_status::bad_crc);

    printf("  decode failure status classification: OK\n");
}

void
test_encode_failure_statuses() {
    const std::string_view key("abc", 3);
    const value_ref vr = make_vr(0, 100, 0, 8, 0);

    std::vector<char> small_put(wal_put_entry_size(static_cast<uint32_t>(key.size())) - 1);
    CHECK(encode_wal_put_entry(std::span<char>(small_put.data(), small_put.size()),
                               1, 2, 3, key, vr, nullptr)
          == wal_entry_encode_status::dst_too_small);

    std::vector<char> small_delete(wal_delete_entry_size(static_cast<uint32_t>(key.size())) - 1);
    CHECK(encode_wal_delete_entry(std::span<char>(small_delete.data(), small_delete.size()),
                                  1, 2, 3, key, nullptr)
          == wal_entry_encode_status::dst_too_small);

    const std::string_view huge_put(reinterpret_cast<const char*>(0x1),
                                    static_cast<size_t>(WAL_PUT_MAX_KEY_LEN) + 1);
    std::vector<char> any_put(64);
    CHECK(encode_wal_put_entry(std::span<char>(any_put.data(), any_put.size()),
                               1, 2, 3, huge_put, vr, nullptr)
          == wal_entry_encode_status::key_too_large);

    const std::string_view huge_delete(reinterpret_cast<const char*>(0x1),
                                       static_cast<size_t>(WAL_DELETE_MAX_KEY_LEN) + 1);
    std::vector<char> any_delete(64);
    CHECK(encode_wal_delete_entry(std::span<char>(any_delete.data(), any_delete.size()),
                                  1, 2, 3, huge_delete, nullptr)
          == wal_entry_encode_status::key_too_large);

    printf("  encode failure status classification: OK\n");
}

} // namespace

int
main() {
    printf("wal format tests:\n");
    test_constants_and_sizes();
    test_segment_header_inspect();
    test_sealed_trailer_inspect();
    test_put_round_trip_and_golden();
    test_delete_round_trip_and_golden();
    test_decode_failure_statuses();
    test_encode_failure_statuses();
    printf("all passed\n");
    return 0;
}
