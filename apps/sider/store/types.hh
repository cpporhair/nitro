#pragma once

#include <cstdint>
#include <cstdlib>

namespace sider::store {

    static constexpr uint32_t PAGE_SIZE = 4096;

    enum size_class_t : uint8_t {
        SC_64   = 0,
        SC_128  = 1,
        SC_256  = 2,
        SC_512  = 3,
        SC_1K   = 4,
        SC_2K   = 5,
        SC_4K   = 6,
        SC_COUNT = 7
    };

    static constexpr uint32_t size_class_bytes[SC_COUNT] = {
        64, 128, 256, 512, 1024, 2048, 4096
    };

    static constexpr uint32_t slots_per_page[SC_COUNT] = {
        64, 32, 16, 8, 4, 2, 1
    };

    // Bitmask with all slot bits set for a given size class.
    static inline uint64_t full_mask_for(size_class_t sc) {
        uint32_t n = slots_per_page[sc];
        return (n == 64) ? ~0ULL : ((1ULL << n) - 1);
    }

    // Find the smallest size class that fits the given value size.
    static inline size_class_t size_class_for(uint32_t size) {
        if (size <= 64)   return SC_64;
        if (size <= 128)  return SC_128;
        if (size <= 256)  return SC_256;
        if (size <= 512)  return SC_512;
        if (size <= 1024) return SC_1K;
        if (size <= 2048) return SC_2K;
        return SC_4K;
    }

    // Byte offset of a slot within a page.
    static inline uint32_t slot_offset(size_class_t sc, uint8_t slot_index) {
        return static_cast<uint32_t>(slot_index) * size_class_bytes[sc];
    }

    // Large value: value_len > PAGE_SIZE
    static inline bool is_large_value(uint32_t size) { return size > PAGE_SIZE; }
    static inline uint32_t pages_for(uint32_t size) { return (size + PAGE_SIZE - 1) / PAGE_SIZE; }

    // Pluggable page allocator.
    // Default: aligned_alloc/free (used by tests without SPDK).
    // main.cc sets these to DMA versions after SPDK init.
    namespace _page_alloc {
        inline char* (*alloc_fn)() = nullptr;
        inline void (*free_fn)(char*) = nullptr;
    }

    static inline char* alloc_page() {
        if (_page_alloc::alloc_fn) return _page_alloc::alloc_fn();
        return static_cast<char*>(std::aligned_alloc(PAGE_SIZE, PAGE_SIZE));
    }

    static inline void free_page(char* ptr) {
        if (_page_alloc::free_fn) _page_alloc::free_fn(ptr);
        else std::free(ptr);
    }

    // Pluggable large value allocator (contiguous DMA-safe memory).
    namespace _large_alloc {
        inline char* (*alloc_fn)(uint32_t size) = nullptr;
        inline void (*free_fn)(char* ptr) = nullptr;
    }

    static inline char* alloc_large(uint32_t size) {
        uint32_t aligned = pages_for(size) * PAGE_SIZE;
        if (_large_alloc::alloc_fn) return _large_alloc::alloc_fn(aligned);
        return static_cast<char*>(std::aligned_alloc(PAGE_SIZE, aligned));
    }

    static inline void free_large(char* ptr) {
        if (_large_alloc::free_fn) _large_alloc::free_fn(ptr);
        else std::free(ptr);
    }

} // namespace sider::store
