#include "notifications_backend.hpp"

#ifdef _WIN32

#include "custom_toast.hpp"
#include "notification_backend_policy.hpp"

#include "../config/config.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/encoding.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#ifdef ACECODE_HAS_WINTOAST
#  include <wintoastlib.h>
#endif

#include <atomic>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

namespace acecode::desktop::notification_backend {
namespace {

std::mutex g_backend_mu;
bool g_initialized = false;
NotificationBackendPreference g_preference = NotificationBackendPreference::Auto;
NotificationBackendChoice g_choice = NotificationBackendChoice::None;
SystemToastSignals g_signals;
std::string g_app_name = "ACECode";
void* g_activation_window = nullptr;

// Set from WinToast's failure callback, which runs on a WinRT thread while the
// facade mutex may be held by the caller that queued the toast. Atomic so the
// callback never has to take that lock.
std::atomic<bool> g_system_delivery_failed{false};

// ---------------------------------------------------------------------------
// Registry probes
//
// WinToast reports success for a toast the OS then drops on the floor, which is
// exactly the Windows 10 failure users see: no popup, no error, nothing in the
// Action Center. These are the settings that silently swallow toasts.
// ---------------------------------------------------------------------------

bool read_dword(HKEY root, const wchar_t* subkey, const wchar_t* name,
                DWORD* out) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 0;
    DWORD type = 0;
    DWORD size = sizeof(value);
    const LONG status = ::RegQueryValueExW(
        key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_DWORD) return false;
    *out = value;
    return true;
}

bool toast_policy_disabled() {
    DWORD value = 0;
    if (read_dword(HKEY_CURRENT_USER,
                   L"SOFTWARE\\Policies\\Microsoft\\Windows\\Explorer",
                   L"DisableNotificationCenter", &value) &&
        value != 0) {
        return true;
    }
    if (read_dword(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\Policies\\Microsoft\\Windows\\Explorer",
                   L"DisableNotificationCenter", &value) &&
        value != 0) {
        return true;
    }
    if (read_dword(HKEY_CURRENT_USER,
                   L"SOFTWARE\\Policies\\Microsoft\\Windows\\CurrentVersion\\"
                   L"PushNotifications",
                   L"NoToastApplicationNotification", &value) &&
        value != 0) {
        return true;
    }
    if (read_dword(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\Policies\\Microsoft\\Windows\\CurrentVersion\\"
                   L"PushNotifications",
                   L"NoToastApplicationNotification", &value) &&
        value != 0) {
        return true;
    }
    return false;
}

bool toasts_enabled_globally() {
    DWORD value = 0;
    if (read_dword(HKEY_CURRENT_USER,
                   L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\"
                   L"PushNotifications",
                   L"ToastEnabled", &value)) {
        return value != 0;
    }
    // Absent on Windows versions that predate the setting; treat as enabled.
    return true;
}

bool app_toasts_enabled(const std::string& application_id) {
    if (application_id.empty()) return true;
    const std::wstring subkey =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Notifications\\"
        L"Settings\\" +
        acecode::utf8_to_wide(application_id);
    DWORD value = 0;
    if (read_dword(HKEY_CURRENT_USER, subkey.c_str(), L"Enabled", &value)) {
        return value != 0;
    }
    // No per-app entry means the user never touched the setting.
    return true;
}

// ---------------------------------------------------------------------------
// WinToast backend
// ---------------------------------------------------------------------------

#ifdef ACECODE_HAS_WINTOAST

bool g_wintoast_ready = false;
std::wstring g_notification_logo_path;

constexpr int kNotificationLogoResourceId = 101;
constexpr int kRawDataResourceType = 10; // RT_RCDATA, wide-resource form

std::wstring materialize_notification_logo() {
    HMODULE module = ::GetModuleHandleW(nullptr);
    HRSRC resource = ::FindResourceW(
        module,
        MAKEINTRESOURCEW(kNotificationLogoResourceId),
        MAKEINTRESOURCEW(kRawDataResourceType));
    if (!resource) {
        LOG_WARN("[notifications] ACECode logo resource was not found");
        return {};
    }
    HGLOBAL loaded = ::LoadResource(module, resource);
    const DWORD size = ::SizeofResource(module, resource);
    const void* bytes = loaded ? ::LockResource(loaded) : nullptr;
    if (!bytes || size == 0) {
        LOG_WARN("[notifications] ACECode logo resource could not be loaded");
        return {};
    }

    const auto path = acecode::path_from_utf8(acecode::get_acecode_dir())
        / "cache" / "acecode-notification-logo.png";
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec &&
        std::filesystem::file_size(path, ec) == size && !ec) {
        return path.wstring();
    }

    const std::string content(
        static_cast<const char*>(bytes),
        static_cast<std::size_t>(size));
    if (!acecode::atomic_write_file(acecode::path_to_utf8(path), content)) {
        LOG_WARN("[notifications] failed to materialize ACECode logo at " +
                 acecode::path_to_utf8(path));
        return {};
    }
    return path.wstring();
}

