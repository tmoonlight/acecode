#include "im/feishu/feishu_transport.hpp"

#include "im/markdown.hpp"
#include "im/redact.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"

#include <algorithm>
#include <thread>

namespace acecode::im::feishu {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kSeenLimit = 2048;
constexpr auto kWriteTimeout = std::chrono::seconds(10);

std::int64_t epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string ms_text(std::chrono::milliseconds value) { return std::to_string(value.count()) + "ms"; }

// WebSocketClient 的错误文本形如 "... (HTTP 403)";取出状态码,没有则为 0。
long http_status_in(const std::string& error) {
    const auto pos = error.find("HTTP ");
    if (pos == std::string::npos) return 0;
    long status = 0;
    for (std::size_t i = pos + 5; i < error.size() && error[i] >= '0' && error[i] <= '9' && status < 1000; ++i)
        status = status * 10 + (error[i] - '0');
    return status;
}

// 消息 id 来自核心保存的 reply_context,拼进路径前做百分号编码。
std::string path_segment(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : value) {
        const auto u = static_cast<unsigned char>(c);
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-' || u == '_' ||
            u == '.' || u == '~') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[u >> 4]);
            out.push_back(kHex[u & 0x0F]);
        }
    }
    return out;
}

std::string result_brief(const ApiResult& result) {
    return "HTTP " + std::to_string(result.status) + ", code " + std::to_string(result.code) +
           (result.log_id.empty() ? std::string{} : ", logid " + result.log_id);
}

} // namespace

FeishuTransport::FeishuTransport(FeishuTransportOptions options)
    : options_(std::move(options)),
      api_(options_.api),
      pacer_(options_.send_gap),
      session_(options_.link_limits) {
    status_.account = options_.api.app_id;
}

FeishuTransport::~FeishuTransport() { stop(); }

Capabilities FeishuTransport::capabilities() const {
    Capabilities caps;
    caps.max_text_units = kMaxTextChars;
    caps.count_utf16 = false;
    caps.batch_turn_output = false;
    caps.supports_typing = false;
    caps.max_upload_bytes = options_.max_upload_bytes;
    caps.max_download_bytes = options_.max_download_bytes;
    return caps;
}

void FeishuTransport::start(TransportCallbacks callbacks) {
    if (running_.exchange(true)) return;
    callbacks_ = std::move(callbacks);
    stopping_ = false;
    rng_.seed(std::random_device{}());
    worker_ = acecode::JoiningThread(&FeishuTransport::run, this);
}

void FeishuTransport::stop() {
    if (!running_.exchange(false)) return;
    stopping_ = true;
    wake_.notify_all();
    // 不调用 ws_.abort():读循环最多 receive_poll 后看到停止标记,先发 close 帧再退出。
    if (worker_.joinable()) worker_.join();
    ws_.close();
    set_status(LinkState::Stopped, {});
}

TransportStatus FeishuTransport::status() const {
    std::lock_guard<std::mutex> lock(status_mu_);
    return status_;
}

BotIdentity FeishuTransport::bot() const {
    std::lock_guard<std::mutex> lock(bot_mu_);
    return bot_;
}

void FeishuTransport::set_status(LinkState state, const std::string& detail, bool retry_stopped, bool force) {
    TransportStatus snapshot;
    {
        std::lock_guard<std::mutex> lock(status_mu_);
        const auto clean = redact_secrets(detail, {options_.api.app_secret});
        if (!force && status_.state == state && status_.detail == clean && status_.retry_stopped == retry_stopped)
            return;
        status_.state = state;
        status_.detail = clean;
        status_.retry_stopped = retry_stopped;
        snapshot = status_;
    }
    if (callbacks_.on_status) callbacks_.on_status(snapshot);
}

bool FeishuTransport::wait_for(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(wake_mu_);
    wake_.wait_for(lock, duration, [this] { return stopping_.load(); });
    return !stopping_;
}

