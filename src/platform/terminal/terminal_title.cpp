#include "terminal_title.hpp"

#include <iostream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace acecode {

void set_terminal_title(std::string_view text) {
#ifdef _WIN32
    if (text.empty()) {
        SetConsoleTitleW(L"");
        return;
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                  static_cast<int>(text.size()),
                                  nullptr, 0);
    if (len <= 0) {
        SetConsoleTitleA(std::string(text).c_str());
        return;
    }
    std::wstring wide(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(),
                        static_cast<int>(text.size()),
                        wide.data(), len);
    SetConsoleTitleW(wide.c_str());
#else
    std::string buf;
    buf.reserve(text.size() + 6);
    buf.append("\x1b]2;");
    buf.append(text.data(), text.size());
    buf.append("\x1b\\");
    std::cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    std::cout.flush();
#endif
}

void clear_terminal_title() {
    set_terminal_title(std::string_view{});
}

} // namespace acecode
