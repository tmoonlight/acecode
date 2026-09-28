#include "gateway.hpp"
#include "remote_control/channel_question_bridge.hpp"
#include "remote_control/remote_control_hub.hpp"
#include "remote_control/remote_control_service.hpp"
#include "remote_control/rc_session_navigation.hpp"
#include "remote_control/session_channel_binder.hpp"
#include "session/attachment_store.hpp"
#include "session/output_attachments.hpp"
#include "session/session_storage.hpp"
#include "utils/utf8_path.hpp"

#include <atomic>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace acecode::channels {
namespace {
std::string required_text(const Json& value, const char* key, std::size_t limit = kMaxTextBytes) {
    auto text = value.at(key).get<std::string>();
    if (text.empty() || text.size() > limit) throw std::runtime_error(std::string("Invalid ") + key);
    return text;
}
std::string read_media(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > kMaxMediaBytes)
        throw std::runtime_error("File missing or larger than 25 MiB");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read attachment");
    std::string bytes;
    char chunk[65536];
    while (file) {
        file.read(chunk, sizeof(chunk));
        if (bytes.size() + static_cast<std::size_t>(file.gcount()) > kMaxMediaBytes)
            throw std::runtime_error("File grew beyond 25 MiB");
        bytes.append(chunk, static_cast<std::size_t>(file.gcount()));
    }
    if (!file.eof()) throw std::runtime_error("Cannot read attachment");
    return bytes;
}
class Sender final : public rc::OutboundSender {
public:
    Sender(State& state, Address address,
           std::function<Json(const std::string&, const Json&)> request,
           std::string session_id, std::string cwd)
        : state_(state), address_(std::move(address)), request_(std::move(request)),
          session_id_(std::move(session_id)), cwd_(std::move(cwd)) {}
    bool send(const rc::OutboundMessage& msg, std::string* error) override {
        try {
            if (!state_.enabled() || !state_.allowed(address_, true))
                throw std::runtime_error("Channel disabled or access revoked");
            Json params{{"account", address_.account}, {"chat", address_.chat},
                        {"quote_id", msg.in_reply_to}};
            if (msg.attachment.is_object()) {
                // Resolve stored attachment identity again, never trust a model path.
                auto record = load_attachment(SessionStorage::get_project_dir(cwd_), session_id_,
                                               required_text(msg.attachment, "id", 128), error);
                if (!record) return false;
                const auto root = path_from_utf8(SessionStorage::get_project_dir(cwd_)) / "attachments" / session_id_;
                if (!path_inside(path_from_utf8(record->path), root))
                    throw std::runtime_error("Outbound attachment is outside its session storage");
                if (!std::filesystem::is_regular_file(path_from_utf8(record->path)) ||
                    std::filesystem::file_size(path_from_utf8(record->path)) > kMaxMediaBytes)
                    throw std::runtime_error("Outbound file missing or larger than 25 MiB");
                params["path"] = record->path;
                params["name"] = record->name;
                params["mime_type"] = record->mime_type;
                request_("send_file", params);
            } else {
                for (const auto& part : rc::chunk_rc_session_output(msg.text)) {
                    params["text"] = part;
                    request_("send", params);
                }
            }
            return true;
        } catch (const std::exception& e) {
            if (error) *error = e.what();
            return false;
        }
    }
private:
    State& state_;
    Address address_;
    std::function<Json(const std::string&, const Json&)> request_;
    std::string session_id_, cwd_;
};

struct Binding {
    Address address;
    std::string id, cwd;
    SessionClient::SubscriptionId subscription = 0;
    rc::RemoteControlHub hub;
    rc::ChannelQuestionBridge questions;
    std::recursive_mutex mu;
    bool closed = false;
    std::string quote_id;
    std::map<std::string, Json> permissions;
    std::set<std::string> closed_permissions;
    std::deque<std::string> closed_order;
    std::set<std::string> sent_attachments;

