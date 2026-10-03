#include "mesh_agent_state.hpp"

#include "agent/agent_loop.hpp"
#include "config/config.hpp"
#include "session/mesh_tree_index.hpp"
#include "session/session_manager.hpp"
#include "session_host/session_registry.hpp"
#include "tool/tool_executor.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <vector>

namespace acecode::mesh {

nlohmann::json agent_status_to_json(const AgentStatus& status) {
    switch (status.kind) {
    case AgentStatus::Kind::PendingInit: return "pending_init";
    case AgentStatus::Kind::Running: return "running";
    case AgentStatus::Kind::Interrupted: return "interrupted";
    case AgentStatus::Kind::Shutdown: return "shutdown";
    case AgentStatus::Kind::Completed:
        return nlohmann::json{{"completed", status.completed
            ? nlohmann::json(*status.completed) : nlohmann::json(nullptr)}};
    case AgentStatus::Kind::Errored:
        return nlohmann::json{{"errored", status.errored}};
    case AgentStatus::Kind::NotFound: break;
    }
    return "not_found";
}

std::string agent_status_label(const AgentStatus& status) {
    switch (status.kind) {
    case AgentStatus::Kind::PendingInit: return "pending_init";
    case AgentStatus::Kind::Running: return "running";
    case AgentStatus::Kind::Interrupted: return "interrupted";
    case AgentStatus::Kind::Completed: return "completed";
    case AgentStatus::Kind::Errored: return "errored";
    case AgentStatus::Kind::Shutdown: return "shutdown";
    case AgentStatus::Kind::NotFound: break;
    }
    return "not_found";
}

MeshAgentService::MeshAgentService(Deps deps)
    : deps_(std::move(deps)), client_(*deps_.registry) {}

MeshAgentService::~MeshAgentService() {
    shutdown();
}

void MeshAgentService::attach() {
    const std::weak_ptr<MeshAgentService> weak = weak_from_this();
    deps_.registry->set_swarm_mode_guard(
        [weak](const std::string& session_id, SwarmMode from, SwarmMode to) -> std::string {
            if (from != SwarmMode::Mesh || to == SwarmMode::Mesh) return {};
            auto self = weak.lock();
            return self ? self->tree_busy_reason(session_id) : std::string{};
        });
}

void MeshAgentService::set_on_agent_loaded(AgentLoadedCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mu_);
    on_agent_loaded_ = std::move(callback);
}

void MeshAgentService::notify_agent_loaded(const std::string& child_id,
                                           const std::string& root_id) {
    AgentLoadedCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mu_);
        callback = on_agent_loaded_;
    }
    if (callback) callback(child_id, root_id);
}

MeshSwarmConfig MeshAgentService::config_snapshot() const {
    if (!deps_.config) return MeshSwarmConfig{};
    std::shared_lock<std::shared_mutex> lock;
    if (deps_.config_mutex) lock = std::shared_lock<std::shared_mutex>(*deps_.config_mutex);
    return deps_.config->swarm.mesh;
}

std::vector<std::string> MeshAgentService::saved_model_names() const {
    std::vector<std::string> names;
    if (!deps_.config) return names;
    std::shared_lock<std::shared_mutex> lock;
    if (deps_.config_mutex) lock = std::shared_lock<std::shared_mutex>(*deps_.config_mutex);
    for (const auto& profile : deps_.config->saved_models) names.push_back(profile.name);
    return names;
}

void MeshAgentService::shutdown() {
    if (stopped_.exchange(true)) return;
    if (deps_.registry) deps_.registry->set_swarm_mode_guard({});
    set_on_agent_loaded({});
    std::vector<ScopedSubscription> subscriptions;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [root, tree] : trees_) {
            for (auto& [path, record] : tree->agents) {
                subscriptions.push_back(std::move(record.subscription));
            }
        }
    }
    // Waiting for in-flight listeners under mu_ would deadlock them.
    subscriptions.clear();
    cv_.notify_all();
}

