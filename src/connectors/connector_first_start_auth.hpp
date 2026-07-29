#pragma once

#include "../config/config.hpp"

#include <vector>

namespace acecode {

constexpr const char* kConnectorFirstStartAuthStateKey =
    "connector_first_start_auth_v1";

struct ConnectorFirstStartAuthPlan {
    bool claimed = false;
    bool persisted = false;
    std::vector<ConnectorConfig> connectors;
};

// Claims the versioned runtime-state gate before returning any executable
// hooks. A failed claim and every later startup both return an empty list.
ConnectorFirstStartAuthPlan plan_connector_first_start_auth(
    const std::vector<ConnectorConfig>& connectors);

} // namespace acecode
