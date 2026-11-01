#include "side_chat_handler.hpp"

namespace acecode::web {

namespace {
bool valid_composer_input(const nlohmann::json& input) {
    if (!input.is_object() || input.dump().size() > 2 * 1024 * 1024) return false;
    for (auto it = input.begin(); it != input.end(); ++it) {
        const auto& key = it.key();
        if (key == "text") { if (!it->is_string()) return false; }
        else if (key == "attachments" || key == "contexts" || key == "session_references") {
            if (!it->is_array()) return false;
        } else if (key == "composer_content") { if (!it->is_object()) return false; }
        else return false;
    }
    return true;
}
}

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
    if (payload.contains("input")) {
        if (!valid_composer_input(payload["input"])) {
            result.error = "invalid side composer input";
            return result;
        }
        result.input = payload["input"];
    }
    const bool has_question = read_string("question", result.question);
    if (result.input.is_null() && (!has_question || result.question.empty() ||
        result.question.size() > kMaxSideQuestionBytes)) {
        result.error = "question must contain 1 to 16000 bytes";
        return result;
    }
    if (payload.contains("model_name") && (!read_string("model_name", result.options.model_name) ||
        result.options.model_name.empty() || result.options.model_name.size() > 256)) {
        result.error = "invalid side model name";
        return result;
    }
    if (payload.contains("reasoning_effort")) {
        std::string effort;
        if (!read_string("reasoning_effort", effort) || effort.size() > 32 || result.options.model_name.empty()) {
            result.error = "invalid side reasoning effort";
            return result;
        }
        result.options.reasoning_effort = std::move(effort);
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
                message.size() != (message.contains("input") ? 3 : 2)) {
                result.error = "history messages require only text role and content";
                return result;
            }
            result.history.push_back({message["role"].get<std::string>(),
                                      message["content"].get<std::string>()});
            if (message.contains("input") && (message["role"] != "user" || !valid_composer_input(message["input"]))) {
                result.error = "only user history can contain a valid composer input";
                return result;
            }
            result.history_inputs.push_back(message.value("input", nlohmann::json{}));
        }
    }
    auto validation_history = result.history;
    for (std::size_t index = 0; index < validation_history.size(); ++index) {
        // The shared input parser will validate attachment-only input and replace
        // this display text with the resolved prompt before invoking the provider.
        if (!result.history_inputs[index].is_null() && validation_history[index].content.empty()) {
            validation_history[index].content = "[attachment]";
        }
    }
    result.error = validate_side_chat_history(validation_history);
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

SideChatSendDecision parse_side_chat_send_decision(const nlohmann::json& payload) {
    SideChatSendDecision result;
    if (!payload.is_object()) {
        result.error = "payload must be an object";
        return result;
    }
    const auto read_id = [&](const char* field, std::string& target) {
        const auto value = payload.find(field);
        if (value == payload.end() || !value->is_string()) return false;
        target = value->get<std::string>();
        return !target.empty() && target.size() <= 128;
    };
    if (!read_id("request_id", result.request_id) || !read_id("send_id", result.send_id)) {
        result.error = "request_id and send_id must contain 1 to 128 bytes";
        return result;
    }
    const auto decision = payload.find("decision");
    if (decision == payload.end() || !decision->is_string() ||
        (*decision != "send" && *decision != "cancel")) {
        result.error = "decision must be send or cancel";
        return result;
    }
    result.send = *decision == "send";
    if (!result.send) return result;
    const auto message = payload.find("message");
    if (message == payload.end() || !message->is_string()) {
        result.error = "message required";
        return result;
    }
    result.message = message->get<std::string>();
    if (result.message.find_first_not_of(" \t\r\n") == std::string::npos ||
        result.message.size() > kMaxSideQuestionBytes) {
        result.error = "message must contain 1 to 16000 bytes";
    }
    return result;
}

ToolResult side_chat_send_tool_result(const SideChatDeliveryResult& delivery, bool edited) {
    const std::string note = edited ? " The user edited the message before sending it." : "";
    switch (delivery.status) {
    case SideChatDelivery::Steered:
        return {"Sent to the main session. The main agent is working and reads it at its "
                "next step." + note, true};
    case SideChatDelivery::Started:
        return {"Sent to the main session. The main agent was idle and started a new turn "
                "with it." + note, true};
    case SideChatDelivery::Queued:
        return {"Queued in the main session. The main agent reads it when its current work "
                "finishes." + note, true};
    default:
        return {"[Error] The message was not delivered: " +
                (delivery.message.empty() ? std::string("the main session is unavailable")
                                          : delivery.message), false};
    }
}

void SideChatSendGate::open(const std::string& send_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_id_ = send_id;
    decision_.reset();
}

bool SideChatSendGate::resolve(const std::string& send_id, bool send, std::string message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || pending_id_.empty() || send_id != pending_id_ || decision_) return false;
        SideChatSendDecision decision;
        decision.send_id = send_id;
        decision.send = send;
        decision.message = std::move(message);
        decision_ = std::move(decision);
    }
    changed_.notify_all();
    return true;
}

std::optional<SideChatSendDecision> SideChatSendGate::wait(const std::atomic<bool>& aborted) {
    std::unique_lock<std::mutex> lock(mutex_);
    // 取消只置 aborted 标志不通知这里,所以按 100ms 轮询;用户作答与关闭会立即唤醒。
    while (!decision_ && !closed_ && !aborted.load()) {
        changed_.wait_for(lock, std::chrono::milliseconds(100));
    }
    auto decision = std::move(decision_);
    decision_.reset();
    pending_id_.clear();
    if (closed_ || aborted.load()) return std::nullopt;
    return decision;
}

void SideChatSendGate::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    changed_.notify_all();
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

bool SideChatConnectionState::decide(const SideChatSendDecision& decision) {
    std::shared_ptr<SideChatRequestState> request;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || !active_ || active_->request_id != decision.request_id) return false;
        request = active_;
    }
    return request->send_gate.resolve(decision.send_id, decision.send, decision.message);
}

void SideChatConnectionState::close() {
    std::shared_ptr<SideChatRequestState> request;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        request = std::move(active_);
    }
    if (!request) return;
    request->cancellation.cancel();
    request->send_gate.close();
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
