#pragma once
#include <string>
#include <string_view>
#include <cstddef>

namespace acecode::tui {
std::string collapse_sidebar_title_whitespace(std::string_view text);
void trim_ascii_space_suffix(std::string& text);
std::string truncate_cells_prefix(std::string_view text, int max_cells);
std::string truncate_cells_middle_ascii(std::string_view text, int max_cells);
std::string format_tool_count(size_t tool_count);
std::string uppercase_ascii(std::string text);
std::string repeat_utf8_glyph(const char* glyph, int count);
} // namespace acecode::tui
