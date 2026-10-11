#include "expert_registry.hpp"

namespace acecode {

const ExpertAgent* ExpertDefinition::selected_agent(const std::string& member_id) const {
    return agent(member_id.empty() ? lead_agent_id : member_id);
}

std::string ExpertDefinition::avatar_identity(const std::string& member_id) const {
    if (references_existing_experts) {
        return "expert:" + (member_id.empty() ? lead_expert_id : member_id);
    }
    if (type == ExpertType::Team) {
        return "expert:" + id + "/" + (member_id.empty() ? lead_agent_id : member_id);
    }
    return "expert:" + id;
}

} // namespace acecode
