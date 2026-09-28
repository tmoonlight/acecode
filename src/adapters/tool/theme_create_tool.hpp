#pragma once

#include "tool_executor.hpp"
#include <filesystem>

namespace acecode {

// Empty root resolves the active ACECode data directory at call time.
ToolImpl create_theme_create_tool(std::filesystem::path theme_root = {});

} // namespace acecode