std::optional<MeshAgentService::Caller> MeshAgentService::caller(const ToolContext& ctx,
                                                                 std::string* error) const {
    SessionManager* sm = ctx.session_manager;
    if (!sm) {
        if (error) *error = "collab manager unavailable";
        return std::nullopt;
    }
    if (sm->current_swarm_mode() != "mesh") {
        if (error) *error = "agent collaboration tools are only available in mesh swarm mode.";
        return std::nullopt;
    }
    Caller caller;
    caller.session_id = sm->current_session_id();
    caller.sm = sm;
    caller.ctx = &ctx;
    caller.project_dir = sm->current_project_dir();
    caller.workspace_cwd = sm->current_cwd();
    caller.no_workspace = sm->is_no_workspace();
    const std::string path = sm->current_agent_path();
    if (path.empty()) {
        caller.path = AgentPath::kRoot;
        caller.root_id = caller.session_id;
    } else {
        std::string parse_error;
        if (!AgentPath::parse(path, &parse_error)) {
            if (error) *error = parse_error;
            return std::nullopt;
        }
        caller.path = path;
        caller.root_id = sm->current_parent_session_id();
    }
    if (caller.session_id.empty() || caller.root_id.empty()) {
        if (error) *error = "collab manager unavailable";
        return std::nullopt;
    }
    return caller;
}

MeshAgentService::Tree& MeshAgentService::tree_locked(const Caller& caller) {
    auto& slot = trees_[caller.root_id];
    if (slot) return *slot;
    slot = std::make_unique<Tree>();
    slot->root_id = caller.root_id;
    slot->project_dir = caller.project_dir;
    slot->workspace_cwd = caller.workspace_cwd;
    slot->no_workspace = caller.no_workspace;
    // daemon 重启后按索引重建目录;已在内存里的(TUI 恢复等)标记为驻留。
    for (const auto& entry : read_mesh_tree_index(caller.project_dir, caller.root_id)) {
        Record& record = slot->agents[entry.path];
        record.session_id = entry.session_id;
        record.path = entry.path;
        record.resident = deps_.registry->acquire(entry.session_id) != nullptr;
    }
    return *slot;
}

void MeshAgentService::persist_index_locked(const Tree& tree) const {
    std::vector<MeshTreeIndexEntry> entries;
    for (const auto& [path, record] : tree.agents) {
        if (!record.session_id.empty()) entries.push_back({path, record.session_id});
    }
    if (!write_mesh_tree_index(tree.project_dir, tree.root_id, entries)) {
        LOG_WARN("[mesh] failed to persist agent tree index for root " + tree.root_id);
    }
}

std::string MeshAgentService::resolve_locked(Tree& tree, const Caller& caller,
                                             const std::string& target, Record** out) const {
    *out = nullptr;
    if (target == tree.root_id) return {};
    for (auto& [path, record] : tree.agents) {
        if (!record.session_id.empty() && record.session_id == target) {
            *out = &record;
            return {};
        }
    }
    const auto self = AgentPath::parse(caller.path);
    std::string error;
    const auto resolved = self ? self->resolve(target, &error) : std::nullopt;
    if (!resolved) return error.empty() ? std::string("agent path must not be empty") : error;
    if (resolved->is_root()) return {};
    const auto it = tree.agents.find(resolved->str());
    if (it == tree.agents.end() || it->second.session_id.empty()) {
        return "live agent path `" + resolved->str() + "` not found";
    }
    *out = &it->second;
    return {};
}

AgentLoop* MeshAgentService::loop_for(const std::string& session_id,
                                      std::shared_ptr<SessionEntry>& keep_alive) const {
    if (deps_.external_root_id && deps_.external_root_loop &&
        deps_.external_root_id() == session_id) {
        return deps_.external_root_loop();
    }
    keep_alive = deps_.registry->acquire(session_id);
    return keep_alive && keep_alive->loop ? keep_alive->loop.get() : nullptr;
}

AgentStatus MeshAgentService::status_locked(const Tree&, const Record& record) const {
    AgentStatus status;
    if (!record.resident || record.evicting) return status;
    std::shared_ptr<SessionEntry> keep;
    AgentLoop* loop = loop_for(record.session_id, keep);
    if (!loop) return status;
    if (loop->is_busy()) {
        status.kind = AgentStatus::Kind::Running;
    } else if (!record.started) {
        status.kind = AgentStatus::Kind::PendingInit;
    } else if (record.last_outcome == "error") {
        status.kind = AgentStatus::Kind::Errored;
        status.errored = record.error_text;
    } else if (record.last_outcome == "aborted") {
        status.kind = AgentStatus::Kind::Interrupted;
    } else if (record.last_outcome == "completed") {
        status.kind = AgentStatus::Kind::Completed;
        status.completed = record.final_text;
    } else {
        status.kind = AgentStatus::Kind::Running;
    }
    return status;
}

void MeshAgentService::touch_locked(Tree& tree, const std::string& session_id) {
    ++activity_clock_;
    for (auto& [path, record] : tree.agents) {
        if (record.session_id == session_id) record.last_activity = activity_clock_;
    }
}

