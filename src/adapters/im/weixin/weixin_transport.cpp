#include "weixin_transport.hpp"

#include "im/markdown.hpp"
#include "im/redact.hpp"
#include "im/text_chunk.hpp"
#include "im/weixin/weixin_media.hpp"
#include "platform/crypto/digest.hpp"
#include "platform/crypto/secure_random.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::im::weixin {
namespace {

// hermes 代码里的单条上限(其文档里的 4000 已过时),按 Unicode 码点计。
constexpr std::size_t kChunkUnits = 2000;
constexpr std::chrono::seconds kSendTimeout{15};
constexpr std::chrono::seconds kConfigTimeout{10};
constexpr std::chrono::seconds kUploadTimeout{120};
constexpr int kUploadAttempts = 3;

bool blank(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

std::string new_client_id() {
    static std::atomic<std::uint64_t> counter{0};
    const auto random = platform::secure_random_bytes(16);
    if (random.size() == 16) return "acecode-weixin-" + platform::to_hex(random);
    // 只用于平台侧幂等去重,不是凭据;随机源不可用时用时间 + 计数保证唯一。
    return "acecode-weixin-" +
           std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" +
           std::to_string(++counter);
}

std::string describe(const ApiResult& result) {
    return "status=" + std::to_string(result.status) + " ret=" + std::to_string(result.ret) +
           " errcode=" + std::to_string(result.errcode) + (result.errmsg.empty() ? "" : " errmsg=" + result.errmsg);
}

} // namespace

WeixinTransport::WeixinTransport(WeixinTransportOptions options)
    : options_(std::move(options)), api_(options_.api), merger_(options_.merge) {
    cursor_ = options_.initial_cursor;
    for (const auto& [peer, token] : options_.initial_context_tokens)
        if (valid_id(peer) && !token.empty()) tokens_[peer] = token;
    status_.account = options_.bot_id;
}

WeixinTransport::~WeixinTransport() { stop(); }

Capabilities WeixinTransport::capabilities() const {
    Capabilities caps;
    caps.max_text_units = kChunkUnits;
    caps.count_utf16 = false;
    // 微信没有消息编辑、发送有频率限制且分段之间要间隔:同一回合的输出合并后再发。
    caps.batch_turn_output = true;
    caps.supports_typing = true;
    caps.max_upload_bytes = options_.max_upload_bytes;
    caps.max_download_bytes = options_.max_download_bytes;
    return caps;
}

void WeixinTransport::start(TransportCallbacks callbacks) {
    if (running_.exchange(true)) return;
    callbacks_ = std::move(callbacks);
    stopping_ = false;
    poller_ = acecode::JoiningThread(&WeixinTransport::poll_loop, this);
    typer_ = acecode::JoiningThread(&WeixinTransport::typing_loop, this);
    keeper_ = acecode::JoiningThread(&WeixinTransport::housekeeping_loop, this);
}

void WeixinTransport::stop() {
    if (!running_.exchange(false)) return;
    {
        // 在锁内置位,等待中的线程不会错过唤醒。
        std::lock_guard<std::mutex> lock(wake_mu_);
        stopping_ = true;
    }
    wake_.notify_all();
    {
        std::lock_guard<std::mutex> lock(typing_mu_);
        typing_dirty_ = true;
    }
    typing_cv_.notify_all();
    if (poller_.joinable()) poller_.join();
    if (keeper_.joinable()) keeper_.join();
    if (typer_.joinable()) typer_.join();
    {
        std::lock_guard<std::mutex> lock(typing_mu_);
        typing_.clear();
        typing_stop_.clear();
        typing_sent_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        poll_active_ = false;
    }
    set_status(LinkState::Stopped, {});
}

TransportStatus WeixinTransport::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

std::string WeixinTransport::cursor() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cursor_;
}

std::map<std::string, std::string> WeixinTransport::context_tokens() const {
    std::lock_guard<std::mutex> lock(mu_);
    return tokens_;
}

