#include "qq_transport.hpp"

#include "im/http.hpp"
#include "im/markdown.hpp"
#include "im/redact.hpp"
#include "im/text_chunk.hpp"
#include "utils/base64.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace acecode::im::qqbot {
namespace {

using Clock = std::chrono::steady_clock;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

std::pair<std::string, std::string> scope_of(const Address& to) {
    return {to.kind == ChatKind::Group ? "group" : "c2c", to.chat};
}

bool client_error(const ApiError& error) { return error.status >= 400 && error.status < 500; }

bool markdown_rejected(const ApiError& error) {
    if (!client_error(error)) return false;
    std::string message = error.message;
    std::transform(message.begin(), message.end(), message.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return message.find("markdown") != std::string::npos || message.find("msg_type") != std::string::npos;
}

constexpr std::size_t kSeenLimit = 1000;

} // namespace

QqTransport::QqTransport(QqTransportOptions options)
    : options_(std::move(options)), api_(options_.api), budget_(options_.limits) {
    markdown_ = options_.markdown;
    status_.account = options_.api.app_id;
}

QqTransport::~QqTransport() { stop(); }

Capabilities QqTransport::capabilities() const {
    Capabilities caps;
    caps.max_text_units = kMaxTextChars;
    caps.count_utf16 = false;
    caps.batch_turn_output = true;
    caps.supports_typing = false;
    caps.max_upload_bytes = options_.max_upload_bytes;
    caps.max_download_bytes = options_.max_download_bytes;
    return caps;
}

void QqTransport::start(TransportCallbacks callbacks) {
    if (running_.exchange(true)) return;
    callbacks_ = std::move(callbacks);
    stopping_ = false;
    worker_ = acecode::JoiningThread(&QqTransport::run, this);
}

void QqTransport::stop() {
    if (!running_.exchange(false)) return;
    stopping_ = true;
    ws_.abort();
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    ws_.close();
    set_status(LinkState::Stopped, {});
}

TransportStatus QqTransport::status() const {
    std::lock_guard<std::mutex> lock(status_mu_);
    auto copy = status_;
    copy.extra["held"] = held_.total();
    copy.extra["markdown"] = markdown_.load();
    return copy;
}

void QqTransport::set_status(LinkState state, const std::string& detail, bool retry_stopped) {
    TransportStatus snapshot;
    {
        std::lock_guard<std::mutex> lock(status_mu_);
        if (status_.state == state && status_.detail == detail && status_.retry_stopped == retry_stopped) return;
        status_.state = state;
        status_.detail = redact_secrets(detail, {options_.api.app_secret});
        status_.retry_stopped = retry_stopped;
        snapshot = status_;
    }
    if (callbacks_.on_status) callbacks_.on_status(snapshot);
}

bool QqTransport::wait_for(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(wake_mu_);
    wake_.wait_for(lock, duration, [this] { return stopping_.load(); });
    return !stopping_;
}

void QqTransport::run() {
    std::size_t attempt = 0;
    while (!stopping_) {
        set_status(attempt == 0 ? LinkState::Connecting : LinkState::Retrying,
                   attempt == 0 ? "正在连接 QQ" : "正在重新连接 QQ");
        std::chrono::milliseconds extra_delay{0};
        std::string error;
        const auto token = api_.access_token(&error);
        if (token.empty()) {
            if (api_.last_token_auth_failed()) {
                // 凭据被拒不会自己好:停止重试,等用户重新配置或重新打开开关。
                set_status(LinkState::Failed, error, true);
                break;
            }
            set_status(LinkState::Retrying, error);
        } else {
            const auto url = api_.gateway_url(&error);
            network::WebSocketConnectOptions ws_options;
            ws_options.url = url;
            ws_options.headers = {{"User-Agent", user_agent()}};
            ws_options.use_proxy = options_.api.use_proxy;
            if (url.empty()) {
                set_status(LinkState::Retrying, error);
            } else if (!ws_.connect(ws_options, &error)) {
                set_status(LinkState::Retrying, "无法连接 QQ 网关:" + error);
            } else {
                session_.on_connected(token, Clock::now());
                bool fatal = false;
                while (!stopping_) {
                    const auto tick = session_.on_tick(Clock::now());
                    if (tick.zombie) {
                        ws_.close(4000, "heartbeat timeout");
                        session_.on_closed(4000);
                        break;
                    }
                    if (tick.heartbeat) ws_.send_text(*tick.heartbeat, std::chrono::seconds(10), nullptr);
                    network::WebSocketMessage message;
                    const auto received = ws_.receive(message, std::chrono::milliseconds(500), &error);
                    if (received == network::WebSocketRecv::Timeout) continue;
                    if (received == network::WebSocketRecv::Message) {
                        auto out = session_.on_frame(message.data, Clock::now());
                        for (const auto& frame : out.frames) ws_.send_text(frame, std::chrono::seconds(10), nullptr);
                        if (out.ready) {
                            attempt = 0;
                            {
                                std::lock_guard<std::mutex> lock(status_mu_);
                                if (!out.bot_name.empty()) status_.display_name = out.bot_name;
                            }
                            set_status(LinkState::Connected, {});
                        }
                        for (const auto& [type, d] : out.dispatches) handle_dispatch(type, d);
                        if (out.reconnect) {
                            ws_.close(4000, "reconnect requested");
                            break;
                        }
                        continue;
                    }
                    if (stopping_) break;
                    const int code = received == network::WebSocketRecv::Closed ? ws_.close_code() : 1006;
                    const auto decision = session_.on_closed(code);
                    if (decision.action == CloseAction::Fatal) {
                        set_status(LinkState::Failed, decision.reason, true);
                        fatal = true;
                    } else {
                        if (decision.action == CloseAction::RefreshToken) api_.invalidate_token();
                        if (decision.action == CloseAction::RateLimited) extra_delay = options_.rate_limit_delay;
                        set_status(LinkState::Retrying, decision.reason);
                    }
                    break;
                }
                ws_.close();
                if (fatal) break;
            }
        }
        if (stopping_) break;
        const auto& backoff = options_.backoff;
        auto delay = backoff.empty() ? std::chrono::milliseconds(1000)
                                     : backoff[(std::min)(attempt, backoff.size() - 1)];
        delay = (std::max)(delay, extra_delay);
        ++attempt;
        if (!wait_for(delay)) break;
    }
    // 致命错误后保持 Failed 状态,线程退出;stop() 负责收尾。
    if (!stopping_) {
        std::unique_lock<std::mutex> lock(wake_mu_);
        wake_.wait(lock, [this] { return stopping_.load(); });
    }
}

bool QqTransport::seen_before(const std::string& message_id) {
    std::lock_guard<std::mutex> lock(seen_mu_);
    if (seen_.count(message_id)) return true;
    seen_.insert(message_id);
    seen_order_.push_back(message_id);
    while (seen_order_.size() > kSeenLimit) {
        seen_.erase(seen_order_.front());
        seen_order_.pop_front();
    }
    return false;
}

void QqTransport::handle_dispatch(const std::string& type, const nlohmann::json& d) {
    auto inbound = parse_message_event(type, d, options_.api.app_id, now_ms());
    if (!inbound || seen_before(inbound->message_id)) return;
    // 先用这条新消息的被动额度补发之前暂存的输出,再交给核心处理新消息。
    auto held = held_.take(inbound->address.key());
    if (!held.empty())
        LOG_INFO("[channels/qq] resending " + std::to_string(held.size()) + " held output(s) before the new message");
    bool first = true;
    for (auto& item : held) {
        if (first && !item.file) item.text = "(补发)" + item.text;
        first = false;
        deliver(inbound->address, item, inbound->reply_context, true);
    }
    if (callbacks_.on_inbound) callbacks_.on_inbound(std::move(*inbound));
}

ApiResult QqTransport::attempt_text(const std::string& scope, const std::string& target,
                                    const std::string& text, const ReplyPlan& plan) {
    auto body_for = [&](bool markdown, std::int64_t seq) {
        nlohmann::json body = markdown
            ? nlohmann::json{{"markdown", {{"content", text}}}, {"msg_type", kMsgTypeMarkdown}}
            : nlohmann::json{{"content", markdown_to_plain(text)}, {"msg_type", kMsgTypeText}};
        body["msg_seq"] = seq;
        if (plan.passive) body["msg_id"] = plan.msg_id;
        return body;
    };
    if (markdown_) {
        auto result = api_.send_message(scope, target, body_for(true, plan.msg_seq));
        if (result.ok || !markdown_rejected(result.error)) return result;
        // 该机器人不支持 Markdown:之后一律发纯文本。
        markdown_ = false;
        LOG_WARN("[channels/qq] markdown rejected, falling back to plain text");
    }
    return api_.send_message(scope, target, body_for(false, budget_.next_seq()));
}

ApiResult QqTransport::attempt(const Address& to, const HeldItem& item, const ReplyPlan& plan) {
    const auto [scope, target] = scope_of(to);
    if (!item.file) return attempt_text(scope, target, item.text, plan);

    ApiResult failure;
    std::error_code ec;
    const auto size = std::filesystem::file_size(item.path, ec);
    if (ec || size > options_.max_upload_bytes) {
        failure.error.message = ec ? "文件不存在或不可读" : "文件超过 QQ 上传上限";
        return failure;
    }
    std::ifstream in(item.path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() != size) {
        failure.error.message = "读取文件失败";
        return failure;
    }
    const auto uploaded = api_.upload_file(scope, target, file_type_for(item.mime_type, item.name),
                                           base64_encode(bytes), item.name);
    if (!uploaded.ok) return uploaded;
    const auto info = uploaded.response.value("file_info", std::string{});
    if (info.empty()) {
        failure.error.message = "QQ 上传结果缺少 file_info";
        return failure;
    }
    nlohmann::json body{{"content", ""}, {"msg_type", kMsgTypeMedia}, {"media", {{"file_info", info}}},
                        {"msg_seq", plan.msg_seq}};
    if (plan.passive) body["msg_id"] = plan.msg_id;
    return api_.send_message(scope, target, body);
}

SendResult QqTransport::deliver(const Address& to, const HeldItem& item, const nlohmann::json& reply_context,
                                bool flushing) {
    const auto key = to.key();
    if (!flushing && held_.size(key) > 0) {
        held_.push(key, item);
        return {SendOutcome::Held, {}};
    }
    auto plan = budget_.plan(reply_context, now_ms());
    auto result = attempt(to, item, plan);
    if (result.ok) return {SendOutcome::Sent, {}};
    if (plan.passive && client_error(result.error)) {
        // 被动回复被拒(窗口已过等):这条消息不再尝试被动,改发主动消息。
        LOG_INFO("[channels/qq] passive reply refused (" + redact_secrets(result.error.message, {options_.api.app_secret}) +
                 "), sending as an active message");
        budget_.exhaust(plan.msg_id);
        plan = ReplyPlan{false, {}, budget_.next_seq()};
        result = attempt(to, item, plan);
        if (result.ok) return {SendOutcome::Sent, {}};
    }
    if (!plan.passive && client_error(result.error)) {
        held_.push(key, item);
        LOG_WARN("[channels/qq] active message refused (" + redact_secrets(result.error.message, {options_.api.app_secret}) +
                 "), held until the contact's next message");
        return {SendOutcome::Held, result.error.message};
    }
    LOG_WARN("[channels/qq] send failed: " + redact_secrets(result.error.message, {options_.api.app_secret}));
    return {SendOutcome::Failed, result.error.message.empty() ? "QQ 发送失败" : result.error.message};
}

SendResult QqTransport::send_text(const Address& to, const std::string& text,
                                  const nlohmann::json& reply_context) {
    SendResult overall{SendOutcome::Sent, {}};
    for (const auto& chunk : chunk_text(text, kMaxTextChars, false)) {
        HeldItem item;
        item.text = chunk;
        const auto result = deliver(to, item, reply_context, false);
        if (result.outcome == SendOutcome::Failed) return result;
        if (result.outcome == SendOutcome::Held) overall = result;
    }
    return overall;
}

SendResult QqTransport::send_file(const Address& to, const std::filesystem::path& path, const std::string& name,
                                  const std::string& mime_type, const nlohmann::json& reply_context) {
    // 超过直传上限(分片上传尚未实现)时改发一条文字说明,对方至少知道有文件没发过去。
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec && size > options_.max_upload_bytes) {
        const auto limit_mib = std::to_string(options_.max_upload_bytes / (1024u * 1024u));
        LOG_INFO("[channels/qq] file of " + std::to_string(size) + " bytes is over the " + limit_mib +
                 " MiB direct upload limit, sending a text notice instead");
        return send_text(to, "文件「" + name + "」超过 QQ 直传上限(" + limit_mib + " MiB),未发送;请在 ACECode 中查看。",
                         reply_context);
    }
    HeldItem item;
    item.file = true;
    item.path = path;
    item.name = name;
    item.mime_type = mime_type;
    return deliver(to, item, reply_context, false);
}

bool QqTransport::download(const Attachment& attachment, const std::filesystem::path& dest, std::string* error) {
    if (attachment.remote_ref.empty()) {
        if (error) *error = "附件缺少下载地址";
        return false;
    }
    if (attachment.size > options_.max_download_bytes) {
        if (error) *error = "附件超过大小限制";
        return false;
    }
    return api_.download(attachment.remote_ref, dest, options_.max_download_bytes, error);
}

} // namespace acecode::im::qqbot
