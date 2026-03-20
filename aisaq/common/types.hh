#ifndef AISAQ_COMMON_TYPES_HH
#define AISAQ_COMMON_TYPES_HH

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace aisaq {

    constexpr uint32_t SECTOR_SIZE = 4096;
    constexpr uint32_t NUM_PQ_CENTROIDS = 256;

    // DiskANN "bin" file header: uint32_t nrows, uint32_t ncols
    struct
    bin_header {
        uint32_t nrows;
        uint32_t ncols;
    };

    // Read a DiskANN binary file: header (nrows, ncols) + data[nrows * ncols]
    // Returns pointer to heap-allocated data, caller owns it
    template <typename T>
    auto
    load_bin_file(const char* path, uint32_t& nrows, uint32_t& ncols) {
        auto* f = fopen(path, "rb");
        if (!f)
            throw std::runtime_error(std::string("failed to open: ") + path);

        bin_header hdr{};
        if (fread(&hdr, sizeof(bin_header), 1, f) != 1) {
            fclose(f);
            throw std::runtime_error(std::string("failed to read header: ") + path);
        }

        nrows = hdr.nrows;
        ncols = hdr.ncols;

        auto* data = new T[static_cast<size_t>(nrows) * ncols];
        if (fread(data, sizeof(T), static_cast<size_t>(nrows) * ncols, f) != static_cast<size_t>(nrows) * ncols) {
            delete[] data;
            fclose(f);
            throw std::runtime_error(std::string("failed to read data: ") + path);
        }

        fclose(f);
        return data;
    }

}

#endif //AISAQ_COMMON_TYPES_HH