std::string wintoast_error_text(WinToastLib::WinToast::WinToastError error) {
    try {
        return acecode::wide_to_utf8(WinToastLib::WinToast::strerror(error));
    } catch (...) {
        return std::to_string(static_cast<int>(error));
    }
}

class SessionToastHandler final : public WinToastLib::IWinToastHandler {
public:
    explicit SessionToastHandler(NotifyPayload payload)
        : payload_(std::move(payload)) {}

    void toastActivated() const override {
        dispatch_notification_activation(payload_);
    }

    void toastActivated(int /*action_index*/) const override {
        dispatch_notification_activation(payload_);
    }

    void toastActivated(std::wstring /*response*/) const override {
        dispatch_notification_activation(payload_);
    }

    void toastDismissed(WinToastDismissalReason /*state*/) const override {}

    void toastFailed() const override {
        LOG_WARN("[notifications] WinToast reported delivery failure for " +
                 payload_.id +
                 "; switching to the self-drawn renderer for later toasts");
        g_system_delivery_failed.store(true, std::memory_order_release);
    }

private:
    NotifyPayload payload_;
};

bool wintoast_compatible() {
    try {
        return WinToastLib::WinToast::isCompatible();
    } catch (...) {
        return false;
    }
}

bool wintoast_initialize(const NotificationInitOptions& options) {
    if (g_wintoast_ready) return true;
    try {
        WinToastLib::setDebugOutputEnabled(false);
        auto* toast = WinToastLib::WinToast::instance();
        toast->setAppName(acecode::utf8_to_wide(
            options.app_name.empty() ? std::string("ACECode") :
                                       options.app_name));
        toast->setAppUserModelId(
            acecode::utf8_to_wide(options.application_id));
        toast->setShortcutPolicy(
            WinToastLib::WinToast::ShortcutPolicy::SHORTCUT_POLICY_REQUIRE_CREATE);

        WinToastLib::WinToast::WinToastError error =
            WinToastLib::WinToast::WinToastError::NoError;
        if (!toast->initialize(&error)) {
            LOG_WARN("[notifications] WinToast initialization failed: " +
                     wintoast_error_text(error));
            return false;
        }
    } catch (const std::exception& e) {
        LOG_WARN("[notifications] WinToast initialization threw: " +
                 std::string(e.what()));
        return false;
    } catch (...) {
        LOG_WARN("[notifications] WinToast initialization threw an unknown "
                 "exception");
        return false;
    }

    g_notification_logo_path = materialize_notification_logo();
    g_wintoast_ready = true;
    return true;
}

bool wintoast_show(const NotifyPayload& payload) {
    if (!g_wintoast_ready) return false;
    try {
        WinToastLib::WinToastTemplate toast(
            WinToastLib::WinToastTemplate::WinToastTemplateType::Text02);
        toast.setFirstLine(acecode::utf8_to_wide(
            payload.title.empty() ? std::string("ACECode") : payload.title));
        toast.setSecondLine(acecode::utf8_to_wide(payload.body));
        toast.setDuration(WinToastLib::WinToastTemplate::Duration::Short);
        if (!g_notification_logo_path.empty()) {
            toast.setImagePath(
                g_notification_logo_path,
                WinToastLib::WinToastTemplate::CropHint::Square);
        }

        WinToastLib::WinToast::WinToastError error =
            WinToastLib::WinToast::WinToastError::NoError;
        const auto toast_id = WinToastLib::WinToast::instance()->showToast(
            toast, new SessionToastHandler(payload), &error);
        if (toast_id < 0) {
            LOG_WARN("[notifications] WinToast delivery failed: " +
                     wintoast_error_text(error));
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        LOG_WARN("[notifications] WinToast delivery threw: " +
                 std::string(e.what()));
        return false;
    } catch (...) {
        LOG_WARN("[notifications] WinToast delivery threw an unknown exception");
        return false;
    }
}

void wintoast_shutdown() {
    if (!g_wintoast_ready) return;
    g_wintoast_ready = false;
    g_notification_logo_path.clear();
    try {
        WinToastLib::WinToast::instance()->clear();
    } catch (...) {
        LOG_WARN("[notifications] WinToast shutdown failed");
    }
}

#else // !ACECODE_HAS_WINTOAST

// MinGW/WinLibs toolchains ship without the WRL headers WinToast needs. The
// self-drawn renderer only uses plain Win32 + GDI, so those builds still get
// notifications instead of the old no-op stub.
bool wintoast_compatible() { return false; }
bool wintoast_initialize(const NotificationInitOptions&) { return false; }
bool wintoast_show(const NotifyPayload&) { return false; }
void wintoast_shutdown() {}

#endif // ACECODE_HAS_WINTOAST

// ---------------------------------------------------------------------------
// Backend routing
// ---------------------------------------------------------------------------

bool start_custom_backend_locked() {
    if (custom_toast::is_available()) return true;
    custom_toast::InitOptions options;
    options.app_name = g_app_name;
    options.activation_window = g_activation_window;
    return custom_toast::initialize(options);
}

// Applies `decision` and starts whatever backend it selected. Returns the
// backend that is actually usable afterwards.
NotificationBackendChoice adopt_decision_locked(
    const NotificationBackendDecision& decision) {
    if (decision.choice == NotificationBackendChoice::Custom &&
        !start_custom_backend_locked()) {
        LOG_WARN("[notifications] self-drawn renderer unavailable, no "
                 "notifications will be shown");
        return NotificationBackendChoice::None;
    }
    return decision.choice;
}

// Returns true when the backend changed, so the caller can publish the new
// authorization state after releasing the backend mutex — the application
// handler runs arbitrary code and must never be invoked under this lock.
bool redecide_locked() {
    g_signals.runtime_delivery_failed =
        g_system_delivery_failed.load(std::memory_order_acquire);
    const NotificationBackendDecision decision =
        decide_notification_backend(g_preference, g_signals);
    const NotificationBackendChoice adopted = adopt_decision_locked(decision);
    if (adopted == g_choice) return false;
    LOG_WARN(std::string("[notifications] switching backend to ") +
             notification_backend_choice_name(adopted) + ": " +
             decision.reason);
    g_choice = adopted;
    return true;
}

} // namespace