void FeishuTransport::run() {
    std::size_t failures = 0;
    bool first = true;
    while (!stopping_) {
        set_status(first ? LinkState::Connecting : LinkState::Retrying,
                   first ? "正在连接飞书" : "正在重新连接飞书");
        first = false;
        const auto attempt = connect_and_serve();
        if (stopping_ || attempt.outcome == Outcome::Stopped) break;
        if (attempt.outcome == Outcome::Fatal) {
            // 凭据无效、连接数超限等不会自己好:停止重试,等用户处理后重新打开。
            LOG_WARN("[channels/feishu] connection failed permanently; retry stopped");
            set_status(LinkState::Failed, attempt.message, true);
            break;
        }
        if (attempt.outcome == Outcome::Dropped) failures = 0;
        else ++failures;
        if (reconnect_exhausted(session_.config(), server_config_, failures)) {
            LOG_WARN("[channels/feishu] reconnect attempts exhausted (server ReconnectCount)");
            set_status(LinkState::Failed, "多次重连飞书失败,已停止重试", true);
            break;
        }
        set_status(LinkState::Retrying, attempt.message);
        const double jitter = std::uniform_real_distribution<double>(0.0, 1.0)(rng_);
        const auto delay = reconnect_delay(session_.config(), server_config_, failures, options_.backoff, jitter);
        LOG_INFO("[channels/feishu] reconnecting in " + ms_text(delay));
        if (!wait_for(delay)) break;
    }
    // 致命错误后保持 Failed 状态,等 stop() 收尾。
    if (!stopping_) {
        std::unique_lock<std::mutex> lock(wake_mu_);
        wake_.wait(lock, [this] { return stopping_.load(); });
    }
}

void FeishuTransport::refresh_bot() {
    std::string error;
    const auto info = api_.bot_info(&error, &stopping_);
    bool ready = false;
    int activate = -1;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(bot_mu_);
        bot_checked_ = Clock::now();
        if (info.ok) {
            bot_.open_id = info.open_id;
            bot_.name = info.name;
            activate_status_ = info.activate_status;
        }
        ready = info.ok ? info.ready() : (!bot_.open_id.empty() && activate_status_ == 2);
        activate = activate_status_;
        name = bot_.name;
    }
    {
        std::lock_guard<std::mutex> lock(status_mu_);
        if (!name.empty()) status_.display_name = name;
        status_.extra["bot_ready"] = ready;
        status_.extra["activate_status"] = activate;
        if (ready) {
            status_.extra.erase("bot_warning");
        } else {
            status_.extra["bot_warning"] = info.ok ? describe_activate_status(info.activate_status)
                                                   : (error.empty() ? describe_activate_status(-1) : error);
        }
    }
    if (!ready)
        LOG_WARN("[channels/feishu] bot identity unavailable or bot inactive (activate_status=" +
                 std::to_string(activate) + "); group @ detection falls back to mention type");
}

FeishuTransport::Attempt FeishuTransport::connect_and_serve() {
    std::string error;
    if (api_.tenant_token(&error, &stopping_).empty()) {
        if (stopping_) return {Outcome::Stopped, {}};
        if (api_.last_token_auth_failed()) return {Outcome::Fatal, error};
        LOG_WARN("[channels/feishu] tenant token unavailable");
        return {Outcome::Failed, error};
    }
    // 每次连接都重新取机器人身份:应用迁移或重新发布后,旧的 open_id 会让群 @ 判定失效。
    refresh_bot();
    if (stopping_) return {Outcome::Stopped, {}};

    const auto endpoint = api_.ws_endpoint(&stopping_);
    if (stopping_) return {Outcome::Stopped, {}};
    if (!endpoint.client_config.is_null()) {
        auto config = session_.config();
        if (apply_client_config(endpoint.client_config, &config)) {
            session_.set_config(config);
            server_config_ = true;
        }
    }
    if (endpoint.fatal) {
        LOG_WARN("[channels/feishu] endpoint rejected the app (code " + std::to_string(endpoint.code) + ")");
        return {Outcome::Fatal, endpoint.message};
    }
    if (!endpoint.ok) {
        LOG_WARN("[channels/feishu] endpoint unavailable (HTTP " + std::to_string(endpoint.status) + ", code " +
                 std::to_string(endpoint.code) + ")");
        return {Outcome::Failed, endpoint.message};
    }

    network::WebSocketConnectOptions ws_options;
    ws_options.url = endpoint.url;  // 鉴权在 URL 查询参数里;不加任何额外请求头
    ws_options.use_proxy = options_.api.use_proxy;
    if (!ws_.connect(ws_options, &error)) {
        const auto query = endpoint.url.find('?');
        const auto secrets = std::vector<std::string>{
            options_.api.app_secret, endpoint.url,
            query == std::string::npos ? std::string{} : endpoint.url.substr(query + 1)};
        LOG_WARN("[channels/feishu] long connection handshake failed: " + redact_secrets(error, secrets));
        // 拿不到 Handshake-Status 响应头时一律按可重试处理(与 Go SDK 一致)。
        const auto decision = classify_handshake(http_status_in(error), 0, 0);
        return {decision.fatal ? Outcome::Fatal : Outcome::Failed, decision.message};
    }
    LOG_INFO("[channels/feishu] long connection established (device " + endpoint.device_id + ")");
    session_.on_connected(endpoint.service_id, Clock::now());
    set_status(LinkState::Connected, {});

    Attempt attempt{Outcome::Dropped, "飞书长连接已断开,正在重连"};
    serve_connection(attempt);
    ws_.close(1000, "bye");  // close 帧很重要:否则平台会继续把消息投给这条已断开的连接
    session_.on_disconnected();
    if (stopping_) return {Outcome::Stopped, {}};
    return attempt;
}

