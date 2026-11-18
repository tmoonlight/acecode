#pragma once

#include "agent_browser_host.hpp"

#include <nlohmann/json.hpp>

namespace acecode::desktop {

nlohmann::json agent_browser_state_json(const AgentBrowserState& state);

}
