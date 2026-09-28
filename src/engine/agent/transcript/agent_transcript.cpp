#include "agent/agent_loop.hpp"

#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/transcript/trajectory_recorder.hpp"
#include "agent/turn/turn_outcome.hpp"
#include "agent/worker/agent_task_queue.hpp"

#include <utility>

namespace acecode {

void AgentLoop::dispatch_message(const std::string& role, const std::string& content,
                                 bool is_tool, nlohmann::json metadata, nlohmann::json content_parts) {
    transcript_->dispatch_message(role, content, is_tool, std::move(metadata), std::move(content_parts));
}
void AgentLoop::append_turn_timing_record(const std::string& id, std::int64_t started,
                                         std::int64_t completed, const std::string& status) {
    transcript_->append_turn_timing_record(session_manager_, id, started, completed, status);
}
void AgentLoop::append_tool_user_prompt(const std::string& content,
                                       const std::string& display, const std::string& source) {
    transcript_->append_tool_user_prompt(session_manager_, content, display, source);
}
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
void AgentLoop::set_session_manager(SessionManager* session) {
    trajectory_.reset();
    session_manager_ = session;
    if (session) trajectory_ = std::make_unique<agent::TrajectoryRecorder>(events_, *history_, *session);
}
void AgentLoop::record_terminal_trajectory_events(nlohmann::json busy, nlohmann::json done) {
    if (trajectory_) trajectory_->record_terminal(std::move(busy), std::move(done));
}
bool AgentLoop::last_turn_failed() const { return turn_outcome_->failed(); }
std::string AgentLoop::last_turn_error() const { return turn_outcome_->error(); }
void AgentLoop::record_turn_outcome(const std::string& status) { turn_outcome_->record(status); }

void AgentLoop::clear_messages() {
    history_->clear_idle(task_queue_->on_worker_thread() || task_queue_->held_by_current_thread());
    last_api_total_tokens_.store(0, std::memory_order_relaxed);
    compact_window_initialized_ = false;
    compact_window_number_ = 0;
    compact_first_window_id_.clear();
    compact_current_window_id_.clear();
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
