#ifndef APPS_INCONEL_FORMAT_CRC32C_HH
#define APPS_INCONEL_FORMAT_CRC32C_HH

#if !defined(__SSE4_2__)
#  error "format/crc32c.hh requires SSE4.2 (-march=native). Inconel targets x86_64 w/ hardware CRC32C."
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <nmmintrin.h>

namespace apps::inconel::format {

    struct crc32c_stream {
        uint32_t st = 0xFFFFFFFFu;

        void update(const void* p, size_t n) noexcept;

        [[nodiscard]] uint32_t finish() const noexcept {
            return st ^ 0xFFFFFFFFu;
        }
    };

    inline void
    crc32c_stream::update(const void* p, size_t n) noexcept {
        const char* b = static_cast<const char*>(p);
        uint64_t    crc = st;

        while (n >= 8) {
            uint64_t w;
            std::memcpy(&w, b, sizeof(w));
            crc = _mm_crc32_u64(crc, w);
            b += sizeof(w);
            n -= sizeof(w);
        }

        while (n--) {
            crc = _mm_crc32_u8(static_cast<uint32_t>(crc),
                               static_cast<unsigned char>(*b++));
        }

        st = static_cast<uint32_t>(crc);
    }

    [[nodiscard]] inline uint32_t
    crc32c(const void* p, size_t n) noexcept {
        crc32c_stream s;
        s.update(p, n);
        return s.finish();
    }

    [[nodiscard]] inline uint32_t
    crc32c_skip(const void* p,
                size_t      n,
                size_t      hole_off,
                size_t      hole_len) noexcept {
        crc32c_stream s;
        s.update(p, hole_off);
        s.update(static_cast<const char*>(p) + hole_off + hole_len,
                 n - hole_off - hole_len);
        return s.finish();
    }

}

#endif // APPS_INCONEL_FORMAT_CRC32C_HH
