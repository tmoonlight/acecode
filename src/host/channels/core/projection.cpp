#include "projection.hpp"

#include "channels/core/commands.hpp"
#include "remote_control/rc_session_navigation.hpp"
#include "remote_control/session_channel_binder.hpp"
#include "session/attachment_store.hpp"
#include "session/output_attachments.hpp"
#include "session/session_storage.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>

namespace acecode::channels::core {
namespace {

constexpr std::size_t kMaxClosedPermissions = 256;

// path 必须在 root 之下(规范化后逐段比较,Windows 不区分大小写)。
bool path_inside(const std::filesystem::path& path, const std::filesystem::path& root) {
    std::error_code ec;
    const auto resolved = std::filesystem::weakly_canonical(path, ec);
    if (ec) return false;
    const auto base = std::filesystem::weakly_canonical(root, ec);
    if (ec || resolved == base) return false;
    auto p = resolved.begin();
    for (auto r = base.begin(); r != base.end(); ++r, ++p) {
        if (p == resolved.end()) return false;
#ifdef _WIN32
        if (_wcsicmp(p->c_str(), r->c_str()) != 0) return false;
#else
        if (*p != *r) return false;
#endif
    }
    return true;
}

// 把出站消息交给当前传输层。附件必须按 id 重新解析,并确认文件仍在该会话自己的
// 附件目录里 —— 绝不信任模型给出的路径。
class ImSender final : public rc::OutboundSender {
public:
    ImSender(im::Address address, std::string session_id, std::string cwd, std::shared_ptr<TransportSlot> slot)
        : address_(std::move(address)), session_id_(std::move(session_id)), cwd_(std::move(cwd)),
          slot_(std::move(slot)) {}

    bool send(const rc::OutboundMessage& msg, std::string* error) override {
        const auto transport = slot_->get();
        if (!transport) {
            if (error) *error = "通道未连接";
            return false;
        }
        nlohmann::json context = nlohmann::json::object();
        if (!msg.in_reply_to.empty()) {
            try {
                context = nlohmann::json::parse(msg.in_reply_to);
            } catch (...) {
            }
        }
        im::SendResult result;
        if (msg.type == "file" && msg.attachment.is_object()) {
            const auto id = msg.attachment.value("id", std::string{});
            const auto project = SessionStorage::get_project_dir(cwd_);
            std::string load_error;
            const auto record = load_attachment(project, session_id_, id, &load_error);
            if (!record) {
                if (error) *error = load_error.empty() ? "附件不存在" : load_error;
                return false;
            }
            const auto root = path_from_utf8(project) / "attachments" / session_id_;
            if (!path_inside(path_from_utf8(record->path), root)) {
                if (error) *error = "附件不在会话附件目录内";
                return false;
            }
            result = transport->send_file(address_, path_from_utf8(record->path), record->name, record->mime_type,
                                          context);
        } else {
            result = transport->send_text(address_, msg.text, context);
        }
        if (!result.ok()) {
            if (error) *error = result.error;
            return false;
        }
        return true;
    }

private:
    im::Address address_;
    std::string session_id_, cwd_;
    std::shared_ptr<TransportSlot> slot_;
};

} // namespace

std::shared_ptr<im::Transport> TransportSlot::get() const {
    std::lock_guard<std::mutex> lock(mu_);
    return transport_;
}

void TransportSlot::set(std::shared_ptr<im::Transport> transport) {
    std::lock_guard<std::mutex> lock(mu_);
    transport_ = std::move(transport);
}

Projection::Projection(BindingRecord record, ProjectionDeps deps)
    : record_(std::move(record)), deps_(std::move(deps)) {}

Projection::~Projection() { stop(); }

void Projection::start() {
    {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        if (started_) return;
        started_ = true;
    }
    hub_.enable("channel-internal", record_.session_id,
                std::make_shared<ImSender>(record_.address, record_.session_id, record_.cwd, deps_.transport));
    std::weak_ptr<Projection> weak = shared_from_this();
    // 先订阅再读快照,避免两者之间产生的请求被漏掉。
    subscription_ = deps_.sessions->subscribe(record_.session_id, [weak](const SessionEvent& event) {
        if (auto self = weak.lock()) self->on_event(event);
    });
    if (auto pending = deps_.sessions->snapshot_pending_questions(record_.session_id)) {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        emit_question_action(questions_.merge_snapshot(std::move(*pending)));
    }
    if (deps_.pending_permissions) {
        const auto pending = deps_.pending_permissions(record_.session_id);
        std::lock_guard<std::recursive_mutex> lock(mu_);
        for (const auto& request : pending) permission_locked(request);
    }
}

void Projection::stop() {
    SessionClient::SubscriptionId subscription = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        if (!started_ || closed_) return;
        flush_locked();
        closed_ = true;
        subscription = subscription_;
        subscription_ = 0;
    }
    if (subscription) deps_.sessions->unsubscribe(record_.session_id, subscription);
    set_typing(false);
    hub_.clear_inbound_route();
    hub_.disable();
}

void Projection::set_typing(bool on) {
    const auto transport = deps_.transport->get();
    if (transport && transport->capabilities().supports_typing) transport->set_typing(record_.address, on);
}

