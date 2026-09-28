#include "side_chat.hpp"

#include <algorithm>
#include <cctype>

namespace acecode {
namespace {

std::string trim_side_text(const std::string& text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    return first < last ? std::string(first, last) : std::string{};
}

} // namespace

void SideChatCancellation::cancel() {
    aborted.store(true);
    std::shared_ptr<LlmProvider> provider;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        provider = provider_.lock();
    }
    if (provider) provider->notify_cancelled_request();
}

void SideChatCancellation::bind_provider(const std::shared_ptr<LlmProvider>& provider) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        provider_ = provider;
    }
    if (aborted.load() && provider) provider->notify_cancelled_request();
}

std::string validate_side_chat_history(const std::vector<SideChatMessage>& history) {
    if (history.size() > kMaxSideChatHistoryMessages || history.size() % 2 != 0) {
        return "history must contain at most 100 complete user/assistant turns";
    }
    std::size_t bytes = 0;
    for (std::size_t index = 0; index < history.size(); ++index) {
        const auto& message = history[index];
        if (message.role != (index % 2 == 0 ? "user" : "assistant")) {
            return "history must alternate user and assistant messages";
        }
        if (trim_side_text(message.content).empty()) return "history messages must not be empty";
        if (message.content.size() > kMaxSideChatHistoryBytes - bytes) {
            return "history exceeds 256 KiB";
        }
        bytes += message.content.size();
    }
    return {};
}

SideChatResult run_side_chat(
    std::shared_ptr<LlmProvider> provider,
    std::vector<ChatMessage> context,
    const std::string& question,
    const std::vector<SideChatMessage>& history,
    SideChatCancellation& cancellation,
    const SideChatStreamCallback& callback) {
    SideChatResult result;
    auto& response = result.response;
    response.question = trim_side_text(question);
    response.error = validate_side_chat_history(history);
    if (response.error.empty() && (response.question.empty() ||
        response.question.size() > kMaxSideQuestionBytes)) {
        response.error = response.question.empty() ? "question required" : "question too long";
    }
    if (!response.error.empty()) {
        response.status = SideQuestionStatus::InvalidQuestion;
        return result;
    }
    if (context.empty()) {
        response.status = SideQuestionStatus::ContextNotReady;
        response.error = "side-question context not ready";
        return result;
    }
    if (!provider) {
        response.status = SideQuestionStatus::ProviderUnavailable;
        response.error = "session provider unavailable";
        return result;
    }
    if (!provider->supports_tool_free_chat()) {
        response.status = SideQuestionStatus::ProviderUnavailable;
        response.error = "current model does not support read-only side chat; select another model";
        result.code = "SIDE_CHAT_PROVIDER_UNSUPPORTED";
        return result;
    }
    cancellation.bind_provider(provider);
    if (cancellation.aborted.load()) {
        response.status = SideQuestionStatus::Ok;
        result.cancelled = true;
        return result;
    }

    ChatMessage instruction;
    instruction.role = "system";
    instruction.content =
        "The following messages are a separate, read-only side conversation "
        "about the main conversation above. Answer the latest side question "
        "using both contexts. Do not call tools, continue the main task, or "
        "claim that you changed files or session state. Answer directly.";
    context.push_back(std::move(instruction));
    for (const auto& previous : history) {
        ChatMessage message;
        message.role = previous.role;
        message.content = previous.content;
        context.push_back(std::move(message));
    }
    ChatMessage message;
    message.role = "user";
    message.content = response.question;
    context.push_back(std::move(message));

    bool forbidden_tools = false;
    bool completed = false;
    try {
        provider->chat_stream(context, {}, [&](const StreamEvent& event) {
            if (cancellation.aborted.load()) return;
            switch (event.type) {
            case StreamEventType::Delta:
                response.answer += event.content;
                if (callback && !event.content.empty()) callback(event.content, false);
                break;
            case StreamEventType::Retry:
                response.answer.clear();
                response.error.clear();
                completed = false;
                if (callback) callback({}, true);
                break;
            case StreamEventType::ToolCall:
            case StreamEventType::ToolCallDelta:
                forbidden_tools = true;
                response.error = "side-question response requested tools";
                cancellation.cancel();
                break;
            case StreamEventType::Error:
                response.error = event.error.empty() ? "side-question provider call failed" : event.error;
                break;
            case StreamEventType::Done:
                completed = true;
                if (event.finish_reason == "tool_calls") {
                    forbidden_tools = true;
                    response.error = "side-question response requested tools";
                } else if (event.finish_reason == "error" && response.error.empty()) {
                    response.error = "side-question provider call failed";
                }
                break;
            default:
                break;
            }
        }, &cancellation.aborted);
    } catch (const std::exception& error) {
        response.error = error.what();
    } catch (...) {
        response.error = "side-question provider call failed";
    }
    result.cancelled = cancellation.aborted.load() && !forbidden_tools;
    if (result.cancelled) {
        response.error.clear();
    } else if (response.error.empty()) {
        if (!completed) response.error = "side-question stream ended before completion";
        else if (trim_side_text(response.answer).empty()) response.error = "side-question response was empty";
    }
    response.status = response.error.empty() ? SideQuestionStatus::Ok : SideQuestionStatus::Failed;
    return result;
}

} // namespace acecode
