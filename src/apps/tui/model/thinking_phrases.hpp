#pragma once
#include "tui/tui_state.hpp"
#include <string>

namespace acecode::tui {
bool is_user_chinese(const TuiState& state);
std::string get_random_thinking_phrase(bool is_zh);
} // namespace acecode::tui
