#include "discord_transport.hpp"

#include "im/redact.hpp"
#include "im/text_chunk.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::im::discord {
namespace {

constexpr std::size_t kSeenLimit = 2000;
constexpr std::size_t kOriginLimit = 2000;
constexpr std::size_t kDmCacheLimit = 4096;

std::string string_field(const nlohmann::json& value, const char* key) {
    if (!value.is_object()) return {};
    const auto it = value.find(key);
    if (it == value.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_unsigned()) return std::to_string(it->get<std::uint64_t>());
    if (it->is_number_integer()) return std::to_string(it->get<std::int64_t>());
    return {};
}

std::int64_t int_field(const nlohmann::json& value, const char* key, std::int64_t fallback) {
    if (!value.is_object()) return fallback;
    const auto it = value.find(key);
    if (it == value.end() || !it->is_number()) return fallback;
    if (it->is_number_float()) return static_cast<std::int64_t>(it->get<double>());
    return it->get<std::int64_t>();
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

const char* action_name(CloseAction action) {
    switch (action) {
        case CloseAction::Resume: return "resume";
        case CloseAction::Identify: return "identify";
        case CloseAction::RateLimited: return "rate limited";
        case CloseAction::Fatal: return "fatal";
    }
    return "resume";
}

nlohmann::json allowed_mentions(bool replied_user) {
    // 只允许提及具体用户:LLM 输出或回显的 @everyone / @here / 角色提及一律不会响铃。
    return {{"parse", nlohmann::json::array({"users"})}, {"replied_user", replied_user}};
}

// 回复定位:第一段带 message_reference(fail_if_not_exists=false,被回复消息已删时退化成普通消息)。
// reply_context 里的频道与目标频道不一致时不带(跨频道回复不合法)。
std::optional<std::string> reply_reference(const nlohmann::json& reply_context, const std::string& channel) {
    const auto message_id = string_field(reply_context, "message_id");
    if (message_id.empty()) return std::nullopt;
    const auto context_channel = string_field(reply_context, "channel_id");
    if (!context_channel.empty() && context_channel != channel) return std::nullopt;
    return message_id;
}

std::string find_attachment_url(const nlohmann::json& message, const std::string& attachment_id) {
    if (!message.is_object()) return {};
    const auto attachments = message.find("attachments");
    if (attachments != message.end() && attachments->is_array()) {
        for (const auto& item : *attachments) {
            if (string_field(item, "id") == attachment_id) return string_field(item, "url");
        }
    }
    const auto referenced = message.find("referenced_message");
    if (referenced != message.end()) {
        auto url = find_attachment_url(*referenced, attachment_id);
        if (!url.empty()) return url;
    }
    const auto snapshots = message.find("message_snapshots");
    if (snapshots != message.end() && snapshots->is_array()) {
        for (const auto& snapshot : *snapshots) {
            if (!snapshot.is_object() || !snapshot.contains("message")) continue;
            auto url = find_attachment_url(snapshot["message"], attachment_id);
            if (!url.empty()) return url;
        }
    }
    return {};
}

} // namespace

DiscordTransport::DiscordTransport(DiscordTransportOptions options)
    : options_(std::move(options)),
      api_(options_.api, options_.rate_limit),
      budget_(options_.identify_limits, options_.identify_window_start_ms, options_.identify_count),
      rng_(std::random_device{}()) {}

DiscordTransport::~DiscordTransport() { stop(); }

Capabilities DiscordTransport::capabilities() const {
    Capabilities caps;
    caps.max_text_units = kMaxTextUnits;
    caps.count_utf16 = true;
    caps.batch_turn_output = false;
    caps.supports_typing = true;
    caps.max_upload_bytes = options_.max_upload_bytes;
    caps.max_download_bytes = options_.max_download_bytes;
    return caps;
}

void DiscordTransport::start(TransportCallbacks callbacks) {
    if (running_.exchange(true)) return;
    callbacks_ = std::move(callbacks);
    {
        std::lock_guard<std::mutex> lock(wake_mu_);
        stopping_ = false;
    }
    api_.reset();
    gateway_ = acecode::JoiningThread(&DiscordTransport::run, this);
    typer_ = acecode::JoiningThread(&DiscordTransport::typing_loop, this);
}

void DiscordTransport::stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(wake_mu_);
        stopping_ = true;
    }
    api_.cancel();
    ws_.abort();
    wake_.notify_all();
    if (gateway_.joinable()) gateway_.join();
    if (typer_.joinable()) typer_.join();
    // 主动停机用 1000 关闭:会话立即作废,机器人马上显示离线;下次启动重新 Identify。
    ws_.close(1000, "shutdown");
    session_.clear_session();
    {
        std::lock_guard<std::mutex> lock(mu_);
        typing_.clear();
    }
    set_status(LinkState::Stopped, {});
}

