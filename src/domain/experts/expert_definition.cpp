#include "expert_registry.hpp"

#include <algorithm>

namespace acecode {
namespace fs = std::filesystem;

const ExpertAgent* ExpertDefinition::agent(const std::string& agent_id) const {
    const auto it = std::find_if(agents.begin(), agents.end(), [&](const ExpertAgent& item) {
        return item.id == agent_id;
    });
    return it == agents.end() ? nullptr : &*it;
}

std::vector<fs::path> ExpertDefinition::selected_skill_roots(
    const std::string& member_id) const {
    std::vector<fs::path> roots = skill_roots;
    if (const ExpertAgent* selected = selected_agent(member_id)) {
        for (const auto& root : selected->skill_roots) {
            if (std::find(roots.begin(), roots.end(), root) == roots.end()) {
                roots.push_back(root);
            }
        }
    }
    return roots;
}

ExpertCapabilityScopes ExpertDefinition::selected_capabilities(
    const std::string& member_id) const {
    if (references_existing_experts) {
        if (const ExpertAgent* selected = selected_agent(member_id)) {
            return selected->capabilities;
        }
    }
    return capabilities;
}

ModelSamplingOptions ExpertDefinition::selected_sampling(const std::string& member_id) const {
    if (const auto* selected = selected_agent(member_id)) {
        return merge_model_sampling_options(sampling, selected->sampling);
    }
    return sampling;
}

bool ExpertDefinition::is_declared_member(const std::string& member_id) const {
    return type == ExpertType::Team &&
           std::find(member_agent_ids.begin(), member_agent_ids.end(), member_id) !=
               member_agent_ids.end();
}

const char* to_string(ExpertType type) {
    return type == ExpertType::Team ? "team" : "agent";
}

bool ExpertRegistry::valid_id(const std::string& id) {
    if (id.empty() || id.size() > 64 || id.front() == '-' || id.back() == '-') return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
    });
}

bool is_known_expert_avatar_state(std::string_view state) {
    return std::find(kExpertAvatarStates.begin(), kExpertAvatarStates.end(),
                     state) != kExpertAvatarStates.end();
}

bool is_supported_expert_avatar_mime(std::string_view mime) {
    return mime == "image/png" || mime == "image/jpeg" ||
           mime == "image/gif" || mime == "image/webp" ||
           mime == "image/bmp" || mime == "image/x-icon";
}

} // namespace acecode
