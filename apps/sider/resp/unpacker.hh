#pragma once

#include "env/scheduler/net/tcp/common/struct.hh"
#include "env/scheduler/net/tcp/common/detail.hh"
#include "resp/batch.hh"

namespace sider::resp {

    using net_frame = pump::scheduler::net::net_frame;
    using packet_buffer = pump::scheduler::tcp::common::packet_buffer;
    using send_req = pump::scheduler::tcp::common::send_req;

    namespace detail {

        static inline char
        peek(const packet_buffer* buf, size_t offset) {
            return buf->_data[(buf->head() + offset) & (buf->_size - 1)];
        }

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

        // Find complete command at arbitrary offset from head.
        static inline size_t
        find_complete_inline_at(const packet_buffer* buf, size_t start) {
            int pos = find_crlf(buf, start);
            if (pos < 0) return 0;
            return static_cast<size_t>(pos) + 2 - start;
        }

        static inline size_t
        find_complete_multibulk_at(const packet_buffer* buf, size_t start) {
            int first_crlf = find_crlf(buf, start + 1);
            if (first_crlf < 0) return 0;
            int n = parse_int(buf, start + 1, first_crlf);
            if (n < 0) return 0;
            size_t pos = static_cast<size_t>(first_crlf) + 2;
            for (int i = 0; i < n; i++) {
                if (pos >= buf->used()) return 0;
                char marker = peek(buf, pos);
                if (marker != '$') return 0;
                int dollar_crlf = find_crlf(buf, pos + 1);
                if (dollar_crlf < 0) return 0;
                int len = parse_int(buf, pos + 1, dollar_crlf);
                if (len < 0) return 0;
                pos = static_cast<size_t>(dollar_crlf) + 2 + static_cast<size_t>(len) + 2;
                if (pos > buf->used()) return 0;
            }
            return pos - start;
        }

        static inline size_t
        find_complete_command_at(const packet_buffer* buf, size_t start) {
            if (buf->used() <= start) return 0;
            char first = peek(buf, start);
            if (first == '*')
                return find_complete_multibulk_at(buf, start);
            else
                return find_complete_inline_at(buf, start);
        }

        static inline size_t
        find_complete_command(const packet_buffer* buf) {
            return find_complete_command_at(buf, 0);
        }

        // Copy ring buffer data to contiguous destination.
        struct batch_copier {
            char* dest;
            size_t operator()() const noexcept { return 0; }
            size_t operator()(const char* p, size_t len) const noexcept {
                std::memcpy(dest, p, len);
                return len;
            }
            size_t operator()(const char* p1, size_t l1,
                              const char* p2, size_t l2) const noexcept {
                std::memcpy(dest, p1, l1);
                std::memcpy(dest + l1, p2, l2);
                return l1 + l2;
            }
        };

    } // namespace detail

    // Original single-command unpacker (kept for reference / other uses).
    struct resp2_unpacker {
        using frame_type = net_frame;

        static frame_type unpack(packet_buffer* buf) {
            auto cmd_len = detail::find_complete_command(buf);
            if (cmd_len == 0) return {};
            auto frame = buf->handle_data(cmd_len,
                pump::scheduler::tcp::common::detail::frame_copier);
            if (frame.size() > 0)
                buf->forward_head(cmd_len);
            return frame;
        }

        static bool empty(const frame_type& f) { return f.size() == 0; }

        static void prepare_send(send_req* req) {
            req->_send_vec[0] = {req->frame._data, req->frame._len};
            req->_send_cnt = 1;
        }
    };

    // Batch unpacker: extracts ALL complete commands as one cmd_batch.
    // Ring buffer's for loop calls on_frame exactly once per on_recv.
    struct resp2_batch_unpacker {
        using frame_type = cmd_batch;

        static frame_type unpack(packet_buffer* buf) {
            uint32_t offsets[cmd_batch::MAX_CMDS + 1];
            uint32_t count = 0;
            size_t total = 0;

            while (count < cmd_batch::MAX_CMDS) {
                size_t cmd_len = detail::find_complete_command_at(buf, total);
                if (cmd_len == 0) break;
                offsets[count] = static_cast<uint32_t>(total);
                count++;
                total += cmd_len;
            }
            if (count == 0) return {};

            offsets[count] = static_cast<uint32_t>(total);

            auto* dest = new char[total];
            buf->handle_data(total, detail::batch_copier{dest});
            buf->forward_head(total);

            cmd_batch batch;
            batch.data = dest;
            batch.data_len = static_cast<uint32_t>(total);
            batch.count = count;
            std::memcpy(batch.offsets, offsets, (count + 1) * sizeof(uint32_t));
            batch.responses.resize(count);
            return batch;
        }

        static bool empty(const frame_type& b) { return b.count == 0; }

        static void prepare_send(send_req* req) {
            req->_send_vec[0] = {req->frame._data, req->frame._len};
            req->_send_cnt = 1;
        }
    };

} // namespace sider::resp
