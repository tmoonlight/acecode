#include "swarm_mode_command.hpp"

#include "session/session_manager.hpp"
#include "session_host/mesh/mesh_agent_service.hpp"
#include "session_host/swarm_command.hpp"
#include "tui/subagent_host.hpp"

#include <mutex>

namespace acecode {

namespace {

void emit(CommandContext& ctx, const std::string& text) {
    {
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        ctx.state.conversation.push_back({"system", text, false});
        ctx.state.chat_follow_tail = true;
    }
    if (ctx.post_event) ctx.post_event();
}

void cmd_swarm(CommandContext& ctx, const std::string& args) {
    if (!ctx.session_manager) {
        emit(ctx, "Swarm mode is unavailable: no session.");
        return;
    }
    const SwarmMode current =
        parse_swarm_mode(ctx.session_manager->current_swarm_mode()).value_or(SwarmMode::Off);
    const auto parsed = parse_swarm_command(args);
    if (!parsed.error.empty()) {
        emit(ctx, parsed.error);
        return;
    }
    if (parsed.show || !parsed.mode) {
        emit(ctx, swarm_command_status_text(current));
        return;
    }
    const SwarmMode next = *parsed.mode;
    if (current == SwarmMode::Mesh && next != SwarmMode::Mesh && ctx.subagent_host) {
        // 与 daemon 的 SessionRegistry 守卫同一判定:子 agent 仍在跑时不许退出网状模式。
        if (auto mesh = ctx.subagent_host->mesh()) {
            const std::string refused =
                mesh->tree_busy_reason(ctx.session_manager->current_session_id());
            if (!refused.empty()) {
                emit(ctx, refused);
                return;
            }
        }
    }
    // AgentLoop 每回合从 SessionManager 读模式,写会话元数据即对下一回合生效。
    ctx.session_manager->set_swarm_mode(swarm_mode_name(next));
    emit(ctx, swarm_command_applied_text(next));
}

} // namespace

void register_swarm_mode_command(CommandRegistry& registry) {
    registry.register_command({
        "swarm",
        "Show or switch swarm mode: /swarm star|mesh|off",
        cmd_swarm,
    });
}

} // namespace acecode
