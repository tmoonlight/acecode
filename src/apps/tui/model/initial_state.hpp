#pragma once
#include "tui/render_mode.hpp"
#include <memory>
#include <string>
namespace acecode { struct TuiState; class McpManager; class LlmProvider; }
namespace acecode::tui {
void initialize_tui_state_before_screen(TuiState& state, const AppConfig& config,
    const std::string& working_dir, bool dangerous_mode, const McpManager& mcp,
    const std::shared_ptr<LlmProvider>& provider);
void maybe_add_legacy_terminal_hint(TuiState& state, const AppConfig& config,
    const TerminalCapabilities& capabilities, ScreenRenderMode mode, bool force_alt_screen);
}
