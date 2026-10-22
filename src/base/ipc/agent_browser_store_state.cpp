#include "agent_browser_store_state.hpp"

#include "utils/state_file.hpp"

#include <algorithm>
#include <iterator>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace acecode::desktop {
namespace {

bool valid_session_char(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9')) {
        return true;
    }
    switch (c) {
    case '-': case '_': case '.': case '~': case ':': case '@': case '+':
        return true;
    default:
        return false;
    }
}

// 去重(保留首次出现)并只保留最新的 max_entries 条(清单尾部是最新追加的)。
std::vector<std::string> normalize_ids(std::vector<std::string> ids,
                                       std::size_t max_entries) {
    std::vector<std::string> result;
    result.reserve(ids.size());
    std::unordered_set<std::string> seen;
    for (auto& id : ids) {
        if (!valid_agent_browser_store_session_id(id)) continue;
        if (!seen.insert(id).second) continue;
        result.push_back(std::move(id));
    }
    if (result.size() > max_entries) {
        result.erase(result.begin(),
                     result.begin() +
                         static_cast<std::ptrdiff_t>(result.size() - max_entries));
    }
    return result;
}

// 同一子会话只保留最后一条;再按上限丢最旧的。
std::vector<AgentBrowserStoreSeed> normalize_seeds(
    std::vector<AgentBrowserStoreSeed> seeds) {
    std::vector<AgentBrowserStoreSeed> reversed;
    reversed.reserve(seeds.size());
    std::unordered_set<std::string> seen;
    for (auto it = seeds.rbegin(); it != seeds.rend(); ++it) {
        if (!valid_agent_browser_store_session_id(it->session_id) ||
            !valid_agent_browser_store_session_id(it->from) ||
            it->session_id == it->from) {
            continue;
        }
        if (!seen.insert(it->session_id).second) continue;
        reversed.push_back(std::move(*it));
    }
    std::vector<AgentBrowserStoreSeed> result(
        std::make_move_iterator(reversed.rbegin()),
        std::make_move_iterator(reversed.rend()));
    if (result.size() > kAgentBrowserMaxPendingSeeds) {
        result.erase(result.begin(),
                     result.begin() + static_cast<std::ptrdiff_t>(
                                          result.size() - kAgentBrowserMaxPendingSeeds));
    }
    return result;
}

std::vector<std::string> read_id_list(const nlohmann::json& section,
                                      const char* key) {
    std::vector<std::string> ids;
    if (!section.is_object()) return ids;
    const auto found = section.find(key);
    if (found == section.end() || !found->is_array()) return ids;
    for (const auto& item : *found) {
        if (item.is_string()) ids.push_back(item.get<std::string>());
    }
    return ids;
}

void normalize(AgentBrowserStoreState& state) {
    state.session_stores =
        normalize_ids(std::move(state.session_stores), kAgentBrowserMaxStoreListEntries);
    state.pending_wipe =
        normalize_ids(std::move(state.pending_wipe), kAgentBrowserMaxStoreListEntries);
    state.pending_deletions =
        normalize_ids(std::move(state.pending_deletions), kAgentBrowserMaxStoreListEntries);
    state.pending_seeds = normalize_seeds(std::move(state.pending_seeds));
    // 档位原文只保留短 ASCII,防止异常写入把任意内容带进界面。
    if (state.cookie_mode.size() > 32) state.cookie_mode.clear();
}

} // namespace

bool valid_agent_browser_store_session_id(const std::string& session_id) {
    if (session_id.empty() || session_id.size() > 128) return false;
    return std::all_of(session_id.begin(), session_id.end(), valid_session_char);
}

