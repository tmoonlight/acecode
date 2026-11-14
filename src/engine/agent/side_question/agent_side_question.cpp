#include "agent/agent_loop.hpp"
#include "side_question_service.hpp"
#include "side_chat_tools.hpp"
#include "agent/request/api_request_builder.hpp"
#include "agent/request/provider_history.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/worker/agent_task_queue.hpp"
#include "session/request_context_record.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <chrono>

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

// 恢复会话时不准备这份上下文:带上下文快照的会话要为它走一遍完整的请求构建
// (连续启动 5~7 个 git 进程),放在恢复里会让每次打开会话都多等几百毫秒,而
// 多数会话打开后根本不会用到侧边对话。推迟到第一次侧问,由 worker 构建。
void AgentLoop::ensure_side_question_context() {
    using namespace std::chrono_literals;
    if (side_questions_->has_context()) return;
    if (!started_ || task_queue_->on_worker_thread() || task_queue_->held_by_current_thread()) {
        prime_side_question_context();
        return;
    }
    // History and the request builder belong to the worker. A turn queued ahead
    // of this control publishes its own request context first, which also ends
    // the wait; the control then finds the context present and does nothing.
    const auto receipt = enqueue_control([owner = lifetime_.ref(*this)] {
        owner.with([](AgentLoop& loop) {
            if (!loop.side_questions_->has_context()) loop.prime_side_question_context();
        });
        return true;
    });
    if (!receipt.accepted) return;
    // 上限只为不让请求线程无限期挂住:正常是 worker 构建完(或前面的回合发出
    // 第一个请求)就返回。git 每条都卡到超时也只有 7 条 × 默认 3 秒,在这个范围内。
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!side_questions_->has_context() && !receipt.wait_for_completion(20ms) &&
           !receipt.cancelled() && std::chrono::steady_clock::now() < deadline) {
    }
}

// The toolset borrows tools_/permissions_/boundary_; shutdown() joins every
// side request before any of them can go away.
SideChatToolset AgentLoop::side_chat_toolset(const SideChatMainSessionSender& send_to_main,
    const std::function<bool(const std::string&)>& extra_read_denied) {
    return agent::build_side_chat_toolset(
        tools_, permissions_, *boundary_, session_manager_, tool_capability_policy(),
        send_to_main, extra_read_denied);
}
SideQuestionResult AgentLoop::ask_side_question(const std::string& question) {
    ensure_side_question_context();
    return side_questions_->ask(question, side_chat_toolset());
}
bool AgentLoop::ask_side_question_async(std::string question, SideQuestionCallback callback) {
    return side_questions_->ask_async(std::move(question), std::move(callback), side_chat_toolset());
}
SideChatResult AgentLoop::stream_side_chat(
    const std::string& question, const std::vector<SideChatMessage>& history,
    SideChatCancellation& cancellation, const SideChatStreamCallback& callback,
    const SideChatToolCallback& on_tool, const SideChatMainSessionSender& send_to_main,
    const SideChatInputOptions& input) {
    ensure_side_question_context();
    return side_questions_->stream(question, history, cancellation, callback,
                                   side_chat_toolset(send_to_main, input.extra_read_denied), on_tool, input);
}

} // namespace acecode
