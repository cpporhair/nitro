//
// tree page format unit tests — slot directory layout
//
// Plan 014 review asked for a directed page-format test, separate from
// the broader tree_lookup integration suites that previously covered
// page builder/reader behavior only by accident. Coverage:
//
//   1. leaf/internal build → parse → get/find/lower_bound/find_child
//      against a known set of records.
//   2. The slot directory offsets are monotonic, in-page, and decode
//      back to the expected record start (i.e. the offset table really
//      does land at record boundaries after the finalize-time memmove).
//   3. internal_page_reader::rightmost_child() reads the tail paddr from
//      `free_space_offset - sizeof(paddr)` directly, regardless of how
//      many separator records precede it.
//   4. 4K leaf / 16K leaf / 16K internal capacity bounds match the new
//      ODF §4.6 + §4.7 estimates after the per-record 2B directory
//      overhead is accounted for.
//   5. CRC tampering inside the payload area is detected by
//      inspect_tree_page → bad_crc, and parse() rejects the page.
//

#include "apps/inconel/test/check.hh"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "apps/inconel/format/tree_page.hh"
#include "apps/inconel/tree/page_builder.hh"
#include "apps/inconel/tree/page_reader.hh"

using namespace apps::inconel::format;
using namespace apps::inconel::tree;

// ── helpers ──

static value_ref
make_vr(uint64_t lba, uint16_t off, uint32_t len) {
    return value_ref{paddr{0, lba}, off, len, 0};
}

static paddr
make_paddr(uint64_t lba) {
    return paddr{0, lba};
}

// 32-byte zero-padded ASCII key from a small integer. Lexicographic and
// numeric ordering coincide because all keys are the same length, so
// callers can mix order checks freely without worrying about how
// std::string::operator< handles short-vs-long.
static std::string
key32(int n) {
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%032d", n);
    return std::string(buf, 32);
}

// ── test 1: leaf build/parse/get/find/lower_bound ──

static void
test_leaf_basic() {
    constexpr uint32_t PS = 4096;
    std::vector<char> page(PS);

    leaf_page_builder b;
    b.init(page.data(), PS);

    // Three values + one tombstone, sorted ascending.
    CHECK(b.add_value(key32(1), 100, make_vr(10, 0, 8)));
    CHECK(b.add_value(key32(3), 200, make_vr(11, 0, 8)));
    CHECK(b.add_tombstone(key32(5), 300));
    CHECK(b.add_value(key32(7), 400, make_vr(12, 0, 8)));
    b.finalize();

    leaf_page_reader r;
    CHECK(r.parse(page.data(), PS));
    CHECK(r.record_count() == 4);

    // get(i) walks via the directory.
    auto e0 = r.get(0);
    CHECK(e0.key == key32(1));
    CHECK(e0.kind == record_kind::value);
    CHECK(e0.data_ver == 100);
    CHECK(e0.vr.base.lba == 10);

    auto e1 = r.get(1);
    CHECK(e1.key == key32(3));
    CHECK(e1.kind == record_kind::value);
    CHECK(e1.data_ver == 200);

    auto e2 = r.get(2);
    CHECK(e2.key == key32(5));
    CHECK(e2.kind == record_kind::tombstone);
    CHECK(e2.data_ver == 300);

    auto e3 = r.get(3);
    CHECK(e3.key == key32(7));
    CHECK(e3.kind == record_kind::value);
    CHECK(e3.data_ver == 400);
    CHECK(e3.vr.base.lba == 12);

    // find: hit / miss-before-first / miss-in-gap / miss-after-last.
    auto h = r.find(key32(3));
    CHECK(h.has_value() && h->data_ver == 200);
    CHECK(!r.find(key32(0)).has_value());
    CHECK(!r.find(key32(4)).has_value());
    CHECK(!r.find(key32(8)).has_value());

    // lower_bound covers each band, including the past-the-end position.
    CHECK(r.lower_bound(key32(0)) == 0);
    CHECK(r.lower_bound(key32(1)) == 0);
    CHECK(r.lower_bound(key32(2)) == 1);
    CHECK(r.lower_bound(key32(5)) == 2);
    CHECK(r.lower_bound(key32(6)) == 3);
    CHECK(r.lower_bound(key32(8)) == 4);

    printf("  leaf basic build/parse/get/find/lower_bound: OK\n");
}

// ── test 2: internal build/parse/get/find_child ──

