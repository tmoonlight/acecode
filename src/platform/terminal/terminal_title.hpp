#pragma once

#include <string>
#include <string_view>

namespace acecode {

// Set the terminal/window title. Windows uses the console title API to avoid
// leaking OSC escape bytes in legacy cmd.exe; POSIX terminals use OSC 2.
// Empty text clears the title.
void set_terminal_title(std::string_view text);

// Convenience: set_terminal_title("").
void clear_terminal_title();

} // namespace acecode
