// Compatibility gate for INC-050 direction B (inline hardware CRC32C).
//
// Role: REVIEW-side test (design doc 053 §7). The production change replaced
// every absl::ComputeCrc32c/ExtendCrc32c call site with format/crc32c.hh. The
// load-bearing invariant is that the new inline implementation produces CRC
// values BIT-IDENTICAL to absl (053 §3.4 I1/I2/I3): same standard CRC-32C, so
// existing on-disk bytes / golden layouts stay valid and the impl is provably
// real Castagnoli, not a self-consistent-but-nonstandard variant.
//
// This test uses absl as the reference oracle. It is NOT a production file and
// the production implementer never reads it.

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <absl/crc/crc32c.h>
#include <absl/strings/string_view.h>

#include "apps/inconel/format/crc32c.hh"
#include "apps/inconel/format/tree_page.hh"
#include "apps/inconel/test/check.hh"

namespace {

using apps::inconel::format::crc32c;
using apps::inconel::format::crc32c_skip;
using apps::inconel::format::crc32c_stream;

uint32_t absl_oracle(const char* p, size_t n) {
    return static_cast<uint32_t>(
        absl::ComputeCrc32c(absl::string_view(p, n)));
}

std::vector<char> make_buf(size_t n, uint64_t seed) {
    std::vector<char> v(n);
    std::mt19937_64 rng(seed);
    for (size_t i = 0; i < n; ++i)
        v[i] = static_cast<char>(rng() & 0xFF);
    return v;
}

// I1: one-shot crc32c == absl, across sizes and unaligned starts.
void test_i1_oneshot() {
    const size_t sizes[] = {0, 1, 2, 3, 4, 5, 6, 7, 8,
                            9, 15, 16, 17, 31, 63, 64, 65,
                            255, 256, 257, 4095, 4096, 4097, 16384};
    for (size_t sz : sizes) {
        // +8 slack so we can also test an unaligned start offset.
        auto buf = make_buf(sz + 8, 0x1111 + sz);
        for (size_t off = 0; off <= 7; ++off) {
            const char* p = buf.data() + off;
            CHECK(crc32c(p, sz) == absl_oracle(p, sz));
        }
    }
}

// I2: streaming chain across arbitrary split points == one-shot == absl,
// regardless of whether the split lands on an 8-byte boundary.
void test_i2_chain() {
    const size_t n = 16384;
    auto buf = make_buf(n, 0x2222);
    const char* p = buf.data();
    const uint32_t whole = absl_oracle(p, n);
    CHECK(crc32c(p, n) == whole);

    const size_t splits[] = {0, 1, 7, 8, 9, 15, 16, 17,
                             4095, 4096, 4097, n - 1, n};
    for (size_t a : splits) {
        crc32c_stream s;
        s.update(p, a);
        s.update(p + a, n - a);
        CHECK(s.finish() == whole);
    }

    // 3-part split mirrors wal_entry_parts_crc (#8): header|value_ref|key.
    for (size_t a : {1u, 8u, 13u, 100u}) {
        for (size_t b : {1u, 8u, 19u, 200u}) {
            if (a + b >= n) continue;
            crc32c_stream s;
            s.update(p, a);
            s.update(p + a, b);
            s.update(p + a + b, n - a - b);
            CHECK(s.finish() == whole);
        }
    }

    // Irregular many-chunk feed mirrors segmented-frame consumption
    // (value_object #5 / page_reader #11): byte-exact split sequence.
    {
        crc32c_stream s;
        std::mt19937_64 rng(0x5151);
        size_t pos = 0;
        while (pos < n) {
            size_t chunk = 1 + (rng() % 777);
            if (pos + chunk > n) chunk = n - pos;
            s.update(p + pos, chunk);
            pos += chunk;
        }
        CHECK(s.finish() == whole);
    }
}

// I3: crc32c_skip == absl two-call (compute prefix, extend suffix),
// for the exact page_crc hole the tree page path uses, plus generic holes.
void test_i3_skip() {
    const size_t n = 16384;
    auto buf = make_buf(n, 0x3333);
    const char* p = buf.data();

    struct hole_t { size_t off, len; };
    const hole_t holes[] = {{15, 4}, {0, 4}, {0, 8}, {1, 1},
                            {100, 16}, {n - 4, 4}};
    for (auto h : holes) {
        absl::crc32c_t oracle =
            absl::ComputeCrc32c(absl::string_view(p, h.off));
        oracle = absl::ExtendCrc32c(
            oracle, absl::string_view(p + h.off + h.len, n - h.off - h.len));
        CHECK(crc32c_skip(p, n, h.off, h.len) ==
              static_cast<uint32_t>(oracle));
    }
}

// Call-site wiring: the production tree_page_compute_crc must equal the
// original absl two-call skip over the real page_crc field offset. Proves the
// #1 replacement preserved the covered range exactly.
void test_tree_page_wiring() {
    using apps::inconel::format::tree_page_compute_crc;
    using apps::inconel::format::tree_slot_header;
    const uint32_t page_size = 16384;
    auto buf = make_buf(page_size, 0x4444);
    const char* p = buf.data();

    const size_t off = offsetof(tree_slot_header, page_crc);
    const size_t len = sizeof(uint32_t);
    absl::crc32c_t oracle = absl::ComputeCrc32c(absl::string_view(p, off));
    oracle = absl::ExtendCrc32c(
        oracle, absl::string_view(p + off + len, page_size - off - len));

    CHECK(tree_page_compute_crc(p, page_size) ==
          static_cast<uint32_t>(oracle));
}

// Known CRC-32C test vector ("123456789" -> 0xE3069283). Anchors that the
// impl is standard Castagnoli, independent of absl agreeing with itself.
void test_known_vector() {
    const char* s = "123456789";
    CHECK(crc32c(s, 9) == 0xE3069283u);
}

} // namespace

int main() {
    test_i1_oneshot();
    test_i2_chain();
    test_i3_skip();
    test_tree_page_wiring();
    test_known_vector();
    std::printf("inconel_test_crc32c: all CRC32C compatibility checks passed\n");
    return 0;
}
