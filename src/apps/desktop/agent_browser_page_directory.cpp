#include "agent_browser_page_directory.hpp"

#include <algorithm>

namespace acecode::desktop {
namespace {

constexpr std::size_t kMaxOwnerFieldLength = 128;

bool owner_field_char_allowed(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
           c == '~' || c == ':' || c == '@' || c == '+';
}

std::string sanitized_owner_field(const nlohmann::json& object,
                                  const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) return {};
    const std::string& value = found->get_ref<const std::string&>();
    if (value.empty() || value.size() > kMaxOwnerFieldLength) return {};
    if (!std::all_of(value.begin(), value.end(), owner_field_char_allowed)) {
        return {};
    }
    return value;
}

} // namespace

bool operator==(const AgentBrowserPageOwner& lhs,
                const AgentBrowserPageOwner& rhs) {
    return lhs.session_id == rhs.session_id &&
           lhs.workspace_hash == rhs.workspace_hash &&
           lhs.root_session_id == rhs.root_session_id;
}

bool operator!=(const AgentBrowserPageOwner& lhs,
                const AgentBrowserPageOwner& rhs) {
    return !(lhs == rhs);
}

AgentBrowserPageOwner parse_agent_browser_page_owner(
    const nlohmann::json& value) {
    AgentBrowserPageOwner owner;
    if (!value.is_object()) return owner;
    owner.session_id = sanitized_owner_field(value, "session_id");
    if (owner.session_id.empty()) return owner;
    owner.workspace_hash = sanitized_owner_field(value, "workspace_hash");
    owner.root_session_id = sanitized_owner_field(value, "root_session_id");
    return owner;
}

nlohmann::json agent_browser_page_owner_json(
    const AgentBrowserPageOwner& owner) {
    if (owner.empty()) return nullptr;
    return {
        {"session_id", owner.session_id},
        {"workspace_hash", owner.workspace_hash},
        {"root_session_id", owner.root_session_id},
    };
}

void AgentBrowserPageDirectory::add_page(const std::string& page_id,
                                         const AgentBrowserPageOwner& owner,
                                         bool agent_target) {
    if (page_id.empty() || contains(page_id)) return;
    order_.push_back(page_id);
    owners_[page_id] = owner;
    if (agent_target && !owner.empty()) {
        agent_target_by_session_[owner.session_id] = page_id;
    }
}

void AgentBrowserPageDirectory::remove_page(const std::string& page_id) {
    const auto found = owners_.find(page_id);
    if (found == owners_.end()) return;
    const AgentBrowserPageOwner owner = found->second;
    owners_.erase(found);
    order_.erase(std::remove(order_.begin(), order_.end(), page_id),
                 order_.end());
    if (displayed_ == page_id) displayed_.clear();
    if (!owner.empty()) {
        const auto target = agent_target_by_session_.find(owner.session_id);
        if (target != agent_target_by_session_.end() &&
            target->second == page_id) {
            const std::string replacement =
                latest_page_for_session(owner.session_id, page_id);
            if (replacement.empty()) {
                agent_target_by_session_.erase(target);
            } else {
                target->second = replacement;
            }
        }
    }
}

void AgentBrowserPageDirectory::clear() {
    order_.clear();
    owners_.clear();
    agent_target_by_session_.clear();
    displayed_.clear();
}

bool AgentBrowserPageDirectory::contains(const std::string& page_id) const {
    return owners_.find(page_id) != owners_.end();
}

std::vector<std::string> AgentBrowserPageDirectory::page_ids_for_session(
    const std::string& session_id) const {
    std::vector<std::string> result;
    if (session_id.empty()) return result;
    for (const std::string& id : order_) {
        const auto found = owners_.find(id);
        if (found != owners_.end() && found->second.session_id == session_id) {
            result.push_back(id);
        }
    }
    return result;
}

AgentBrowserPageOwner AgentBrowserPageDirectory::owner_of(
    const std::string& page_id) const {
    const auto found = owners_.find(page_id);
    return found == owners_.end() ? AgentBrowserPageOwner{} : found->second;
}

bool AgentBrowserPageDirectory::set_displayed_page(const std::string& page_id) {
    if (!contains(page_id)) return false;
    displayed_ = page_id;
    return true;
}

std::string AgentBrowserPageDirectory::agent_target_for(
    const AgentBrowserPageOwner& owner) const {
    if (owner.empty()) return {};
    const auto found = agent_target_by_session_.find(owner.session_id);
    if (found == agent_target_by_session_.end()) return {};
    return contains(found->second) ? found->second : std::string{};
}

bool AgentBrowserPageDirectory::set_agent_target(
    const AgentBrowserPageOwner& owner,
    const std::string& page_id) {
    if (owner.empty() || !contains(page_id)) return false;
    agent_target_by_session_[owner.session_id] = page_id;
    return true;
}

bool AgentBrowserPageDirectory::is_agent_target(
    const std::string& page_id) const {
    if (page_id.empty()) return false;
    for (const auto& [session_id, target] : agent_target_by_session_) {
        if (target == page_id) return true;
    }
    return false;
}

AgentBrowserPageResolution AgentBrowserPageDirectory::resolve_agent_page(
    const std::string& requested_page_id,
    const AgentBrowserPageOwner& owner) const {
    AgentBrowserPageResolution resolution;
    if (!requested_page_id.empty()) {
        resolution.page_id = requested_page_id;
        return resolution;
    }
    if (owner.empty()) {
        if (!displayed_.empty()) {
            resolution.page_id = displayed_;
        } else {
            resolution.create = true;
        }
        return resolution;
    }
    const std::string target = agent_target_for(owner);
    if (!target.empty()) {
        resolution.page_id = target;
        return resolution;
    }
    if (!displayed_.empty() && owner_of(displayed_).same_session(owner)) {
        resolution.page_id = displayed_;
        return resolution;
    }
    resolution.create = true;
    return resolution;
}

std::string AgentBrowserPageDirectory::next_displayed_after_close(
    const std::string& page_id) const {
    if (displayed_ != page_id) return displayed_;
    const AgentBrowserPageOwner owner = owner_of(page_id);
    if (!owner.empty()) {
        const std::string sibling =
            latest_page_for_session(owner.session_id, page_id);
        if (!sibling.empty()) return sibling;
    }
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        if (*it != page_id) return *it;
    }
    return {};
}

std::string AgentBrowserPageDirectory::latest_page_for_session(
    const std::string& session_id,
    const std::string& excluding) const {
    if (session_id.empty()) return {};
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        if (*it == excluding) continue;
        const auto found = owners_.find(*it);
        if (found != owners_.end() && found->second.session_id == session_id) {
            return *it;
        }
    }
    return {};
}

} // namespace acecode::desktop
