#include "agent/agent_loop.hpp"
#include "side_question_service.hpp"
#include "agent/request/provider_history.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "utils/logger.hpp"

namespace acecode {

using agent::detail::model_facing_provider_messages;

void AgentLoop::publish_side_question_context(const std::vector<ChatMessage>& messages) {
    side_questions_->publish(messages);
}
std::vector<ChatMessage> AgentLoop::side_question_context_snapshot() const {
    return side_questions_->snapshot();
}
void AgentLoop::prime_side_question_context() {
    if (is_busy()) {
        LOG_WARN("Skipped side-question context priming while the loop is busy");
        return;
    }

    auto context = build_compaction_initial_context();
    auto history = model_facing_provider_messages(history_->view(), "side-question-prime");
    context.insert(context.end(), history.begin(), history.end());
    publish_side_question_context(context);
}

SideQuestionResult AgentLoop::ask_side_question(const std::string& question) {
    return side_questions_->ask(question);
}
bool AgentLoop::ask_side_question_async(std::string question, SideQuestionCallback callback) {
    return side_questions_->ask_async(std::move(question), std::move(callback));
}
SideChatResult AgentLoop::stream_side_chat(
    const std::string& question, const std::vector<SideChatMessage>& history,
    SideChatCancellation& cancellation, const SideChatStreamCallback& callback) {
    return side_questions_->stream(question, history, cancellation, callback);
}

} // namespace acecode