void FeishuTransport::serve_connection(Attempt& attempt) {
    std::string error;
    while (!stopping_) {
        const auto now = Clock::now();
        const auto tick = session_.on_tick(now);
        if (tick.dead) {
            LOG_WARN("[channels/feishu] no frame within the read timeout, reconnecting");
            attempt.message = "飞书长连接无响应,正在重连";
            return;
        }
        if (tick.ping && !ws_.send_binary(*tick.ping, kWriteTimeout, &error)) {
            LOG_WARN("[channels/feishu] ping write failed, reconnecting");
            return;
        }
        bool bot_ready = true;
        bool bot_due = false;
        {
            std::lock_guard<std::mutex> lock(bot_mu_);
            bot_ready = !bot_.open_id.empty() && activate_status_ == 2;
            bot_due = now - bot_checked_ >= options_.bot_refresh_interval;
        }
        if (!bot_ready && bot_due) {
            // 用户在后台发布版本后,机器人身份才可用;不重连也要能补上。
            refresh_bot();
            set_status(LinkState::Connected, {}, false, true);
        }

        network::WebSocketMessage message;
        const auto received = ws_.receive(message, options_.receive_poll, &error);
        if (received == network::WebSocketRecv::Timeout) continue;
        if (received == network::WebSocketRecv::Message) {
            if (!message.binary) {
                LOG_WARN("[channels/feishu] ignored a non-binary frame");
                continue;
            }
            auto out = session_.on_message(message.data, Clock::now());
            if (!out.warning.empty()) LOG_WARN("[channels/feishu] " + out.warning);
            if (out.config_updated)
                LOG_INFO("[channels/feishu] server updated ClientConfig (ping " +
                         std::to_string(session_.config().ping_interval_s) + "s)");
            bool write_failed = false;
            for (const auto& frame : out.frames) {
                if (!ws_.send_binary(frame, kWriteTimeout, &error)) write_failed = true;
            }
            for (const auto& event : out.events) handle_event(event);
            if (write_failed) {
                LOG_WARN("[channels/feishu] ack write failed, reconnecting");
                return;
            }
            continue;
        }
        if (stopping_) return;
        const int code = received == network::WebSocketRecv::Closed ? ws_.close_code() : 1006;
        LOG_INFO("[channels/feishu] long connection closed (code " + std::to_string(code) + ")");
        return;
    }
}

