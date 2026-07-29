#include "connector_first_start_auth.hpp"

#include "../utils/state_file.hpp"

namespace acecode {

ConnectorFirstStartAuthPlan plan_connector_first_start_auth(
    const std::vector<ConnectorConfig>& connectors) {
    const StateFlagClaimResult claim =
        try_claim_state_flag(kConnectorFirstStartAuthStateKey);

    ConnectorFirstStartAuthPlan plan;
    plan.claimed = claim.claimed;
    plan.persisted = claim.persisted;
    if (claim.claimed && claim.persisted) {
        plan.connectors = startup_hook_connectors(connectors);
    }
    return plan;
}

} // namespace acecode
