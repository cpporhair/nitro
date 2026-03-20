#pragma once

#include <cstring>
#include <cstdio>
#include <string_view>

namespace sider::resp {

    // Owning buffer for a RESP2 response. Move-only.
    struct resp_buffer {
        char* _data = nullptr;
        uint32_t _len = 0;

        resp_buffer() = default;
        resp_buffer(char* data, uint32_t len) : _data(data), _len(len) {}

        resp_buffer(resp_buffer&& o) noexcept : _data(o._data), _len(o._len) {
            o._data = nullptr; o._len = 0;
        }

        resp_buffer& operator=(resp_buffer&& o) noexcept {
            if (this != &o) {
                delete[] _data;
                _data = o._data; _len = o._len;
                o._data = nullptr; o._len = 0;
            }
            return *this;
        }

        resp_buffer(const resp_buffer&) = delete;
        resp_buffer& operator=(const resp_buffer&) = delete;

        ~resp_buffer() { delete[] _data; }

        // Release ownership for tcp::send.
        char* release() noexcept { auto* p = _data; _data = nullptr; _len = 0; return p; }
        uint32_t size() const { return _len; }
    };

    namespace detail {
        static inline resp_buffer
        make_const(const char* str, size_t len) {
            auto* buf = new char[len];
            std::memcpy(buf, str, len);
            return {buf, static_cast<uint32_t>(len)};
        }
    }

    // +OK\r\n
    static inline resp_buffer ok() {
        return detail::make_const("+OK\r\n", 5);
    }

    // +PONG\r\n
    static inline resp_buffer pong() {
        return detail::make_const("+PONG\r\n", 7);
    }

    // $-1\r\n
    static inline resp_buffer nil() {
        return detail::make_const("$-1\r\n", 5);
    }

    // *0\r\n
    static inline resp_buffer empty_array() {
        return detail::make_const("*0\r\n", 4);
    }

    // -ERR msg\r\n
    static inline resp_buffer error(std::string_view msg) {
        // "-ERR " + msg + "\r\n"
        size_t len = 5 + msg.size() + 2;
        auto* buf = new char[len];
        std::memcpy(buf, "-ERR ", 5);
        std::memcpy(buf + 5, msg.data(), msg.size());
        buf[len - 2] = '\r';
        buf[len - 1] = '\n';
        return {buf, static_cast<uint32_t>(len)};
    }

    // $len\r\ndata\r\n
    static inline resp_buffer bulk_string(const char* data, uint32_t data_len) {
        char hdr[32];
        int hdr_len = snprintf(hdr, sizeof(hdr), "$%u\r\n", data_len);
        size_t total = hdr_len + data_len + 2;
        auto* buf = new char[total];
        std::memcpy(buf, hdr, hdr_len);
        std::memcpy(buf + hdr_len, data, data_len);
        buf[total - 2] = '\r';
        buf[total - 1] = '\n';
        return {buf, static_cast<uint32_t>(total)};
    }

    // :N\r\n
    static inline resp_buffer integer(int64_t n) {
        char tmp[32];
        int len = snprintf(tmp, sizeof(tmp), ":%ld\r\n", n);
        return detail::make_const(tmp, len);
    }

} // namespace sider::resp
