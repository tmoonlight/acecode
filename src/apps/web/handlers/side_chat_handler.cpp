#include "side_chat_handler.hpp"

namespace acecode::web {

SideChatStartRequest parse_side_chat_start(const nlohmann::json& payload) {
    SideChatStartRequest result;
    if (!payload.is_object()) {
        result.error = "payload must be an object";
        return result;
    }
    const auto read_string = [&](const char* field, std::string& target) {
        const auto value = payload.find(field);
        if (value == payload.end() || !value->is_string()) return false;
        target = value->get<std::string>();
        return true;
    };
    if (!read_string("request_id", result.request_id) || result.request_id.empty() ||
        result.request_id.size() > 128) {
        result.error = "request_id must contain 1 to 128 bytes";
        return result;
    }
    if (!read_string("session_id", result.session_id) || result.session_id.empty() ||
        result.session_id.size() > 256) {
        result.error = "session_id required";
        return result;
    }
    if (!read_string("question", result.question) || result.question.empty() ||
        result.question.size() > kMaxSideQuestionBytes) {
        result.error = "question must contain 1 to 16000 bytes";
        return result;
    }
    const auto history = payload.find("history");
    if (history != payload.end()) {
        if (!history->is_array() || history->size() > kMaxSideChatHistoryMessages) {
            result.error = "history must contain at most 200 messages";
            return result;
        }
        for (const auto& message : *history) {
            if (!message.is_object() || !message.contains("role") || !message["role"].is_string() ||
                !message.contains("content") || !message["content"].is_string() ||
                message.size() != 2) {
                result.error = "history messages require only text role and content";
                return result;
            }
            result.history.push_back({message["role"].get<std::string>(),
                                      message["content"].get<std::string>()});
        }
    }
    result.error = validate_side_chat_history(result.history);
    return result;
}

std::string side_chat_error_code(SideQuestionStatus status) {
    switch (status) {
    case SideQuestionStatus::InvalidQuestion: return "INVALID_SIDE_CHAT";
    case SideQuestionStatus::UnknownSession: return "UNKNOWN_SESSION";
    case SideQuestionStatus::ContextNotReady: return "SIDE_QUESTION_CONTEXT_NOT_READY";
    case SideQuestionStatus::ProviderUnavailable: return "SIDE_QUESTION_PROVIDER_UNAVAILABLE";
    default: return "SIDE_CHAT_FAILED";
    }
}

std::shared_ptr<SideChatRequestState> SideChatConnectionState::start(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || active_) return {};
    active_ = std::make_shared<SideChatRequestState>(request_id);
    return active_;
}

void SideChatConnectionState::stop(const std::string& request_id) {
    std::shared_ptr<SideChatRequestState> request;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ && active_->request_id == request_id) request = active_;
    }
    if (request) request->cancellation.cancel();
}

void SideChatConnectionState::close() {
    std::shared_ptr<SideChatRequestState> request;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        request = std::move(active_);
    }
    if (request) request->cancellation.cancel();
}

bool SideChatConnectionState::deliver(
    const std::shared_ptr<SideChatRequestState>& request,
    bool final, const std::function<void()>& send) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || !request || active_ != request) return false;
    if (final) active_.reset();
    if (send) send();
    return true;
}

} // namespace acecode::web
