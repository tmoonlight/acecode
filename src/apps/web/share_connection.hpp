#pragma once
#include "session_share/share_store.hpp"
#include "peers/peer_store.hpp"
#include "network/websocket_client.hpp"
#include "agent/side_question/side_chat.hpp"
#include "share/share_projection.hpp"
#include "session/scoped_subscription.hpp"
#include <crow.h>
#include <mutex>

namespace acecode::web {
struct ShareConnection {
    std::mutex mu;
    // Borrowed only while connected and under mu; cleared before onclose returns.
    crow::websocket::connection* connection = nullptr;
    std::optional<share::ShareRecord> share;
    std::optional<peers::PeerRecord> peer;
    // Cancellation and transport are shared with owned workers; neither owns us.
    std::shared_ptr<SideChatCancellation> question;
    std::shared_ptr<network::WebSocketClient> upstream;
    std::int64_t accepted_at = 0;
    bool closed = false;
    std::vector<SideChatMessage> history;
    share::ShareStreamRedactor transcript_stream;
    ScopedSubscription subscription;

    void send(const nlohmann::json& value) {
        std::lock_guard<std::mutex> lock(mu);
        if (connection && !closed) connection->send_text(value.dump());
    }
    void disconnect(const std::string& reason) {
        std::shared_ptr<SideChatCancellation> cancel;
        std::shared_ptr<network::WebSocketClient> transport;
        {
            std::lock_guard<std::mutex> lock(mu);
            cancel = question; transport = upstream;
            if (connection && !closed) connection->close(reason);
            closed = true;
        }
        if (cancel) cancel->cancel();
        if (transport) transport->abort();
    }
};
} // namespace acecode::web