void WeixinTransport::set_status(LinkState state, const std::string& detail, bool retry_stopped) {
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

bool WeixinTransport::wait_for(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(wake_mu_);
    wake_.wait_for(lock, duration, [this] { return stopping_.load(); });
    return !stopping_;
}

void WeixinTransport::promote_if_polling() {
    TransportStatus snapshot;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!poll_active_ || Clock::now() - poll_started_ < options_.connect_grace) return;
        if (status_.state != LinkState::Connecting && status_.state != LinkState::Retrying) return;
        status_.state = LinkState::Connected;
        status_.detail.clear();
        status_.retry_stopped = false;
        snapshot = status_;
    }
    if (callbacks_.on_status) callbacks_.on_status(snapshot);
}

void WeixinTransport::poll_loop() {
    const std::function<bool()> cancel = [&stopping = stopping_] { return stopping.load(); };
    std::size_t attempt = 0;
    auto backoff = [&attempt, &steps = options_.backoff] {
        const auto delay = steps.empty() ? std::chrono::milliseconds(1000) : steps[(std::min)(attempt, steps.size() - 1)];
        ++attempt;
        return delay;
    };
    set_status(LinkState::Connecting, "正在连接微信");
    if (options_.api.token.empty() || options_.bot_id.empty()) {
        set_status(LinkState::Failed, "缺少微信登录凭据,请在设置页扫码登录", true);
    } else {
        auto hold = options_.poll_timeout;
        while (!stopping_) {
            std::string cursor;
            {
                std::lock_guard<std::mutex> lock(mu_);
                cursor = cursor_;
                poll_active_ = true;
                poll_started_ = Clock::now();
            }
            const auto result =
                api_.post(kEpGetUpdates, {{"get_updates_buf", cursor}}, hold + options_.poll_margin, cancel);
            {
                std::lock_guard<std::mutex> lock(mu_);
                poll_active_ = false;
            }
            if (result.cancelled || stopping_) break;
            if (result.ok) {
                attempt = 0;
                set_status(LinkState::Connected, {});
                const auto next_hold = result.body.value("longpolling_timeout_ms", std::int64_t{0});
                if (next_hold > 0)
                    hold = std::clamp(std::chrono::milliseconds(next_hold), std::chrono::milliseconds(100),
                                      std::chrono::milliseconds(std::chrono::minutes(2)));
                handle_batch(result.body);
                continue;
            }
            // 客户端等到超时:当作空批次,游标不变,立即再轮询。
            if (result.timed_out) continue;
            if (result.session_expired) {
                LOG_WARN("[channels/weixin] getupdates reports the bot session expired (" + describe(result) +
                         "), waiting for a new QR login");
                set_status(LinkState::Failed, kSessionExpiredText, true);
                break;
            }
            LOG_WARN("[channels/weixin] getupdates failed (" + describe(result) + ")");
            set_status(LinkState::Retrying, result.error);
            if (!wait_for(backoff())) break;
        }
    }
    if (!stopping_) {
        std::unique_lock<std::mutex> lock(wake_mu_);
        wake_.wait(lock, [this] { return stopping_.load(); });
    }
}

void WeixinTransport::handle_batch(const nlohmann::json& body) {
    if (body.contains("get_updates_buf") && body["get_updates_buf"].is_string()) {
        const auto next = body["get_updates_buf"].get<std::string>();
        bool changed = false;
        if (!next.empty()) {
            std::lock_guard<std::mutex> lock(mu_);
            changed = next != cursor_;
            cursor_ = next;
        }
        // 先持久化游标再处理消息:崩溃时宁可漏处理也不重复执行(重复由 id 去重兜底)。
        if (changed && options_.on_cursor) options_.on_cursor(next);
    }
    if (!body.contains("msgs") || !body["msgs"].is_array()) return;
    for (const auto& message : body["msgs"]) {
        auto parsed = parse_message(message, options_.bot_id);
        if (!parsed.from_user) {
            LOG_DEBUG("[channels/weixin] inbound skipped: " + parsed.drop_reason);
            continue;
        }
        bool duplicate = false;
        {
            std::lock_guard<std::mutex> lock(deliver_mu_);
            duplicate = dedupe_.seen_id(parsed.message_id);
        }
        if (duplicate) {
            LOG_DEBUG("[channels/weixin] duplicate inbound message ignored");
            continue;
        }
        if (!parsed.context_token.empty()) remember_context_token(parsed.sender, parsed.context_token);
        if (!parsed.inbound) {
            LOG_DEBUG("[channels/weixin] inbound skipped: " + parsed.drop_reason);
            continue;
        }
        std::lock_guard<std::mutex> lock(deliver_mu_);
        const auto now = Clock::now();
        if (dedupe_.seen_content(parsed.sender, parsed.inbound->text, parsed.create_time_ms, now)) {
            LOG_INFO("[channels/weixin] inbound resent with a different message id, ignored");
            continue;
        }
        deliver_locked(merger_.push(std::move(*parsed.inbound), now));
    }
}