std::string MeshAgentService::reserve_slot(const std::string& root_id,
                                           const std::string& protected_id) {
    while (!stopped_.load()) {
        std::string victim_id;
        ScopedSubscription victim_subscription;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto tree_it = trees_.find(root_id);
            if (tree_it == trees_.end()) return "collab manager unavailable";
            Tree& tree = *tree_it->second;
            const MeshSwarmConfig config = config_snapshot();
            // Codex effective_agent_max_threads(V2): the root holds one slot.
            const std::size_t capacity =
                static_cast<std::size_t>(std::max(1, config.max_concurrent_agents - 1));
            std::size_t residents = tree.pending_slots;
            for (const auto& [path, record] : tree.agents) {
                if (record.resident || record.evicting) ++residents;
            }
            if (residents < capacity) {
                ++tree.pending_slots;
                return {};
            }
            // Codex is_unloadable: finished turn, no active work, empty mailbox.
            Record* victim = nullptr;
            bool dropped_stale = false;
            for (auto& [path, record] : tree.agents) {
                if (!record.resident || record.evicting || record.restoring) continue;
                std::shared_ptr<SessionEntry> keep;
                AgentLoop* loop = loop_for(record.session_id, keep);
                if (!loop) {
                    // Destroyed elsewhere (user purge, shutdown): free its slot.
                    record.resident = false;
                    dropped_stale = true;
                    continue;
                }
                if (record.session_id == protected_id || !record.held_mail.empty()) continue;
                if (!record.started || record.last_outcome.empty()) continue;
                if (loop->has_pending_work() || loop->pending_mailbox_count() > 0) continue;
                if (!victim || record.last_activity < victim->last_activity) victim = &record;
            }
            if (dropped_stale) continue;
            if (!victim) return kAgentLimitReached;
            victim->evicting = true;
            victim_id = victim->session_id;
            victim_subscription = std::move(victim->subscription);
        }
        victim_subscription.reset();
        LOG_INFO("[mesh] evicting idle agent " + victim_id + " from tree " + root_id);
        deps_.registry->destroy(victim_id);
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto tree_it = trees_.find(root_id);
            if (tree_it != trees_.end()) {
                for (auto& [path, record] : tree_it->second->agents) {
                    if (record.session_id != victim_id) continue;
                    record.evicting = false;
                    record.resident = false;
                }
            }
        }
        cv_.notify_all();
    }
    return "collab manager unavailable";
}

void MeshAgentService::release_slot(const std::string& root_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = trees_.find(root_id);
    if (it != trees_.end() && it->second->pending_slots > 0) --it->second->pending_slots;
}

void MeshAgentService::commit_resident(const std::string& root_id,
                                       const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = trees_.find(root_id);
    if (it == trees_.end()) return;
    Tree& tree = *it->second;
    if (tree.pending_slots > 0) --tree.pending_slots;
    for (auto& [path, record] : tree.agents) {
        if (record.session_id == session_id) record.resident = true;
    }
    touch_locked(tree, session_id);
}

UserInput MeshAgentService::make_envelope(InterAgentMessageType type, const std::string& recipient,
                                          const std::string& sender,
                                          const std::string& sender_session,
                                          const std::string& payload,
                                          const std::string& status) const {
    InterAgentEnvelope envelope;
    envelope.type = type;
    envelope.recipient = recipient;
    envelope.sender = sender;
    envelope.sender_session_id = sender_session;
    envelope.payload = payload;
    envelope.final_status = status;
    UserInput input;
    input.text = render_inter_agent_message(envelope);
    input.metadata = nlohmann::json::object();
    input.metadata[kInterAgentMetadataKey] = inter_agent_metadata(envelope);
    return input;
}

bool MeshAgentService::deliver_envelope_locked(Tree& tree, Record& recipient, UserInput envelope,
                                               bool trigger_turn) {
    if (!recipient.resident || recipient.evicting) {
        recipient.held_mail.emplace_back(std::move(envelope), trigger_turn);
        return false;
    }
    std::shared_ptr<SessionEntry> keep;
    AgentLoop* loop = loop_for(recipient.session_id, keep);
    if (!loop) {
        recipient.resident = false;
        recipient.held_mail.emplace_back(std::move(envelope), trigger_turn);
        return false;
    }
    loop->deliver_inter_agent_message(std::move(envelope), trigger_turn);
    touch_locked(tree, recipient.session_id);
    return true;
}