TransportStatus DiscordTransport::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

std::string DiscordTransport::bot_user_id() const {
    std::lock_guard<std::mutex> lock(mu_);
    return bot_id_;
}

void DiscordTransport::set_status(LinkState state, const std::string& detail, bool retry_stopped) {
    TransportStatus snapshot;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto clean = redact_secrets(detail, {options_.api.token, api_.token()});
        if (status_.state == state && status_.detail == clean && status_.retry_stopped == retry_stopped) return;
        status_.state = state;
        status_.detail = clean;
        status_.retry_stopped = retry_stopped;
        snapshot = status_;
    }
    if (callbacks_.on_status) callbacks_.on_status(snapshot);
}

bool DiscordTransport::wait_for(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(wake_mu_);
    wake_.wait_for(lock, duration, [this] { return stopping_.load(); });
    return !stopping_;
}

std::int64_t DiscordTransport::wall_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

double DiscordTransport::random_jitter() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng_); }

std::chrono::milliseconds DiscordTransport::random_between(std::chrono::milliseconds low,
                                                           std::chrono::milliseconds high) {
    if (high <= low) return low;
    return std::chrono::milliseconds(
        std::uniform_int_distribution<long long>(low.count(), high.count())(rng_));
}

void DiscordTransport::run() {
    std::size_t attempt = 0;
    int resume_connect_failures = 0;
    set_status(LinkState::Connecting, "正在连接 Discord");
    while (!stopping_) {
        ConnectionEnd end;
        const bool fresh = !session_.can_resume();
        std::string url;
        if (fresh) {
            // 登录预算:最小间隔、连续失败退避、24 小时上限、session_start_limit。
            const auto now = wall_ms();
            const auto allowed = budget_.next_allowed_ms(now);
            if (allowed > now) {
                const auto wait = std::chrono::milliseconds(allowed - now);
                if (budget_.daily_cap_reached(now)) {
                    LOG_WARN("[channels/discord] " + std::to_string(budget_.count()) +
                             " identifies in the current 24h window, pausing for " +
                             std::to_string(wait.count() / 60000) + " min to protect the token");
                    set_status(LinkState::Retrying, "Discord 24 小时内登录次数接近上限,暂停自动连接,约 " +
                                                        std::to_string(wait.count() / 60000 + 1) + " 分钟后恢复");
                } else {
                    LOG_INFO("[channels/discord] waiting " + std::to_string(wait.count()) + " ms before identifying");
                }
                if (!wait_for(wait)) break;
                continue;
            }
            const auto gateway = api_.gateway_bot();
            if (gateway.cancelled || stopping_) break;
            if (!gateway.ok) {
                if (gateway.status == 401 || gateway.error.code == 40001) {
                    LOG_WARN("[channels/discord] token rejected by GET /gateway/bot (HTTP 401), stopping");
                    set_status(LinkState::Failed,
                               "Bot Token 无效或已被重置,请在 Discord 开发者后台重新复制并保存", true);
                    break;
                }
                LOG_WARN("[channels/discord] GET /gateway/bot failed: HTTP " + std::to_string(gateway.status) +
                         " " + gateway.error.message);
                set_status(LinkState::Retrying, "无法获取 Discord 网关地址:" + describe_error(gateway.error));
            } else {
                url = string_field(gateway.body, "url");
                const auto limit = gateway.body.is_object() && gateway.body.contains("session_start_limit")
                                       ? gateway.body["session_start_limit"]
                                       : nlohmann::json::object();
                const auto remaining = int_field(limit, "remaining", -1);
                const auto reset_after = int_field(limit, "reset_after", 0);
                if (remaining == 0 && reset_after > 0) {
                    budget_.block_until(now + reset_after);
                    LOG_WARN("[channels/discord] session start limit exhausted, next identify in " +
                             std::to_string(reset_after / 1000) + " s");
                    set_status(LinkState::Retrying, "Discord 登录次数已用完,约 " +
                                                        std::to_string(reset_after / 60000 + 1) + " 分钟后自动连接");
                    continue;
                }
                if (url.empty()) set_status(LinkState::Retrying, "Discord 返回的网关地址无效");
                else gateway_url_ = url;
            }
        } else {
            url = session_.resume_url().empty() ? gateway_url_ : session_.resume_url();
            if (url.empty()) {
                session_.clear_session();
                continue;
            }
        }
        if (!url.empty()) {
            network::WebSocketConnectOptions ws_options;
            ws_options.url = gateway_connect_url(url);
            ws_options.headers = {{"User-Agent", discord_user_agent()}};
            ws_options.max_message_bytes = options_.max_message_bytes;
            ws_options.use_proxy = options_.api.use_proxy;
            std::string error;
            if (!ws_.connect(ws_options, &error)) {
                if (stopping_) break;
                error = redact_secrets(error, {options_.api.token, api_.token()});
                LOG_WARN("[channels/discord] gateway connect failed: " + error);
                set_status(LinkState::Retrying, "无法连接 Discord 网关:" + error);
                if (!fresh && ++resume_connect_failures >= 3) {
                    LOG_INFO("[channels/discord] resume connect failed 3 times, falling back to identify");
                    session_.clear_session();
                    resume_connect_failures = 0;
                }
            } else {
                resume_connect_failures = 0;
                LOG_INFO(std::string("[channels/discord] gateway connected, ") +
                         (fresh ? "identifying" : "resuming the previous session"));
                end = serve_connection();
            }
        }
        if (end.fatal || stopping_) break;
        if (end.ready) attempt = 0;
        const auto& backoff = options_.backoff;
        auto delay = backoff.empty() ? std::chrono::milliseconds(1000)
                                     : backoff[(std::min)(attempt, backoff.size() - 1)];
        delay = (std::max)(delay, end.extra_delay);
        ++attempt;
        if (!wait_for(delay)) break;
    }
    // 致命错误后保持 Failed 状态,线程等到 stop() 再退出。
    if (!stopping_) {
        std::unique_lock<std::mutex> lock(wake_mu_);
        wake_.wait(lock, [this] { return stopping_.load(); });
    }
}

