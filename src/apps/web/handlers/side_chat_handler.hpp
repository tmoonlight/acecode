#pragma once

#include "agent/side_question/side_chat.hpp"

#include <condition_variable>
#include <optional>

namespace acecode::web {

struct SideChatStartRequest {
    std::string session_id;
    std::string request_id;
    std::string question;
    std::vector<SideChatMessage> history;
    nlohmann::json input;
    std::vector<nlohmann::json> history_inputs;
    SideChatInputOptions options;
    std::string error;
};

SideChatStartRequest parse_side_chat_start(const nlohmann::json& payload);
std::string side_chat_error_code(SideQuestionStatus status);

// The user's answer to a model request to send a message to the main session.
// message is the (possibly edited) text to deliver when send is true.
struct SideChatSendDecision {
    std::string request_id;
    std::string send_id;
    bool send = false;
    std::string message;
    std::string error;
};

SideChatSendDecision parse_side_chat_send_decision(const nlohmann::json& payload);

// Text fed back to the side model after a send request was answered.
ToolResult side_chat_send_tool_result(const SideChatDeliveryResult& delivery, bool edited);

// One pending confirmation per side request: the side model's tool call blocks
// in wait() until the user answers in the side chat window, the request is
// cancelled, or the connection closes.
class SideChatSendGate {
public:
    void open(const std::string& send_id);
    // False when no confirmation with this id is pending (stale or duplicate).
    bool resolve(const std::string& send_id, bool send, std::string message);
    std::optional<SideChatSendDecision> wait(const std::atomic<bool>& aborted);
    void close();

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::string pending_id_;
    std::optional<SideChatSendDecision> decision_;
    bool closed_ = false;
};

struct SideChatRequestState {
    explicit SideChatRequestState(std::string id) : request_id(std::move(id)) {}
    std::string request_id;
    SideChatCancellation cancellation;
    SideChatSendGate send_gate;
};

// Owns the one-request-per-connection contract independently of Crow. Delivery
// and close share a lock, so no callback can reach a transport after close.
class SideChatConnectionState {
public:
    std::shared_ptr<SideChatRequestState> start(const std::string& request_id);
    void stop(const std::string& request_id);
    // Routes the user's answer to the active request's pending confirmation.
    bool decide(const SideChatSendDecision& decision);
    void close();
    bool deliver(const std::shared_ptr<SideChatRequestState>& request,
                 bool final, const std::function<void()>& send);

private:
    std::mutex mutex_;
    bool closed_ = false;
    std::shared_ptr<SideChatRequestState> active_;
};

} // namespace acecode::web
