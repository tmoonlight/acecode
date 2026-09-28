#pragma once

#include "window_background.hpp"

#include <optional>
#include <string>

namespace acecode::desktop {

struct TaskbarBadge {
    int count = 0;
    WindowBackgroundColor background;
    WindowBackgroundColor foreground;
    WindowBackgroundColor outline;

    bool operator==(const TaskbarBadge& other) const {
        return count == other.count && background == other.background &&
               foreground == other.foreground && outline == other.outline;
    }
};

std::string taskbar_badge_label(int count);
std::optional<TaskbarBadge> parse_taskbar_badge_args(const std::string& args);

} // namespace acecode::desktop