DiscordTransport::ConnectionEnd DiscordTransport::serve_connection() {
    ConnectionEnd end;
    bool identified = false;
    const auto secrets = std::vector<std::string>{options_.api.token, api_.token()};
    session_.set_hello_timeout(options_.hello_timeout);
    session_.on_connected(api_.token(), Clock::now(), random_jitter());
    while (!stopping_) {
        const auto tick = session_.on_tick(Clock::now());
        if (tick.hello_timeout || tick.zombie) {
            LOG_WARN(tick.zombie ? "[channels/discord] heartbeat not acknowledged, reconnecting"
                                 : "[channels/discord] no Hello from the gateway, reconnecting");
            // 非 1000 关闭码:会话保留,可以 Resume。
            ws_.close(4000, tick.zombie ? "heartbeat timeout" : "hello timeout");
            session_.on_closed(4000);
            if (identified && !end.ready) budget_.record_failure();
            set_status(LinkState::Retrying, "Discord 网关没有响应,正在重连");
            return end;
        }
        if (tick.heartbeat) ws_.send_text(*tick.heartbeat, std::chrono::seconds(10), nullptr);
        network::WebSocketMessage message;
        std::string error;
        const auto received = ws_.receive(message, options_.receive_slice, &error);
        if (received == network::WebSocketRecv::Timeout) continue;
        if (received == network::WebSocketRecv::Message) {
            if (message.binary) {
                // 没有请求压缩,不应出现二进制帧;当作协议错误重连。
                LOG_WARN("[channels/discord] unexpected binary gateway frame, reconnecting");
                ws_.close(4000, "unexpected binary frame");
                session_.on_closed(4000);
                set_status(LinkState::Retrying, "Discord 网关数据异常,正在重连");
                return end;
            }
            auto out = session_.on_frame(message.data, Clock::now());
            for (const auto& frame : out.frames) ws_.send_text(frame, std::chrono::seconds(10), nullptr);
            if (out.identified) {
                identified = true;
                budget_.record_identify(wall_ms());
                LOG_INFO("[channels/discord] identify sent (" + std::to_string(budget_.count()) +
                         " in the current 24h window)");
                if (options_.on_identify_ledger) {
                    try {
                        options_.on_identify_ledger(budget_.window_start_ms(), budget_.count());
                    } catch (const std::exception& e) {
                        LOG_WARN(std::string("[channels/discord] persisting the identify ledger failed: ") + e.what());
                    }
                }
            }
            if (out.ready) {
                end.ready = true;
                if (!out.resumed) budget_.record_ready();
                if (out.ready_info) on_ready(*out.ready_info);
                LOG_INFO(out.resumed ? "[channels/discord] session resumed" : "[channels/discord] ready");
                set_status(LinkState::Connected, {});
            }
            for (const auto& [type, d] : out.dispatches) handle_dispatch(type, d);
            if (out.reconnect) {
                if (out.invalid_session) {
                    LOG_INFO("[channels/discord] session invalidated by the gateway (op 9), will identify again");
                    if (identified && !end.ready) budget_.record_failure();
                    // 会话已经作废,1000 关闭即可;等 1–5 秒再登录。
                    ws_.close(1000, "invalid session");
                    end.extra_delay = random_between(options_.invalid_session_delay_min,
                                                     options_.invalid_session_delay_max);
                    set_status(LinkState::Retrying, "Discord 会话已失效,正在重新登录");
                } else {
                    LOG_INFO("[channels/discord] gateway asked to reconnect, resuming");
                    ws_.close(4000, "reconnect requested");
                    set_status(LinkState::Retrying, "Discord 要求重新连接,正在恢复");
                }
                session_.on_closed(4000);
                return end;
            }
            continue;
        }
        if (stopping_) break;
        const int code = received == network::WebSocketRecv::Closed ? ws_.close_code() : 1006;
        if (received == network::WebSocketRecv::Error)
            LOG_WARN("[channels/discord] gateway receive error: " + redact_secrets(error, secrets));
        const auto decision = session_.on_closed(code);
        LOG_INFO("[channels/discord] gateway closed with code " + std::to_string(code) + " (" +
                 action_name(decision.action) + ")");
        if (identified && !end.ready) budget_.record_failure();
        ws_.close(4000);
        if (decision.action == CloseAction::Fatal) {
            LOG_WARN("[channels/discord] gateway close code " + std::to_string(code) + " is fatal, not retrying");
            set_status(LinkState::Failed, decision.reason, true);
            end.fatal = true;
        } else {
            if (decision.action == CloseAction::RateLimited) end.extra_delay = options_.rate_limit_close_delay;
            set_status(LinkState::Retrying, decision.reason);
        }
        return end;
    }
    // 停机:以 1000 关闭,会话作废。
    ws_.close(1000, "shutdown");
    session_.clear_session();
    return end;
}