std::string MeshAgentService::ensure_loaded(const Caller& caller, const std::string& path) {
    std::string session_id;
    {
        std::unique_lock<std::mutex> lock(mu_);
        Tree& tree = tree_locked(caller);
        cv_.wait(lock, [&tree, &path, this] {
            const auto it = tree.agents.find(path);
            return stopped_.load() || it == tree.agents.end() ||
                   (!it->second.evicting && !it->second.restoring);
        });
        auto it = tree.agents.find(path);
        if (stopped_.load() || it == tree.agents.end()) return "collab manager unavailable";
        Record& record = it->second;
        if (record.resident && deps_.registry->acquire(record.session_id)) {
            touch_locked(tree, record.session_id);
            return {};
        }
        record.resident = false;
        record.restoring = true;
        session_id = record.session_id;
    }
    auto finish = [this, &caller, &path](bool loaded) {
        std::deque<std::pair<UserInput, bool>> held;
        {
            std::lock_guard<std::mutex> lock(mu_);
            Tree& tree = tree_locked(caller);
            auto it = tree.agents.find(path);
            if (it != tree.agents.end()) {
                it->second.restoring = false;
                if (loaded) held.swap(it->second.held_mail);
                for (auto& [input, trigger] : held) {
                    deliver_envelope_locked(tree, it->second, std::move(input), trigger);
                }
            }
        }
        cv_.notify_all();
    };
    if (auto error = reserve_slot(caller.root_id, caller.session_id); !error.empty()) {
        finish(false);
        return error == kAgentLimitReached ? "collab tool failed: agent thread limit reached" : error;
    }
    SessionOptions opts;
    opts.cwd = caller.workspace_cwd;
    opts.no_workspace = caller.no_workspace;
    opts.reuse_no_workspace_cwd = caller.no_workspace;
    if (caller.ctx) opts.write_root = caller.ctx->write_root;
    bool resumed = false;
    try {
        resumed = deps_.registry->resume(session_id, opts);
    } catch (const std::exception& e) {
        LOG_WARN(std::string("[mesh] restoring agent failed: ") + e.what());
    }
    if (!resumed) {
        release_slot(caller.root_id);
        finish(false);
        return "collab tool failed: agent `" + path + "` could not be restored";
    }
    subscribe_child(caller.root_id, session_id);
    commit_resident(caller.root_id, session_id);
    LOG_INFO("[mesh] restored agent " + path + " (" + session_id + ")");
    finish(true);
    notify_agent_loaded(session_id, caller.root_id);
    return {};
}

std::string MeshAgentService::deliver(const ToolContext& ctx, const std::string& target,
                                      const std::string& message, bool trigger_turn) {
    if (message.find_first_not_of(" \t\r\n") == std::string::npos) {
        return "Empty message can't be sent to an agent";
    }
    std::string error;
    const auto self = caller(ctx, &error);
    if (!self) return error;
    std::string target_path;
    {
        std::lock_guard<std::mutex> lock(mu_);
        Tree& tree = tree_locked(*self);
        Record* record = nullptr;
        if (auto resolve_error = resolve_locked(tree, *self, target, &record);
            !resolve_error.empty()) {
            return resolve_error;
        }
        if (!record) {
            if (trigger_turn) return "Follow-up tasks can't target the root agent";
            std::shared_ptr<SessionEntry> keep;
            AgentLoop* root = loop_for(tree.root_id, keep);
            if (!root) return "collab tool failed: the root agent is not loaded";
            root->deliver_inter_agent_message(
                make_envelope(trigger_turn ? InterAgentMessageType::NewTask
                                           : InterAgentMessageType::Message,
                              AgentPath::kRoot, self->path, self->session_id, message),
                false);
            return {};
        }
        target_path = record->path;
    }
    if (auto load_error = ensure_loaded(*self, target_path); !load_error.empty()) {
        return load_error;
    }
    std::lock_guard<std::mutex> lock(mu_);
    Tree& tree = tree_locked(*self);
    auto it = tree.agents.find(target_path);
    if (it == tree.agents.end()) return "live agent path `" + target_path + "` not found";
    deliver_envelope_locked(tree, it->second,
        make_envelope(trigger_turn ? InterAgentMessageType::NewTask : InterAgentMessageType::Message,
                      target_path, self->path, self->session_id, message),
        trigger_turn);
    return {};
}

