#include "side_question_service.hpp"
#include "agent/turn/user_turn_message.hpp"
#include "utils/text.hpp"

#include <utility>

namespace acecode::agent {

struct SideQuestionService::State {
    explicit State(ProviderAccessor accessor) : provider(std::move(accessor)) {}
    ProviderAccessor provider;
    std::mutex context_mu;
    std::vector<ChatMessage> context;
    std::atomic<bool> stopped{false};
};

SideQuestionService::SideQuestionService(ProviderAccessor provider)
    : state_(std::make_shared<State>(std::move(provider))) {}

SideQuestionService::~SideQuestionService() {
    stop_requests();
    join();
}

void SideQuestionService::stop_requests() {
    state_->stopped.store(true);
}

void SideQuestionService::join() {
    threads_.shutdown();
}

void SideQuestionService::publish(const std::vector<ChatMessage>& messages) {
    std::lock_guard<std::mutex> lock(state_->context_mu);
    state_->context = messages;
}

std::vector<ChatMessage> SideQuestionService::snapshot() const {
    std::lock_guard<std::mutex> lock(state_->context_mu);
    return state_->context;
}

SideQuestionResult SideQuestionService::ask(const std::string& question) {
    return ask(state_, question);
}

using detail::build_side_question_message;
using utils::trim_ascii_copy;

SideQuestionResult SideQuestionService::ask(
    const std::shared_ptr<State>& state, const std::string& raw_question) {
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

    std::vector<ChatMessage> context;
    {
        std::lock_guard<std::mutex> lock(state->context_mu);
        context = state->context;
    }
    if (context.empty()) {
        result.status = SideQuestionStatus::ContextNotReady;
        result.error = "side-question context not ready";
        return result;
    }

    std::shared_ptr<LlmProvider> provider;
    if (state->provider) provider = state->provider();
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

SideChatResult SideQuestionService::stream(
    const std::string& question,
    const std::vector<SideChatMessage>& history,
    SideChatCancellation& cancellation,
    const SideChatStreamCallback& callback) {
    auto provider = state_->provider ? state_->provider() : nullptr;
    return run_side_chat(std::move(provider), snapshot(), question, history,
                         cancellation, callback);
}

bool SideQuestionService::ask_async(std::string question, Callback callback) {
    if (state_->stopped.load()) return false;
    // State and the callback are owned by the request. The worker never captures
    // the facade or this service; joining cannot leave borrowed state behind.
    return threads_.spawn([state = state_, question = std::move(question),
                           callback = std::move(callback)]() mutable {
        auto result = ask(state, question);
        if (!state->stopped.load() && callback) callback(std::move(result));
    });
}

} // namespace acecode::agent