void DiscordTransport::on_ready(const ReadyInfo& info) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!info.bot_id.empty()) bot_id_ = info.bot_id;
    status_.account = bot_id_;
    if (!info.bot_name.empty()) {
        status_.display_name = info.bot_name;
        status_.extra["username"] = info.bot_name;
    }
    if (!info.application_id.empty()) {
        status_.extra["application_id"] = info.application_id;
        status_.extra["invite_url"] = invite_url(info.application_id);
    }
    status_.extra["guilds"] = info.guild_count;
    status_.extra["identifies_24h"] = budget_.count();
    LOG_INFO("[channels/discord] bot " + bot_id_ + " is in " + std::to_string(info.guild_count) + " guild(s)");
}

bool DiscordTransport::seen_before(const std::string& message_id) {
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

void DiscordTransport::remember_attachments(const Inbound& inbound) {
    if (inbound.attachments.empty()) return;
    const auto channel = string_field(inbound.reply_context, "channel_id");
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& attachment : inbound.attachments) {
        const auto id = attachment_id_from_url(attachment.remote_ref);
        if (id.empty() || origins_.count(id)) continue;
        origins_[id] = {channel, inbound.message_id};
        origin_order_.push_back(id);
    }
    while (origin_order_.size() > kOriginLimit) {
        origins_.erase(origin_order_.front());
        origin_order_.pop_front();
    }
}