void Projection::enqueue_text_locked(const std::string& text, const nlohmann::json& reply_context) {
    if (closed_ || text.empty()) return;
    rc::OutboundMessage message;
    message.type = "assistant_message";
    message.text = text;
    message.in_reply_to = reply_context.is_object() && !reply_context.empty() ? reply_context.dump() : std::string{};
    hub_.notify_outbound(std::move(message));
}

void Projection::flush_locked() {
    if (batch_.empty()) return;
    auto text = std::move(batch_);
    batch_.clear();
    enqueue_text_locked(text, reply_context_);
}

void Projection::say(const std::string& text, const nlohmann::json& reply_context) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    flush_locked();
    enqueue_text_locked(text, reply_context);
}

void Projection::emit_question_action(const rc::ChannelQuestionAction& action) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (action.outbound_texts.empty()) return;
    flush_locked();
    for (const auto& text : action.outbound_texts) enqueue_text_locked(text, reply_context_);
}

void Projection::permission_locked(const nlohmann::json& request) {
    const auto id = request.value("request_id", std::string{});
    if (id.empty() || closed_permissions_.count(id) || permissions_.count(id)) return;
    permissions_[id] = request;
    flush_locked();
    const auto args = rc::chunk_rc_session_output(request.value("args", nlohmann::json::object()).dump(), 1500);
    enqueue_text_locked(texts::permission_prompt(id, request.value("tool", std::string{}), args.empty() ? "" : args.front()),
                        reply_context_);
}

bool Projection::take_permission(const std::string& request_id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    return permissions_.erase(request_id) > 0;
}

std::vector<std::string> Projection::pending_permission_ids() const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::vector<std::string> ids;
    for (const auto& [id, request] : permissions_) ids.push_back(id);
    return ids;
}

std::vector<std::string> Projection::pending_question_texts() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    return questions_.announce_current().outbound_texts;
}

void Projection::on_event(const SessionEvent& event) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (closed_ || !event.payload.is_object()) return;
    try {
        const auto& p = event.payload;
        const auto transport = deps_.transport->get();
        const bool batch = transport && transport->capabilities().batch_turn_output;
        switch (event.kind) {
            case SessionEventKind::Message:
                if (p.value("role", std::string{}) == "user") {
                    flush_locked();
                    reply_context_ = nlohmann::json::object();
                    const auto meta = p.value("metadata", nlohmann::json::object());
                    if (meta.is_object() && meta.contains("channel") && meta["channel"].is_object())
                        reply_context_ = meta["channel"].value("reply_context", nlohmann::json::object());
                    sent_attachments_.clear();
                    return;
                }
                break;
            case SessionEventKind::BusyChanged:
                if (p.value("busy", false)) {
                    set_typing(true);
                } else {
                    flush_locked();
                    set_typing(false);
                }
                return;
            case SessionEventKind::Done:
                flush_locked();
                set_typing(false);
                return;
            case SessionEventKind::QuestionRequest: {
                auto request = rc::ChannelQuestionBridge::request_from_event(
                    p, event.seq, rc::ChannelQuestionBridge::Clock::now());
                if (request) emit_question_action(questions_.add_request(std::move(*request)));
                return;
            }
            case SessionEventKind::QuestionClosed:
                emit_question_action(questions_.close_request(p.value("request_id", std::string{}),
                                                              p.value("reason", std::string("closed"))));
                return;
            case SessionEventKind::PermissionRequest:
                permission_locked(p);
                return;
            case SessionEventKind::PermissionClosed: {
                const auto id = p.value("request_id", std::string{});
                if (permissions_.erase(id)) {
                    flush_locked();
                    enqueue_text_locked(texts::permission_closed(id, p.value("choice", std::string("closed"))),
                                        reply_context_);
                }
                closed_permissions_.insert(id);
                closed_order_.push_back(id);
                if (closed_order_.size() > kMaxClosedPermissions) {
                    closed_permissions_.erase(closed_order_.front());
                    closed_order_.erase(closed_order_.begin());
                }
                return;
            }
            default:
                break;
        }
        const auto output = rc::classify_session_event(event.kind, p);
        if (output.kind == rc::OutboundEventAction::Kind::AssistantText && !output.text.empty()) {
            if (batch) {
                if (!batch_.empty()) batch_ += "\n\n";
                batch_ += output.text;
                if (batch_.size() > 32 * 1024) flush_locked();
            } else {
                enqueue_text_locked(output.text, reply_context_);
            }
        }
        nlohmann::json files = nlohmann::json::array();
        if (event.kind == SessionEventKind::Message && p.value("role", std::string{}) == "assistant")
            files = output_attachments_from_content_parts(p.value("content_parts", nlohmann::json::array()));
        if (event.kind == SessionEventKind::ToolEnd) files = p.value("attachments", nlohmann::json::array());
        if (files.is_array() && !files.empty()) {
            flush_locked();
            for (const auto& file : files) {
                if (!file.is_object()) continue;
                const auto id = file.value("id", std::string{});
                if (id.empty() || !sent_attachments_.insert(id).second) continue;
                rc::OutboundMessage message;
                message.type = "file";
                message.attachment = file;
                message.in_reply_to = reply_context_.empty() ? std::string{} : reply_context_.dump();
                hub_.notify_outbound(std::move(message));
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN(std::string("[channels] cannot project session event: ") + e.what());
    }
}

} // namespace acecode::channels::core
