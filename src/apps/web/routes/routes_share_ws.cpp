#include "web/server_impl.hpp"
#include "memory/secret_redaction.hpp"
#include "utils/scope_exit.hpp"

namespace acecode::web {
using nlohmann::json;

void WebServer::Impl::open_sharing_connection(crow::websocket::connection& connection) {
    std::shared_ptr<ShareConnection> state;
    {
        std::lock_guard<std::mutex> lock(sharing_mu);
        const auto found = pending_sharing.find(connection.userdata());
        if (found != pending_sharing.end()) {
            state = found->second; pending_sharing.erase(found);
            sharing_connections.emplace(&connection, state);
        }
    }
    if (!state) { connection.close("invalid_connection"); return; }
    { std::lock_guard<std::mutex> lock(state->mu); state->connection = &connection; }
    if (!state->share) return;
    const auto now = now_unix_ms() / 1000;
    state->send({{"type", "hello"}, {"allow_questions", state->share->allow_questions},
        {"remaining", 30 - (state->share->question_day == now / 86400 ? state->share->question_count : 0)},
        {"resets_at", (now / 86400 + 1) * 86400}});
}

void WebServer::Impl::close_sharing_connection(crow::websocket::connection& connection) {
    std::shared_ptr<ShareConnection> state;
    {
        std::lock_guard<std::mutex> lock(sharing_mu);
        const auto found = sharing_connections.find(&connection);
        if (found == sharing_connections.end()) return;
        state = found->second; sharing_connections.erase(found);
    }
    ScopedSubscription subscription;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        state->closed = true; state->connection = nullptr;
        subscription = std::move(state->subscription);
    }
    state->disconnect("closed");
}

void WebServer::Impl::register_share_socket() {
    CROW_WEBSOCKET_ROUTE(app, "/ws/share/v1")
    .onaccept([this](const crow::request& req, void** userdata) {
        if (shutdown_requested.load()) return false;
        share::ShareRecord record;
        if (require_share_auth(req, record) || !acquire_shared_session(record)) return false;
        auto state = std::make_shared<ShareConnection>();
        state->share = std::move(record); state->accepted_at = now_unix_ms() / 1000;
        std::lock_guard<std::mutex> lock(sharing_mu);
        *userdata = state.get(); pending_sharing.emplace(state.get(), std::move(state));
        return true;
    })
    .onopen([this](crow::websocket::connection& connection) { open_sharing_connection(connection); })
    .onmessage([this](crow::websocket::connection& connection, const std::string& data, bool binary) {
        if (binary) { connection.close("text_only"); return; }
        handle_guest_message(connection, data);
    })
    .onclose([this](crow::websocket::connection& connection, const std::string&, std::uint16_t) {
        close_sharing_connection(connection);
    });
}