void DiscordTransport::handle_dispatch(const std::string& type, const nlohmann::json& d) {
    if (type == "GUILD_CREATE") {
        // 学会机器人自己的托管角色:有人在补全里选了角色(<@&id>)也算 @机器人。
        for (auto& id : bot_role_ids_in_guild(d, bot_user_id())) bot_roles_.insert(std::move(id));
        return;
    }
    if (type != "MESSAGE_CREATE") return;
    auto parsed = parse_message_create(d, bot_user_id(), bot_roles_);
    if (!parsed.inbound) {
        LOG_DEBUG("[channels/discord] inbound message dropped: " + parsed.drop_reason);
        return;
    }
    // Resume 补发与“最多推送 N 次”的投递语义都会带来重复,按消息 id 去重。
    if (seen_before(parsed.inbound->message_id)) {
        LOG_DEBUG("[channels/discord] duplicate message " + parsed.inbound->message_id + " ignored");
        return;
    }
    if (!parsed.dm_user.empty() && !parsed.dm_channel.empty()) {
        std::lock_guard<std::mutex> lock(mu_);
        if (dm_channels_.size() >= kDmCacheLimit) dm_channels_.clear();
        dm_channels_[parsed.dm_user] = parsed.dm_channel;
    }
    remember_attachments(*parsed.inbound);
    if (callbacks_.on_inbound) callbacks_.on_inbound(std::move(*parsed.inbound));
}

