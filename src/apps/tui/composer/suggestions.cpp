#include "tui/composer/suggestions.hpp"
#include "tui/path_reference_input.hpp"
#include "tui/slash_dropdown.hpp"

namespace acecode::tui {
void refresh_input_suggestions(TuiState& state,
                                      CommandRegistry& cmd_registry,
                                      const std::string& cwd) {
    acecode::tui::refresh_path_reference_state(state, cwd);
    refresh_slash_dropdown(state, cmd_registry);
}

}
