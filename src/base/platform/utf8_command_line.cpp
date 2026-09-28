#include "utf8_command_line.hpp"
#include "utils/encoding.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef _WIN32
#include <shellapi.h>
#endif

namespace acecode::platform {

std::vector<std::string> argv_tail(int argc, char* argv[], int start) {
    std::vector<std::string> tokens;
    for (int i = start; i < argc; ++i) {
        tokens.emplace_back(argv[i]);
    }
    return tokens;
}

std::vector<std::string> utf8_command_line_tail(int argc, char* argv[],
    const wchar_t* windows_command_line) {
    std::vector<std::string> tokens;
#ifdef _WIN32
    int wargc = 0;
    LPWSTR* wargv = ::CommandLineToArgvW(windows_command_line ? windows_command_line : ::GetCommandLineW(), &wargc);
    if (wargv) {
        for (int i = 1; i < wargc; ++i) {
            tokens.push_back(acecode::wide_to_utf8(wargv[i]));
        }
        ::LocalFree(wargv);
    } else {
        tokens = argv_tail(argc, argv, 1);
    }
#else
    (void)windows_command_line;
    tokens = argv_tail(argc, argv, 1);
#endif
    return tokens;
}

} // namespace acecode::platform
