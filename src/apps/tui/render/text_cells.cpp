#include "tui/render/text_cells.hpp"
#include <algorithm>
#include <cctype>
#include <vector>
#include <ftxui/screen/string.hpp>
using ftxui::Utf8ToGlyphs;
using ftxui::string_width;

namespace acecode::tui {
std::string collapse_sidebar_title_whitespace(std::string_view text) {
    std::string out;
    bool in_space = false;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            if (!out.empty() && !in_space) {
                out.push_back(' ');
            }
            in_space = true;
        } else {
            out.push_back(static_cast<char>(c));
            in_space = false;
        }
    }
    if (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

void trim_ascii_space_suffix(std::string& text) {
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
}

std::string truncate_cells_prefix(std::string_view text, int max_cells) {
    if (max_cells <= 0) {
        return {};
    }
    std::string out;
    int used = 0;
    for (const auto& glyph : Utf8ToGlyphs(std::string(text))) {
        if (glyph.empty()) {
            continue;
        }
        const int width = std::max(0, string_width(glyph));
        if (used + width > max_cells) {
            break;
        }
        out += glyph;
        used += width;
    }
    return out;
}

std::string truncate_cells_middle_ascii(std::string_view text, int max_cells) {
    if (max_cells <= 0) {
        return {};
    }
    const std::string input(text);
    if (string_width(input) <= max_cells) {
        return input;
    }
    if (max_cells <= 3) {
        return truncate_cells_prefix(input, max_cells);
    }

    const int body_cells = max_cells - 3;
    const int head_cells = std::max(1, body_cells / 2);
    const int tail_cells = std::max(0, body_cells - head_cells);
    const auto glyphs = Utf8ToGlyphs(input);

    std::string head;
    int used_head = 0;
    for (const auto& glyph : glyphs) {
        const int width = std::max(0, string_width(glyph));
        if (used_head + width > head_cells) {
            break;
        }
        head += glyph;
        used_head += width;
    }

    std::vector<std::string> tail_glyphs;
    int used_tail = 0;
    for (std::size_t i = glyphs.size(); i > 0; --i) {
        const auto& glyph = glyphs[i - 1];
        const int width = std::max(0, string_width(glyph));
        if (used_tail + width > tail_cells) {
            break;
        }
        tail_glyphs.push_back(glyph);
        used_tail += width;
    }
    std::reverse(tail_glyphs.begin(), tail_glyphs.end());

    std::string out = head + "...";
    for (const auto& glyph : tail_glyphs) {
        out += glyph;
    }
    return out;
}

std::string format_tool_count(size_t tool_count) {
    return std::to_string(tool_count) + (tool_count == 1 ? " tool" : " tools");
}

std::string uppercase_ascii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

std::string repeat_utf8_glyph(const char* glyph, int count) {
    std::string out;
    if (count <= 0) {
        return out;
    }
    const std::string g(glyph);
    out.reserve(g.size() * static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        out += g;
    }
    return out;
}


} // namespace acecode::tui
