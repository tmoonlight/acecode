// MeshAgentService 的子 agent 事件:状态跟踪与完成回报(Codex
// notify_parent_of_terminal_turn)。监听器跑在子 agent 的 worker 线程上,只捕获
// weak_ptr 与值(C5);持有服务锁时只做叶子操作(投信入箱),从不 join。
#include "mesh_agent_state.hpp"

#include "agent/agent_loop.hpp"
#include "session_host/session_registry.hpp"
#include "utils/logger.hpp"
#include "utils/encoding.hpp"

namespace acecode::mesh {

namespace {

// The final answer of the turn that just ended: the last assistant reply
// without tool calls after the turn's opening input (a real user message or a
// NEW_TASK envelope). Mid-turn MESSAGE / FINAL_ANSWER envelopes do not open a
// turn. Empty when the turn ended without a text answer (Codex Completed(None)).
std::optional<std::string> turn_final_answer(const std::vector<ChatMessage>& history) {
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (it->role == "assistant") {
            const bool has_tool_calls = !it->tool_calls.is_null() && !it->tool_calls.empty();
            if (!has_tool_calls && !it->content.empty()) return it->content;
            continue;
        }
        if (it->role != "user") continue;
        const auto envelope = inter_agent_envelope_from_metadata(it->metadata);
        if (envelope && envelope->type != InterAgentMessageType::NewTask) continue;
        const bool internal = it->metadata.is_object() &&
                              (it->metadata.value("hidden_goal_context", false) ||
                               it->metadata.value("transcript_only", false));
        if (!internal) break;
    }
    return std::nullopt;
}

} // namespace

void MeshAgentService::subscribe_child(const std::string& root_id, const std::string& session_id) {
    const std::weak_ptr<MeshAgentService> weak = weak_from_this();
    const auto id = client_.subscribe(session_id,
        [weak, root_id, session_id](const SessionEvent& event) {
            if (auto self = weak.lock()) self->on_child_event(root_id, session_id, event);
        });
    if (id == 0) return;
    ScopedSubscription subscription(client_, session_id, id);
    ScopedSubscription previous;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto tree_it = trees_.find(root_id);
        if (tree_it != trees_.end() && !stopped_.load()) {
            for (auto& [path, record] : tree_it->second->agents) {
                if (record.session_id != session_id) continue;
                previous = std::move(record.subscription);
                record.subscription = std::move(subscription);
                break;
            }
        }
    }
    // previous / an unused subscription unsubscribe here, outside mu_.
}

void MeshAgentService::on_child_event(const std::string& root_id, const std::string& session_id,
                                      const SessionEvent& event) {
    if (stopped_.load()) return;
    if (event.kind == SessionEventKind::BusyChanged) {
        if (!event.payload.value("busy", false)) return;
        std::lock_guard<std::mutex> lock(mu_);
        auto tree_it = trees_.find(root_id);
        if (tree_it == trees_.end()) return;
        for (auto& [path, record] : tree_it->second->agents) {
            if (record.session_id != session_id) continue;
            record.started = true;
            record.last_outcome.clear();
        }
        touch_locked(*tree_it->second, session_id);
        return;
    }
    if (event.kind != SessionEventKind::Done) return;
    // Compaction and user shell tasks emit Done without a chat turn id.
    const std::string outcome = event.payload.value("outcome", std::string{});
    if (outcome.empty() || !event.payload.contains("turn_id")) return;
    route_completion(root_id, session_id, outcome);
}

void MeshAgentService::route_completion(const std::string& root_id,
                                        const std::string& session_id,
                                        const std::string& outcome) {
    std::shared_ptr<SessionEntry> keep;
    AgentLoop* child = loop_for(session_id, keep);
    std::optional<std::string> final_text;
    std::string error_text;
    // Done is emitted on the child's own worker, so its history is safe to read.
    if (child && outcome == "completed") final_text = turn_final_answer(child->messages());
    if (child && outcome == "error") error_text = child->last_turn_error();
    std::lock_guard<std::mutex> lock(mu_);
    auto tree_it = trees_.find(root_id);
    if (tree_it == trees_.end()) return;
    Tree& tree = *tree_it->second;
    Record* record = nullptr;
    for (auto& [path, candidate] : tree.agents) {
        if (candidate.session_id == session_id) record = &candidate;
    }
    if (!record) return;
    record->started = true;
    record->last_outcome = outcome;
    record->final_text = final_text;
    record->error_text = error_text;
    touch_locked(tree, session_id);
    // Codex format_inter_agent_completion_message: interrupted turns send nothing.
    std::string payload;
    if (outcome == "completed") {
        payload = final_text.value_or(std::string{});
    } else if (outcome == "error") {
        payload = "Agent errored: " + truncate_utf8_prefix(error_text, kCompletionErrorMaxChars) +
                  "\n\n" + kErrorNextAction;
    } else {
        return;
    }
    const auto child_path = AgentPath::parse(record->path);
    const auto parent_path = child_path ? child_path->parent() : std::nullopt;
    if (!parent_path) return;
    UserInput envelope = make_envelope(InterAgentMessageType::FinalAnswer, parent_path->str(),
                                       record->path, session_id, payload,
                                       outcome == "completed" ? "completed" : "errored");
    if (parent_path->is_root()) {
        std::shared_ptr<SessionEntry> root_keep;
        if (AgentLoop* root = loop_for(root_id, root_keep)) {
            root->deliver_inter_agent_message(std::move(envelope), false);
        } else {
            LOG_WARN("[mesh] root " + root_id + " is not loaded; dropped FINAL_ANSWER from " +
                     record->path);
        }
        return;
    }
    auto parent_it = tree.agents.find(parent_path->str());
    if (parent_it == tree.agents.end()) return;
    deliver_envelope_locked(tree, parent_it->second, std::move(envelope), false);
}

} // namespace acecode::mesh
