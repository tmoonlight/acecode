#pragma once
#include <cstdint>
#include <string>
namespace acecode::desktop::agent_browser_common {
std::int64_t now_unix_ms();
void assign_error(std::string* target, const std::string& value);
std::string agent_browser_title_or_default(std::string title);
} // namespace acecode::desktop::agent_browser_common
