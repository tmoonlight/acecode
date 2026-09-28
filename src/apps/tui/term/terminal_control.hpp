#pragma once
#include <string_view>
namespace acecode::tui::term {
void write_terminal_control_sequence(std::string_view sequence);
void set_ftxui_full_repaint_mode(bool enabled);
void reset_cursor();
void flush_terminal_input_buffer();
}