static void
test_internal_basic() {
    constexpr uint32_t PS = 4096;
    std::vector<char> page(PS);

    internal_page_builder b;
    b.init(page.data(), PS);

    // Three separators with children + a rightmost.
    CHECK(b.add_child(key32(10), make_paddr(100)));
    CHECK(b.add_child(key32(20), make_paddr(200)));
    CHECK(b.add_child(key32(30), make_paddr(300)));
    b.set_rightmost_child(make_paddr(999));
    b.finalize();

    internal_page_reader r;
    CHECK(r.parse(page.data(), PS));
    CHECK(r.record_count() == 3);

    // get(i) goes through the directory.
    CHECK(r.get(0).separator_key == key32(10));
    CHECK(r.get(0).child_base.lba == 100);
    CHECK(r.get(1).separator_key == key32(20));
    CHECK(r.get(1).child_base.lba == 200);
    CHECK(r.get(2).separator_key == key32(30));
    CHECK(r.get(2).child_base.lba == 300);

    // find_child: first separator strictly greater than `key` wins,
    // otherwise rightmost.
    //
    //   child[0] covers (-inf, 10)
    //   child[1] covers [10, 20)
    //   child[2] covers [20, 30)
    //   rightmost  covers [30, inf)
    CHECK(r.find_child(key32(5)).lba  == 100);
    CHECK(r.find_child(key32(10)).lba == 200);
    CHECK(r.find_child(key32(15)).lba == 200);
    CHECK(r.find_child(key32(20)).lba == 300);
    CHECK(r.find_child(key32(25)).lba == 300);
    CHECK(r.find_child(key32(30)).lba == 999);
    CHECK(r.find_child(key32(99)).lba == 999);

    // rightmost_child is also reachable directly.
    CHECK(r.rightmost_child().lba == 999);

    printf("  internal basic build/parse/get/find_child: OK\n");
}

// ── test 3: directory offsets monotonic / in-page / decode at record start ──

static void
test_directory_offsets_valid() {
    constexpr uint32_t PS = 16384;
    std::vector<char> page(PS);

    leaf_page_builder b;
    b.init(page.data(), PS);

    constexpr int N = 30;
    for (int i = 0; i < N; ++i) {
        // Mix kinds so the records have varying byte sizes — this
        // catches "directory offset off by sizeof(value_ref)" bugs that
        // a uniform-record loop would mask.
        if (i % 5 == 0) {
            CHECK(b.add_tombstone(key32(i), 1000 + i));
        } else {
            CHECK(b.add_value(key32(i), 1000 + i, make_vr(static_cast<uint64_t>(i + 1), 0, 16)));
        }
    }
    b.finalize();

    leaf_page_reader r;
    CHECK(r.parse(page.data(), PS));
    CHECK(r.record_count() == N);

    const auto* hdr = reinterpret_cast<const tree_slot_header*>(page.data());
    const uint32_t header_end   = sizeof(tree_slot_header);
    const uint32_t dir_bytes    = tree_slot_directory_bytes(N);
    const uint32_t payload_begin = header_end + dir_bytes;

    uint16_t prev_off = 0;
    for (uint16_t i = 0; i < N; ++i) {
        const uint16_t off = load_tree_slot_offset(hdr, i);

        // (1) in-page legal range — at or past the directory's tail and
        // strictly before the page's free-space frontier.
        CHECK(off >= payload_begin);
        CHECK(off <  hdr->free_space_offset);

        // (2) monotonic ascending — records were appended in sorted
        // logical key order, so their byte offsets must also be sorted.
        if (i > 0) CHECK(off > prev_off);
        prev_off = off;

        // (3) the offset really points at a record start — get(i) must
        // decode back to the key we put in.
        auto rec = r.get(i);
        CHECK(rec.key == key32(i));
        CHECK(rec.data_ver == static_cast<uint64_t>(1000 + i));
        if (i % 5 == 0) {
            CHECK(rec.kind == record_kind::tombstone);
        } else {
            CHECK(rec.kind == record_kind::value);
            CHECK(rec.vr.base.lba == static_cast<uint64_t>(i + 1));
        }
    }

    printf("  directory offsets monotonic/in-page/decode-aligned (N=%d): OK\n", N);
}

// ── test 4: rightmost_child reads from free_space_offset - sizeof(paddr) ──

static void
test_internal_rightmost_tail() {
    constexpr uint32_t PS = 4096;
    std::vector<char> page(PS);

    internal_page_builder b;
    b.init(page.data(), PS);

    CHECK(b.add_child(key32(1), make_paddr(11)));
    CHECK(b.add_child(key32(2), make_paddr(22)));
    b.set_rightmost_child(make_paddr(0xDEADBEEFULL));
    b.finalize();

    internal_page_reader r;
    CHECK(r.parse(page.data(), PS));

    // O(1) reader path.
    paddr via_reader = r.rightmost_child();
    CHECK(via_reader.lba == 0xDEADBEEFULL);
    CHECK(via_reader.device_id == 0);

    // Direct byte-level confirmation: the tail of the payload area is
    // exactly a paddr starting at `free_space_offset - sizeof(paddr)`.
    // If page_builder ever forgets to advance free_space_offset past the
    // rightmost child base, this CHECK fires before the lookup tests
    // would notice.
    const auto* hdr = reinterpret_cast<const tree_slot_header*>(page.data());
    paddr direct;
    std::memcpy(&direct,
                page.data() + hdr->free_space_offset - sizeof(paddr),
                sizeof(paddr));
    CHECK(direct.lba == 0xDEADBEEFULL);
    CHECK(direct.device_id == 0);

    printf("  internal rightmost_child reads tail at free_space_offset - sizeof(paddr): OK\n");
}