std::string MeshAgentService::list(const ToolContext& ctx, const std::string& path_prefix,
                                   std::vector<ListedAgent>& out) {
    out.clear();
    std::string error;
    const auto self = caller(ctx, &error);
    if (!self) return error;
    std::optional<AgentPath> prefix;
    if (!path_prefix.empty()) {
        const auto base = AgentPath::parse(self->path);
        prefix = base ? base->resolve(path_prefix, &error) : std::nullopt;
        if (!prefix) return error;
    }
    std::lock_guard<std::mutex> lock(mu_);
    Tree& tree = tree_locked(*self);
    std::shared_ptr<SessionEntry> keep;
    if (AgentLoop* root = loop_for(tree.root_id, keep);
        root && (!prefix || AgentPath::root().has_prefix(prefix->str()))) {
        ListedAgent agent;
        agent.path = AgentPath::kRoot;
        agent.session_id = tree.root_id;
        agent.status.kind = root->is_busy() ? AgentStatus::Kind::Running
                                             : AgentStatus::Kind::Completed;
        out.push_back(std::move(agent));
    }
    for (const auto& [path, record] : tree.agents) {
        if (record.session_id.empty() || !record.resident || record.evicting) continue;
        const auto parsed = AgentPath::parse(path);
        if (prefix && (!parsed || !parsed->has_prefix(prefix->str()))) continue;
        out.push_back({path, record.session_id, status_locked(tree, record)});
    }
    return {};
}

std::string MeshAgentService::interrupt(const ToolContext& ctx, const std::string& target,
                                        AgentStatus& previous) {
    previous = AgentStatus{};
    std::string error;
    const auto self = caller(ctx, &error);
    if (!self) return error;
    std::lock_guard<std::mutex> lock(mu_);
    Tree& tree = tree_locked(*self);
    Record* record = nullptr;
    if (auto resolve_error = resolve_locked(tree, *self, target, &record); !resolve_error.empty()) {
        return resolve_error;
    }
    if (!record) return "root is not a spawned agent";
    if (record->session_id == self->session_id) {
        return "an agent cannot interrupt itself; return your result and let the parent interrupt "
               "you if needed";
    }
    previous = status_locked(tree, *record);
    if (record->resident && !record->evicting) {
        std::shared_ptr<SessionEntry> keep;
        if (AgentLoop* loop = loop_for(record->session_id, keep)) loop->abort();
    }
    return {};
}

std::string MeshAgentService::wait(const ToolContext& ctx, std::chrono::milliseconds timeout,
                                   WaitResult& result) {
    std::string error;
    const auto self = caller(ctx, &error);
    if (!self) return error;
    std::shared_ptr<SessionEntry> keep;
    AgentLoop* loop = loop_for(self->session_id, keep);
    if (!loop) return "collab manager unavailable";
    switch (loop->wait_for_mailbox_activity(timeout, ctx.abort_flag)) {
    case AgentLoop::MailboxWaitOutcome::Mailbox: result = WaitResult::Mailbox; break;
    case AgentLoop::MailboxWaitOutcome::Steered: result = WaitResult::Steered; break;
    case AgentLoop::MailboxWaitOutcome::TimedOut: result = WaitResult::TimedOut; break;
    case AgentLoop::MailboxWaitOutcome::Aborted: result = WaitResult::Aborted; break;
    }
    return {};
}

std::string MeshAgentService::tree_busy_reason(const std::string& root_id) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = trees_.find(root_id);
    if (it == trees_.end()) return {};
    for (const auto& [path, record] : it->second->agents) {
        for (const auto& held : record.held_mail) {
            if (held.second) return "agent `" + path + "` has an undelivered follow-up task";
        }
        if (!record.resident || record.evicting) continue;
        std::shared_ptr<SessionEntry> keep;
        AgentLoop* loop = loop_for(record.session_id, keep);
        if (!loop) continue;
        if (loop->has_pending_work() || loop->has_pending_trigger_mail()) {
            return "agent `" + path + "` is still running; wait for it or interrupt it first";
        }
    }
    return {};
}

std::vector<TreeAgentInfo> MeshAgentService::tree_agents(const std::string& root_id) {
    std::vector<TreeAgentInfo> out;
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = trees_.find(root_id);
    if (it == trees_.end()) return out;
    for (const auto& [path, record] : it->second->agents) {
        if (record.session_id.empty()) continue;
        out.push_back({record.session_id, path, record.resident && !record.evicting,
                       status_locked(*it->second, record), record.held_mail.size()});
    }
    return out;
}

} // namespace acecode::mesh
