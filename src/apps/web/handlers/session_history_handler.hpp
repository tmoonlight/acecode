#pragma once

#include "session/session_history_page.hpp"
#include <charconv>
#include <string_view>

namespace acecode::web {

struct ParsedHistoryRequest {
    bool paged = false;
    SessionHistoryRequest request;
    std::string error;
};

inline ParsedHistoryRequest parse_history_request(
    const char* limit, const char* before, const char* after,
    const char* position, const char* ordinal, std::uint64_t since) {
    ParsedHistoryRequest result;
    result.paged = limit || before || after || position || ordinal;
    if (!result.paged) return result;
    const auto fail = [&] { result.error = "Invalid history pagination parameters"; return result; };
    if (since != 0 || (before && after) || (position && ordinal) || (after && (position || ordinal))) return fail();
    if (limit) {
        const std::string_view value(limit);
        if (value.empty()) return fail();
        std::size_t count = 0;
        for (const auto c : value) {
            if (c < '0' || c > '9') return fail();
            count = (std::min)(kHistoryMaxLimit, count * 10 + static_cast<std::size_t>(c - '0'));
        }
        if (count == 0) return fail();
        result.request.limit = count;
    }
    if (before) {
        result.request.before = decode_history_cursor(before);
        if (!result.request.before) return fail();
    }
    if (after) {
        result.request.after = decode_history_cursor(after);
        if (!result.request.after) return fail();
    }
    const auto number = [](const char* raw, std::optional<std::uint64_t>& output) {
        if (!raw) return true;
        const std::string_view text(raw);
        std::uint64_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return false;
        output = value;
        return true;
    };
    if (!number(position, result.request.from_position) || !number(ordinal, result.request.from_ordinal)) return fail();
    return result;
}

} // namespace acecode::web
