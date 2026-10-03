// MeshAgentService::spawn — agent_spawn(Codex Multi-Agent V2 spawn_agent)。
#include "mesh_agent_state.hpp"

#include "agent/agent_loop.hpp"
#include "config/config.hpp"
#include "experts/expert_registry.hpp"
#include "session/compact_checkpoint.hpp"
#include "session/session_manager.hpp"
#include "session_host/session_registry.hpp"
#include "tool/tool_executor.hpp"
#include "utils/logger.hpp"

#include <algorithm>

namespace acecode::mesh {

namespace {

std::string join_names(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& name : names) {
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out.empty() ? std::string("(none)") : out;
}

// Child expert binding. agent_type names a member of the caller's team expert
// (Codex agent_type -> ACECode expert member). Omitted: a full-history fork
// inherits the caller's binding (Codex inherits the parent agent type),
// otherwise the child starts without an expert.
std::string resolve_expert(const ExpertRegistry* experts, const std::string& cwd,
                           SessionManager& caller_sm, const SpawnArgs& args,
                           SessionOptions& opts) {
    const std::string expert_id = caller_sm.current_expert_id();
    if (args.agent_type.empty()) {
        if (args.fork.kind == ForkTurns::Kind::All && !expert_id.empty()) {
            opts.expert_id = expert_id;
            opts.expert_member_id = caller_sm.current_expert_member_id();
        }
        return {};
    }
    if (expert_id.empty() || !experts) {
        return "agent_type is only available when this session is bound to a team expert";
    }
    const auto expert = experts->find(cwd, expert_id);
    if (!expert || expert->type != ExpertType::Team) {
        return "agent_type is only available when this session is bound to a team expert";
    }
    if (!expert->is_declared_member(args.agent_type)) {
        return "unknown agent_type '" + args.agent_type + "'";
    }
    opts.expert_id = expert_id;
    opts.expert_member_id = args.agent_type;
    return {};
}

} // namespace

SpawnResult MeshAgentService::spawn(const ToolContext& ctx, const SpawnArgs& args) {
    SpawnResult result;
    if (args.message.find_first_not_of(" \t\r\n") == std::string::npos) {
        result.error = "Empty message can't be sent to an agent";
        return result;
    }
    std::string error;
    const auto self = caller(ctx, &error);
    if (!self) {
        result.error = error;
        return result;
    }
    const auto parent_path = AgentPath::parse(self->path);
    const auto child_path = parent_path ? parent_path->join(args.task_name, &error) : std::nullopt;
    if (!child_path) {
        result.error = error;
        return result;
    }
    SessionOptions opts;
    if (!args.model.empty()) {
        const auto names = saved_model_names();
        if (std::find(names.begin(), names.end(), args.model) == names.end()) {
            result.error = "Unknown model `" + args.model +
                           "` for agent_spawn. Available models: " + join_names(names);
            return result;
        }
        opts.model_name = args.model;
    } else {
        // Codex: spawned agents inherit the parent's model and reasoning effort.
        opts.model_name = self->sm->current_model_preset();
    }
    opts.reasoning_effort = args.reasoning_effort
        ? args.reasoning_effort
        : (args.model.empty() ? self->sm->current_reasoning_effort() : std::nullopt);
    if (auto expert_error = resolve_expert(deps_.experts, self->workspace_cwd, *self->sm, args, opts);
        !expert_error.empty()) {
        result.error = expert_error;
        return result;
    }

    // Phase 1: claim the path so a concurrent spawn of the same name fails.
    {
        std::lock_guard<std::mutex> lock(mu_);
        Tree& tree = tree_locked(*self);
        if (tree.agents.count(child_path->str()) != 0) {
            result.error = "agent path `" + child_path->str() + "` already exists";
            return result;
        }
        Record& record = tree.agents[child_path->str()];
        record.path = child_path->str();
        record.restoring = true;
    }
    auto abandon = [this, &self, &child_path] {
        {
            std::lock_guard<std::mutex> lock(mu_);
            tree_locked(*self).agents.erase(child_path->str());
        }
        cv_.notify_all();
    };
    if (auto slot_error = reserve_slot(self->root_id, self->session_id); !slot_error.empty()) {
        abandon();
        result.error = slot_error;
        return result;
    }

    // Phase 2: create the child outside the service lock. Like spawn_subagent,
    // a child shares the caller's worktree and write boundary and runs in the
    // caller's workspace scope so the whole tree shares one project dir.
    const WorktreeSessionInfo worktree = self->sm->active_worktree();
    opts.cwd = self->workspace_cwd;
    opts.no_workspace = self->no_workspace;
    opts.reuse_no_workspace_cwd = self->no_workspace;
    if (worktree.active()) opts.inherited_worktree = worktree;
    opts.write_root = ctx.write_root;
    opts.permission_mode = self->sm->current_permission_mode();
    opts.subagent_depth = 1;
    opts.parent_session_id = self->root_id;
    opts.swarm_mode = "mesh";
    opts.agent_path = child_path->str();
    if (auto caller_entry = deps_.registry->acquire(self->session_id);
        caller_entry && caller_entry->loop_execution) {
        opts.loop_execution = true;
        opts.loop_id = caller_entry->loop_id;
        opts.loop_run_id = caller_entry->loop_run_id;
        if (caller_entry->loop) {
            opts.loop_system_context = caller_entry->loop->loop_execution_policy().system_context;
        }
    }
    std::string child_id;
    try {
        child_id = deps_.registry->create(opts);
    } catch (const std::exception& e) {
        release_slot(self->root_id);
        abandon();
        result.error = std::string("collab spawn failed: ") + e.what();
        return result;
    }
    auto child = deps_.registry->acquire(child_id);
    if (!child || !child->loop || !child->sm) {
        release_slot(self->root_id);
        abandon();
        result.error = "collab spawn failed: child session unavailable";
        return result;
    }
    // fork_turns: inherit the caller's conversation (Codex keep_forked_rollout_item).
    if (args.fork.kind != ForkTurns::Kind::None) {
        const auto history = build_fork_history(
            reconstruct_effective_model_history(self->sm->load_active_messages()), args.fork);
        for (const auto& message : history) {
            child->loop->push_message(message);
            child->sm->on_message(message);
        }
    }
    // Codex shows a child by its task name; no hidden title-model call.
    if (child->sm->try_set_generated_session_title(args.task_name)) {
        child->loop->events().emit(SessionEventKind::SessionUpdated,
            nlohmann::json{{"title", args.task_name}, {"title_source", "generated"}});
    }

    // Phase 3: publish the record, start event routing, then deliver the task.
    {
        std::lock_guard<std::mutex> lock(mu_);
        Tree& tree = tree_locked(*self);
        Record& record = tree.agents[child_path->str()];
        record.session_id = child_id;
        record.restoring = false;
        persist_index_locked(tree);
    }
    cv_.notify_all();
    subscribe_child(self->root_id, child_id);
    commit_resident(self->root_id, child_id);
    notify_agent_loaded(child_id, self->root_id);
    {
        std::lock_guard<std::mutex> lock(mu_);
        Tree& tree = tree_locked(*self);
        Record& record = tree.agents[child_path->str()];
        deliver_envelope_locked(tree, record,
            make_envelope(InterAgentMessageType::NewTask, child_path->str(), self->path,
                          self->session_id, args.message),
            true);
    }
    LOG_INFO("[mesh] spawned " + child_path->str() + " (" + child_id + ") from " + self->path);
    result.session_id = child_id;
    result.path = child_path->str();
    return result;
}

} // namespace acecode::mesh
