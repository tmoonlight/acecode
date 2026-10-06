#include "tg_transport.hpp"

#include "im/markdown.hpp"
#include "im/redact.hpp"
#include "im/text_chunk.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <thread>

namespace acecode::im::telegram {
namespace {

using Clock = std::chrono::steady_clock;

// Telegram 单条消息上限 4096(按 UTF-16 计、实体解析之后);Markdown 源文本转换后只会更短,
// 留一些余量按 4000 切。
constexpr std::size_t kChunkUnits = 4000;

bool contains_ci(const std::string& text, const std::string& needle) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find(needle) != std::string::npos;
}

bool non_ascii(const std::string& text) {
    return std::any_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; });
}

bool is_image(const std::string& mime, const std::string& name) {
    if (mime.rfind("image/", 0) == 0) return mime != "image/svg+xml";
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* ext : {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"}) {
        const std::string e(ext);
        if (lower.size() >= e.size() && lower.compare(lower.size() - e.size(), e.size(), e) == 0) return true;
    }
    return false;
}

nlohmann::json chat_id_value(const std::string& chat) {
    try {
        std::size_t used = 0;
        const auto value = std::stoll(chat, &used);
        if (used == chat.size()) return value;
    } catch (...) {
    }
    return chat;
}

} // namespace

TelegramTransport::TelegramTransport(TelegramTransportOptions options)
    : options_(std::move(options)), api_(options_.api), limiter_(options_.limits) {
    offset_ = options_.initial_offset;
}

TelegramTransport::~TelegramTransport() { stop(); }

Capabilities TelegramTransport::capabilities() const {
    Capabilities caps;
    caps.max_text_units = kChunkUnits;
    caps.count_utf16 = true;
    caps.batch_turn_output = false;
    caps.supports_typing = true;
    caps.max_upload_bytes = options_.max_upload_bytes;
    caps.max_download_bytes = options_.max_download_bytes;
    return caps;
}

void TelegramTransport::start(TransportCallbacks callbacks) {
    if (running_.exchange(true)) return;
    callbacks_ = std::move(callbacks);
    stopping_ = false;
    poller_ = acecode::JoiningThread(&TelegramTransport::poll_loop, this);
    typer_ = acecode::JoiningThread(&TelegramTransport::typing_loop, this);
}

void TelegramTransport::stop() {
    if (!running_.exchange(false)) return;
    stopping_ = true;
    wake_.notify_all();
    if (poller_.joinable()) poller_.join();
    if (typer_.joinable()) typer_.join();
    {
        std::lock_guard<std::mutex> lock(mu_);
        typing_.clear();
    }
    set_status(LinkState::Stopped, {});
}

TransportStatus TelegramTransport::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

BotIdentity TelegramTransport::bot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return bot_;
}

void TelegramTransport::set_status(LinkState state, const std::string& detail, bool retry_stopped) {
    TransportStatus snapshot;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto clean = redact_secrets(detail, {options_.api.token});
        if (status_.state == state && status_.detail == clean && status_.retry_stopped == retry_stopped) return;
        status_.state = state;
        status_.detail = clean;
        status_.retry_stopped = retry_stopped;
        snapshot = status_;
    }
    if (callbacks_.on_status) callbacks_.on_status(snapshot);
}

bool TelegramTransport::wait_for(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(wake_mu_);
    wake_.wait_for(lock, duration, [this] { return stopping_.load() || webhook_cleared_.load(); });
    return !stopping_;
}

