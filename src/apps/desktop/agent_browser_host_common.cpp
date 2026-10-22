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

} // namespace acecode::desktop::agent_browser_common
