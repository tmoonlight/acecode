#pragma once

#include "session/side_chat.hpp"

namespace acecode::web {

struct SideChatStartRequest {
    std::string session_id;
    std::string request_id;
    std::string question;
    std::vector<SideChatMessage> history;
    std::string error;
};

SideChatStartRequest parse_side_chat_start(const nlohmann::json& payload);
std::string side_chat_error_code(SideQuestionStatus status);

struct SideChatRequestState {
    explicit SideChatRequestState(std::string id) : request_id(std::move(id)) {}
    std::string request_id;
    SideChatCancellation cancellation;
};

// Owns the one-request-per-connection contract independently of Crow. Delivery
// and close share a lock, so no callback can reach a transport after close.
class SideChatConnectionState {
public:
    std::shared_ptr<SideChatRequestState> start(const std::string& request_id);
    void stop(const std::string& request_id);
    void close();
    bool deliver(const std::shared_ptr<SideChatRequestState>& request,
                 bool final, const std::function<void()>& send);

private:
    std::mutex mutex_;
    bool closed_ = false;
    std::shared_ptr<SideChatRequestState> active_;
};

} // namespace acecode::web
