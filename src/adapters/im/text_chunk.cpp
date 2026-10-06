#include "text_chunk.hpp"

#include <algorithm>

namespace acecode::im {
namespace {

// 当前位置 UTF-8 序列的字节长度;非法字节按 1 处理,且不越过末尾。
std::size_t sequence_length(std::string_view text, std::size_t pos) {
    const auto lead = static_cast<unsigned char>(text[pos]);
    std::size_t length = 1;
    if (lead >= 0xF0 && lead <= 0xF4) length = 4;
    else if (lead >= 0xE0) length = 3;
    else if (lead >= 0xC2 && lead <= 0xDF) length = 2;
    if (pos + length > text.size()) return 1;
    for (std::size_t i = 1; i < length; ++i) {
        if ((static_cast<unsigned char>(text[pos + i]) & 0xC0) != 0x80) return 1;
    }
    return length;
}

std::size_t units_of(std::size_t length, bool utf16) { return utf16 && length == 4 ? 2 : 1; }

// 不超过 budget 个单位时能放下的最长前缀(字节数),落在字符边界上。
std::size_t prefix_within(std::string_view text, std::size_t budget, bool utf16) {
    std::size_t pos = 0, used = 0;
    while (pos < text.size()) {
        const auto length = sequence_length(text, pos);
        const auto units = units_of(length, utf16);
        if (used + units > budget) break;
        used += units;
        pos += length;
    }
    return pos;
}

bool is_fence_line(std::string_view line) {
    std::size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') ++i;
    return line.substr(i, 3) == "```";
}

// 扫描文本,返回末尾仍未闭合的围栏行(含语言标记);没有未闭合围栏则返回空。
std::string open_fence_at_end(std::string_view text) {
    std::string open;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        const auto line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (is_fence_line(line)) {
            if (open.empty()) {
                open = std::string(line);
                while (!open.empty() && (open.back() == '\r' || open.back() == ' ')) open.pop_back();
            } else {
                open.clear();
            }
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return open;
}

// 在 [min_pos, limit] 范围内从后往前找分隔符;返回分隔符起点,找不到返回 npos。
std::size_t find_break(std::string_view text, std::size_t min_pos, std::size_t limit,
                       std::string_view separator) {
    if (limit < separator.size()) return std::string_view::npos;
    auto pos = text.rfind(separator, limit - separator.size());
    if (pos == std::string_view::npos || pos < min_pos) return std::string_view::npos;
    return pos;
}

std::string_view trim_leading_breaks(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size() && (text[i] == '\n' || text[i] == '\r' || text[i] == ' ')) ++i;
    return text.substr(i);
}

bool blank(std::string_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; });
}

} // namespace

std::size_t text_units(std::string_view utf8, bool utf16) {
    std::size_t pos = 0, used = 0;
    while (pos < utf8.size()) {
        const auto length = sequence_length(utf8, pos);
        used += units_of(length, utf16);
        pos += length;
    }
    return used;
}

std::vector<std::string> chunk_text(std::string_view utf8, std::size_t max_units, bool utf16) {
    std::vector<std::string> chunks;
    max_units = std::max<std::size_t>(max_units, 16);
    const bool has_fences = utf8.find("```") != std::string_view::npos;
    // 有代码块时预留闭合围栏("\n```")的空间。
    const std::size_t budget = has_fences ? max_units - 8 : max_units;

    std::string current(utf8);
    while (!blank(current)) {
        if (text_units(current, utf16) <= max_units) {
            chunks.push_back(current);
            break;
        }
        const std::string_view view = current;
        const std::size_t limit = prefix_within(view, budget, utf16);
        const std::size_t min_pos = limit / 3;
        std::size_t cut = std::string_view::npos;
        std::size_t skip = 0;
        for (const std::string_view separator : {std::string_view("\n\n"), std::string_view("\n"),
                                                 std::string_view(" ")}) {
            const auto pos = find_break(view, min_pos, limit, separator);
            if (pos != std::string_view::npos && pos > 0) {
                cut = pos;
                skip = separator.size();
                break;
            }
        }
        if (cut == std::string_view::npos) {
            cut = std::max<std::size_t>(limit, sequence_length(view, 0));
            skip = 0;
        }
        std::string chunk = current.substr(0, cut);
        std::string next = current.substr(cut + skip);
        std::string reopen;
        if (has_fences) {
            const auto fence = open_fence_at_end(chunk);
            if (!fence.empty()) {
                chunk += "\n```";
                reopen = fence + "\n";
            }
        }
        if (!blank(chunk)) chunks.push_back(std::move(chunk));
        // 重开围栏会把内容加长;极小上限下若这样会让剩余文本不再缩短,就放弃重开,保证循环收敛。
        if (!reopen.empty() && reopen.size() + next.size() < current.size()) {
            current = reopen + next;
        } else {
            current = std::string(trim_leading_breaks(next));
        }
    }
    return chunks;
}

} // namespace acecode::im
