// SessionRegistry 的会话级蜂群模式(add-mesh-swarm-mode):模式与网状身份随会话
// meta 持久化,经 AgentLoop::set_swarm_context 在下一回合生效;单独成文件让
// session_registry.cpp 保持在行数基线内。
#include "session_registry.hpp"

#include "swarm_command.hpp"
#include "session/agent_path.hpp"
#include "session/system_notice.hpp"
#include "utils/logger.hpp"

#include <shared_mutex>

namespace acecode {

void SessionRegistry::set_swarm_mode_guard(SwarmModeGuard guard) {
    std::lock_guard<std::mutex> lock(swarm_guard_mu_);
    swarm_guard_ = std::move(guard);
}

agent::SwarmContext SessionRegistry::swarm_context_for(SwarmMode mode,
                                                       const std::string& agent_path) const {
    MeshSwarmConfig mesh_config;
    if (deps_.config) {
        std::shared_lock<std::shared_mutex> lock;
        if (deps_.config_mutex) lock = std::shared_lock<std::shared_mutex>(*deps_.config_mutex);
        mesh_config = deps_.config->swarm.mesh;
    }
    return agent::make_swarm_context(mode, agent_path, mesh_config);
}

agent::SwarmContext SessionRegistry::apply_swarm_identity(SessionEntry& entry,
                                                          const SessionOptions& opts,
                                                          const SessionMeta* resumed_meta) {
    std::string mode_name = opts.swarm_mode;
    std::string path = opts.agent_path;
    if (resumed_meta) {
        if (mode_name.empty()) mode_name = resumed_meta->swarm_mode;
        if (path.empty()) path = resumed_meta->agent_path;
    }
    const SwarmMode mode = parse_swarm_mode(mode_name).value_or(SwarmMode::Off);
    if (mode != SwarmMode::Mesh) path.clear();
    if (entry.sm) {
        entry.sm->set_swarm_mode(swarm_mode_name(mode));
        entry.sm->set_mesh_agent_path(path);
    }
    return swarm_context_for(mode, path);
}

bool SessionRegistry::set_swarm_mode(const std::string& id, SwarmMode mode, std::string* error) {
    auto entry = acquire(id);
    if (!entry || !entry->loop || !entry->sm) {
        if (error) *error = "unknown session";
        return false;
    }
    const SwarmMode current =
        parse_swarm_mode(entry->sm->current_swarm_mode()).value_or(SwarmMode::Off);
    if (current == mode) return true;
    const std::string path = entry->sm->current_agent_path();
    if (current == SwarmMode::Mesh && !path.empty() && path != mesh::AgentPath::kRoot) {
        if (error) *error = "swarm mode cannot be changed for a mesh swarm sub-agent";
        return false;
    }
    SwarmModeGuard guard;
    {
        std::lock_guard<std::mutex> lock(swarm_guard_mu_);
        guard = swarm_guard_;
    }
    if (guard) {
        const std::string refused = guard(id, current, mode);
        if (!refused.empty()) {
            if (error) *error = refused;
            return false;
        }
    }
    // AgentLoop 每回合从 SessionManager 读模式,写会话元数据即对下一回合生效。
    entry->sm->set_swarm_mode(swarm_mode_name(mode));
    // 输入框的模式芯片跟随服务端:/swarm、其它标签页的切换都经这条事件同步。
    entry->loop->events().emit(SessionEventKind::SessionUpdated,
                               nlohmann::json{{"swarm_mode", swarm_mode_name(mode)}});
    LOG_INFO(std::string("[registry] session ") + id + " swarm mode -> " + swarm_mode_name(mode));
    return true;
}

std::optional<SwarmMode> SessionRegistry::swarm_mode(const std::string& id) const {
    std::shared_ptr<SessionEntry> entry;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = entries_.find(id);
        if (it != entries_.end()) entry = it->second;
    }
    if (!entry || !entry->sm) return std::nullopt;
    return parse_swarm_mode(entry->sm->current_swarm_mode()).value_or(SwarmMode::Off);
}

BuiltinCommandResult SessionRegistry::execute_swarm_builtin(SessionEntry& entry,
                                                            const BuiltinCommandRequest& request) {
    if (!entry.loop || !entry.sm) return {BuiltinCommandStatus::Failed, "session unavailable"};
    const SwarmMode current =
        parse_swarm_mode(entry.sm->current_swarm_mode()).value_or(SwarmMode::Off);
    const auto parsed = parse_swarm_command(request.args);
    std::string text;
    bool ok = parsed.error.empty();
    if (!ok) {
        text = parsed.error;
    } else if (parsed.show || !parsed.mode) {
        text = swarm_command_status_text(current);
    } else {
        std::string error;
        ok = set_swarm_mode(entry.id, *parsed.mode, &error);
        text = ok ? swarm_command_applied_text(*parsed.mode) : error;
    }
    entry.loop->emit_system_message(text, make_system_notice_metadata("swarm_status"));
    if (!ok) return {BuiltinCommandStatus::Failed, text};
    return {BuiltinCommandStatus::Accepted, "ok"};
}

} // namespace acecode
