#include "computer_use_handler.hpp"
#include "computer_use/runtime.hpp"

namespace acecode::web {

bool computer_use_supported() {
    return computer_use::supported();
}

nlohmann::json computer_use_settings(const AppConfig& config) {
#ifdef _WIN32
    constexpr auto platform = "windows";
#elif defined(__APPLE__)
    constexpr auto platform = "macos";
#else
    constexpr auto platform = "linux";
#endif
    return {{"enabled", config.computer_use.enabled},
            {"pointer_style", config.computer_use.pointer_style}, {"pointer_color", config.computer_use.pointer_color},
            {"supported", computer_use_supported()}, {"platform", platform}};
}

bool apply_computer_use_settings(AppConfig& config,
                                 const nlohmann::json& patch,
                                 std::string& error) {
    error.clear();
    if (!patch.is_object()) {
        error = "expected a computer use settings object";
        return false;
    }
    auto next = config.computer_use;
    for (const auto& item : patch.items()) {
        if (item.key() == "enabled") {
            if (!item.value().is_boolean()) {
                error = "enabled must be a boolean";
                return false;
            }
            next.enabled = item.value().get<bool>();
        } else if (item.key() == "pointer_style") {
            if (!item.value().is_string() || !computer_use::pointer_appearance::valid_style(item.value().get<std::string>())) {
                error = "pointer_style must be ace or plain";
                return false;
            }
            next.pointer_style = item.value().get<std::string>();
        } else if (item.key() == "pointer_color") {
            const auto color = item.value().is_string()
                ? computer_use::pointer_appearance::normalize_color(item.value().get<std::string>()) : std::nullopt;
            if (!color) {
                error = "pointer_color must be #RRGGBB";
                return false;
            }
            next.pointer_color = *color;
        } else {
            error = "expected only enabled, pointer_style, or pointer_color";
            return false;
        }
    }
    if (patch.contains("enabled") && next.enabled && !computer_use_supported()) {
        error = "Computer Use requires Windows or macOS 14 or later";
        return false;
    }
    config.computer_use = next;
    return true;
}

} // namespace acecode::web
