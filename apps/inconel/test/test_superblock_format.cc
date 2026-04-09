//
// superblock format unit tests
//
// Reviewer-owned tests for step 016. These lock the byte-level layout,
// CRC/status semantics, and A/B selection behavior promised by ODF §2 /
// plan 016 before any format/recovery production code is wired up.
//

#include "apps/inconel/test/check.hh"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>

#include <absl/crc/crc32c.h>
#include <absl/strings/string_view.h>

#include "apps/inconel/format/superblock.hh"

using namespace apps::inconel::format;

namespace {

static_assert(std::is_trivially_copyable_v<superblock>);
static_assert(sizeof(superblock) == 157);
static_assert(sizeof(superblock) <= 4096);
static_assert(offsetof(superblock, magic) == 0);
static_assert(offsetof(superblock, format_version) == 8);
static_assert(offsetof(superblock, namespace_size) == 12);
static_assert(offsetof(superblock, lba_size) == 20);
static_assert(offsetof(superblock, tree_page_size) == 24);
static_assert(offsetof(superblock, shadow_slots_per_range) == 28);
static_assert(offsetof(superblock, wal_base_paddr) == 32);
static_assert(offsetof(superblock, wal_segment_size) == 42);
static_assert(offsetof(superblock, wal_segment_count) == 46);
static_assert(offsetof(superblock, data_area_base_paddr) == 50);
static_assert(offsetof(superblock, data_area_end_paddr) == 60);
static_assert(offsetof(superblock, value_size_class_count) == 70);
static_assert(offsetof(superblock, value_size_classes) == 71);
static_assert(offsetof(superblock, root_base_paddr) == 135);
static_assert(offsetof(superblock, generation) == 145);
static_assert(offsetof(superblock, crc) == 153);

constexpr uint32_t SAMPLE_CLASSES[16] = {
    0x00000040u, 0x00000060u, 0x00000080u, 0x00000100u,
    0x00000180u, 0x00000200u, 0x00000400u, 0x00000800u,
    0x00001000u, 0x00001800u, 0x00002000u, 0x00003000u,
    0x00004000u, 0x00006000u, 0x00008000u, 0x0000C000u,
};

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
append_paddr_le(std::vector<char>& out, uint16_t device_id, uint64_t lba) {
    append_u16_le(out, device_id);
    append_u64_le(out, lba);
}

bool
bytes_eq(const void* lhs, const std::vector<char>& rhs) {
    return std::memcmp(lhs, rhs.data(), rhs.size()) == 0;
}

superblock
make_sample_superblock() {
    superblock sb{};
    sb.magic                   = SUPERBLOCK_MAGIC;
    sb.format_version          = 1;
    sb.namespace_size          = 0x1122334455667788ULL;
    sb.lba_size                = 4096;
    sb.tree_page_size          = 16384;
    sb.shadow_slots_per_range  = 7;
    sb.wal_base_paddr          = paddr{3, 0x0102030405060708ULL};
    sb.wal_segment_size        = 0x00A0B0C0u;
    sb.wal_segment_count       = 257;
    sb.data_area_base_paddr    = paddr{4, 0x1112131415161718ULL};
    sb.data_area_end_paddr     = paddr{5, 0x2122232425262728ULL};
    sb.value_size_class_count  = 16;
    std::memcpy(sb.value_size_classes, SAMPLE_CLASSES, sizeof(SAMPLE_CLASSES));
    sb.root_base_paddr         = paddr{6, 0x3132333435363738ULL};
    sb.generation              = 0x4142434445464748ULL;
    sb.crc                     = 0xA1B2C3D4u;
    return sb;
}

superblock
make_valid_superblock(uint64_t generation, uint64_t root_lba) {
    superblock sb = make_sample_superblock();
    sb.generation      = generation;
    sb.root_base_paddr = paddr{0, root_lba};
    sb.crc             = 0;
    sb.crc             = superblock_compute_crc(sb);
    return sb;
}

std::vector<char>
make_expected_sample_superblock_bytes() {
    std::vector<char> out;
    out.reserve(sizeof(superblock));

    append_u64_le(out, SUPERBLOCK_MAGIC);
    append_u32_le(out, 1);
    append_u64_le(out, 0x1122334455667788ULL);
    append_u32_le(out, 4096);
    append_u32_le(out, 16384);
    append_u32_le(out, 7);
    append_paddr_le(out, 3, 0x0102030405060708ULL);
    append_u32_le(out, 0x00A0B0C0u);
    append_u32_le(out, 257);
    append_paddr_le(out, 4, 0x1112131415161718ULL);
    append_paddr_le(out, 5, 0x2122232425262728ULL);
    append_u8(out, 16);
    for (uint32_t v : SAMPLE_CLASSES) append_u32_le(out, v);
    append_paddr_le(out, 6, 0x3132333435363738ULL);
    append_u64_le(out, 0x4142434445464748ULL);
    append_u32_le(out, 0xA1B2C3D4u);

    CHECK(out.size() == sizeof(superblock));
    return out;
}

void
test_constants_layout_and_golden() {
    CHECK(SUPERBLOCK_MAGIC == 0x494E434F4E454C31ULL);
    CHECK(sizeof(superblock) == 157);
    CHECK(sizeof(superblock) <= 4096);

    const superblock sb = make_sample_superblock();
    const std::vector<char> expected = make_expected_sample_superblock_bytes();

    CHECK(bytes_eq(&sb, expected));

    printf("  constants/layout/golden bytes: OK\n");
}

void
test_crc_and_inspect() {
    superblock sb = make_sample_superblock();
    sb.crc = 0;

    const uint32_t expected_crc = static_cast<uint32_t>(absl::ComputeCrc32c(
        absl::string_view(reinterpret_cast<const char*>(&sb), offsetof(superblock, crc))));
    CHECK(superblock_compute_crc(sb) == expected_crc);

    sb.crc = expected_crc;
    CHECK(inspect_superblock(sb) == superblock_status::ok);

    superblock bad_magic = sb;
    bad_magic.magic = 0x0123456789ABCDEFULL;
    bad_magic.crc = superblock_compute_crc(bad_magic);
    CHECK(inspect_superblock(bad_magic) == superblock_status::bad_magic);

    superblock bad_version = sb;
    bad_version.format_version = 2;
    bad_version.crc = superblock_compute_crc(bad_version);
    CHECK(inspect_superblock(bad_version)
          == superblock_status::bad_format_version);

    superblock bad_crc = sb;
    bad_crc.namespace_size ^= 0x1000ULL;
    CHECK(inspect_superblock(bad_crc) == superblock_status::bad_crc);

    printf("  CRC helper + inspect status classification: OK\n");
}

void
test_choose_newer_superblock() {
    const superblock older = make_valid_superblock(7, 100);
    const superblock newer = make_valid_superblock(8, 200);

    superblock_choice choice = choose_newer_superblock(older, newer);
    CHECK(choice.chosen == &newer);
    CHECK(choice.which == superblock_choice::source::b);

    choice = choose_newer_superblock(newer, older);
    CHECK(choice.chosen == &newer);
    CHECK(choice.which == superblock_choice::source::a);

    superblock bad = newer;
    bad.generation = 99;
    bad.root_base_paddr.lba ^= 1ULL;  // break CRC without changing chooser inputs elsewhere

    choice = choose_newer_superblock(older, bad);
    CHECK(choice.chosen == &older);
    CHECK(choice.which == superblock_choice::source::a);

    choice = choose_newer_superblock(bad, older);
    CHECK(choice.chosen == &older);
    CHECK(choice.which == superblock_choice::source::b);

    superblock bad2 = older;
    bad2.shadow_slots_per_range ^= 1u;  // break CRC
    choice = choose_newer_superblock(bad, bad2);
    CHECK(choice.chosen == nullptr);
    CHECK(choice.which == superblock_choice::source::none);

    const superblock same_gen_a = make_valid_superblock(42, 1000);
    const superblock same_gen_b = make_valid_superblock(42, 2000);
    choice = choose_newer_superblock(same_gen_a, same_gen_b);
    CHECK(choice.chosen == nullptr);
    CHECK(choice.which == superblock_choice::source::none);

    printf("  A/B choice helper: newer/good-vs-bad/bad-vs-bad/same-gen-conflict: OK\n");
}

} // namespace

int
main() {
    printf("superblock format tests:\n");
    test_constants_layout_and_golden();
    test_crc_and_inspect();
    test_choose_newer_superblock();
    printf("all passed\n");
    return 0;
}
