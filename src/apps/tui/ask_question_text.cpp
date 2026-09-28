#include "ask_question_text.hpp"

#include <algorithm>
#include <cstdint>

namespace acecode::tui {
namespace {

bool is_continuation(unsigned char byte) {
    return (byte & 0xc0) == 0x80;
}

bool decode_one(std::string_view text, std::size_t start, std::uint32_t& codepoint,
                std::size_t& length) {
    if (start >= text.size()) return false;
    const auto first = static_cast<unsigned char>(text[start]);
    if (first < 0x80) {
        codepoint = first;
        length = 1;
        return true;
    }
    int expected = 0;
    std::uint32_t value = 0;
    if (first >= 0xc2 && first <= 0xdf) {
        expected = 2;
        value = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
        expected = 3;
        value = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
        expected = 4;
        value = first & 0x07;
    } else {
        codepoint = 0;
        length = 1;
        return false;
    }
    if (start + static_cast<std::size_t>(expected) > text.size()) {
        codepoint = 0;
        length = 1;
        return false;
    }
    for (int i = 1; i < expected; ++i) {
        const auto byte = static_cast<unsigned char>(text[start + i]);
        if (!is_continuation(byte)) {
            codepoint = 0;
            length = 1;
            return false;
        }
        value = (value << 6) | (byte & 0x3f);
    }
    if ((expected == 3 && value < 0x800) ||
        (expected == 4 && value < 0x10000) || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) {
        codepoint = 0;
        length = 1;
        return false;
    }
    codepoint = value;
    length = static_cast<std::size_t>(expected);
    return true;
}

bool is_combining(std::uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036f) ||
           (cp >= 0x1ab0 && cp <= 0x1aff) ||
           (cp >= 0x1dc0 && cp <= 0x1dff) ||
           (cp >= 0x20d0 && cp <= 0x20ff) ||
           (cp >= 0xfe20 && cp <= 0xfe2f);
}

bool is_wide(std::uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115f) ||
           (cp >= 0x2329 && cp <= 0x232a) ||
           (cp >= 0x2e80 && cp <= 0xa4cf) ||
           (cp >= 0xac00 && cp <= 0xd7a3) ||
           (cp >= 0xf900 && cp <= 0xfaff) ||
           (cp >= 0xfe10 && cp <= 0xfe19) ||
           (cp >= 0xfe30 && cp <= 0xfe6f) ||
           (cp >= 0xff00 && cp <= 0xff60) ||
           (cp >= 0xffe0 && cp <= 0xffe6) ||
           (cp >= 0x1f300 && cp <= 0x1faff) ||
           (cp >= 0x20000 && cp <= 0x3fffd);
}

} // namespace

int ask_question_codepoint_width(std::string_view codepoint) {
    std::uint32_t cp = 0;
    std::size_t length = 0;
    if (!decode_one(codepoint, 0, cp, length) || length != codepoint.size()) return 1;
    if (cp == 0 || cp < 0x20 || (cp >= 0x7f && cp < 0xa0) || is_combining(cp)) return 0;
    return is_wide(cp) ? 2 : 1;
}

int ask_question_text_width(std::string_view text) {
    int width = 0;
    for (std::size_t pos = 0; pos < text.size();) {
        std::uint32_t cp = 0;
        std::size_t length = 1;
        decode_one(text, pos, cp, length);
        width += ask_question_codepoint_width(text.substr(pos, length));
        pos += length;
    }
    return std::max(0, width);
}

} // namespace acecode::tui
