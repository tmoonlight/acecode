#pragma once
#include "tui/tui_state.hpp"
#include "tui/composer/input_wrap_view.hpp"
#include <ftxui/component/component_base.hpp>

namespace acecode::tui {
ftxui::Component make_composer_input(TuiState& state, InputTextHitLayout& hit_layout);
}
