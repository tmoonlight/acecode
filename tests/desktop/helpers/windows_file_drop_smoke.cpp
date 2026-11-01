// Opt-in, isolated WebView2 smoke. The second process owns a disposable capture
// window; neither process activates or sends input to the user's applications.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "desktop/web_host_file_drop_win.hpp"
#include "platform/process/unique_resources.hpp"

#include <webview/webview.h>
#include <iostream>

int main(int argc, char**) {
    using acecode::desktop::detail::LegacyWindowsFileDrop;
    if (argc > 1) {
        HWND source = ::CreateWindowExW(0, L"STATIC", L"ACECode capture fixture",
            WS_POPUP, -30000, -30000, 40, 40, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (!source) return 2;
        acecode::ScopeExit destroy([source] { ::DestroyWindow(source); });
        ::SetCapture(source);
        ::Sleep(600);
        ::ReleaseCapture();
        return 0;
    }

    webview::webview host(false, nullptr);
    const auto root = static_cast<HWND>(host.window().value());
    const auto widget = static_cast<HWND>(host.widget().value());
    ::SetWindowPos(root, nullptr, -30000, -30000, 640, 480, SWP_NOACTIVATE | SWP_NOZORDER);
    std::unique_ptr<LegacyWindowsFileDrop> bridge;
    acecode::platform::UniqueProcess source_process;
    bool saw_capture = false;
    int ticks = 0;
    int result = 1;
    host.bind("aceDropSmokeReady", [&](const std::string&) -> std::string {
        bridge = std::make_unique<LegacyWindowsFileDrop>(widget, [](auto, POINTL) {});
        if (!bridge->ready()) { host.terminate(); return "null"; }
        wchar_t executable[32768]{};
        if (!::GetModuleFileNameW(nullptr, executable, 32768)) {
            host.terminate();
            return "null";
        }
        std::wstring command = std::wstring(L"\"") + executable + L"\" --capture";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
            host.terminate();
            return "null";
        }
        acecode::platform::UniqueHandle thread(process.hThread);
        source_process.reset(process.hProcess);
        return "null";
    });
    host.bind("aceDropSmokeTick", [&](const std::string&) -> std::string {
        if (!bridge) return "null";
        const bool visible = ::IsWindowVisible(bridge->window());
        saw_capture = saw_capture || visible;
        if (++ticks > 20 || (source_process.wait_exit(0) && !visible)) {
            result = bridge->ready() && saw_capture && !visible ? 0 : 1;
            std::cout << "external_capture=" << saw_capture << " hidden_after_release=" << !visible
                      << " ok=" << (result == 0) << std::endl;
            host.terminate();
        }
        return "null";
    });
    host.set_html("<script>addEventListener('DOMContentLoaded',()=>{"
                  "aceDropSmokeReady();setInterval(aceDropSmokeTick,100)})</script>");
    const auto timer = ::SetTimer(nullptr, 0, 10000,
        +[](HWND, UINT, UINT_PTR, DWORD) { ::PostQuitMessage(1); });
    acecode::ScopeExit stop_timer([timer] { if (timer) ::KillTimer(nullptr, timer); });
    host.run();
    return result;
}