bool FeishuTransport::seen_before(const std::string& message_id) {
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

void FeishuTransport::handle_event(const std::string& payload) {
    auto parsed = parse_event(payload, bot(), options_.api.app_id, epoch_ms());
    switch (parsed.disposition) {
        case EventDisposition::Message:
            // 平台至少投递一次,重连或 ACK 超时后会重投;按消息 id 去重。
            if (seen_before(parsed.message_id)) {
                LOG_DEBUG("[channels/feishu] duplicate message " + parsed.message_id + " ignored");
                return;
            }
            if (callbacks_.on_inbound) callbacks_.on_inbound(std::move(*parsed.inbound));
            return;
        case EventDisposition::Stale:
            LOG_INFO("[channels/feishu] dropped stale message " + parsed.message_id + " (older than 30 minutes)");
            return;
        case EventDisposition::FromBot:
            LOG_DEBUG("[channels/feishu] ignored a message sent by a bot");
            return;
        case EventDisposition::Empty:
            LOG_DEBUG("[channels/feishu] ignored an empty message " + parsed.message_id);
            return;
        case EventDisposition::Invalid:
            LOG_WARN("[channels/feishu] ignored a malformed event (" + parsed.event_type + ")");
            return;
        case EventDisposition::Ignored:
            LOG_DEBUG("[channels/feishu] ignored event " + parsed.event_type);
            return;
    }
}

ApiResult FeishuTransport::with_retry(const std::string& pace_key, const std::function<ApiResult()>& attempt) {
    std::size_t limited = 0;
    std::size_t transient = 0;
    while (true) {
        if (!pace_key.empty()) {
            const auto at = pacer_.reserve(pace_key, Clock::now());
            const auto now = Clock::now();
            if (at > now) std::this_thread::sleep_for(at - now);
        }
        auto result = attempt();
        if (result.ok) return result;
        if (is_rate_limited(result) && limited < options_.rate_limit_backoff.size()) {
            // 限流表示平台没有收下这条消息,等待后重发是安全的;同一调用线程串行,顺序不乱。
            const auto wait = rate_limit_wait(result, options_.rate_limit_backoff[limited++]);
            LOG_INFO("[channels/feishu] rate limited (" + result_brief(result) + "), retrying in " + ms_text(wait));
            if (pace_key.empty()) std::this_thread::sleep_for(wait);
            else pacer_.penalize(pace_key, wait, Clock::now());
            continue;
        }
        if (is_transient(result) && transient < options_.retry_backoff.size()) {
            const auto wait = options_.retry_backoff[transient++];
            LOG_INFO("[channels/feishu] request failed temporarily (" + result_brief(result) + "), retrying in " +
                     ms_text(wait));
            std::this_thread::sleep_for(wait);
            continue;
        }
        return result;
    }
}

ApiResult FeishuTransport::deliver(SendTarget& target, bool reply, const std::string& msg_type,
                                   const nlohmann::json& content) {
    const auto text = dump_json(content);
    if (reply && !target.reply_to.empty()) {
        // uuid 在重发之间保持不变:平台一小时内同一 uuid 只发一条,网络重试不会重复送达。
        const nlohmann::json body{{"msg_type", msg_type}, {"content", text}, {"uuid", generate_uuid()}};
        const auto path = "/open-apis/im/v1/messages/" + path_segment(target.reply_to) + "/reply";
        auto result = with_retry(target.receive_id,
                                 [&api = api_, &path, &body] { return api.call("POST", path, body); });
        if (result.ok || !is_reply_target_gone(result.code)) return result;
        LOG_INFO("[channels/feishu] reply target is gone (code " + std::to_string(result.code) +
                 "), sending to the chat instead");
        target.reply_to.clear();
        target.in_thread = false;
    }
    const nlohmann::json body{{"receive_id", target.receive_id},
                              {"msg_type", msg_type},
                              {"content", text},
                              {"uuid", generate_uuid()}};
    const auto path = "/open-apis/im/v1/messages?receive_id_type=" + target.receive_id_type;
    return with_retry(target.receive_id, [&api = api_, &path, &body] { return api.call("POST", path, body); });
}

SendResult FeishuTransport::send_text(const Address& to, const std::string& text,
                                      const nlohmann::json& reply_context) {
    // 整条消息只判定一次:否则不含标记的分段按纯文本发,与相邻分段格式不一致。
    const bool markdown = looks_like_markdown(text);
    auto target = send_target(to, reply_context);
    bool first = true;
    for (const auto& chunk : split_outbound(text, markdown)) {
        const bool reply = !target.reply_to.empty() && (first || target.in_thread);
        first = false;
        auto result = deliver(target, reply, markdown ? "post" : "text",
                              markdown ? build_post_content(chunk) : build_text_content(chunk));
        if (!result.ok && markdown && is_post_rejected(result)) {
            LOG_INFO("[channels/feishu] post rejected (code " + std::to_string(result.code) +
                     "), resending the chunk as plain text");
            result = deliver(target, reply && !target.reply_to.empty(), "text",
                             build_text_content(markdown_to_plain(chunk)));
        }
        if (!result.ok) {
            LOG_WARN("[channels/feishu] send failed (" + result_brief(result) + ")");
            return {SendOutcome::Failed, describe_error(result)};
        }
    }
    return {SendOutcome::Sent, {}};
}

ApiResult FeishuTransport::upload_file(const std::filesystem::path& path, const std::string& name,
                                       const std::string& mime_type, const FileRoute& route, std::string* key) {
    std::vector<HttpPart> parts;
    std::string endpoint;
    HttpPart file;
    file.file = path;
    file.filename = name;
    file.content_type = mime_type.empty() ? "application/octet-stream" : mime_type;
    if (route.image) {
        endpoint = "/open-apis/im/v1/images";
        parts.push_back(HttpPart{"image_type", "message"});
        file.name = "image";
    } else {
        endpoint = "/open-apis/im/v1/files";
        parts.push_back(HttpPart{"file_type", route.file_type});
        parts.push_back(HttpPart{"file_name", name});  // 原名(UTF-8)放在字段里;文件部件的名字只能是 ASCII
        file.name = "file";
    }
    parts.push_back(std::move(file));
    auto result = with_retry({}, [&api = api_, &endpoint, &parts] { return api.upload(endpoint, parts); });
    if (result.ok) {
        const char* field = route.image ? "image_key" : "file_key";
        if (result.data.contains(field) && result.data[field].is_string()) *key = result.data[field].get<std::string>();
        if (key->empty()) {
            result.ok = false;
            result.code = -1;
            result.msg = "上传结果缺少文件标识";
        }
    }
    return result;
}

SendResult FeishuTransport::send_file(const Address& to, const std::filesystem::path& path, const std::string& name,
                                      const std::string& mime_type, const nlohmann::json& reply_context) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return {SendOutcome::Failed, "文件不存在或不可读"};
    if (size == 0) return {SendOutcome::Failed, "文件为空,未发送"};
    const auto display = name.empty() ? path_to_utf8(path.filename()) : name;
    auto route = route_file(display, mime_type, size);
    if (!route.image && size > options_.max_upload_bytes) {
        // 超过上传上限时改发一条文字说明,对方至少知道有文件没发过去。
        const auto limit_mib = std::to_string(options_.max_upload_bytes / (1024u * 1024u));
        LOG_INFO("[channels/feishu] file of " + std::to_string(size) + " bytes is over the " + limit_mib +
                 " MiB upload limit, sending a text notice instead");
        return send_text(to, "文件「" + display + "」超过飞书上传上限(" + limit_mib + " MiB),未发送;请在 ACECode 中查看。",
                         reply_context);
    }
    const auto unrecoverable = [](const ApiResult& result) {
        return result.auth_failed || is_transient(result) || is_rate_limited(result);
    };
    std::string key;
    auto uploaded = upload_file(path, display, mime_type, route, &key);
    if (!uploaded.ok && route.image && !unrecoverable(uploaded)) {
        // 图片接口不认(格式、分辨率):改按普通文件发送。
        LOG_INFO("[channels/feishu] image upload rejected (code " + std::to_string(uploaded.code) +
                 "), sending as a file");
        route = FileRoute{false, "stream", "file"};
        uploaded = upload_file(path, display, mime_type, route, &key);
    }
    if (!uploaded.ok) {
        LOG_WARN("[channels/feishu] upload failed (" + result_brief(uploaded) + ")");
        return {SendOutcome::Failed, describe_error(uploaded)};
    }
    auto target = send_target(to, reply_context);
    const auto content_for = [](const FileRoute& r, const std::string& k) {
        return r.image ? nlohmann::json{{"image_key", k}} : nlohmann::json{{"file_key", k}};
    };
    auto result = deliver(target, !target.reply_to.empty(), route.msg_type, content_for(route, key));
    if (!result.ok && (route.msg_type == "audio" || route.msg_type == "media") && !unrecoverable(result)) {
        // 平台不认这段音视频(编码不符等):重新按普通文件上传发送。
        LOG_INFO("[channels/feishu] " + route.msg_type + " message rejected (code " + std::to_string(result.code) +
                 "), resending as a plain file");
        route = FileRoute{false, "stream", "file"};
        uploaded = upload_file(path, display, mime_type, route, &key);
        if (!uploaded.ok) return {SendOutcome::Failed, describe_error(uploaded)};
        result = deliver(target, !target.reply_to.empty(), route.msg_type, content_for(route, key));
    }
    if (!result.ok) {
        LOG_WARN("[channels/feishu] file message failed (" + result_brief(result) + ")");
        return {SendOutcome::Failed, describe_error(result)};
    }
    return {SendOutcome::Sent, {}};
}

bool FeishuTransport::download(const Attachment& attachment, const std::filesystem::path& dest, std::string* error) {
    const auto ref = parse_resource_ref(attachment.remote_ref);
    if (!ref) {
        if (error) *error = "附件缺少下载信息";
        return false;
    }
    if (attachment.size > options_.max_download_bytes) {
        if (error) *error = "附件超过大小限制";
        return false;
    }
    return api_.download_resource(*ref, dest, options_.max_download_bytes, error);
}

} // namespace acecode::im::feishu
