#pragma once
#include "tui/tui_state.hpp"
namespace acecode { class CommandRegistry; }

namespace acecode::tui {
void refresh_input_suggestions(TuiState& state, CommandRegistry& commands, const std::string& cwd);
}