void TelegramTransport::poll_loop() {
    const auto cancel = [&stopping = stopping_] { return stopping.load(); };
    std::size_t attempt = 0;
    auto backoff = [&attempt, &steps = options_.backoff] {
        const auto delay = steps.empty() ? std::chrono::milliseconds(1000) : steps[(std::min)(attempt, steps.size() - 1)];
        ++attempt;
        return delay;
    };
    bool identified = false;
    set_status(LinkState::Connecting, "正在连接 Telegram");
    while (!stopping_) {
        if (!identified) {
            const auto me = api_.call("getMe", nlohmann::json::object(), std::chrono::seconds(20), cancel);
            if (me.cancelled || stopping_) break;
            if (!me.ok) {
                if (me.status == 401 || me.status == 404) {
                    set_status(LinkState::Failed, "token 无效或已被吊销", true);
                    break;
                }
                set_status(LinkState::Retrying, me.description);
                if (!wait_for(backoff())) break;
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                const auto& r = me.result;
                bot_.id = r.contains("id") ? (r["id"].is_number_integer() ? std::to_string(r["id"].get<std::int64_t>())
                                                                           : r.value("id", std::string{}))
                                           : std::string{};
                bot_.username = r.value("username", std::string{});
                status_.account = bot_.id;
                status_.display_name = bot_.username.empty() ? std::string{} : "@" + bot_.username;
                status_.extra["username"] = bot_.username;
                status_.extra["privacy_mode"] = !r.value("can_read_all_group_messages", false);
                status_.extra["link"] = bot_.username.empty() ? std::string{} : "https://t.me/" + bot_.username;
            }
            identified = true;
            // getMe 成功已证明 token 与网络可用;不要等第一次长轮询返回(没有新消息时要等满
            // poll_timeout)才显示已连接。之后的 409 / 网络错误会再把状态改掉。
            set_status(LinkState::Connected, {});
        }

        nlohmann::json params{{"offset", offset_.load()},
                              {"timeout", options_.poll_timeout.count()},
                              {"allowed_updates", nlohmann::json::array({"message"})}};
        const auto updates = api_.call("getUpdates", params, options_.poll_timeout + std::chrono::seconds(15), cancel);
        if (updates.cancelled || stopping_) break;
        if (updates.ok) {
            attempt = 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                status_.extra.erase("webhook");
            }
            set_status(LinkState::Connected, {});
            std::int64_t next = offset_.load();
            const auto me = bot();
            if (updates.result.is_array()) {
                for (const auto& update : updates.result) {
                    auto parsed = parse_update(update, me, me.id);
                    if (parsed.update_id >= next) next = parsed.update_id + 1;
                    if (parsed.inbound && callbacks_.on_inbound) callbacks_.on_inbound(std::move(*parsed.inbound));
                }
            }
            if (next != offset_.load()) {
                offset_ = next;
                if (options_.on_offset) options_.on_offset(next);
            }
            continue;
        }
        switch (conflict_kind(updates)) {
            case ConflictKind::Webhook: {
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    status_.extra["webhook"] = true;
                }
                set_status(LinkState::Failed, "该机器人已配置 webhook,长轮询不可用;确认后可移除 webhook 并继续", true);
                webhook_cleared_ = false;
                std::unique_lock<std::mutex> lock(wake_mu_);
                wake_.wait(lock, [this] { return stopping_.load() || webhook_cleared_.load(); });
                webhook_cleared_ = false;
                continue;
            }
            case ConflictKind::OtherPoller:
                set_status(LinkState::Retrying, "机器人被其他程序占用(同一 token 正在别处长轮询)");
                if (!wait_for(options_.conflict_retry)) return;
                continue;
            case ConflictKind::None:
                break;
        }
        if (updates.status == 401 || updates.status == 404) {
            set_status(LinkState::Failed, "token 无效或已被吊销", true);
            break;
        }
        if (updates.status == 429 && updates.retry_after > 0) {
            set_status(LinkState::Retrying, "Telegram 限频,稍后继续");
            if (!wait_for(std::chrono::seconds(updates.retry_after))) break;
            continue;
        }
        set_status(LinkState::Retrying, updates.description);
        if (!wait_for(backoff())) break;
    }
    if (!stopping_) {
        std::unique_lock<std::mutex> lock(wake_mu_);
        wake_.wait(lock, [this] { return stopping_.load(); });
    }
}

void TelegramTransport::typing_loop() {
    while (!stopping_) {
        std::vector<std::pair<std::string, std::string>> chats;
        {
            std::lock_guard<std::mutex> lock(mu_);
            chats.assign(typing_.begin(), typing_.end());
        }
        for (const auto& [chat, thread] : chats) {
            nlohmann::json params{{"chat_id", chat_id_value(chat)}, {"action", "typing"}};
            if (!thread.empty()) params["message_thread_id"] = chat_id_value(thread);
            api_.call("sendChatAction", params, std::chrono::seconds(10));
        }
        std::unique_lock<std::mutex> lock(wake_mu_);
        wake_.wait_for(lock, options_.typing_interval, [this] { return stopping_.load(); });
    }
}

void TelegramTransport::set_typing(const Address& to, bool on) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (on) typing_.insert({to.chat, to.thread});
        else typing_.erase({to.chat, to.thread});
    }
    if (on) wake_.notify_all();
}

ApiResult TelegramTransport::send_request(const Address& to, const std::string& method,
                                          const nlohmann::json& params, const std::vector<HttpPart>& parts) {
    const bool group = to.kind == ChatKind::Group;
    ApiResult result;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto at = limiter_.reserve(to.chat, group, Clock::now());
        const auto now = Clock::now();
        if (at > now) std::this_thread::sleep_for(at - now);
        result = parts.empty() ? api_.call(method, params) : api_.call_multipart(method, parts);
        if (result.status == 429 && result.retry_after > 0) {
            // 429 表示平台明确拒收了这条消息,等待后重发是安全的;顺序由同一线程串行保证。
            LOG_INFO("[channels/telegram] " + method + " rate limited, resending after " +
                     std::to_string(result.retry_after) + "s");
            limiter_.penalize(to.chat, std::chrono::seconds(result.retry_after), Clock::now());
            continue;
        }
        return result;
    }
    return result;
}

