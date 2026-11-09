#pragma once

#include "tool_executor.hpp"

#include <optional>
#include <string>

namespace acecode {

// Create and return a BashTool implementation ready for registration
ToolImpl create_bash_tool();

// 命令开头那一段只是在睡觉时(sleep N / Start-Sleep N / timeout /t N /
// ping -n N 127.0.0.1)返回睡眠秒数,否则 nullopt。
std::optional<double> bash_leading_sleep_seconds(const std::string& command);

} // namespace acecode