    void text(const std::string& value) {
        std::lock_guard<std::recursive_mutex> lock(mu);
        if (closed || value.empty()) return;
        rc::OutboundMessage msg;
        msg.type = "assistant_message"; msg.text = value; msg.in_reply_to = quote_id;
        hub.notify_outbound(std::move(msg));
    }
    std::string action(const rc::ChannelQuestionAction& result) {
        std::string reply;
        for (const auto& value : result.outbound_texts) {
            text(value);
            if (!reply.empty()) reply += '\n';
            reply += value;
        }
        return reply;
    }
    void permission(const Json& request) {
        const auto request_id = required_text(request, "request_id", 128);
        if (closed_permissions.count(request_id) || permissions.count(request_id)) return;
        permissions[request_id] = request;
        text("Permission: " + request.value("tool", std::string{}) + "\n" +
             rc::chunk_rc_session_output(request.value("args", Json::object()).dump(), 2000).front() +
             "\n/approve " + request_id + "\n/deny " + request_id);
    }
    void event(const SessionEvent& event) {
        std::lock_guard<std::recursive_mutex> lock(mu);
        if (closed || !event.payload.is_object()) return;
        try {
            const auto& p = event.payload;
            if (event.kind == SessionEventKind::Message && p.value("role", "") == "user") {
                quote_id.clear();
                const auto meta = p.value("metadata", Json::object());
                if (meta.is_object() && meta.contains("channel"))
                    quote_id = meta.at("channel").value("message_id", std::string{});
                sent_attachments.clear();
            }
            if (event.kind == SessionEventKind::QuestionRequest) {
                auto request = rc::ChannelQuestionBridge::request_from_event(
                    p, event.seq, rc::ChannelQuestionBridge::Clock::now());
                if (request) action(questions.add_request(std::move(*request)));
            } else if (event.kind == SessionEventKind::QuestionClosed) {
                action(questions.close_request(p.value("request_id", ""), p.value("reason", "closed")));
            } else if (event.kind == SessionEventKind::PermissionRequest) {
                permission(p);
            } else if (event.kind == SessionEventKind::PermissionClosed) {
                const auto id = p.value("request_id", std::string{});
                if (permissions.erase(id)) text("Permission closed: " + id + " (" + p.value("choice", "closed") + ")");
                closed_permissions.insert(id); closed_order.push_back(id);
                if (closed_order.size() > 256) {
                    closed_permissions.erase(closed_order.front()); closed_order.pop_front();
                }
            } else {
                const auto output = rc::classify_session_event(event.kind, p);
                if (output.kind == rc::OutboundEventAction::Kind::AssistantText) text(output.text);
                Json files = Json::array();
                if (event.kind == SessionEventKind::Message && p.value("role", "") == "assistant")
                    files = output_attachments_from_content_parts(p.value("content_parts", Json::array()));
                if (event.kind == SessionEventKind::ToolEnd) files = p.value("attachments", Json::array());
                if (files.is_array()) for (const auto& file : files) {
                    if (!file.is_object()) continue;
                    const auto id = file.value("id", std::string{});
                    if (id.empty() || !sent_attachments.insert(id).second) continue;
                    rc::OutboundMessage msg;
                    msg.type = "file"; msg.attachment = file; msg.in_reply_to = quote_id;
                    hub.notify_outbound(std::move(msg));
                }
            }
        } catch (const std::exception&) { text("Cannot render a channel event; inspect the session locally."); }
    }
};
} // namespace

struct Gateway::Impl {
    State& state;
    GatewayDeps deps;
    std::mutex mu;
    bool closed = false;
    std::map<std::string, std::shared_ptr<Binding>> bindings;
    struct Pair { Address address; std::chrono::steady_clock::time_point expires; };
    std::map<std::string, Pair> pairs;

