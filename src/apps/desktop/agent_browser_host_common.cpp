#include "agent_browser_host_common.hpp"
#include "agent_browser_host.hpp"
#include <chrono>
#include <utility>
namespace acecode::desktop::agent_browser_common {
std::int64_t now_unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void assign_error(std::string* target, const std::string& value) {
    if (target) *target = value;
}

std::string agent_browser_title_or_default(std::string title) {
    return title.find_first_not_of(" \t\r\n") == std::string::npos
        ? std::string(kAgentBrowserDefaultTitle)
        : std::move(title);
}

bool valid_agent_browser_bounds(const AgentBrowserBounds& bounds,
                                std::string* error) {
    if (bounds.x < 0 || bounds.y < 0 || bounds.width < 0 || bounds.height < 0 ||
        bounds.width > 32768 || bounds.height > 32768) {
        assign_error(error, "Agent Browser bounds are invalid");
        return false;
    }
    if (bounds.occlusion_rects.size() > kAgentBrowserMaxOcclusionRects) {
        assign_error(error, "Agent Browser has too many occlusion rectangles");
        return false;
    }
    for (const auto& rect : bounds.occlusion_rects) {
        if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 ||
            rect.x > 32768 || rect.y > 32768 || rect.width > 32768 ||
            rect.height > 32768 || rect.radius < 0 || rect.radius > 32768 ||
            rect.square_corners < 0 || rect.square_corners > 0xF) {
            assign_error(error, "Agent Browser occlusion rectangle is invalid");
            return false;
        }
    }
    return true;
}

} // namespace acecode::desktop::agent_browser_common
