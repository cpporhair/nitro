#pragma once

#include "env/scheduler/net/tcp/common/struct.hh"
#include "env/scheduler/net/tcp/common/detail.hh"

namespace sider::resp {

    using net_frame = pump::scheduler::net::net_frame;
    using packet_buffer = pump::scheduler::tcp::common::packet_buffer;
    using send_req = pump::scheduler::tcp::common::send_req;

    namespace detail {

        static inline char
        peek(const packet_buffer* buf, size_t offset) {
            return buf->_data[(buf->head() + offset) & (buf->_size - 1)];
        }

        // Find \r\n starting from `start` offset (relative to head).
        // Returns offset of \r, or -1 if not found.
        static inline int
        find_crlf(const packet_buffer* buf, size_t start) {
            size_t used = buf->used();
            if (used < start + 2) return -1;
            for (size_t i = start; i + 1 < used; i++) {
                if (peek(buf, i) == '\r' && peek(buf, i + 1) == '\n')
                    return static_cast<int>(i);
            }
            return -1;
        }

        // Parse integer from buffer between positions [start, end).
        static inline int
        parse_int(const packet_buffer* buf, size_t start, size_t end) {
            if (start >= end) return -1;
            int val = 0;
            bool neg = false;
            size_t i = start;
            if (peek(buf, i) == '-') { neg = true; i++; }
            for (; i < end; i++) {
                char c = peek(buf, i);
                if (c < '0' || c > '9') return -1;
                val = val * 10 + (c - '0');
            }
            return neg ? -val : val;
        }

        // Check if a complete inline command exists. Returns total length including \r\n, or 0.
        static inline size_t
        find_complete_inline(const packet_buffer* buf) {
            int pos = find_crlf(buf, 0);
            if (pos < 0) return 0;
            return static_cast<size_t>(pos) + 2;
        }

        // Check if a complete multibulk command exists. Returns total length, or 0.
        static inline size_t
        find_complete_multibulk(const packet_buffer* buf) {
            // Parse *N\r\n
            int first_crlf = find_crlf(buf, 1);
            if (first_crlf < 0) return 0;

            int n = parse_int(buf, 1, first_crlf);
            if (n < 0) return 0;

            size_t pos = static_cast<size_t>(first_crlf) + 2;

            for (int i = 0; i < n; i++) {
                if (pos >= buf->used()) return 0;
                char marker = peek(buf, pos);
                if (marker != '$') return 0;  // protocol error

                int dollar_crlf = find_crlf(buf, pos + 1);
                if (dollar_crlf < 0) return 0;

                int len = parse_int(buf, pos + 1, dollar_crlf);
                if (len < 0) return 0;

                // $len\r\n + data + \r\n
                pos = static_cast<size_t>(dollar_crlf) + 2 + static_cast<size_t>(len) + 2;
                if (pos > buf->used()) return 0;
            }

            return pos;
        }

        static inline size_t
        find_complete_command(const packet_buffer* buf) {
            if (buf->used() == 0) return 0;
            char first = peek(buf, 0);
            if (first == '*')
                return find_complete_multibulk(buf);
            else
                return find_complete_inline(buf);
        }

    } // namespace detail

    // RESP2 unpacker for tcp_ring_buffer.
    // Extracts one complete RESP2 command per unpack() call.
    struct resp2_unpacker {
        using frame_type = net_frame;

        static frame_type
        unpack(packet_buffer* buf) {
            auto cmd_len = detail::find_complete_command(buf);
            if (cmd_len == 0) return {};

            auto frame = buf->handle_data(cmd_len,
                pump::scheduler::tcp::common::detail::frame_copier);
            if (frame.size() > 0)
                buf->forward_head(cmd_len);
            return frame;
        }

        static bool
        empty(const frame_type& f) { return f.size() == 0; }

        // RESP2 responses are self-delimiting, no framing needed.
        static void
        prepare_send(send_req* req) {
            req->_send_vec[0] = {req->frame._data, req->frame._len};
            req->_send_cnt = 1;
        }
    };

} // namespace sider::resp
