#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace sider::resp {

    // Response descriptor — no copy, just describes what to write at send time.
    struct resp_slot {
        enum type_t : uint8_t { EMPTY, BULK, NIL, OK, PONG, INTEGER, ERR, EMPTY_ARRAY };
        type_t type = EMPTY;
        const char* data = nullptr;   // BULK: points to slab memory; ERR: error message
        uint32_t len = 0;             // BULK: value length; ERR: message length
        int64_t int_val = 0;          // INTEGER: value

        // Compute RESP wire size for this slot.
        uint32_t wire_size() const {
            switch (type) {
                case BULK: {
                    // "$<len>\r\n<data>\r\n"
                    char tmp[16];
                    int hdr = snprintf(tmp, sizeof(tmp), "$%u\r\n", len);
                    return hdr + len + 2;
                }
                case NIL:         return 5;   // "$-1\r\n"
                case OK:          return 5;   // "+OK\r\n"
                case PONG:        return 7;   // "+PONG\r\n"
                case EMPTY_ARRAY: return 4;   // "*0\r\n"
                case INTEGER: {
                    char tmp[32];
                    return snprintf(tmp, sizeof(tmp), ":%ld\r\n", int_val);
                }
                case ERR: {
                    // "-ERR <msg>\r\n"
                    return 5 + len + 2;
                }
                default: return 0;
            }
        }

        // Write RESP wire format into buf. Returns bytes written.
        uint32_t write_to(char* buf) const {
            switch (type) {
                case BULK: {
                    int hdr = snprintf(buf, 16, "$%u\r\n", len);
                    std::memcpy(buf + hdr, data, len);
                    buf[hdr + len] = '\r';
                    buf[hdr + len + 1] = '\n';
                    return hdr + len + 2;
                }
                case NIL:
                    std::memcpy(buf, "$-1\r\n", 5); return 5;
                case OK:
                    std::memcpy(buf, "+OK\r\n", 5); return 5;
                case PONG:
                    std::memcpy(buf, "+PONG\r\n", 7); return 7;
                case EMPTY_ARRAY:
                    std::memcpy(buf, "*0\r\n", 4); return 4;
                case INTEGER:
                    return snprintf(buf, 32, ":%ld\r\n", int_val);
                case ERR: {
                    std::memcpy(buf, "-ERR ", 5);
                    std::memcpy(buf + 5, data, len);
                    buf[5 + len] = '\r';
                    buf[5 + len + 1] = '\n';
                    return 5 + len + 2;
                }
                default: return 0;
            }
        }
    };

    // A batch of RESP commands from one recv cycle.
    struct cmd_batch {
        char* data = nullptr;
        uint32_t data_len = 0;

        static constexpr uint32_t MAX_CMDS = 128;
        uint32_t offsets[MAX_CMDS + 1] = {};
        uint32_t count = 0;

        // Response slots — filled by execute, consumed by batch_send.
        std::vector<resp_slot> responses;
        bool has_quit = false;

        // DMA buffers from cold reads — returned to pool on destruction.
        struct owned_dma {
            char* ptr;
            void (*free_fn)(char*);
        };
        std::vector<owned_dma> owned_bufs;

        const char* cmd_data(uint32_t i) const { return data + offsets[i]; }
        uint32_t    cmd_len(uint32_t i)  const { return offsets[i + 1] - offsets[i]; }

        cmd_batch() = default;

        cmd_batch(cmd_batch&& o) noexcept
            : data(o.data), data_len(o.data_len), count(o.count)
            , responses(std::move(o.responses)), has_quit(o.has_quit)
            , owned_bufs(std::move(o.owned_bufs)) {
            std::memcpy(offsets, o.offsets, (count + 1) * sizeof(uint32_t));
            o.data = nullptr; o.data_len = 0; o.count = 0;
        }
        cmd_batch& operator=(cmd_batch&& o) noexcept {
            if (this != &o) {
                for (auto& d : owned_bufs) d.free_fn(d.ptr);
                delete[] data;
                data = o.data; data_len = o.data_len; count = o.count;
                responses = std::move(o.responses); has_quit = o.has_quit;
                owned_bufs = std::move(o.owned_bufs);
                std::memcpy(offsets, o.offsets, (count + 1) * sizeof(uint32_t));
                o.data = nullptr; o.data_len = 0; o.count = 0;
            }
            return *this;
        }
        cmd_batch(const cmd_batch&) = delete;
        cmd_batch& operator=(const cmd_batch&) = delete;
        ~cmd_batch() {
            for (auto& d : owned_bufs) d.free_fn(d.ptr);
            delete[] data;
        }
    };

} // namespace sider::resp
