#pragma once

#include <cstddef>
#include <string_view>

namespace acecode::tui {

// Width of terminal cells occupied by valid UTF-8 text. Invalid bytes are
// treated as one cell so layout remains bounded and never splits a byte while
// wrapping.
int ask_question_text_width(std::string_view text);
int ask_question_codepoint_width(std::string_view codepoint);

} // namespace acecode::tui
