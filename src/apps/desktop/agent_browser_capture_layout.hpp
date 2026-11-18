#pragma once

#include "agent_browser_capture_cdp.hpp"

namespace acecode::desktop::browser_capture {

void capture_preserving_layout(Command command, Current current,
                               nlohmann::json params, CommandResult complete);

}