AgentBrowserStoreState agent_browser_store_state_from_json(
    const nlohmann::json& section) {
    AgentBrowserStoreState state;
    if (!section.is_object()) return state;
    const auto mode = section.find("cookie_mode");
    if (mode != section.end() && mode->is_string()) {
        state.cookie_mode = mode->get<std::string>();
    }
    state.session_stores = read_id_list(section, "session_stores");
    state.pending_wipe = read_id_list(section, "pending_wipe");
    state.pending_deletions = read_id_list(section, "pending_deletions");
    const auto shared_wipe = section.find("shared_wipe_pending");
    state.shared_wipe_pending = shared_wipe != section.end() &&
                                shared_wipe->is_boolean() &&
                                shared_wipe->get<bool>();
    const auto seeds = section.find("pending_seeds");
    if (seeds != section.end() && seeds->is_array()) {
        for (const auto& item : *seeds) {
            if (!item.is_object()) continue;
            const auto child = item.find("session_id");
            const auto from = item.find("from");
            if (child == item.end() || !child->is_string() ||
                from == item.end() || !from->is_string()) {
                continue;
            }
            AgentBrowserStoreSeed seed;
            seed.session_id = child->get<std::string>();
            seed.from = from->get<std::string>();
            const auto at = item.find("at_ms");
            if (at != item.end() && at->is_number_integer()) {
                seed.at_ms = at->get<std::int64_t>();
            }
            state.pending_seeds.push_back(std::move(seed));
        }
    }
    normalize(state);
    return state;
}

nlohmann::json agent_browser_store_state_to_json(const AgentBrowserStoreState& input) {
    AgentBrowserStoreState state = input;
    normalize(state);
    nlohmann::json section = nlohmann::json::object();
    if (!state.cookie_mode.empty()) section["cookie_mode"] = state.cookie_mode;
    if (!state.session_stores.empty()) section["session_stores"] = state.session_stores;
    if (!state.pending_wipe.empty()) section["pending_wipe"] = state.pending_wipe;
    if (state.shared_wipe_pending) section["shared_wipe_pending"] = true;
    if (!state.pending_deletions.empty()) {
        section["pending_deletions"] = state.pending_deletions;
    }
    if (!state.pending_seeds.empty()) {
        nlohmann::json seeds = nlohmann::json::array();
        for (const auto& seed : state.pending_seeds) {
            seeds.push_back({{"session_id", seed.session_id},
                             {"from", seed.from},
                             {"at_ms", seed.at_ms}});
        }
        section["pending_seeds"] = std::move(seeds);
    }
    return section;
}

AgentBrowserStoreState read_agent_browser_store_state() {
    const nlohmann::json state = read_state_json();
    if (!state.is_object()) return {};
    const auto found = state.find(kAgentBrowserStoreStateKey);
    if (found == state.end()) return {};
    return agent_browser_store_state_from_json(*found);
}

bool update_agent_browser_store_state(
    const std::function<bool(AgentBrowserStoreState&)>& mutate) {
    return update_state_json([&mutate](nlohmann::json& state) {
        if (!state.is_object()) state = nlohmann::json::object();
        const auto found = state.find(kAgentBrowserStoreStateKey);
        AgentBrowserStoreState section = found == state.end()
            ? AgentBrowserStoreState{}
            : agent_browser_store_state_from_json(*found);
        if (!mutate(section)) return false;
        nlohmann::json written = agent_browser_store_state_to_json(section);
        if (written.empty()) {
            if (found == state.end()) return false;
            state.erase(kAgentBrowserStoreStateKey);
        } else {
            state[kAgentBrowserStoreStateKey] = std::move(written);
        }
        return true;
    });
}

bool record_agent_browser_store_deletion(const std::string& session_id) {
    if (!valid_agent_browser_store_session_id(session_id)) return false;
    return update_agent_browser_store_state([&session_id](AgentBrowserStoreState& state) {
        state.pending_deletions.push_back(session_id);
        // 子会话被删掉之后它的分叉记录就没用了;以它为父的记录保留 —— 复制
        // 发生在子会话第一次建存储时,父会话的存储那时若已删除,复制自然跳过。
        state.pending_seeds.erase(
            std::remove_if(state.pending_seeds.begin(), state.pending_seeds.end(),
                           [&session_id](const AgentBrowserStoreSeed& seed) {
                               return seed.session_id == session_id;
                           }),
            state.pending_seeds.end());
        return true;
    });
}

bool record_agent_browser_store_seed(const std::string& session_id,
                                     const std::string& from_session_id,
                                     std::int64_t at_ms) {
    if (!valid_agent_browser_store_session_id(session_id) ||
        !valid_agent_browser_store_session_id(from_session_id) ||
        session_id == from_session_id) {
        return false;
    }
    return update_agent_browser_store_state(
        [&session_id, &from_session_id, at_ms](AgentBrowserStoreState& state) {
        state.pending_seeds.push_back({session_id, from_session_id, at_ms});
        return true;
    });
}

} // namespace acecode::desktop