void WebServer::Impl::handle_guest_message(crow::websocket::connection& connection, const std::string& data) {
    std::shared_ptr<ShareConnection> state;
    { std::lock_guard<std::mutex> lock(sharing_mu);
      auto found = sharing_connections.find(&connection);
      if (found != sharing_connections.end()) state = found->second; }
    if (!state || !state->share) return;
    const auto auth = shares->authorize(state->share->credential(), now_unix_ms() / 1000);
    if (!auth.share) { state->disconnect(auth.error); return; }
    if (data.size() > 65536) { state->disconnect("message_too_large"); return; }
    const auto message = json::parse(data, nullptr, false);
    if (!message.is_object() || !message.contains("type") || !message["type"].is_string()) {
        state->send({{"type", "error"}, {"error", "invalid_message"}}); return;
    }
    const auto type = message["type"].get<std::string>();
    if (type == "ping") { state->send({{"type", "pong"}}); return; }
    if (type == "unsubscribe") {
        ScopedSubscription subscription;
        { std::lock_guard<std::mutex> lock(state->mu); subscription = std::move(state->subscription); }
        return;
    }
    if (type == "subscribe") {
        if (!deps.session_client) return;
        { std::lock_guard<std::mutex> lock(state->mu); if (state->subscription) return; }
        const std::weak_ptr<ShareConnection> weak = state;
        const auto record = *auth.share;
        auto id = deps.session_client->subscribe(record.session_id,
            [weak, store = shares, record](const SessionEvent& event) {
                auto target = weak.lock();
                if (!target) return;
                if (!store->authorize(record.credential(), now_unix_ms() / 1000).share) {
                    target->disconnect("share_invalid"); return;
                }
                json payload;
                {
                    std::lock_guard<std::mutex> lock(target->mu);
                    if (target->closed) return;
                    if (event.kind == SessionEventKind::Token) {
                        const auto text = target->transcript_stream.append(event.payload.value("text", ""));
                        if (!text.empty()) payload = {{"type", "token"}, {"text", text}};
                    } else if (event.kind == SessionEventKind::Message) {
                        target->transcript_stream.reset();
                        payload = {{"type", "changed"}};
                    } else if (event.kind == SessionEventKind::BusyChanged || event.kind == SessionEventKind::Done) {
                        target->transcript_stream.reset();
                        payload = {{"type", "changed"}};
                    } else if (event.kind == SessionEventKind::ToolStart || event.kind == SessionEventKind::ToolEnd) {
                        payload = share::redact_share_json({{"type", "progress"}, {"tool", event.payload.value("tool", "")}});
                    }
                    if (!payload.is_null() && target->connection) target->connection->send_text(payload.dump());
                }
            });
        bool cancelled = false;
        ScopedSubscription subscription(*deps.session_client, record.session_id, id);
        { std::lock_guard<std::mutex> lock(state->mu);
          cancelled = state->closed || static_cast<bool>(state->subscription);
          if (!cancelled) state->subscription = std::move(subscription); }
        if (cancelled) return;
        state->send({{"type", "changed"}});
        return;
    }
    if (type == "ask_stop") {
        std::shared_ptr<SideChatCancellation> cancel;
        { std::lock_guard<std::mutex> lock(state->mu); cancel = state->question; }
        if (cancel) cancel->cancel();
        return;
    }
    if (type != "ask_start") { state->send({{"type", "error"}, {"error", "unsupported_message"}}); return; }
    if (!auth.share->allow_questions) { state->send({{"type", "error"}, {"error", "read_only"}}); return; }
    if (!message.contains("question") || !message["question"].is_string()) {
        state->send({{"type", "error"}, {"error", "invalid_question"}}); return;
    }
    const auto question = message["question"].get<std::string>();
    if (question.empty() || question.size() > kMaxSideQuestionBytes) {
        state->send({{"type", "error"}, {"error", "invalid_question"}}); return;
    }
    const auto now = now_unix_ms() / 1000;
    const auto quota = shares->begin_question(auth.share->id, now);
    if (!quota.empty()) {
        state->send({{"type", "error"}, {"error", quota}, {"resets_at", (now / 86400 + 1) * 86400}}); return;
    }
    auto entry = acquire_shared_session(*auth.share);
    if (!entry || !entry->loop) {
        shares->end_question(auth.share->id); state->send({{"type", "error"}, {"error", "session_unavailable"}}); return;
    }
    auto cancellation = std::make_shared<SideChatCancellation>();
    std::vector<SideChatMessage> history;
    { std::lock_guard<std::mutex> lock(state->mu);
      if (state->closed) { shares->end_question(auth.share->id); return; }
      state->question = cancellation; history = state->history; }
    const std::weak_ptr<ShareConnection> weak = state;
    const auto id = auth.share->id;
    const auto name = message.contains("name") && message["name"].is_string()
        ? message["name"].get<std::string>().substr(0, 128) : std::string("Guest");
    LOG_INFO("[share] question id=" + id + " source=" + connection.get_remote_ip());
    const bool started = sharing_workers.spawn([weak, store = shares, entry, cancellation, history, question, id, name, now] {
        ScopeExit finish([store, id] { store->end_question(id); });
        try {
            share::ShareStreamRedactor redactor;
            SideChatInputOptions options;
            options.extra_read_denied = [](const std::string& path) { return share::guest_sensitive_path(path); };
            const auto result = entry->loop->stream_side_chat(question, history, *cancellation,
                [weak, &redactor](const std::string& delta, bool reset) {
                    if (reset) redactor.reset();
                    const auto text = redactor.append(delta);
                    if (auto target = weak.lock(); target && (reset || !text.empty()))
                        target->send({{"type", "ask_delta"}, {"text", text}, {"reset", reset}});
                }, {}, {}, options);
            const auto answer = redact_secrets(result.response.answer).text;
            store->append_question(id, {{"time", now}, {"name", redact_secrets(name).text},
                {"question", redact_secrets(question).text}, {"answer", answer}, {"cancelled", result.cancelled}});
            if (auto target = weak.lock()) {
                { std::lock_guard<std::mutex> lock(target->mu);
                  target->question.reset();
                  target->history.push_back({"user", question, {}});
                  target->history.push_back({"assistant", answer, {}});
                  while (target->history.size() > 20) target->history.erase(target->history.begin()); }
                const auto latest = store->find(id);
                const auto finished_at = now_unix_ms() / 1000;
                store->end_question(id); finish.release();
                target->send({{"type", "ask_done"}, {"answer", answer}, {"cancelled", result.cancelled},
                    {"remaining", latest && latest->question_day == finished_at / 86400 ? 30 - latest->question_count : 30},
                    {"resets_at", (finished_at / 86400 + 1) * 86400},
                    {"error", result.response.status == SideQuestionStatus::Ok ? "" : "question_failed"}});
            }
        } catch (const std::exception&) {
            if (auto target = weak.lock()) {
                { std::lock_guard<std::mutex> lock(target->mu); target->question.reset(); }
                target->send({{"type", "error"}, {"error", "question_failed"}});
            }
        }
    });
    if (!started) { shares->end_question(id); cancellation->cancel(); state->send({{"type", "error"}, {"error", "server_stopping"}}); }
}
} // namespace acecode::web
