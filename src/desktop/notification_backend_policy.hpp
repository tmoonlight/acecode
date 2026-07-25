#pragma once

// Chooses between the OS toast backend (WinToast / Action Center) and the
// self-drawn toast window.
//
// Motivation: on a non-trivial share of Windows 10 machines WinToast reports a
// successful `initialize()` *and* a successful `showToast()` while nothing ever
// appears on screen. The notification is dropped downstream by the OS — the
// Start Menu shortcut carrying the AppUserModelID is missing, notifications are
// switched off for the app or globally, a "debloat" utility disabled the
// notification platform, or group policy forbids toasts. None of that is
// observable through WinToast's return values, so the failure is silent.
//
// The decision below turns those OS-side settings into an explicit signal set
// so the caller can pick the self-drawn renderer *before* delivering a
// notification into a black hole. Keeping it as a pure function makes the
// policy testable on every platform.

#include <string>
#include <string_view>

namespace acecode::desktop {

// `config.desktop.notifications.backend`.
enum class NotificationBackendPreference {
    // Prefer OS toasts, fall back to the self-drawn renderer when the OS path
    // is unavailable or suppressed.
    Auto,
    // OS toasts only. When the OS path is unusable no notification is shown —
    // an explicit choice is not silently overridden.
    System,
    // Always use the self-drawn renderer.
    Custom,
};

NotificationBackendPreference parse_notification_backend_preference(
    std::string_view value,
    NotificationBackendPreference fallback = NotificationBackendPreference::Auto);

std::string_view notification_backend_preference_value(
    NotificationBackendPreference preference);

// Everything the platform layer can observe about the OS toast pipeline.
// Defaults describe "the OS would deliver a toast" so a probe that cannot read
// a given setting stays neutral instead of forcing a fallback.
struct SystemToastSignals {
    // WinToast compiled in, the Windows version supports toasts, and
    // initialization succeeded.
    bool platform_supported = false;
    // HKCU\...\PushNotifications\ToastEnabled
    bool toasts_enabled_globally = true;
    // HKCU\...\Notifications\Settings\<AUMID>\Enabled
    bool app_toasts_enabled = true;
    // Group policy (NoToastApplicationNotification / DisableNotificationCenter).
    bool policy_disabled = false;
    // Set once a delivery attempt failed at runtime. Sticky for the process.
    bool runtime_delivery_failed = false;
};

enum class NotificationBackendChoice {
    None,
    System,
    Custom,
};

struct NotificationBackendDecision {
    NotificationBackendChoice choice = NotificationBackendChoice::None;
    // Short English phrase for the startup log line.
    std::string reason;
};

NotificationBackendDecision decide_notification_backend(
    NotificationBackendPreference preference,
    const SystemToastSignals& signals);

const char* notification_backend_choice_name(NotificationBackendChoice choice);

} // namespace acecode::desktop
