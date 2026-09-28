#include "terminal_control.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <direct.h>
#else
#include <termios.h>
#include <unistd.h>
#endif
#include <cstdlib>
#include <iostream>

namespace acecode::tui::term {

void write_terminal_control_sequence(std::string_view seq) {
#ifdef _WIN32
    auto stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD out_mode = 0;
    const bool restore_mode =
        stdout_handle != INVALID_HANDLE_VALUE &&
        GetConsoleMode(stdout_handle, &out_mode);
    if (!restore_mode) {
        return;
    }
    constexpr DWORD enable_virtual_terminal_processing = 0x0004;
    constexpr DWORD disable_newline_auto_return = 0x0008;
    SetConsoleMode(stdout_handle,
                   out_mode | enable_virtual_terminal_processing |
                       disable_newline_auto_return);
#endif

    std::cout.write(seq.data(), static_cast<std::streamsize>(seq.size()));
    std::cout.flush();

#ifdef _WIN32
    SetConsoleMode(stdout_handle, out_mode);
#endif
}

void set_ftxui_full_repaint_mode(bool enabled) {
#ifdef _WIN32
    _putenv_s("ACECODE_FTXUI_FULL_REPAINT", enabled ? "1" : "0");
#else
    if (enabled) {
        setenv("ACECODE_FTXUI_FULL_REPAINT", "1", 1);
    } else {
        unsetenv("ACECODE_FTXUI_FULL_REPAINT");
    }
#endif
}

void reset_cursor() {
    // DECTCEM: show cursor (ESC [ ? 25 h)
    write_terminal_control_sequence("\033[?25h");
}

void flush_terminal_input_buffer() {
#ifdef _WIN32
    auto stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    if (stdin_handle == INVALID_HANDLE_VALUE) {
        return;
    }
    FlushConsoleInputBuffer(stdin_handle);
#else
    if (isatty(STDIN_FILENO)) {
        tcflush(STDIN_FILENO, TCIFLUSH);
    }
#endif
}


} // namespace acecode::tui::term
