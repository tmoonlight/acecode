#include "agent/agent_loop.hpp"
#include "agent/compaction/compaction_controller.hpp"

#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/transcript/trajectory_recorder.hpp"
#include "agent/turn/turn_outcome.hpp"
#include "agent/worker/agent_task_queue.hpp"

#include <utility>

namespace acecode {

void AgentLoop::emit_system_message(const std::string& content, nlohmann::json metadata) {
    transcript_->emit_system_message(content, std::move(metadata));
}
void AgentLoop::emit_transcript_system_message(const std::string& content, nlohmann::json metadata) {
    transcript_->emit_transcript_system_message(session_manager_, content, std::move(metadata));
}
void AgentLoop::inject_shell_turn(const std::string& command, const std::string& out,
                                 const std::string& err, int exit_code) {
    transcript_->inject_shell_turn(command, out, err, exit_code);
}
bool AgentLoop::last_turn_failed() const { return turn_outcome_->failed(); }
std::string AgentLoop::last_turn_error() const { return turn_outcome_->error(); }

void AgentLoop::clear_messages() {
    history_->clear_idle(task_queue_->on_worker_thread() || task_queue_->held_by_current_thread());
    last_api_total_tokens_.store(0, std::memory_order_relaxed);
    compaction_->reset_window();
}
void AgentLoop::push_message(const ChatMessage& message) {
    history_->restore(message, task_queue_->on_worker_thread() || task_queue_->held_by_current_thread());
}
const std::vector<ChatMessage>& AgentLoop::messages() const { return history_->view(); }
void AgentLoop::history_on_worker(
    const std::function<void(agent::ConversationHistory&)>& operation) {
    history_->on_worker(operation, task_queue_->on_worker_thread() || task_queue_->held_by_current_thread());
}

} // namespace acecode
