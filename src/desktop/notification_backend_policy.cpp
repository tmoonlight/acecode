#include "notification_backend_policy.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::desktop {
namespace {

std::string normalize(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        const auto uch = static_cast<unsigned char>(ch);
        if (std::isspace(uch)) continue;
        out.push_back(static_cast<char>(std::tolower(uch)));
    }
    return out;
}

} // namespace

NotificationBackendPreference parse_notification_backend_preference(
    std::string_view value,
    NotificationBackendPreference fallback) {
    const std::string normalized = normalize(value);
    if (normalized == "auto") return NotificationBackendPreference::Auto;
    if (normalized == "system" || normalized == "os" ||
        normalized == "wintoast") {
        return NotificationBackendPreference::System;
    }
    if (normalized == "custom" || normalized == "builtin" ||
        normalized == "self") {
        return NotificationBackendPreference::Custom;
    }
    return fallback;
}

std::string_view notification_backend_preference_value(
    NotificationBackendPreference preference) {
    switch (preference) {
        case NotificationBackendPreference::Auto:
            return "auto";
        case NotificationBackendPreference::System:
            return "system";
        case NotificationBackendPreference::Custom:
            return "custom";
    }
    return "auto";
}

NotificationBackendDecision decide_notification_backend(
    NotificationBackendPreference preference,
    const SystemToastSignals& signals) {
    if (preference == NotificationBackendPreference::Custom) {
        return {NotificationBackendChoice::Custom, "configured backend=custom"};
    }

    if (preference == NotificationBackendPreference::System) {
        // An explicit request for OS toasts is honored verbatim: falling back
        // to a self-drawn window here would defeat the reason for asking.
        if (!signals.platform_supported) {
            return {NotificationBackendChoice::None,
                    "configured backend=system but the OS toast backend is "
                    "unavailable"};
        }
        return {NotificationBackendChoice::System, "configured backend=system"};
    }

    if (!signals.platform_supported) {
        return {NotificationBackendChoice::Custom,
                "OS toast backend unavailable"};
    }
    if (signals.runtime_delivery_failed) {
        return {NotificationBackendChoice::Custom,
                "OS toast delivery failed earlier in this process"};
    }
    if (signals.policy_disabled) {
        return {NotificationBackendChoice::Custom,
                "OS toasts disabled by group policy"};
    }
    if (!signals.toasts_enabled_globally) {
        return {NotificationBackendChoice::Custom,
                "OS toasts disabled system-wide"};
    }
    if (!signals.app_toasts_enabled) {
        return {NotificationBackendChoice::Custom,
                "OS toasts disabled for this application"};
    }
    return {NotificationBackendChoice::System, "OS toast backend available"};
}

const char* notification_backend_choice_name(NotificationBackendChoice choice) {
    switch (choice) {
        case NotificationBackendChoice::None:
            return "none";
        case NotificationBackendChoice::System:
            return "system";
        case NotificationBackendChoice::Custom:
            return "custom";
    }
    return "none";
}

} // namespace acecode::desktop
