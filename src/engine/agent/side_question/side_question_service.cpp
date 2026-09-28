#include "agent/agent_loop.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/request/provider_history.hpp"
#include "agent/turn/user_turn_message.hpp"
#include "llm/tool_protocol_names.hpp"
#include "permissions/shell_write_guard.hpp"
#include "session/session_client.hpp"
#include "session/session_storage.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/stream_processing.hpp"
#include "utils/text.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>
#include <thread>

namespace acecode {

using agent::detail::model_facing_provider_messages;
using agent::detail::build_side_question_message;
using utils::trim_ascii_copy;

void AgentLoop::join_side_question_threads() {
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(side_question_threads_mu_);
        threads.swap(side_question_threads_);
    }
    for (auto& thread : threads) {
        if (thread.joinable()) thread.join();
    }
}

void AgentLoop::publish_side_question_context(
    const std::vector<ChatMessage>& messages_with_system) {
    std::lock_guard<std::mutex> lk(side_question_context_mu_);
    side_question_context_ = messages_with_system;
}

std::vector<ChatMessage> AgentLoop::side_question_context_snapshot() const {
    std::lock_guard<std::mutex> lk(side_question_context_mu_);
    return side_question_context_;
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

SideQuestionResult AgentLoop::ask_side_question(
    const std::string& raw_question) {
    SideQuestionResult result;
    result.question = trim_ascii_copy(raw_question);
    if (result.question.empty() ||
        result.question.size() > kMaxSideQuestionBytes) {
        result.status = SideQuestionStatus::InvalidQuestion;
        result.error = result.question.empty()
            ? "question required"
            : "question too long";
        return result;
    }

    auto context = side_question_context_snapshot();
    if (context.empty()) {
        result.status = SideQuestionStatus::ContextNotReady;
        result.error = "side-question context not ready";
        return result;
    }

    std::shared_ptr<LlmProvider> provider;
    if (provider_accessor_) provider = provider_accessor_();
    if (!provider) {
        result.status = SideQuestionStatus::ProviderUnavailable;
        result.error = "session provider unavailable";
        return result;
    }

    context.push_back(build_side_question_message(result.question));
    try {
        ChatResponse response = provider->chat(context, {});
        result.answer = trim_ascii_copy(response.content);
        if (response.finish_reason == "error") {
            result.status = SideQuestionStatus::Failed;
            result.error = result.answer.empty()
                ? "side-question provider call failed"
                : result.answer;
            result.answer.clear();
            return result;
        }
        if (response.has_tool_calls()) {
            result.status = SideQuestionStatus::Failed;
            result.error = "side-question response requested tools";
            result.answer.clear();
            return result;
        }
        if (result.answer.empty()) {
            result.status = SideQuestionStatus::Failed;
            result.error = "side-question response was empty";
            return result;
        }
    } catch (const std::exception& e) {
        result.status = SideQuestionStatus::Failed;
        result.error = e.what();
        return result;
    } catch (...) {
        result.status = SideQuestionStatus::Failed;
        result.error = "side-question provider call failed";
        return result;
    }

    result.status = SideQuestionStatus::Ok;
    return result;
}

SideChatResult AgentLoop::stream_side_chat(
    const std::string& question,
    const std::vector<SideChatMessage>& history,
    SideChatCancellation& cancellation,
    const SideChatStreamCallback& callback) {
    auto provider = provider_accessor_ ? provider_accessor_() : nullptr;
    return run_side_chat(std::move(provider), side_question_context_snapshot(),
                         question, history, cancellation, callback);
}

bool AgentLoop::ask_side_question_async(
    std::string question,
    SideQuestionCallback callback) {
    std::lock_guard<std::mutex> lk(side_question_threads_mu_);
    if (side_question_shutdown_.load()) return false;
    side_question_threads_.emplace_back(
        [this, question = std::move(question),
         callback = std::move(callback)]() mutable {
            auto result = ask_side_question(question);
            if (!side_question_shutdown_.load() && callback) {
                callback(std::move(result));
            }
        });
    return true;
}

} // namespace acecode
