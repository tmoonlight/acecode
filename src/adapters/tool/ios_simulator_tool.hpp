#pragma once

#include "tool_executor.hpp"
#include "config/config.hpp"

namespace acecode {

// ios_simulator: observe and drive an Xcode iOS Simulator (macOS only).
// ios_simulator_build: xcodebuild for the simulator with the selected Xcode.
std::vector<ToolImpl> create_ios_simulator_tools();
// Applies config.ios_simulator to the runtime and (un)registers the tools.
void refresh_ios_simulator_tools(ToolExecutor& tools, const AppConfig& config);

} // namespace acecode
