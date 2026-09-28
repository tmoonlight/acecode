#include "tui/render/ask_question_style.hpp"
#include "tui/theme_palette.hpp"
#include "tui/text_style.hpp"
#include <algorithm>
#include <utility>
#include <ftxui/screen/string.hpp>
using ftxui::border;

namespace acecode::tui {
AskQuestionPanelColors ask_question_panel_colors() {
    const auto& palette = acecode::tui::theme();
    acecode::tui::AskQuestionPanelColors colors;
    colors.border = palette.ui.border;
    colors.question = palette.ui.text_primary;
    colors.answer = palette.ui.text_primary;
    colors.description = palette.ui.text_muted;
    colors.placeholder = palette.ui.text_dim;
    colors.focus_bg = palette.ui.selection_bg;
    colors.panel_bg = palette.ui.input_bg;
    colors.secondary = palette.ui.text_secondary;
    colors.selection_fg = palette.ui.selection_fg;
    colors.selection_bg = palette.ui.selection_bg;
    return colors;
}

}