std::string DiscordTransport::resolve_channel(const Address& to, const nlohmann::json& reply_context,
                                              std::string* error) {
    if (to.kind == ChatKind::Group) {
        if (to.chat.empty()) {
            if (error) *error = "目标频道为空";
            return {};
        }
        return to.chat;
    }
    if (to.chat.empty()) {
        if (error) *error = "目标用户为空";
        return {};
    }
    // 私聊:优先用触发本回合的那条私信所在的频道(reply_context 里没有 guild_id 才是私聊)。
    const auto context_channel = string_field(reply_context, "channel_id");
    if (!context_channel.empty() && string_field(reply_context, "guild_id").empty()) {
        std::lock_guard<std::mutex> lock(mu_);
        dm_channels_[to.chat] = context_channel;
        return context_channel;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = dm_channels_.find(to.chat);
        if (it != dm_channels_.end()) return it->second;
    }
    // 没有可用的私聊频道(Desktop 里输入的回复、给机主发绑定码):打开(或取回已有的)私聊频道。
    const auto result = api_.open_dm(to.chat);
    if (!result.ok) {
        LOG_WARN("[channels/discord] open DM failed: HTTP " + std::to_string(result.status) + " code " +
                 std::to_string(result.error.code));
        if (error) *error = result.cancelled ? "Discord 通道已停止" : describe_error(result.error);
        return {};
    }
    const auto channel = string_field(result.body, "id");
    if (channel.empty()) {
        if (error) *error = "Discord 没有返回私聊频道";
        return {};
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (dm_channels_.size() >= kDmCacheLimit) dm_channels_.clear();
    dm_channels_[to.chat] = channel;
    return channel;
}

SendResult DiscordTransport::send_text(const Address& to, const std::string& text,
                                       const nlohmann::json& reply_context) {
    std::string error;
    const auto chunks = chunk_text(format_markdown(text), kMaxTextUnits, true);
    if (chunks.empty()) return {SendOutcome::Sent, {}};
    const auto channel = resolve_channel(to, reply_context, &error);
    if (channel.empty()) return {SendOutcome::Failed, error};
    const auto reference = reply_reference(reply_context, channel);
    std::lock_guard<std::mutex> lock(send_mu_);
    bool first = true;
    for (const auto& chunk : chunks) {
        const bool with_reference = first && reference.has_value();
        nlohmann::json body{{"content", chunk}, {"allowed_mentions", allowed_mentions(with_reference)}};
        if (with_reference)
            body["message_reference"] = {{"message_id", *reference}, {"fail_if_not_exists", false}};
        auto result = api_.create_message(channel, body);
        if (!result.ok && with_reference && reference_rejected(result.error)) {
            LOG_INFO("[channels/discord] reply reference rejected (code " + std::to_string(result.error.code) +
                     "), resending without it");
            body.erase("message_reference");
            body["allowed_mentions"] = allowed_mentions(false);
            result = api_.create_message(channel, body);
        }
        if (!result.ok) {
            if (result.cancelled) return {SendOutcome::Failed, "Discord 通道已停止"};
            LOG_WARN("[channels/discord] send to channel " + channel + " failed: HTTP " +
                     std::to_string(result.status) + " code " + std::to_string(result.error.code));
            return {SendOutcome::Failed, describe_error(result.error)};
        }
        first = false;
    }
    return {SendOutcome::Sent, {}};
}

SendResult DiscordTransport::oversize_notice(const Address& to, const std::string& name,
                                             const nlohmann::json& reply_context, bool local_limit) {
    std::string limit;
    if (local_limit) limit = "(" + std::to_string(options_.max_upload_bytes / (1024u * 1024u)) + " MiB)";
    LOG_INFO("[channels/discord] file is over the upload limit, sending a text notice instead");
    return send_text(to, "文件「" + name + "」超过 Discord 上传上限" + limit + ",未发送;请在 ACECode 中查看。",
                     reply_context);
}

SendResult DiscordTransport::send_file(const Address& to, const std::filesystem::path& path,
                                       const std::string& name, const std::string& mime_type,
                                       const nlohmann::json& reply_context) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return {SendOutcome::Failed, "文件不存在或不可读"};
    const auto display = name.empty() ? path_to_utf8(path.filename()) : name;
    if (size > options_.max_upload_bytes) return oversize_notice(to, display, reply_context, true);
    std::string error;
    const auto channel = resolve_channel(to, reply_context, &error);
    if (channel.empty()) return {SendOutcome::Failed, error};
    const auto reference = reply_reference(reply_context, channel);
    // payload_json 里的 filename 决定对方看到的文件名;multipart 的文件名只能是 ASCII。
    auto payload_for = [&display](const std::optional<std::string>& ref) {
        const nlohmann::json attachment{{"id", 0}, {"filename", display}};
        nlohmann::json payload{{"attachments", nlohmann::json::array({attachment})},
                               {"allowed_mentions", allowed_mentions(ref.has_value())}};
        if (ref) payload["message_reference"] = {{"message_id", *ref}, {"fail_if_not_exists", false}};
        return payload;
    };
    ApiResult result;
    {
        std::lock_guard<std::mutex> lock(send_mu_);
        result = api_.create_message_with_file(channel, payload_for(reference), path, display, mime_type);
        if (!result.ok && reference && reference_rejected(result.error)) {
            LOG_INFO("[channels/discord] reply reference rejected for an upload, resending without it");
            result = api_.create_message_with_file(channel, payload_for(std::nullopt), path, display, mime_type);
        }
    }
    if (!result.ok && upload_too_large(result.error)) return oversize_notice(to, display, reply_context, false);
    if (!result.ok) {
        if (result.cancelled) return {SendOutcome::Failed, "Discord 通道已停止"};
        LOG_WARN("[channels/discord] upload to channel " + channel + " failed: HTTP " + std::to_string(result.status) +
                 " code " + std::to_string(result.error.code));
        return {SendOutcome::Failed, describe_error(result.error)};
    }
    // 已知坑:Discord 偶尔返回 200 但 attachments 为空,文件其实没发出去。
    const auto attachments = result.body.is_object() && result.body.contains("attachments")
                                 ? result.body["attachments"]
                                 : nlohmann::json();
    if (!attachments.is_array() || attachments.empty()) {
        LOG_WARN("[channels/discord] upload to channel " + channel + " returned no attachments");
        return {SendOutcome::Failed, "Discord 没有收到文件,请重试"};
    }
    return {SendOutcome::Sent, {}};
}

