#pragma once

#include "agent_browser_capture.hpp"

namespace acecode::desktop::browser_capture {

using CommandResult = std::function<void(nlohmann::json, std::string)>;
using Command = std::function<void(const std::string&, const nlohmann::json&, CommandResult)>;

void capture_cdp(std::string page_id, const nlohmann::json& params,
                 Command command, Current current, Complete complete, Validate validate = {},
                 RegisterCleanup cleanup = {});

}
