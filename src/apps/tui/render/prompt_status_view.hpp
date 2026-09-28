#pragma once
#include "tui/tui_state.hpp"
#include "tui/ask_question_adapter.hpp"
#include "tui/composer/input_wrap_view.hpp"
#include "permissions/permissions.hpp"
#include <functional>

namespace acecode::tui {
struct PromptStatusView { ftxui::Element prompt, status; };
// render_composer is borrowed for this call only; ask/confirm must never invoke it.
PromptStatusView render_prompt_status_view(const TuiState& state,
    const AskQuestionFrame& ask_question_frame, InputTextHitLayout& input_hit_layout,
    const PermissionManager& permissions, int terminal_width,
    bool show_regular_sidebar, bool conhost_compat_layout, bool dangerous_mode,
    const std::function<ftxui::Element()>& render_composer);
}