    Impl(State& state, GatewayDeps deps) : state(state), deps(std::move(deps)) {}
    std::shared_ptr<Binding> bind(const Address& address, bool create) {
        auto it = bindings.find(address.key());
        if (it != bindings.end()) return it->second;
        SessionOptions options; options.no_workspace = true; options.permission_mode = "default";
        options.inherit_dangerous_mode = false;
        auto id = state.session(address);
        if (id) {
            if (!deps.sessions.resume_session(*id, options))
                throw std::runtime_error("Cannot resume bound session " + *id);
        } else {
            if (!create) throw std::runtime_error("Unknown channel conversation");
            id = deps.sessions.create_session(options);
            if (id->empty()) throw std::runtime_error("Cannot create channel session");
            try { state.bind(address, *id); }
            catch (...) { deps.sessions.destroy_session(*id); throw; }
        }
        auto b = std::make_shared<Binding>();
        b->address = address; b->id = *id;
        b->cwd = deps.session_cwd(*id);
        b->hub.enable("channel-internal", *id,
            std::make_shared<Sender>(state, address, deps.request, *id, b->cwd));
        std::weak_ptr<Binding> weak = b;
        b->subscription = deps.sessions.subscribe(*id, [weak](const SessionEvent& e) {
            if (auto binding = weak.lock()) binding->event(e);
        });
        bindings[address.key()] = b;
        // Live closure tombstones prevent a concurrent snapshot from reopening a request.
        if (auto pending = deps.sessions.snapshot_pending_questions(*id))
            b->action(b->questions.merge_snapshot(std::move(*pending)));
        if (deps.permissions) {
            auto pending = deps.permissions(*id);
            std::lock_guard<std::recursive_mutex> lock(b->mu);
            for (const auto& p : pending) b->permission(p);
        }
        return b;
    }
    std::shared_ptr<Binding> by_id(const std::string& id) {
        const auto snapshot = state.snapshot();
        for (const auto& item : snapshot.at("bindings").items()) {
            if (item.value().at("session_id") == id)
                return bind(Address::parse(item.value().at("address")), false);
        }
        throw std::runtime_error("Unknown channel session");
    }
    std::optional<std::string> control_input(const std::shared_ptr<Binding>& b, const std::string& text) {
        if (text == "/stop") { deps.sessions.abort(b->id); b->text("Stop requested."); return "Stop requested."; }
        if (text == "/status") {
            auto report = "Session: " + b->id + "\n";
            std::lock_guard<std::recursive_mutex> lock(b->mu);
            for (const auto& p : b->permissions) report += "Permission: " + p.first + "\n";
            for (const auto& text : b->questions.announce_current().outbound_texts) report += text + '\n';
            b->text(report); return report;
        }
        if (rc::ChannelQuestionBridge::is_control_input(text)) {
            auto action = b->questions.handle_input(text);
            auto reply = b->action(action);
            if (action.submission) {
                const auto& s = *action.submission;
                auto result = deps.sessions.respond_question(b->id, s.request_id, s.response);
                reply += b->action(b->questions.complete_submission(s.request_id, result));
            }
            return reply;
        }
        std::istringstream in(text); std::string verb, id, extra; in >> verb;
        if (verb != "/approve" && verb != "/deny") return std::nullopt;
        in >> id >> extra;
        {
            std::lock_guard<std::recursive_mutex> lock(b->mu);
            if (id.empty() || !extra.empty() || !b->permissions.count(id)) {
                const std::string error = "Unknown or closed permission. Use /approve <request_id> or /deny <request_id>.";
                b->text(error); return error;
            }
            b->permissions.erase(id);
        }
        deps.sessions.respond_permission(b->id, {id, verb == "/approve"
            ? PermissionDecisionChoice::Allow : PermissionDecisionChoice::Deny});
        const auto reply = "Permission response submitted: " + id;
        b->text(reply); return reply;
    }
    UserInput input(const std::shared_ptr<Binding>& b, const Json& message) {
        if (message.contains("error")) throw std::runtime_error(required_text(message, "error", 1000));
        UserInput input;
        input.text = message.value("text", std::string{});
        if (input.text.size() > kMaxTextBytes) throw std::runtime_error("Message is too large");
        input.display_text = input.text;
        input.metadata["channel"] = b->address.json();
        input.metadata["channel"]["platform"] = "whatsapp";
        input.metadata["channel"]["message_id"] = message.at("id");
        if (message.contains("quote") && message["quote"].is_object()) {
            input.metadata["channel"]["quote"] = message["quote"];
            auto quote = message["quote"].value("text", std::string{});
            if (!quote.empty()) input.text = "[Quoted WhatsApp message]\n" + rc::chunk_rc_session_output(quote, 8000).front() +
                                            "\n[End quote]\n" + input.text;
        }
        if (message.contains("media") && !message["media"].is_null()) {
            const auto& media = message["media"];
            if (media.contains("error")) throw std::runtime_error(required_text(media, "error", 1000));
            const auto kind = media.value("kind", std::string{});
            if (kind != "image" && kind != "document") throw std::runtime_error("Only images and documents are supported");
            const auto downloaded = deps.request("download", {{"account", b->address.account},
                {"chat", b->address.chat}, {"id", message.at("id")}});
            const auto path = path_from_utf8(required_text(downloaded, "path", 4096));
            if (!path_inside(path, state.transport_directory() / "media") || !std::filesystem::is_regular_file(path) ||
                std::filesystem::file_size(path) > kMaxMediaBytes) throw std::runtime_error("Invalid media cache file");
            const auto bytes = read_media(path);
            std::string error;
            auto record = save_attachment(SessionStorage::get_project_dir(b->cwd), b->id,
                media.value("name", "attachment"), media.value("mime_type", "application/octet-stream"), bytes, &error);
            if (!record) throw std::runtime_error(error);
            if (!input.text.empty()) input.content_parts.push_back({{"type", "text"}, {"text", input.text}});
            input.content_parts.push_back(attachment_content_part(*record));
            input.metadata["attachments"] = Json::array({attachment_to_json(*record)});
            std::error_code ec; std::filesystem::remove(path, ec);
        }
        if (input.empty()) throw std::runtime_error("Empty message");
        return input;
    }
};