void DiscordTransport::set_typing(const Address& to, bool on) {
    const auto key = to.key();
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!on) {
            typing_.erase(key);
            return;
        }
        if (typing_.count(key)) return;
        typing_[key] = {to, Clock::now()};
    }
    {
        std::lock_guard<std::mutex> lock(wake_mu_);
        typing_dirty_ = true;
    }
    wake_.notify_all();
}

void DiscordTransport::typing_loop() {
    while (!stopping_) {
        std::vector<Address> due;
        const auto now = Clock::now();
        auto next = now + options_.typing_interval;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& [key, entry] : typing_) {
                if (entry.due <= now) {
                    due.push_back(entry.address);
                    entry.due = now + options_.typing_interval;
                }
                next = (std::min)(next, entry.due);
            }
        }
        for (const auto& address : due) {
            if (stopping_) break;
            std::string error;
            const auto channel = resolve_channel(address, nlohmann::json::object(), &error);
            ApiResult result;
            if (!channel.empty()) result = api_.trigger_typing(channel);
            if (!result.ok && !result.cancelled && !stopping_) {
                // 429 已在 Api 里等待重发;其它错误(没权限、频道没了)不再继续刷输入状态。
                LOG_DEBUG("[channels/discord] typing indicator stopped: HTTP " + std::to_string(result.status));
                std::lock_guard<std::mutex> lock(mu_);
                typing_.erase(address.key());
            }
        }
        std::unique_lock<std::mutex> lock(wake_mu_);
        wake_.wait_until(lock, next, [this] { return stopping_.load() || typing_dirty_.load(); });
        typing_dirty_ = false;
    }
}

bool DiscordTransport::host_allowed(const std::string& url) const {
    if (options_.download_hosts.empty()) return true;
    const auto host = url_host(url);
    if (host.empty()) return false;
    return std::any_of(options_.download_hosts.begin(), options_.download_hosts.end(),
                       [&host](const std::string& allowed) { return lower(allowed) == host; });
}

std::string DiscordTransport::refresh_attachment_url(const std::string& url) {
    const auto id = attachment_id_from_url(url);
    if (id.empty()) return {};
    AttachmentOrigin origin;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = origins_.find(id);
        if (it == origins_.end()) return {};
        origin = it->second;
    }
    if (origin.channel_id.empty() || origin.message_id.empty()) return {};
    const auto message = api_.get_message(origin.channel_id, origin.message_id);
    if (!message.ok) return {};
    return find_attachment_url(message.body, id);
}

bool DiscordTransport::download(const Attachment& attachment, const std::filesystem::path& dest,
                                std::string* error) {
    if (attachment.remote_ref.empty()) {
        if (error) *error = "附件缺少下载地址";
        return false;
    }
    if (attachment.size > options_.max_download_bytes) {
        if (error) *error = "附件超过大小限制";
        return false;
    }
    if (!host_allowed(attachment.remote_ref)) {
        LOG_WARN("[channels/discord] refusing to download an attachment from host " + url_host(attachment.remote_ref));
        if (error) *error = "附件地址不是 Discord 的 CDN,已拒绝下载";
        return false;
    }
    auto result = api_.download(attachment.remote_ref, dest, options_.max_download_bytes);
    if (!result.ok && !result.too_large && (result.status == 403 || result.status == 404)) {
        // 签名地址过期:重新取一次消息,拿到新的签名地址再下载。
        const auto fresh = refresh_attachment_url(attachment.remote_ref);
        if (!fresh.empty() && host_allowed(fresh)) {
            LOG_INFO("[channels/discord] attachment URL expired, downloading with a refreshed URL");
            result = api_.download(fresh, dest, options_.max_download_bytes);
        }
    }
    if (result.too_large) {
        if (error) *error = "附件超过大小限制";
        return false;
    }
    if (!result.ok) {
        LOG_WARN("[channels/discord] attachment download failed: " + result.error);
        if (error) {
            *error = result.cancelled ? "Discord 通道已停止"
                     : result.status == 0 ? "无法连接 Discord,附件下载失败"
                                          : "下载附件失败:HTTP " + std::to_string(result.status);
        }
        return false;
    }
    return true;
}

} // namespace acecode::im::discord
