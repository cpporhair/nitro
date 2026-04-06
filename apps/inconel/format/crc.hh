#ifndef APPS_INCONEL_FORMAT_CRC_HH
#define APPS_INCONEL_FORMAT_CRC_HH

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <nmmintrin.h>

namespace apps::inconel::format {

    inline uint32_t
    crc32c(const void* data, size_t len, uint32_t crc = 0) {
        auto* p = static_cast<const uint8_t*>(data);

        while (len >= 4) {
            uint32_t val;
            std::memcpy(&val, p, 4);
            crc = _mm_crc32_u32(crc, val);
            p += 4;
            len -= 4;
        }
        while (len > 0) {
            crc = _mm_crc32_u8(crc, *p);
            ++p;
            --len;
        }
        return crc;
    }

}

#endif //APPS_INCONEL_FORMAT_CRC_HH