// ── test 5: 4K and 16K capacity bounds (matches ODF §4.6 / §4.7) ──

static void
test_capacity_4k_leaf_32b() {
    constexpr uint32_t PS = 4096;
    std::vector<char> page(PS);

    leaf_page_builder b;
    b.init(page.data(), PS);

    int n = 0;
    while (b.add_value(key32(n), 1, make_vr(1, 0, 8))) ++n;
    b.finalize();

    // 32B-key value record = 11 (header) + 32 (key) + 18 (value_ref) = 61
    // bytes; plus 2B directory entry = 63. Available after header =
    // 4096 - 19 = 4077. floor(4077 / 63) = 64.
    CHECK(n == 64);

    leaf_page_reader r;
    CHECK(r.parse(page.data(), PS));
    CHECK(r.record_count() == 64);

    printf("  4K leaf 32B-key capacity = %d (expected 64): OK\n", n);
}

static void
test_capacity_16k_leaf_32b() {
    constexpr uint32_t PS = 16384;
    std::vector<char> page(PS);

    leaf_page_builder b;
    b.init(page.data(), PS);

    int n = 0;
    while (b.add_value(key32(n), 1, make_vr(1, 0, 8))) ++n;
    b.finalize();

    // Available 16365, per record + dir = 63 → floor(16365 / 63) = 259.
    CHECK(n == 259);

    leaf_page_reader r;
    CHECK(r.parse(page.data(), PS));
    CHECK(r.record_count() == 259);

    printf("  16K leaf 32B-key capacity = %d (expected 259): OK\n", n);
}

static void
test_capacity_16k_internal_32b() {
    constexpr uint32_t PS = 16384;
    std::vector<char> page(PS);

    internal_page_builder b;
    b.init(page.data(), PS);

    int n = 0;
    while (b.add_child(key32(n), make_paddr(static_cast<uint64_t>(n + 1)))) ++n;
    b.set_rightmost_child(make_paddr(0xFFFF));
    b.finalize();

    // 32B separator = 2 + 32 + 10 = 44; plus 2B directory = 46. Plus a
    // trailing 10B rightmost child base. Total fanout =
    // floor((16365 - 10) / 46) = 355 separators + 1 rightmost = 356.
    CHECK(n == 355);

    internal_page_reader r;
    CHECK(r.parse(page.data(), PS));
    CHECK(r.record_count() == 355);
    CHECK(r.rightmost_child().lba == 0xFFFF);

    printf("  16K internal 32B-key capacity = %d separators + 1 rightmost = %d children (expected 356): OK\n",
           n, n + 1);
}

// ── test 6: CRC tampering is rejected ──

static void
test_crc_tamper_rejected() {
    constexpr uint32_t PS = 4096;
    std::vector<char> page(PS);

    leaf_page_builder b;
    b.init(page.data(), PS);

    CHECK(b.add_value(key32(1), 1, make_vr(7, 0, 4)));
    CHECK(b.add_value(key32(2), 2, make_vr(8, 0, 4)));
    b.finalize();

    // Sanity: page is currently good.
    CHECK(inspect_tree_page(page.data(), PS) == tree_page_status::ok);

    // Flip a byte deep in the payload area (well past the header /
    // directory / CRC field). Any single-bit flip in covered bytes must
    // produce a different CRC-32C.
    const auto* hdr = reinterpret_cast<const tree_slot_header*>(page.data());
    const uint32_t flip_at = hdr->free_space_offset - 1;
    page[flip_at] ^= 0x01;

    auto status = inspect_tree_page(page.data(), PS);
    CHECK(status == tree_page_status::bad_crc);

    leaf_page_reader r;
    CHECK(!r.parse(page.data(), PS));

    printf("  CRC tampering rejected (status=%s): OK\n", tree_page_status_to_string(status));
}

int main() {
    printf("tree page format tests:\n");

    test_leaf_basic();
    test_internal_basic();
    test_directory_offsets_valid();
    test_internal_rightmost_tail();
    test_capacity_4k_leaf_32b();
    test_capacity_16k_leaf_32b();
    test_capacity_16k_internal_32b();
    test_crc_tamper_rejected();

    printf("all passed\n");
    return 0;
}
