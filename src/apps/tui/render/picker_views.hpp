#pragma once
#include "tui/tui_state.hpp"
#include <ftxui/dom/elements.hpp>

namespace acecode::tui {
struct PickerViews {
    ftxui::Element resume, rewind, model, mode;
};
PickerViews render_picker_views(const TuiState& state);
}
