#pragma once
#include <cstdint>
#include <string>
namespace acecode::desktop {
struct AgentBrowserBounds;
}
namespace acecode::desktop::agent_browser_common {
std::int64_t now_unix_ms();
void assign_error(std::string* target, const std::string& value);
std::string agent_browser_title_or_default(std::string title);
// set_bounds 的入参校验。两个平台的宿主各写了一遍同样的判定，收在这里避免
// 一边加了字段另一边漏校验。失败时写 error 并返回 false。
bool valid_agent_browser_bounds(const AgentBrowserBounds& bounds,
                                std::string* error);
} // namespace acecode::desktop::agent_browser_common
