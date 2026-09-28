#include "tui/model/input_state.hpp"
#include "tui/ctrl_c_exit.hpp"

namespace acecode::tui {
void cancel_ctrl_c_exit_locked(TuiState& state) {
    acecode::tui::clear_ctrl_c_exit_state(
        state.ctrl_c_armed, state.last_ctrl_c_time);
}

}
