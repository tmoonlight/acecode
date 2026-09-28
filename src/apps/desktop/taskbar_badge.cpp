#include "taskbar_badge.hpp"

#include <limits>
#include <nlohmann/json.hpp>

namespace acecode::desktop {

std::string taskbar_badge_label(int count) {
    if (count <= 0) return {};
    return count > 99 ? "99+" : std::to_string(count);
}

std::optional<TaskbarBadge> parse_taskbar_badge_args(const std::string& args) {
    const auto values = nlohmann::json::parse(args, nullptr, false);
    if (!values.is_array() || values.size() != 1) return std::nullopt;
    const auto& payload = values[0];
    if (!payload.is_object() || !payload.contains("count")) return std::nullopt;
    const auto& count = payload["count"];
    if (!count.is_number_integer() || count < 0 ||
        count > std::numeric_limits<int>::max()) return std::nullopt;

    TaskbarBadge badge;
    badge.count = count.get<int>();
    if (badge.count == 0) return badge;
    auto color = [&](const char* name) -> std::optional<WindowBackgroundColor> {
        const auto item = payload.find(name);
        if (item == payload.end() || !item->is_string()) return std::nullopt;
        return parse_window_background_color(item->get_ref<const std::string&>());
    };
    const auto background = color("background");
    const auto foreground = color("foreground");
    const auto outline = color("outline");
    if (!background || !foreground || !outline) return std::nullopt;
    badge.background = *background;
    badge.foreground = *foreground;
    badge.outline = *outline;
    return badge;
}

} // namespace acecode::desktop
