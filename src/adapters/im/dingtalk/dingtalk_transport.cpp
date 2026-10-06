#include "dingtalk_transport.hpp"

#include "im/redact.hpp"
#include "im/text_chunk.hpp"
#include "utils/logger.hpp"

#include <algorithm>

namespace acecode::im::dingtalk {
namespace {

constexpr std::chrono::seconds kAckTimeout{10};

std::int64_t wall_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

SendResult sent() { return {SendOutcome::Sent, {}}; }
SendResult failed(const std::string& error) { return {SendOutcome::Failed, error}; }
SendResult held(const std::string& error) { return {SendOutcome::Held, error}; }

// OpenAPI 明确拒绝(4xx 或 200 但未送达)且不是限流:等对方下一条消息带来新 webhook 时补发。
bool refused(const ApiError& error) {
    return error.status >= 200 && error.status < 500 && throttle_kind(error) == Throttle::None;
}

std::string markdown_escape_label(const std::string& name) {
    std::string out;
    for (const char c : name) {
        if (c == '[' || c == ']' || c == '(' || c == ')' || c == '\n' || c == '\r') continue;
        out.push_back(c);
    }
    return out.empty() ? std::string("image") : out;
}

} // namespace

DingTalkTransport::DingTalkTransport(DingTalkTransportOptions options)
    : options_(std::move(options)),
      api_(options_.api),
      session_(options_.idle_timeout),
      limiter_(options_.limits),
      rng_(static_cast<std::minstd_rand::result_type>(Clock::now().time_since_epoch().count())) {
    status_.account = options_.api.client_id;
    status_.extra["robot_code"] = api_.robot_code();
}

DingTalkTransport::~DingTalkTransport() { stop(); }

Capabilities DingTalkTransport::capabilities() const {
    Capabilities caps;
    caps.max_text_units = kMaxTextChars;
    caps.count_utf16 = false;
    // 每会话每分钟最多 20 条且没有编辑接口:同一回合的输出合并成尽量少的消息。
    caps.batch_turn_output = true;
    caps.supports_typing = false;
    caps.max_upload_bytes = options_.max_upload_bytes;
    caps.max_download_bytes = options_.max_download_bytes;
    return caps;
}

void DingTalkTransport::start(TransportCallbacks callbacks) {
    if (running_.exchange(true)) return;
    callbacks_ = std::move(callbacks);
    stopping_ = false;
    reader_ = acecode::JoiningThread(&DingTalkTransport::run, this);
    dispatcher_ = acecode::JoiningThread(&DingTalkTransport::dispatch_loop, this);
}

void DingTalkTransport::stop() {
    if (!running_.exchange(false)) return;
    {
        // 置位与通知都在各自的锁内完成,等待方不会错过唤醒。
        std::lock_guard<std::mutex> lock(wake_mu_);
        stopping_ = true;
    }
    ws_.abort();
    wake_.notify_all();
    {
        // 分发线程在 inbox_mu_ 下检查 stopping_;先拿一次这把锁再通知,避免它错过唤醒。
        std::lock_guard<std::mutex> lock(inbox_mu_);
    }
    inbox_cv_.notify_all();
    if (reader_.joinable()) reader_.join();
    if (dispatcher_.joinable()) dispatcher_.join();
    ws_.close();
    {
        std::lock_guard<std::mutex> lock(inbox_mu_);
        inbox_.clear();  // 未处理的消息已回执;平台不会重投,只能丢弃并记日志
    }
    set_status(LinkState::Stopped, {});
}

TransportStatus DingTalkTransport::status() const {
    std::lock_guard<std::mutex> lock(status_mu_);
    auto copy = status_;
    copy.extra["held"] = held_count();
    return copy;
}

void DingTalkTransport::set_status(LinkState state, const std::string& detail, bool retry_stopped) {
    TransportStatus snapshot;
    {
        std::lock_guard<std::mutex> lock(status_mu_);
        const auto clean = redact_secrets(detail, {options_.api.client_secret});
        if (status_.state == state && status_.detail == clean && status_.retry_stopped == retry_stopped) return;
        status_.state = state;
        status_.detail = clean;
        status_.retry_stopped = retry_stopped;
        snapshot = status_;
    }
    if (callbacks_.on_status) callbacks_.on_status(snapshot);
}

bool DingTalkTransport::wait_for(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(wake_mu_);
    wake_.wait_for(lock, duration, [this] { return stopping_.load(); });
    return !stopping_;
}

bool DingTalkTransport::sleep_until(Clock::time_point at) {
    const auto now = Clock::now();
    if (at <= now) return true;
    return wait_for(std::chrono::duration_cast<std::chrono::milliseconds>(at - now) + std::chrono::milliseconds(1));
}

std::chrono::milliseconds DingTalkTransport::next_backoff(std::size_t attempt) {
    std::chrono::milliseconds jitter{0};
    if (options_.jitter) jitter = std::chrono::milliseconds(static_cast<long long>(rng_() % 1000));
    return stream_backoff(attempt, options_.backoff_base, options_.backoff_cap, jitter);
}

std::size_t DingTalkTransport::pending_count() {
    std::lock_guard<std::mutex> lock(inbox_mu_);
    return inbox_.size();
}

void DingTalkTransport::run() {
    std::size_t attempt = 0;
    while (!stopping_) {
        set_status(attempt == 0 ? LinkState::Connecting : LinkState::Retrying,
                   attempt == 0 ? "正在连接钉钉" : "正在重新连接钉钉");
        bool immediate = false;
        const auto ticket = api_.open_stream(options_.open_timeout);
        if (!ticket.ok) {
            if (ticket.auth_failed || ticket.rejected) {
                // 凭据无效 / 应用未发布等不会自己好:停止重试,等用户处理后重新打开。
                set_status(LinkState::Failed, ticket.reason, true);
                break;
            }
            set_status(LinkState::Retrying, ticket.reason);
        } else {
            network::WebSocketConnectOptions ws_options;
            // ticket 一次性有效;每次连接都重新注册,绝不复用。不带任何自定义头。
            ws_options.url = ticket.endpoint + (ticket.endpoint.find('?') == std::string::npos ? "?" : "&") +
                             "ticket=" + url_encode(ticket.ticket);
            ws_options.connect_timeout = options_.open_timeout;
            ws_options.use_proxy = options_.api.use_proxy;
            std::string error;
            if (!ws_.connect(ws_options, &error)) {
                LOG_WARN("[channels/dingtalk] stream handshake failed: " +
                         redact_secrets(error, {options_.api.client_secret, ticket.ticket}));
                set_status(LinkState::Retrying, "无法连接钉钉 Stream 服务,稍后重试");
            } else {
                session_.on_connected(Clock::now());
                LOG_INFO("[channels/dingtalk] stream connected");
                set_status(LinkState::Connected, {});
                std::string reason;
                const auto end = read_loop(&reason);
                ws_.close();
                if (end == ReadEnd::Stopped || stopping_) break;
                const bool got_frames = session_.frames() > 0;
                const bool healthy = got_frames && Clock::now() - session_.connected_at() >= options_.healthy_after;
                if (got_frames) attempt = 0;  // 新连接收到过帧:退避从头计数
                immediate = healthy && (end == ReadEnd::Disconnect || end == ReadEnd::Closed);
                set_status(LinkState::Retrying, reason);
            }
        }
        if (stopping_) break;
        if (immediate) continue;
        const auto delay = next_backoff(attempt);
        ++attempt;
        if (!wait_for(delay)) break;
    }
    // 致命错误后保持 Failed 状态,线程退出前等 stop();stop() 负责收尾。
    if (!stopping_) {
        std::unique_lock<std::mutex> lock(wake_mu_);
        wake_.wait(lock, [this] { return stopping_.load(); });
    }
}

DingTalkTransport::ReadEnd DingTalkTransport::read_loop(std::string* reason) {
    while (!stopping_) {
        if (session_.idle_expired(Clock::now())) {
            LOG_WARN("[channels/dingtalk] no stream frame for too long, reconnecting");
            *reason = "钉钉连接长时间无响应,正在重连";
            return ReadEnd::Idle;
        }
        network::WebSocketMessage message;
        std::string error;
        const auto received = ws_.receive(message, std::chrono::milliseconds(500), &error);
        if (received == network::WebSocketRecv::Timeout) continue;
        if (received == network::WebSocketRecv::Message) {
            if (message.binary) continue;  // 应用层流量全是文本帧
            const bool accept = pending_count() < options_.max_pending_inbound;
            auto out = session_.on_frame(message.data, Clock::now(), accept);
            // 先回执,再处理:回执只能发回送来该帧的这条连接。
            for (const auto& reply : out.replies) {
                std::string send_error;
                if (!ws_.send_text(reply, kAckTimeout, &send_error))
                    LOG_WARN("[channels/dingtalk] stream ack failed: " + send_error);
            }
            if (out.invalid)
                LOG_WARN("[channels/dingtalk] ignored an unparsable stream frame (" +
                         std::to_string(message.data.size()) + " bytes)");
            if (!out.system_topic.empty() && out.system_topic != "ping")
                LOG_INFO("[channels/dingtalk] stream system frame: " + out.system_topic);
            if (!out.ignored_topic.empty())
                LOG_INFO("[channels/dingtalk] ignored unsubscribed stream topic: " + out.ignored_topic);
            if (out.deferred)
                LOG_WARN("[channels/dingtalk] inbound queue full, leaving a robot message for redelivery");
            bool queued = false;
            for (auto& item : out.messages) {
                if (!item.message_id.empty() && frames_seen_.seen(item.message_id)) {
                    LOG_DEBUG("[channels/dingtalk] duplicate stream delivery skipped");
                    continue;
                }
                std::lock_guard<std::mutex> lock(inbox_mu_);
                inbox_.push_back({std::move(item.message_id), std::move(item.data)});
                queued = true;
            }
            if (queued) inbox_cv_.notify_all();
            if (out.disconnect) {
                // 平台下线 / 负载切换的正常指令:回执已发,关闭后立即用新 ticket 重连。
                LOG_INFO("[channels/dingtalk] server requested reconnect");
                *reason = "钉钉要求重新连接";
                return ReadEnd::Disconnect;
            }
            continue;
        }
        if (stopping_) break;
        if (received == network::WebSocketRecv::Closed) {
            LOG_INFO("[channels/dingtalk] stream closed by server (code " + std::to_string(ws_.close_code()) + ")");
            *reason = "钉钉连接已断开,正在重连";
            return ReadEnd::Closed;
        }
        LOG_WARN("[channels/dingtalk] stream receive failed: " + redact_secrets(error, {options_.api.client_secret}));
        *reason = "钉钉连接出错,正在重连";
        return ReadEnd::Error;
    }
    return ReadEnd::Stopped;
}

void DingTalkTransport::dispatch_loop() {
    while (true) {
        Pending pending;
        {
            std::unique_lock<std::mutex> lock(inbox_mu_);
            inbox_cv_.wait(lock, [this] { return stopping_.load() || !inbox_.empty(); });
            if (stopping_) break;
            pending = std::move(inbox_.front());
            inbox_.pop_front();
        }
        try {
            handle_message(pending);
        } catch (const std::exception& e) {
            LOG_WARN(std::string("[channels/dingtalk] cannot handle robot message: ") + e.what());
        }
    }
}

void DingTalkTransport::handle_message(const Pending& pending) {
    nlohmann::json data;
    try {
        data = nlohmann::json::parse(pending.data);
    } catch (...) {
        LOG_WARN("[channels/dingtalk] robot message payload is not JSON (" + std::to_string(pending.data.size()) +
                 " bytes)");
        return;
    }
    auto inbound = parse_robot_message(data, options_.api.client_id, pending.message_id);
    if (!inbound) {
        LOG_DEBUG("[channels/dingtalk] robot message without usable content skipped");
        return;
    }
    // 重投的消息帧头 messageId 变了,msgId 不变:这里拦第二层。
    if (messages_seen_.seen(inbound->message_id)) {
        LOG_DEBUG("[channels/dingtalk] duplicate robot message skipped");
        return;
    }
    routes_.remember(inbound->address.key(), route_from_context(inbound->reply_context));
    // 先用这条新消息的 webhook 补发之前暂存的输出,再交给核心处理新消息,保证顺序。
    if (has_held(inbound->address.key())) flush_held(inbound->address, inbound->reply_context);
    if (callbacks_.on_inbound) callbacks_.on_inbound(std::move(*inbound));
}

std::shared_ptr<std::mutex> DingTalkTransport::chat_lock(const std::string& key) {
    std::lock_guard<std::mutex> lock(locks_mu_);
    auto& slot = chat_locks_[key];
    if (!slot) slot = std::make_shared<std::mutex>();
    return slot;
}

bool DingTalkTransport::has_held(const std::string& key) const {
    std::lock_guard<std::mutex> lock(held_mu_);
    const auto it = held_.find(key);
    return it != held_.end() && !it->second.empty();
}

void DingTalkTransport::push_held(const std::string& key, std::vector<HeldItem> items) {
    if (items.empty()) return;
    std::lock_guard<std::mutex> lock(held_mu_);
    auto& queue = held_[key];
    for (auto& item : items) queue.push_back(std::move(item));
}

std::size_t DingTalkTransport::held_count() const {
    std::lock_guard<std::mutex> lock(held_mu_);
    std::size_t total = 0;
    for (const auto& [key, queue] : held_) total += queue.size();
    return total;
}

void DingTalkTransport::flush_held(const Address& to, const nlohmann::json& reply_context) {
    const auto key = to.key();
    const auto guard = chat_lock(key);
    std::lock_guard<std::mutex> lock(*guard);
    std::deque<HeldItem> items;
    {
        std::lock_guard<std::mutex> held_lock(held_mu_);
        const auto it = held_.find(key);
        if (it == held_.end()) return;
        items.swap(it->second);
        held_.erase(it);
    }
    LOG_INFO("[channels/dingtalk] resending " + std::to_string(items.size()) +
             " held output(s) before the new message");
    bool first = true;
    while (!items.empty()) {
        auto item = items.front();
        if (first && !item.file) item.text = "(补发)" + item.text;
        first = false;
        const auto result = item.file ? send_file_locked(to, item, reply_context)
                                      : deliver_markdown(to, item.text, reply_context);
        if (result.outcome == SendOutcome::Held) {
            if (item.file) {
                // 文件只能走 OpenAPI;仍被拒时改发一句说明,不再无限暂存。
                deliver_markdown(to, "文件「" + item.name + "」未能通过钉钉发送,请在 ACECode 中查看。", reply_context);
                items.pop_front();
                continue;
            }
            std::lock_guard<std::mutex> held_lock(held_mu_);
            auto& queue = held_[key];
            for (auto it = items.rbegin(); it != items.rend(); ++it) queue.push_front(*it);
            return;
        }
        if (result.outcome == SendOutcome::Failed)
            LOG_WARN("[channels/dingtalk] held output dropped after resend failed: " + result.error);
        items.pop_front();
    }
}

template <typename Send>
ApiResult DingTalkTransport::throttled(const std::string& chat, Send&& send) {
    ApiResult result;
    auto rate_wait = options_.throttle_delay;
    for (int attempt = 0; attempt <= options_.max_send_retries; ++attempt) {
        if (!sleep_until(limiter_.reserve(chat, Clock::now()))) {
            result = ApiResult{};
            result.error.status = 0;
            result.error.message = "钉钉通道已停止";
            return result;
        }
        result = send();
        if (result.ok) return result;
        const auto kind = throttle_kind(result.error);
        if (kind == Throttle::None) return result;
        // 平台明确拒收了这条:等待后重发同一条是安全的;同一会话的发送由会话锁串行,不会乱序。
        std::chrono::milliseconds wait = options_.qps_delay;
        if (kind == Throttle::Rate) {
            wait = rate_wait;
            rate_wait = std::min(rate_wait * 2, options_.throttle_cap);
        }
        LOG_INFO("[channels/dingtalk] send throttled, retrying in " +
                 std::to_string(std::chrono::duration_cast<std::chrono::seconds>(wait).count()) + "s");
        limiter_.penalize(chat, wait, Clock::now());
    }
    return result;
}

std::string DingTalkTransport::staff_for(const Address& to, const Route& route) const {
    if (!route.staff_id.empty()) return route.staff_id;
    if (to.kind == ChatKind::Private && !is_encrypted_sender_id(to.chat)) return to.chat;
    return {};
}

SendResult DingTalkTransport::deliver_openapi(const Address& to, const Route& route, const std::string& msg_key,
                                              const nlohmann::json& msg_param) {
    const auto robot = route.robot_code.empty() ? api_.robot_code() : route.robot_code;
    ApiResult result;
    if (to.kind == ChatKind::Group) {
        result = throttled(to.chat, [&api = api_, &robot, &to, &msg_key, &msg_param] {
            return api.send_group(robot, to.chat, msg_key, msg_param);
        });
    } else {
        const auto staff = staff_for(to, route);
        if (staff.empty()) {
            LOG_INFO("[channels/dingtalk] no valid session webhook for an external user, holding output");
            return held("对方不是本组织成员,会话回复地址已过期;对方下一条消息到来时补发");
        }
        result = throttled(to.chat, [&api = api_, &robot, &staff, &msg_key, &msg_param] {
            return api.send_oto(robot, staff, msg_key, msg_param);
        });
    }
    if (result.ok) return sent();
    const auto reason = redact_secrets(describe_api_error(result.error), api_.secrets());
    if (refused(result.error)) {
        LOG_WARN("[channels/dingtalk] robot API refused the message, holding it until the next inbound message");
        return held(reason);
    }
    return failed(reason);
}

SendResult DingTalkTransport::deliver_markdown(const Address& to, const std::string& markdown,
                                               const nlohmann::json& reply_context) {
    const auto title = markdown_title(markdown);
    const auto route = routes_.resolve(to.key(), reply_context, wall_now_ms(), options_.webhook_margin.count());
    if (!route.webhook.empty()) {
        const nlohmann::json body{{"msgtype", "markdown"}, {"markdown", {{"title", title}, {"text", markdown}}}};
        const auto result = throttled(to.chat, [&api = api_, &route, &body] { return api.send_webhook(route.webhook, body); });
        if (result.ok) return sent();
        if (!is_webhook_gone(result.error))
            return failed(redact_secrets(describe_api_error(result.error), api_.secrets()));
        LOG_INFO("[channels/dingtalk] session webhook expired, falling back to the robot API");
        routes_.mark_webhook_dead(route.webhook);
    }
    return deliver_openapi(to, route, "sampleMarkdown", {{"title", title}, {"text", markdown}});
}

SendResult DingTalkTransport::send_text_locked(const Address& to, const std::string& text,
                                               const nlohmann::json& reply_context) {
    const auto key = to.key();
    const auto chunks = chunk_text(normalize_markdown(text), kMaxTextChars, false);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        if (has_held(key)) {
            // 该会话已有暂存:新输出排在后面,保证顺序。
            std::vector<HeldItem> rest;
            for (std::size_t j = i; j < chunks.size(); ++j) rest.push_back(HeldItem{false, chunks[j], {}, {}, {}});
            push_held(key, std::move(rest));
            return held({});
        }
        const auto result = deliver_markdown(to, chunks[i], reply_context);
        if (result.outcome == SendOutcome::Failed) return result;
        if (result.outcome == SendOutcome::Held) {
            std::vector<HeldItem> rest;
            for (std::size_t j = i; j < chunks.size(); ++j) rest.push_back(HeldItem{false, chunks[j], {}, {}, {}});
            push_held(key, std::move(rest));
            return result;
        }
    }
    return sent();
}

