// Opt-in fixture: isolated profiles, fake audio supplied by the driver, and a
// hidden main WebHost. Exercises the production pet window and action bridge.
#include "desktop/web_host.hpp"
#include "desktop/desktop_pet.hpp"
#include "utils/scope_exit.hpp"

#include <windows.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <cwchar>
#include <memory>
#include <type_traits>

namespace {
struct GdiDeleter { void operator()(HGDIOBJ value) const { if (value) ::DeleteObject(value); } };
using UniqueRegion = std::unique_ptr<std::remove_pointer_t<HRGN>, GdiDeleter>;
HWND pet_window() {
    struct Search { DWORD process; HWND window; } search{::GetCurrentProcessId(), nullptr};
    ::EnumWindows(+[](HWND window, LPARAM context) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(context);
        DWORD process = 0; ::GetWindowThreadProcessId(window, &process);
        wchar_t name[128]{}; ::GetClassNameW(window, name, 128);
        if (process == search.process && std::wcscmp(name, L"ACECodeDesktopPetWindow") == 0) {
            search.window = window; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.window;
}
nlohmann::json rect_json(const RECT& rect) {
    return {{"x", rect.left}, {"y", rect.top}, {"width", rect.right - rect.left}, {"height", rect.bottom - rect.top}};
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    using acecode::desktop::WebHost;
    WebHost host(false, WebHost::StartupWindowMode::OffscreenUntilReady);
    const auto main_window = static_cast<HWND>(host.native_window()); // Borrowed from host.
    host.bind("aceMiniSmokeHide", [main_window](const std::string&) {
        ::ShowWindow(main_window, SW_HIDE);
        return std::string("true");
    });
    host.bind("aceMiniSmokeState", [main_window](const std::string&) {
        RECT rect{}; MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
        const auto pet = pet_window();
        ::GetWindowRect(pet, &rect); ::GetMonitorInfoW(::MonitorFromWindow(pet, MONITOR_DEFAULTTOPRIMARY), &monitor);
        return nlohmann::json{{"visible", ::IsWindowVisible(main_window) != FALSE},
            {"foreground", ::GetForegroundWindow() == main_window}, {"pet", rect_json(rect)}, {"work", rect_json(monitor.rcWork)}}.dump();
    });
    host.bind("aceMiniSmokePosition", [](const std::string& message) {
        const auto request = nlohmann::json::parse(message).at(0).get<std::string>();
        const auto pet = pet_window(); if (!pet) return std::string("false");
        RECT rect{}; ::GetWindowRect(pet, &rect);
        MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
        if (!::GetMonitorInfoW(::MonitorFromWindow(pet, MONITOR_DEFAULTTOPRIMARY), &monitor)) return std::string("false");
        const int x = request == "top-left" ? monitor.rcWork.left : monitor.rcWork.right - (rect.right - rect.left);
        const int y = request == "top-left" ? monitor.rcWork.top : monitor.rcWork.bottom - (rect.bottom - rect.top);
        ::SetWindowPos(pet, nullptr, x, y, 0, 0, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
        return std::string("true");
    });
    host.bind("aceMiniSmokeHit", [](const std::string& message) {
        const auto point = nlohmann::json::parse(message).at(0);
        UniqueRegion region(::CreateRectRgn(0, 0, 0, 0));
        if (!region || ::GetWindowRgn(pet_window(), region.get()) == ERROR) return std::string("null");
        return nlohmann::json(::PtInRegion(region.get(), point.at("x").get<int>(), point.at("y").get<int>()) != FALSE).dump();
    });
    host.bind("aceMiniSmokeDone", [](const std::string& message) {
        std::cout << message << std::endl;
        ::PostQuitMessage(0);
        return std::string("true");
    });
    acecode::desktop::DesktopPet pet(host);
    const auto timer = ::SetTimer(nullptr, 0, 120000,
        +[](HWND, UINT, UINT_PTR, DWORD) { ::PostQuitMessage(1); });
    acecode::ScopeExit stop_timer([timer] { if (timer) ::KillTimer(nullptr, timer); });
    host.navigate(argv[1]);
    host.run();
    return 0;
}