bool initialize(const NotificationInitOptions& options) {
    std::lock_guard<std::mutex> lock(g_backend_mu);
    if (g_initialized) return true;

    g_preference = parse_notification_backend_preference(options.backend);
    g_app_name = options.app_name.empty() ? std::string("ACECode")
                                          : options.app_name;
    g_activation_window = options.activation_window;
    g_system_delivery_failed.store(false, std::memory_order_release);

    g_signals = SystemToastSignals{};
    g_signals.platform_supported =
        !options.application_id.empty() && wintoast_compatible();
    g_signals.policy_disabled = toast_policy_disabled();
    g_signals.toasts_enabled_globally = toasts_enabled_globally();
    g_signals.app_toasts_enabled = app_toasts_enabled(options.application_id);

    NotificationBackendDecision decision =
        decide_notification_backend(g_preference, g_signals);
    if (decision.choice == NotificationBackendChoice::System &&
        !wintoast_initialize(options)) {
        // Initialization failure is a hard signal that the OS path is unusable
        // on this machine; re-run the decision without it.
        g_signals.platform_supported = false;
        decision = decide_notification_backend(g_preference, g_signals);
    }

    g_choice = adopt_decision_locked(decision);
    g_initialized = g_choice != NotificationBackendChoice::None;

    LOG_INFO(std::string("[notifications] backend=") +
             notification_backend_choice_name(g_choice) + " (preference=" +
             std::string(notification_backend_preference_value(g_preference)) +
             ", " + decision.reason + ")");

    if (!g_initialized) {
        wintoast_shutdown();
        notification_detail::publish_authorization_state({
            NotificationAuthorizationStatus::Unavailable, false, false});
        return false;
    }

    notification_detail::publish_authorization_state({
        NotificationAuthorizationStatus::Authorized, false, false});
    return true;
}

bool show(const NotifyPayload& payload) {
    bool shown = false;
    bool backend_changed = false;
    NotificationBackendChoice choice = NotificationBackendChoice::None;
    {
        std::lock_guard<std::mutex> lock(g_backend_mu);
        if (!g_initialized) return false;

        if (g_choice == NotificationBackendChoice::System &&
            g_system_delivery_failed.load(std::memory_order_acquire)) {
            backend_changed = redecide_locked();
        }

        if (g_choice == NotificationBackendChoice::System) {
            shown = wintoast_show(payload);
            if (!shown) {
                g_system_delivery_failed.store(true, std::memory_order_release);
                backend_changed = redecide_locked() || backend_changed;
            }
        }

        if (!shown && g_choice == NotificationBackendChoice::Custom) {
            shown = custom_toast::show(payload);
        }
        choice = g_choice;
    }

    if (backend_changed) {
        notification_detail::publish_authorization_state({
            choice == NotificationBackendChoice::None
                ? NotificationAuthorizationStatus::Unavailable
                : NotificationAuthorizationStatus::Authorized,
            false, false});
    }
    return shown;
}

void shutdown() {
    bool was_initialized = false;
    {
        std::lock_guard<std::mutex> lock(g_backend_mu);
        was_initialized = g_initialized;
        g_initialized = false;
        g_choice = NotificationBackendChoice::None;
        g_activation_window = nullptr;
        g_system_delivery_failed.store(false, std::memory_order_release);
        if (was_initialized) wintoast_shutdown();
    }
    // Outside the lock: the renderer joins its own thread, and a click handler
    // running there can re-enter this backend.
    if (was_initialized) custom_toast::shutdown();
}

bool refresh_authorization() {
    std::lock_guard<std::mutex> lock(g_backend_mu);
    return g_initialized;
}

bool request_authorization() {
    std::lock_guard<std::mutex> lock(g_backend_mu);
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