SendResult DingTalkTransport::send_text(const Address& to, const std::string& text,
                                        const nlohmann::json& reply_context) {
    const auto guard = chat_lock(to.key());
    std::lock_guard<std::mutex> lock(*guard);
    return send_text_locked(to, text, reply_context);
}

SendResult DingTalkTransport::send_file_locked(const Address& to, const HeldItem& item,
                                               const nlohmann::json& reply_context) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(item.path, ec);
    if (ec) return failed("文件不存在或不可读");
    if (size > options_.max_upload_bytes) {
        // 分片上传尚未实现:改发一条文字说明,对方至少知道有文件没发过去。
        const auto limit_mib = std::to_string(options_.max_upload_bytes / (1024u * 1024u));
        LOG_INFO("[channels/dingtalk] file of " + std::to_string(size) + " bytes is over the " + limit_mib +
                 " MiB upload limit, sending a text notice instead");
        return deliver_markdown(to, "文件「" + item.name + "」超过钉钉上传上限(" + limit_mib + " MiB),未发送;请在 ACECode 中查看。",
                                reply_context);
    }
    if (is_image_file(item.mime_type, item.name)) {
        // 图片以 mediaId 嵌进 Markdown:webhook 与 OpenAPI 都能发,也不额外消耗 OpenAPI 额度。
        const auto uploaded = api_.upload_media("image", item.path, item.name, item.mime_type);
        const auto media = uploaded.response.value("media_id", std::string{});
        if (uploaded.ok && !media.empty())
            return deliver_markdown(to, "![" + markdown_escape_label(item.name) + "](" + media + ")", reply_context);
        LOG_WARN("[channels/dingtalk] image upload failed, retrying as a plain file");
    }
    const auto route = routes_.resolve(to.key(), reply_context, wall_now_ms(), options_.webhook_margin.count());
    if (to.kind == ChatKind::Private && staff_for(to, route).empty()) {
        // 组织外用户只能经 webhook 收文字。
        return deliver_markdown(to, "文件「" + item.name + "」无法发送给组织外的钉钉用户,请在 ACECode 中查看。",
                                reply_context);
    }
    const auto uploaded = api_.upload_media("file", item.path, item.name, item.mime_type);
    const auto media = uploaded.response.value("media_id", std::string{});
    if (!uploaded.ok || media.empty())
        return failed("上传文件失败:" + redact_secrets(describe_api_error(uploaded.error), api_.secrets()));
    const auto ext = file_extension(item.name);
    return deliver_openapi(to, route, "sampleFile",
                           {{"mediaId", media}, {"fileName", item.name}, {"fileType", ext.empty() ? "file" : ext}});
}

SendResult DingTalkTransport::send_file(const Address& to, const std::filesystem::path& path, const std::string& name,
                                        const std::string& mime_type, const nlohmann::json& reply_context) {
    const auto key = to.key();
    const auto guard = chat_lock(key);
    std::lock_guard<std::mutex> lock(*guard);
    HeldItem item{true, {}, path, name, mime_type};
    if (has_held(key)) {
        push_held(key, {item});
        return held({});
    }
    auto result = send_file_locked(to, item, reply_context);
    if (result.outcome == SendOutcome::Held) push_held(key, {item});
    return result;
}

bool DingTalkTransport::download(const Attachment& attachment, const std::filesystem::path& dest,
                                 std::string* error) {
    if (attachment.remote_ref.empty()) {
        if (error) *error = "附件缺少下载码";
        return false;
    }
    if (attachment.size > options_.max_download_bytes) {
        if (error) *error = "附件超过大小限制";
        return false;
    }
    return api_.download(attachment.remote_ref, api_.robot_code(), dest, options_.max_download_bytes, error);
}

} // namespace acecode::im::dingtalk
