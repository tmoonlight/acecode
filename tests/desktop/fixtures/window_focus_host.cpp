// Standalone Windows smoke host for web/scripts/test-desktop-focus.mjs.
// Link against the production web_host.cpp and desktop support libraries.
#include "desktop/web_host.hpp"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>

// Test driver only: simulate an explicit user switch from a background runner.
static void activate_test_window(HWND window) {
    const DWORD foreground = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD current = GetCurrentThreadId();
    const bool attached = foreground && foreground != current && AttachThreadInput(current, foreground, TRUE);
    SetForegroundWindow(window);
    SetFocus(window);
    if (attached) AttachThreadInput(current, foreground, FALSE);
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (!GetForegroundWindow()) {
        std::cout << "[skip] Native keyboard test requires an active, unlocked Windows desktop" << std::endl;
        return 77;
    }
    using acecode::desktop::WebHost;
    WebHost host(false, WebHost::StartupWindowMode::OffscreenUntilReady);
    host.set_title("ACECode focus verification");
    host.set_size(1170, 820);
    const auto hwnd = static_cast<HWND>(host.native_window());
    const HWND previous = GetForegroundWindow();
    using UniqueWindow = std::unique_ptr<std::remove_pointer_t<HWND>, decltype(&DestroyWindow)>;
    UniqueWindow peer(CreateWindowExW(0, L"STATIC", L"Focus verification peer",
        WS_OVERLAPPEDWINDOW, 100, 100, 300, 160, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr),
        &DestroyWindow);
    const HWND other = peer.get(); // Borrowed for synchronous test operations.
    if (!other) return 3;
    int result = 1;
    host.bind("aceDesktop_focusActiveWebview", [&host](const std::string&) {
        return nlohmann::json{{"ok", host.focus_active_webview()}}.dump();
    });
    host.set_window_focus_handler([&host] {
        host.eval("window.dispatchEvent(new Event('acecode:desktop-window-focus'))");
    });
    host.bind("aceFocusTest", [&](const std::string& request) {
        const auto args = nlohmann::json::parse(request);
        const std::string op = args.at(0);
        if (op == "show" || op == "return" || op == "restore") {
            ShowWindow(other, SW_HIDE);
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            host.set_visible(true);
            activate_test_window(hwnd);
        } else if (op == "away") {
            ShowWindow(other, SW_SHOW);
            activate_test_window(other);
        } else if (op == "minimize") {
            ShowWindow(hwnd, SW_MINIMIZE);
        } else if (op == "maximize") {
            ShowWindow(hwnd, SW_MAXIMIZE);
        } else if (op == "unmaximize") {
            ShowWindow(hwnd, SW_RESTORE);
        } else if (op == "hide") {
            host.set_visible(false);
        } else if (op == "type") {
            if (GetForegroundWindow() != hwnd) return std::string("{\"ok\":false}");
            INPUT input[2]{};
            input[0].type = input[1].type = INPUT_KEYBOARD;
            input[0].ki.wScan = input[1].ki.wScan = L'Z';
            input[0].ki.dwFlags = KEYEVENTF_UNICODE;
            input[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            if (SendInput(2, input, sizeof(INPUT)) != 2) return std::string("{\"ok\":false}");
        } else if (op == "done") {
            result = args.at(1).get<bool>() ? 0 : 1;
            std::cout << args.at(2).dump() << std::endl;
            host.request_quit();
        }
        return std::string("{\"ok\":true}");
    });
    host.navigate(argv[1]);
    host.run();
    peer.reset();
    if (IsWindow(previous)) SetForegroundWindow(previous);
    return result;
}