void WeixinTransport::deliver_locked(std::vector<Inbound> ready) {
    for (auto& inbound : ready)
        if (callbacks_.on_inbound) callbacks_.on_inbound(std::move(inbound));
}

void WeixinTransport::housekeeping_loop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(wake_mu_);
            wake_.wait_for(lock, options_.housekeeping_tick, [this] { return stopping_.load(); });
        }
        const bool stopping = stopping_.load();
        {
            std::lock_guard<std::mutex> lock(deliver_mu_);
            deliver_locked(stopping ? merger_.take_all() : merger_.take_due(Clock::now()));
        }
        if (stopping) break;
        promote_if_polling();
    }
}

std::string WeixinTransport::context_token_for(const std::string& peer) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = tokens_.find(peer);
    return it == tokens_.end() ? std::string{} : it->second;
}

void WeixinTransport::remember_context_token(const std::string& peer, const std::string& token) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto& slot = tokens_[peer];
        if (slot == token) return;
        slot = token;
    }
    if (options_.on_context_token) options_.on_context_token(peer, token);
}

void WeixinTransport::forget_context_token(const std::string& peer, const std::string& rejected) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = tokens_.find(peer);
        if (it == tokens_.end() || it->second != rejected) return;
        tokens_.erase(it);
    }
    if (options_.on_context_token) options_.on_context_token(peer, std::string{});
}

ApiResult WeixinTransport::deliver_item(const std::string& peer, const nlohmann::json& item) {
    const auto client_id = new_client_id();  // 同一段内容的重发沿用它,平台据此去重
    auto token = context_token_for(peer);
    ApiResult result;
    for (int attempt = 0; attempt <= options_.send_retries; ++attempt) {
        result = api_.post(kEpSendMessage, build_send_body(peer, client_id, item, token), kSendTimeout);
        if (result.ok) return result;
        if (result.session_expired && !token.empty()) {
            // sendmessage 的 -14 也可能只是 context_token 过期;平台接受不带 token 的发送。
            // 只删除被拒的那个 token:期间用户若又发来消息,就改用新 token 重发。
            LOG_INFO("[channels/weixin] sendmessage rejected the cached context token, resending");
            forget_context_token(peer, token);
            token = context_token_for(peer);
            continue;
        }
        if (result.session_expired) break;
        std::chrono::milliseconds delay{0};
        if (result.rate_limited) {
            delay = options_.rate_limit_delay * (attempt + 1);
        } else if (result.status == 0 || result.status >= 500) {
            delay = options_.send_retry_delay * (attempt + 1);
        } else {
            break;  // 平台明确拒绝(参数、权限等),重发没有意义
        }
        if (attempt == options_.send_retries) break;
        LOG_INFO("[channels/weixin] sendmessage will be resent (" + describe(result) + ")");
        if (!wait_for(delay)) break;
    }
    LOG_WARN("[channels/weixin] sendmessage failed (" + describe(result) + ")");
    return result;
}

