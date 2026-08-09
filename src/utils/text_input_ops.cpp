#include "text_input_ops.hpp"

#include <algorithm>

namespace acecode {

namespace {

// UTF-8 continuation byte 判定:10xxxxxx(高两位固定为 10)。这一条和
// main.cpp ArrowLeft / Backspace / Delete / ArrowRight 分支逐字扫描用的
// 判定字节级等价,改动时两边 MUST 同步。
inline bool is_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

inline void clamp_cursor(const std::string& text, std::size_t& cursor) {
    cursor = clamp_utf8_boundary(text, cursor);
}

} // namespace

std::size_t clamp_utf8_boundary(const std::string& text,
                                std::size_t offset) {
    offset = std::min(offset, text.size());
    while (offset > 0 && offset < text.size() &&
           is_continuation(static_cast<unsigned char>(text[offset]))) {
        --offset;
    }
    return offset;
}

std::optional<TextSelectionRange> text_selection_range(
    const std::string& text,
    std::size_t cursor,
    const std::optional<std::size_t>& anchor) {
    if (!anchor.has_value()) {
        return std::nullopt;
    }
    cursor = clamp_utf8_boundary(text, cursor);
    const std::size_t normalized_anchor =
        clamp_utf8_boundary(text, *anchor);
    if (cursor == normalized_anchor) {
        return std::nullopt;
    }
    return TextSelectionRange{
        std::min(cursor, normalized_anchor),
        std::max(cursor, normalized_anchor),
    };
}

bool has_text_selection(const std::string& text,
                        std::size_t cursor,
                        const std::optional<std::size_t>& anchor) {
    return text_selection_range(text, cursor, anchor).has_value();
}

bool erase_text_selection(std::string& text,
                          std::size_t& cursor,
                          std::optional<std::size_t>& anchor) {
    const auto range = text_selection_range(text, cursor, anchor);
    if (!range.has_value()) {
        cursor = clamp_utf8_boundary(text, cursor);
        anchor.reset();
        return false;
    }
    text.erase(range->begin, range->end - range->begin);
    cursor = range->begin;
    anchor.reset();
    return true;
}

void move_cursor_with_selection(const std::string& text,
                                std::size_t& cursor,
                                std::optional<std::size_t>& anchor,
                                std::size_t target,
                                bool extend) {
    cursor = clamp_utf8_boundary(text, cursor);
    target = clamp_utf8_boundary(text, target);
    if (extend) {
        if (!anchor.has_value()) {
            anchor = cursor;
        } else {
            anchor = clamp_utf8_boundary(text, *anchor);
        }
    } else {
        anchor.reset();
    }
    cursor = target;
}

bool collapse_selection_left(const std::string& text,
                             std::size_t& cursor,
                             std::optional<std::size_t>& anchor) {
    const auto range = text_selection_range(text, cursor, anchor);
    if (!range.has_value()) {
        anchor.reset();
        return false;
    }
    cursor = range->begin;
    anchor.reset();
    return true;
}

bool collapse_selection_right(const std::string& text,
                              std::size_t& cursor,
                              std::optional<std::size_t>& anchor) {
    const auto range = text_selection_range(text, cursor, anchor);
    if (!range.has_value()) {
        anchor.reset();
        return false;
    }
    cursor = range->end;
    anchor.reset();
    return true;
}

void select_all_text(const std::string& text,
                     std::size_t& cursor,
                     std::optional<std::size_t>& anchor) {
    anchor = std::size_t{0};
    cursor = text.size();
}

void insert_at_cursor(std::string& text, std::size_t& cursor, std::string_view ch) {
    clamp_cursor(text, cursor);
    text.insert(cursor, ch.data(), ch.size());
    cursor += ch.size();
}

void insert_replacing_selection(std::string& text,
                                std::size_t& cursor,
                                std::optional<std::size_t>& anchor,
                                std::string_view ch) {
    erase_text_selection(text, cursor, anchor);
    insert_at_cursor(text, cursor, ch);
}

void backspace_replacing_selection(
    std::string& text,
    std::size_t& cursor,
    std::optional<std::size_t>& anchor) {
    if (!erase_text_selection(text, cursor, anchor)) {
        backspace_utf8(text, cursor);
    }
}

void delete_replacing_selection(
    std::string& text,
    std::size_t& cursor,
    std::optional<std::size_t>& anchor) {
    if (!erase_text_selection(text, cursor, anchor)) {
        delete_utf8(text, cursor);
    }
}

void backspace_utf8(std::string& text, std::size_t& cursor) {
    clamp_cursor(text, cursor);
    if (text.empty() || cursor == 0) return;
    std::size_t pos = cursor - 1;
    while (pos > 0 &&
           is_continuation(static_cast<unsigned char>(text[pos]))) {
        pos--;
    }
    text.erase(pos, cursor - pos);
    cursor = pos;
}

void delete_utf8(std::string& text, std::size_t& cursor) {
    clamp_cursor(text, cursor);
    if (cursor >= text.size()) return;
    std::size_t next = cursor + 1;
    while (next < text.size() &&
           is_continuation(static_cast<unsigned char>(text[next]))) {
        next++;
    }
    text.erase(cursor, next - cursor);
}

void move_cursor_left_utf8(const std::string& text, std::size_t& cursor) {
    clamp_cursor(text, cursor);
    if (cursor == 0) return;
    std::size_t pos = cursor - 1;
    while (pos > 0 &&
           is_continuation(static_cast<unsigned char>(text[pos]))) {
        pos--;
    }
    cursor = pos;
}

void move_cursor_right_utf8(const std::string& text, std::size_t& cursor) {
    clamp_cursor(text, cursor);
    if (cursor >= text.size()) return;
    std::size_t next = cursor + 1;
    while (next < text.size() &&
           is_continuation(static_cast<unsigned char>(text[next]))) {
        next++;
    }
    cursor = next;
}

} // namespace acecode