SendResult TelegramTransport::send_text(const Address& to, const std::string& text,
                                        const nlohmann::json& reply_context) {
    bool first = true;
    for (const auto& chunk : chunk_text(text, kChunkUnits, true)) {
        nlohmann::json params{{"chat_id", chat_id_value(to.chat)},
                              {"text", markdown_to_telegram_html(chunk)},
                              {"parse_mode", "HTML"}};
        if (!to.thread.empty()) params["message_thread_id"] = chat_id_value(to.thread);
        if (first && reply_context.is_object() && reply_context.contains("message_id"))
            params["reply_parameters"] = {{"message_id", reply_context["message_id"]},
                                          {"allow_sending_without_reply", true}};
        auto result = send_request(to, "sendMessage", params, {});
        if (!result.ok && result.status == 400 && contains_ci(result.description, "parse")) {
            // HTML 被拒(实体解析失败):同一段内容改用纯文本发送,不丢消息。
            LOG_INFO("[channels/telegram] HTML rejected, resending the same chunk as plain text");
            params.erase("parse_mode");
            params["text"] = markdown_to_plain(chunk);
            result = send_request(to, "sendMessage", params, {});
        }
        if (!result.ok) {
            LOG_WARN("[channels/telegram] sendMessage failed: " + redact_secrets(result.description, {options_.api.token}));
            return {SendOutcome::Failed, result.description};
        }
        first = false;
    }
    return {SendOutcome::Sent, {}};
}

SendResult TelegramTransport::send_file(const Address& to, const std::filesystem::path& path,
                                        const std::string& name, const std::string& mime_type,
                                        const nlohmann::json& reply_context) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return {SendOutcome::Failed, "文件不存在或不可读"};
    const bool photo = is_image(mime_type, name) && size <= options_.max_photo_bytes;
    if (!photo && size > options_.max_upload_bytes) return {SendOutcome::Failed, "文件超过 Telegram 50 MB 上限"};
    std::vector<HttpPart> parts;
    parts.push_back(HttpPart{"chat_id", to.chat});
    if (!to.thread.empty()) parts.push_back(HttpPart{"message_thread_id", to.thread});
    if (reply_context.is_object() && reply_context.contains("message_id")) {
        parts.push_back(HttpPart{"reply_parameters",
                                 nlohmann::json{{"message_id", reply_context["message_id"]},
                                                {"allow_sending_without_reply", true}}.dump()});
    }
    // multipart 文件名只能用 ASCII,原名带非 ASCII 字符时放进说明文字里展示。
    if (non_ascii(name)) parts.push_back(HttpPart{"caption", name});
    HttpPart file;
    file.name = photo ? "photo" : "document";
    file.file = path;
    file.filename = name.empty() ? path_to_utf8(path.filename()) : name;
    file.content_type = mime_type.empty() ? "application/octet-stream" : mime_type;
    parts.push_back(std::move(file));
    const auto result = send_request(to, photo ? "sendPhoto" : "sendDocument", nlohmann::json::object(), parts);
    if (!result.ok) {
        LOG_WARN(std::string("[channels/telegram] ") + (photo ? "sendPhoto" : "sendDocument") +
                 " failed: " + redact_secrets(result.description, {options_.api.token}));
        return {SendOutcome::Failed, result.description};
    }
    return {SendOutcome::Sent, {}};
}

bool TelegramTransport::download(const Attachment& attachment, const std::filesystem::path& dest,
                                 std::string* error) {
    if (attachment.remote_ref.empty()) {
        if (error) *error = "附件缺少 file_id";
        return false;
    }
    if (attachment.size > options_.max_download_bytes) {
        if (error) *error = "文件超出 Telegram 20 MB 下载上限";
        return false;
    }
    const auto info = api_.call("getFile", {{"file_id", attachment.remote_ref}}, std::chrono::seconds(20));
    if (!info.ok) {
        if (error) *error = info.description;
        return false;
    }
    const auto file_path = info.result.value("file_path", std::string{});
    if (file_path.empty()) {
        if (error) *error = "文件超出 Telegram 20 MB 下载上限";
        return false;
    }
    if (info.result.value("file_size", std::int64_t{0}) > static_cast<std::int64_t>(options_.max_download_bytes)) {
        if (error) *error = "文件超出 Telegram 20 MB 下载上限";
        return false;
    }
    return api_.download(file_path, dest, options_.max_download_bytes, error);
}

nlohmann::json TelegramTransport::action(const std::string& name, const nlohmann::json& args) {
    (void)args;
    if (name != "remove_webhook") throw std::runtime_error("Unsupported Telegram action: " + name);
    const auto result = api_.call("deleteWebhook", {{"drop_pending_updates", false}}, std::chrono::seconds(20));
    if (!result.ok) throw std::runtime_error("移除 webhook 失败:" + result.description);
    LOG_INFO("[channels/telegram] webhook removed by user request");
    webhook_cleared_ = true;
    wake_.notify_all();
    return {{"ok", true}};
}

} // namespace acecode::im::telegram