Gateway::Gateway(State& state, GatewayDeps deps) : impl_(std::make_unique<Impl>(state, std::move(deps))) {}
Gateway::~Gateway() { shutdown(); }
Json Gateway::receive(const Json& message) {
    auto& g = *impl_;
    std::lock_guard<std::mutex> lock(g.mu);
    if (g.closed || !g.state.enabled()) return {{"status", "disabled"}};
    const auto address = Address::parse(message);
    if (!address.valid()) throw std::runtime_error("Invalid incoming address");
    const auto id = required_text(message, "id", 256);
    if (!g.state.allowed(address, message.value("mentioned", false))) {
        if (address.group) return {{"status", "ignored"}};
        const auto now = std::chrono::steady_clock::now();
        for (auto it = g.pairs.begin(); it != g.pairs.end();) {
            if (it->second.expires <= now) it = g.pairs.erase(it); else ++it;
        }
        for (const auto& pair : g.pairs)
            if (pair.second.address.key() == address.key()) return {{"status", "pairing_required"}};
        if (g.pairs.size() == 64) return {{"status", "pairing_limit"}};
        const auto code = rc::generate_remote_control_token().substr(0, 10);
        g.pairs[code] = {address, now + std::chrono::minutes(10)};
        g.deps.request("send", {{"account", address.account}, {"chat", address.chat}, {"quote_id", id},
            {"text", "ACECode pairing code: " + code +
                "\nAsk the local operator to run acecode channels approve " + code + ". Expires in 10 minutes."}});
        return {{"status", "pairing_required"}};
    }
    if (g.state.receipt(address, id)) return {{"status", "duplicate"}};
    auto b = g.bind(address, true);
    try {
        // Persist before dispatch: a crash may require a resend, never replay a tool turn.
        g.state.remember(address, id);
        if (auto reply = g.control_input(b, message.value("text", std::string{}))) {
            return {{"status", "control"}, {"text", *reply}};
        }
        auto input = g.input(b, message);
        if (!g.deps.sessions.send_input(b->id, input)) throw std::runtime_error("Session rejected input");
        return {{"status", "accepted"}, {"session_id", b->id}};
    } catch (const std::exception& e) {
        g.state.forget(address, id);
        b->text(std::string("Message failed: ") + e.what());
        throw;
    }
}
Json Gateway::control(const Json& command) {
    auto& g = *impl_;
    std::lock_guard<std::mutex> lock(g.mu);
    if (g.closed) throw std::runtime_error("Channel gateway stopped");
    const auto op = required_text(command, "op", 32);
    if (op == "sessions") {
        auto sessions = Json::array();
        const auto snapshot = g.state.snapshot();
        for (const auto& item : snapshot.at("bindings").items()) {
            auto value = item.value();
            const auto it = g.bindings.find(item.key());
            if (it != g.bindings.end()) {
                const auto stats = it->second->hub.stats();
                value["sent"] = stats.outbound_sent; value["failed"] = stats.outbound_failed;
                value["dropped"] = stats.outbound_dropped;
            }
            sessions.push_back(std::move(value));
        }
        return {{"sessions", sessions}};
    }
    if (op == "pending") {
        auto pairs = Json::array();
        for (const auto& item : g.pairs) if (item.second.expires > std::chrono::steady_clock::now())
            pairs.push_back({{"code", item.first}, {"address", item.second.address.json()}});
        return {{"pairing", pairs}};
    }
    if (op == "approve") {
        const auto code = required_text(command, "code", 32);
        const auto it = g.pairs.find(code);
        if (it == g.pairs.end() || it->second.expires <= std::chrono::steady_clock::now())
            throw std::runtime_error("Unknown or expired pairing code");
        const auto a = it->second.address;
        g.state.set_access(a.account, a.sender, true); g.pairs.erase(it);
        return {{"text", "Allowed " + a.sender + ". Send a new message to start."}};
    }
    if (op == "allow" || op == "revoke") {
        const auto account = required_text(command, "account", 100);
        const auto jid = required_text(command, "jid", 100);
        g.state.set_access(account, jid, op == "allow");
        if (op == "revoke") for (const auto& item : g.bindings) {
            const auto& b = item.second;
            if (b->address.account == account && (b->address.sender == jid || b->address.chat == jid))
                g.deps.sessions.abort(b->id);
        }
        return {{"text", "Access updated."}};
    }
    auto b = g.by_id(required_text(command, "session_id", 128));
    if (op == "show") {
        Json pending = Json::array();
        { std::lock_guard<std::recursive_mutex> lock(b->mu); for (const auto& p : b->permissions) pending.push_back(p.second); }
        return {{"messages", g.deps.transcript(b->id)}, {"permissions", pending},
                {"questions", b->questions.announce_current().outbound_texts}};
    }
    if (op == "stop") { g.deps.sessions.abort(b->id); return {{"text", "Stop requested."}}; }
    if (op == "send") {
        const auto text = required_text(command, "text");
        if (auto reply = g.control_input(b, text)) return {{"text", *reply}};
        if (!g.deps.sessions.send_input(b->id, text)) throw std::runtime_error("Session rejected input");
        return {{"text", "Input queued."}};
    }
    if (op == "file") {
        const auto path = path_from_utf8(required_text(command, "path", 4096));
        const auto bytes = read_media(path);
        std::string error;
        auto record = save_attachment(SessionStorage::get_project_dir(b->cwd), b->id,
            path_to_utf8(path.filename()), "", bytes, &error);
        if (!record) throw std::runtime_error(error);
        rc::OutboundMessage msg; msg.type = "file"; msg.attachment = attachment_to_json(*record);
        b->hub.notify_outbound(std::move(msg));
        return {{"text", "File queued for delivery."}};
    }
    throw std::runtime_error("Unknown channel operation");
}
void Gateway::restore(const std::string& account) {
    auto& g = *impl_; std::lock_guard<std::mutex> lock(g.mu);
    const auto snapshot = g.state.snapshot();
    std::string error;
    for (const auto& item : snapshot.at("bindings").items()) {
        const auto address = Address::parse(item.value().at("address"));
        if (address.account == account && g.state.allowed(address, true)) {
            try { g.bind(address, false); }
            catch (const std::exception& e) { error = e.what(); }
        }
    }
    if (!error.empty()) throw std::runtime_error(error);
}
void Gateway::shutdown() {
    if (!impl_) return;
    auto& g = *impl_; std::lock_guard<std::mutex> lock(g.mu);
    if (g.closed) return;
    g.closed = true;
    for (const auto& item : g.bindings) {
        auto& b = item.second;
        { std::lock_guard<std::recursive_mutex> guard(b->mu); b->closed = true; }
        g.deps.sessions.unsubscribe(b->id, b->subscription);
        b->hub.clear_inbound_route();
        b->hub.disable();
    }
    g.bindings.clear();
}
} // namespace acecode::channels
