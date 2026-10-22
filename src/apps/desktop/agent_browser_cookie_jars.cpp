#include "agent_browser_cookie_jars.hpp"

#include "utils/sha256.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::desktop {
namespace {

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void erase_value(std::vector<std::string>& values, const std::string& value) {
    values.erase(std::remove(values.begin(), values.end(), value), values.end());
}

} // namespace

const char* agent_browser_cookie_mode_name(AgentBrowserCookieMode mode) {
    switch (mode) {
    case AgentBrowserCookieMode::UntilQuit: return kAgentBrowserCookieModeUntilQuit;
    case AgentBrowserCookieMode::PerSession: return kAgentBrowserCookieModePerSession;
    case AgentBrowserCookieMode::Shared: break;
    }
    return kAgentBrowserCookieModeShared;
}

std::optional<AgentBrowserCookieMode> agent_browser_cookie_mode_from_name(
    const std::string& name) {
    if (name == kAgentBrowserCookieModeUntilQuit) return AgentBrowserCookieMode::UntilQuit;
    if (name == kAgentBrowserCookieModeShared) return AgentBrowserCookieMode::Shared;
    if (name == kAgentBrowserCookieModePerSession) return AgentBrowserCookieMode::PerSession;
    return std::nullopt;
}

AgentBrowserCookieMode effective_agent_browser_cookie_mode(const std::string& stored) {
    return agent_browser_cookie_mode_from_name(stored).value_or(
        AgentBrowserCookieMode::Shared);
}

std::string AgentBrowserCookieStoreKey::id() const {
    switch (kind) {
    case AgentBrowserCookieStoreKind::UntilQuit: return kAgentBrowserCookieModeUntilQuit;
    case AgentBrowserCookieStoreKind::Session: return "session:" + session_id;
    case AgentBrowserCookieStoreKind::Shared: break;
    }
    return kAgentBrowserCookieModeShared;
}

std::string agent_browser_cookie_store_session(const AgentBrowserPageOwner& owner) {
    if (owner.empty()) return {};
    return owner.root_session_id.empty() ? owner.session_id : owner.root_session_id;
}

AgentBrowserCookieStoreKey agent_browser_cookie_store_key(
    AgentBrowserCookieMode mode,
    const AgentBrowserPageOwner& owner) {
    AgentBrowserCookieStoreKey key;
    if (mode == AgentBrowserCookieMode::UntilQuit) {
        key.kind = AgentBrowserCookieStoreKind::UntilQuit;
        return key;
    }
    if (mode == AgentBrowserCookieMode::PerSession) {
        const std::string session = agent_browser_cookie_store_session(owner);
        if (valid_agent_browser_store_session_id(session)) {
            key.kind = AgentBrowserCookieStoreKind::Session;
            key.session_id = session;
        }
    }
    return key;
}

std::string agent_browser_session_profile_name(const std::string& session_id) {
    return "s-" + acecode::sha256_hex(session_id).substr(0, 24);
}

std::string agent_browser_session_profile_directory_name(const std::string& session_id) {
    return "WV2Profile_" + agent_browser_session_profile_name(session_id);
}

AgentBrowserWindowsProfile agent_browser_windows_profile(
    const AgentBrowserCookieStoreKey& key) {
    AgentBrowserWindowsProfile profile;
    switch (key.kind) {
    case AgentBrowserCookieStoreKind::UntilQuit:
        profile.name = kAgentBrowserUntilQuitProfileName;
        profile.in_private = true;
        break;
    case AgentBrowserCookieStoreKind::Session:
        profile.name = agent_browser_session_profile_name(key.session_id);
        break;
    case AgentBrowserCookieStoreKind::Shared:
        break;
    }
    return profile;
}

std::string agent_browser_session_store_uuid(const std::string& session_id) {
    // 加前缀做命名空间,避免与别处对同一 id 取的哈希重合。
    std::string hex = acecode::sha256_hex(
        "acecode.agent-browser.session-store:" + session_id).substr(0, 32);
    for (auto& c : hex) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    // 版本 8(自定义)放在第 13 个十六进制位;变体 10xx 放在第 17 位。
    hex[12] = '8';
    static const char kVariant[] = "89AB";
    const int nibble = std::isdigit(static_cast<unsigned char>(hex[16]))
        ? hex[16] - '0' : hex[16] - 'A' + 10;
    hex[16] = kVariant[nibble & 0x3];
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) +
           "-" + hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

AgentBrowserCookieModeSwitch plan_agent_browser_cookie_mode_switch(
    AgentBrowserCookieMode from,
    AgentBrowserCookieMode to) {
    AgentBrowserCookieModeSwitch plan;
    if (from == to) return plan;
    plan.changed = true;
    if (to == AgentBrowserCookieMode::UntilQuit) {
        plan.clear_shared_now = true;
        plan.mark_session_stores_for_wipe = true;
    }
    if (from == AgentBrowserCookieMode::UntilQuit) {
        plan.release_until_quit_store = true;
    }
    return plan;
}

std::vector<std::string> agent_browser_pages_to_rebuild(
    const std::vector<AgentBrowserPageStoreBinding>& pages,
    AgentBrowserCookieMode new_mode) {
    std::vector<std::string> result;
    for (const auto& page : pages) {
        if (agent_browser_cookie_store_key(new_mode, page.owner) != page.store) {
            result.push_back(page.page_id);
        }
    }
    return result;
}

std::vector<std::string> agent_browser_pages_of_root_session(
    const std::vector<AgentBrowserPageStoreBinding>& pages,
    const std::string& root_session_id) {
    std::vector<std::string> result;
    if (root_session_id.empty()) return result;
    for (const auto& page : pages) {
        if (agent_browser_cookie_store_session(page.owner) == root_session_id) {
            result.push_back(page.page_id);
        }
    }
    return result;
}

AgentBrowserSessionStorePreparation prepare_agent_browser_session_store(
    const std::string& session_id,
    const AgentBrowserStoreState& state) {
    AgentBrowserSessionStorePreparation preparation;
    if (!valid_agent_browser_store_session_id(session_id)) return preparation;
    preparation.wipe_first = contains(state.pending_wipe, session_id);
    const bool registered = contains(state.session_stores, session_id);
    preparation.register_store = !registered;
    if (registered) return preparation;
    // 只给全新的存储复制:已经用过的存储(哪怕刚被清空)不再从父会话补 cookie。
    for (auto it = state.pending_seeds.rbegin(); it != state.pending_seeds.rend(); ++it) {
        if (it->session_id != session_id) continue;
        const std::string& parent = it->from;
        const bool parent_usable = contains(state.session_stores, parent) &&
                                   !contains(state.pending_wipe, parent) &&
                                   !contains(state.pending_deletions, parent);
        if (parent_usable) preparation.seed_from = parent;
        break;
    }
    return preparation;
}

void apply_agent_browser_session_store_preparation(
    const std::string& session_id,
    AgentBrowserStoreState& state) {
    if (!valid_agent_browser_store_session_id(session_id)) return;
    if (!contains(state.session_stores, session_id)) {
        state.session_stores.push_back(session_id);
    }
    erase_value(state.pending_wipe, session_id);
    state.pending_seeds.erase(
        std::remove_if(state.pending_seeds.begin(), state.pending_seeds.end(),
                       [&session_id](const AgentBrowserStoreSeed& seed) {
                           return seed.session_id == session_id;
                       }),
        state.pending_seeds.end());
}

} // namespace acecode::desktop