SendResult WeixinTransport::send_text(const Address& to, const std::string& text, const nlohmann::json& reply_context) {
    (void)reply_context;  // 回复定位只靠 context_token;它按对方用户缓存,总是最新的
    const auto& peer = to.chat;
    if (!valid_id(peer)) return {SendOutcome::Failed, "收件人无效"};
    std::vector<std::string> chunks;
    for (auto& chunk : chunk_text(markdown_to_plain(text), kChunkUnits, false))
        if (!blank(chunk)) chunks.push_back(std::move(chunk));  // 绝不发送空白气泡
    std::lock_guard<std::mutex> lock(send_mu_);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        if (i > 0 && !wait_for(options_.chunk_delay)) return {SendOutcome::Failed, "通道已关闭,剩余内容未发送"};
        const auto result = deliver_item(peer, text_item(chunks[i]));
        if (!result.ok) return {SendOutcome::Failed, result.error};
    }
    return {SendOutcome::Sent, {}};
}

SendResult WeixinTransport::send_file(const Address& to, const std::filesystem::path& path, const std::string& name,
                                      const std::string& mime_type, const nlohmann::json& reply_context) {
    (void)reply_context;
    const auto& peer = to.chat;
    if (!valid_id(peer)) return {SendOutcome::Failed, "收件人无效"};
    std::string data, error;
    if (!read_whole_file(path, options_.max_upload_bytes, data, &error)) return {SendOutcome::Failed, error};
    const auto display = name.empty() ? path_to_utf8(path.filename()) : name;
    // 音频(包括 .silk)一律按文件发送:原生语音气泡在上游实现里也没有验证可用。
    const auto type = upload_media_type(mime_type, display);
    PreparedUpload upload;
    if (!prepare_upload(data, upload, &error)) return {SendOutcome::Failed, error};
    data.clear();
    data.shrink_to_fit();

    std::lock_guard<std::mutex> lock(send_mu_);
    ApiResult target;
    for (int attempt = 0; attempt < kUploadAttempts; ++attempt) {
        target = api_.post(kEpGetUploadUrl, upload_url_request(upload, type, peer), kSendTimeout);
        if (target.ok || !(target.status == 0 || target.status >= 500 || target.rate_limited)) break;
        if (attempt + 1 < kUploadAttempts && !wait_for(options_.send_retry_delay * (attempt + 1))) break;
    }
    if (!target.ok) {
        LOG_WARN("[channels/weixin] getuploadurl failed (" + describe(target) + ")");
        return {SendOutcome::Failed, target.error};
    }
    const auto url = upload_target(target.body, options_.api.cdn_base, upload.filekey);
    if (url.empty()) return {SendOutcome::Failed, "微信没有返回文件上传地址"};
    CdnUploadResult uploaded;
    for (int attempt = 0; attempt < kUploadAttempts; ++attempt) {
        uploaded = cdn_upload(url, upload.ciphertext, options_.api.use_proxy, kUploadTimeout);
        if (uploaded.ok || !uploaded.retryable) break;
        if (attempt + 1 < kUploadAttempts && !wait_for(options_.send_retry_delay * (attempt + 1))) break;
    }
    if (!uploaded.ok) {
        LOG_WARN("[channels/weixin] CDN upload failed (status=" + std::to_string(uploaded.status) + ")");
        return {SendOutcome::Failed, uploaded.error};
    }
    const auto item = media_item(type, uploaded.encrypted_param, upload.aes_key, upload.ciphertext.size(),
                                 upload.raw_size, upload.md5_hex, display);
    const auto result = deliver_item(peer, item);
    if (!result.ok) return {SendOutcome::Failed, result.error};
    return {SendOutcome::Sent, {}};
}

bool WeixinTransport::download(const Attachment& attachment, const std::filesystem::path& dest, std::string* error) {
    const auto ref = decode_media_ref(attachment.remote_ref);
    if (!ref) {
        if (error) *error = "附件缺少下载信息";
        return false;
    }
    if (attachment.size > options_.max_download_bytes) {
        if (error) *error = "文件超过微信 " + size_label(options_.max_download_bytes) + " 下载上限";
        return false;
    }
    return download_media(options_.api, *ref, dest, options_.max_download_bytes, error);
}

