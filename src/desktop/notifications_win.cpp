#include "notifications_backend.hpp"

#ifdef _WIN32

#include "custom_toast.hpp"

#include "../utils/logger.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <mutex>
#include <string>

namespace acecode::desktop::notification_backend {
namespace {

std::mutex g_mu;
bool g_initialized = false;
std::string g_app_name = "ACECode";
void* g_activation_window = nullptr;

bool start_custom_backend_locked() {
    if (custom_toast::is_available()) return true;
    custom_toast::InitOptions options;
    options.app_name = g_app_name;
    options.activation_window = g_activation_window;
    return custom_toast::initialize(options);
}

} // namespace

bool initialize(const NotificationInitOptions& options) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_initialized) return true;

    g_app_name = options.app_name.empty() ? std::string("ACECode")
                                          : options.app_name;
    g_activation_window = options.activation_window;

    // Windows notifications always use the self-drawn layered popup. The
    // WinRT / WinToast path was removed: OS toasts silently drop on many
    // machines and cannot match the tray-menu soft-shadow look.
    if (!start_custom_backend_locked()) {
        LOG_WARN("[notifications] self-drawn toast renderer unavailable");
        notification_detail::publish_authorization_state({
            NotificationAuthorizationStatus::Unavailable, false, false});
        return false;
    }

    g_initialized = true;
    LOG_INFO("[notifications] backend=custom (self-drawn toast)");
    notification_detail::publish_authorization_state({
        NotificationAuthorizationStatus::Authorized, false, false});
    return true;
}

bool show(const NotifyPayload& payload) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_initialized) return false;
    return custom_toast::show(payload);
}

void shutdown() {
    bool was_initialized = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        was_initialized = g_initialized;
        g_initialized = false;
        g_activation_window = nullptr;
    }
    // Outside the lock: the renderer joins its own thread, and a click handler
    // running there can re-enter this backend.
    if (was_initialized) custom_toast::shutdown();
}

bool refresh_authorization() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_initialized;
}

bool request_authorization() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_initialized;
}

bool open_settings() {
    return false;
}

void* capture_tui_window() {
    HWND console = ::GetConsoleWindow();
    if (console && ::IsWindow(console) && ::IsWindowVisible(console)) {
        return console;
    }
    HWND foreground = ::GetForegroundWindow();
    if (foreground && ::IsWindow(foreground)) return foreground;
    return console && ::IsWindow(console) ? console : nullptr;
}

bool window_is_foreground(void* native_window) {
    auto* hwnd = static_cast<HWND>(native_window);
    return hwnd && ::IsWindow(hwnd) && ::GetForegroundWindow() == hwnd;
}

bool activate_window(void* native_window) {
    auto* hwnd = static_cast<HWND>(native_window);
    if (!hwnd || !::IsWindow(hwnd)) return false;
    if (::IsIconic(hwnd)) {
        ::ShowWindowAsync(hwnd, SW_RESTORE);
    } else {
        ::ShowWindowAsync(hwnd, SW_SHOW);
    }
    ::BringWindowToTop(hwnd);
    if (::SetForegroundWindow(hwnd)) return true;

    constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW;
    ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
    ::SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    return ::SetForegroundWindow(hwnd) != FALSE ||
           ::GetForegroundWindow() == hwnd;
}

} // namespace acecode::desktop::notification_backend

#endif // _WIN32
