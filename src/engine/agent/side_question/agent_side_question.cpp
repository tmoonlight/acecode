#include "agent/agent_loop.hpp"
#include "side_question_service.hpp"
#include "side_chat_tools.hpp"
#include "agent/request/api_request_builder.hpp"
#include "agent/request/provider_history.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "session/request_context_record.hpp"
#include "utils/logger.hpp"

#include <algorithm>

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

    if (std::any_of(history_->view().begin(), history_->view().end(),
                    is_request_context_snapshot)) {
        const auto provider = provider_accessor_ ? provider_accessor_() : nullptr;
        auto request = request_builder_->compaction_request(
            request_context_options(provider), history_->view());
        // Reuse the stored window and skill index. Side requests still obtain
        // their own restricted toolset through side_chat_toolset().
        publish_side_question_context(request.messages_with_system);
        return;
    }
    auto context = build_compaction_initial_context();
    auto history = model_facing_provider_messages(history_->view(), "side-question-prime");
    context.insert(context.end(), history.begin(), history.end());
    publish_side_question_context(context);
}

// The toolset borrows tools_/permissions_/boundary_; shutdown() joins every
// side request before any of them can go away.
SideChatToolset AgentLoop::side_chat_toolset() {
    return agent::build_side_chat_toolset(
        tools_, permissions_, *boundary_, session_manager_, tool_capability_policy());
}
SideQuestionResult AgentLoop::ask_side_question(const std::string& question) {
    return side_questions_->ask(question, side_chat_toolset());
}
bool AgentLoop::ask_side_question_async(std::string question, SideQuestionCallback callback) {
    return side_questions_->ask_async(std::move(question), std::move(callback), side_chat_toolset());
}
SideChatResult AgentLoop::stream_side_chat(
    const std::string& question, const std::vector<SideChatMessage>& history,
    SideChatCancellation& cancellation, const SideChatStreamCallback& callback,
    const SideChatToolCallback& on_tool) {
    return side_questions_->stream(question, history, cancellation, callback,
                                   side_chat_toolset(), on_tool);
}

} // namespace acecode
