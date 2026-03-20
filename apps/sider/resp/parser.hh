#pragma once

#include <cstring>
#include <string_view>
#include "env/scheduler/net/common/frame.hh"

namespace sider::resp {

    static constexpr uint8_t MAX_ARGS = 16;

    struct parsed_command {
        std::string_view argv[MAX_ARGS];
        uint8_t argc = 0;

        std::string_view name() const { return argc > 0 ? argv[0] : std::string_view{}; }
        std::string_view arg(uint8_t i) const { return i < argc ? argv[i] : std::string_view{}; }
    };

    namespace detail {

        static inline bool
        ci_equal(std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); i++) {
                char ca = a[i] | 0x20;  // tolower for ASCII letters
                char cb = b[i] | 0x20;
                if (ca != cb) return false;
            }
            return true;
        }

        // Parse inline command: "COMMAND arg1 arg2\r\n"
        static inline parsed_command
        parse_inline(const char* data, size_t len) {
            parsed_command cmd;
            // Strip trailing \r\n
            while (len > 0 && (data[len - 1] == '\r' || data[len - 1] == '\n'))
                len--;

            size_t i = 0;
            while (i < len && cmd.argc < MAX_ARGS) {
                // Skip spaces
                while (i < len && data[i] == ' ') i++;
                if (i >= len) break;

                // Find end of token
                size_t start = i;
                while (i < len && data[i] != ' ') i++;
                cmd.argv[cmd.argc++] = std::string_view(data + start, i - start);
            }
            return cmd;
        }

        // Parse multibulk command: "*N\r\n$len\r\ndata\r\n..."
        static inline parsed_command
        parse_multibulk(const char* data, size_t len) {
            parsed_command cmd;
            size_t pos = 0;

            // Skip *N\r\n
            while (pos < len && data[pos] != '\n') pos++;
            pos++;  // skip \n

            while (pos < len && cmd.argc < MAX_ARGS) {
                if (data[pos] != '$') break;
                pos++;  // skip $

                // Parse length
                int bulk_len = 0;
                while (pos < len && data[pos] != '\r') {
                    bulk_len = bulk_len * 10 + (data[pos] - '0');
                    pos++;
                }
                pos += 2;  // skip \r\n

                if (pos + bulk_len > len) break;
                cmd.argv[cmd.argc++] = std::string_view(data + pos, bulk_len);
                pos += bulk_len + 2;  // skip data + \r\n
            }
            return cmd;
        }

    } // namespace detail

    static inline parsed_command
    parse_command(const pump::scheduler::net::net_frame& frame) {
        auto* data = frame.data();
        auto len = frame.size();
        if (len == 0) return {};

        if (data[0] == '*')
            return detail::parse_multibulk(data, len);
        else
            return detail::parse_inline(data, len);
    }

    static inline bool
    cmd_is(const parsed_command& cmd, std::string_view name) {
        return detail::ci_equal(cmd.name(), name);
    }

} // namespace sider::resp