std::string WeixinTransport::typing_ticket(const std::string& peer, bool allow_fetch) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = tickets_.find(peer);
        if (it != tickets_.end() && it->second.expires > Clock::now()) return it->second.value;
    }
    if (!allow_fetch) return {};
    nlohmann::json body{{"ilink_user_id", peer}};
    const auto token = context_token_for(peer);
    if (!token.empty()) body["context_token"] = token;
    const std::function<bool()> cancel = [&stopping = stopping_] { return stopping.load(); };
    const auto result = api_.post(kEpGetConfig, body, kConfigTimeout, cancel);
    const auto ticket = result.ok && result.body.contains("typing_ticket") && result.body["typing_ticket"].is_string()
                            ? result.body["typing_ticket"].get<std::string>()
                            : std::string{};
    if (ticket.empty()) {
        if (!result.cancelled) LOG_DEBUG("[channels/weixin] getconfig gave no typing ticket (" + describe(result) + ")");
        return {};
    }
    std::lock_guard<std::mutex> lock(mu_);
    tickets_[peer] = Ticket{ticket, Clock::now() + options_.typing_ticket_ttl};
    return ticket;
}

void WeixinTransport::send_typing(const std::string& peer, int status, bool allow_fetch) {
    const auto ticket = typing_ticket(peer, allow_fetch);
    if (ticket.empty()) return;
    const nlohmann::json body{{"ilink_user_id", peer}, {"typing_ticket", ticket}, {"status", status}};
    // 停机时的取消不能被停机标志打断,只限制时间。
    const auto result = allow_fetch
                            ? api_.post(kEpSendTyping, body, kConfigTimeout,
                                        [&stopping = stopping_] { return stopping.load(); })
                            : api_.post(kEpSendTyping, body, std::chrono::seconds(3));
    if (!result.ok && !result.cancelled) {
        LOG_DEBUG("[channels/weixin] sendtyping failed (" + describe(result) + ")");
        std::lock_guard<std::mutex> lock(mu_);
        tickets_.erase(peer);  // 下次重新取 ticket
    }
}

void WeixinTransport::typing_loop() {
    while (!stopping_) {
        std::vector<std::string> stops, due;
        std::chrono::milliseconds wait = options_.typing_interval;
        {
            std::lock_guard<std::mutex> lock(typing_mu_);
            stops.assign(typing_stop_.begin(), typing_stop_.end());
            typing_stop_.clear();
            const auto now = Clock::now();
            for (const auto& peer : typing_) {
                const auto it = typing_sent_.find(peer);
                if (it == typing_sent_.end() || now - it->second >= options_.typing_interval) {
                    due.push_back(peer);
                    typing_sent_[peer] = now;
                } else {
                    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                        it->second + options_.typing_interval - now);
                    wait = (std::min)(wait, left);
                }
            }
        }
        for (const auto& peer : stops) send_typing(peer, 2, true);
        for (const auto& peer : due) send_typing(peer, 1, true);
        std::unique_lock<std::mutex> lock(typing_mu_);
        typing_cv_.wait_for(lock, wait, [this] { return stopping_.load() || typing_dirty_; });
        typing_dirty_ = false;
    }
    // 停机时对仍显示“正在输入”的用户补发取消(只用缓存的 ticket,不再请求 getconfig)。
    std::set<std::string> pending;
    {
        std::lock_guard<std::mutex> lock(typing_mu_);
        pending.insert(typing_.begin(), typing_.end());
        pending.insert(typing_stop_.begin(), typing_stop_.end());
    }
    for (const auto& peer : pending) send_typing(peer, 2, false);
}

void WeixinTransport::set_typing(const Address& to, bool on) {
    const auto& peer = to.chat;
    if (!valid_id(peer)) return;
    {
        std::lock_guard<std::mutex> lock(typing_mu_);
        if (on) {
            if (typing_.insert(peer).second) {
                typing_stop_.erase(peer);
                typing_sent_.erase(peer);
            }
        } else if (typing_.erase(peer)) {
            typing_stop_.insert(peer);
            typing_sent_.erase(peer);
        }
        typing_dirty_ = true;
    }
    typing_cv_.notify_all();
}

} // namespace acecode::im::weixin
