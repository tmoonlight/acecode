// LineTransport 的发送部分:回复 / 推送选择、每次最多 5 条、推送额度用完时暂存补发、
// 图片临时链接、文件改发说明、“正在输入”动画、附件下载。

#include "im/line/line_transport.hpp"

#include "im/redact.hpp"
#include "im/text_chunk.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <cstdio>

namespace acecode::im::line {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kUsedTokenLimit = 4096;

std::int64_t now_wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string human_size(std::uint64_t bytes) {
    char buffer[32];
    if (bytes >= 1024u * 1024u)
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else if (bytes >= 1024u)
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    else
        std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
    return buffer;
}

nlohmann::json messages_of(const std::vector<nlohmann::json>& all, std::size_t begin, std::size_t count) {
    nlohmann::json out = nlohmann::json::array();
    for (std::size_t i = begin; i < begin + count && i < all.size(); ++i) out.push_back(all[i]);
    return out;
}

std::string push_error(const ApiResult& result) {
    if (result.status == 0) return result.message.empty() ? "无法连接 LINE" : result.message;
    if (result.status == 403) return "LINE 拒绝推送(当前方案或权限不允许):" + result.message;
    if (result.status == 400) return "LINE 拒绝推送(对方可能已封锁官方账号或不在群里):" + result.message;
    return "LINE 推送失败:" + result.message;
}

} // namespace

SendResult LineTransport::send_text(const Address& to, const std::string& text, const nlohmann::json& reply_context) {
    HeldItem item;
    item.text = text;
    std::lock_guard<std::mutex> lock(send_mu_);
    return deliver_locked(to, {item}, reply_context, false);
}

SendResult LineTransport::send_file(const Address& to, const std::filesystem::path& path, const std::string& name,
                                    const std::string& mime_type, const nlohmann::json& reply_context) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return {SendOutcome::Failed, "文件不存在或不可读"};
    const auto display = name.empty() ? path_to_utf8(path.filename()) : name;
    std::string notice;
    if (!is_line_image(mime_type, display)) {
        notice = "文件「" + display + "」(" + human_size(size) + ")已生成,但 LINE 机器人不能发送文件,请在 ACECode 中查看。";
    } else if (size > options_.max_image_bytes) {
        notice = "图片「" + display + "」超过 LINE 的 10 MB 上限,未发送;请在 ACECode 中查看。";
    } else if (public_base().empty()) {
        notice = "图片「" + display + "」暂时无法发送(LINE 回调地址尚未就绪),请在 ACECode 中查看。";
    }
    std::lock_guard<std::mutex> lock(send_mu_);
    if (!notice.empty()) {
        LOG_INFO("[channels/line] file of " + std::to_string(size) + " bytes cannot be sent as a LINE message, sending a notice");
        HeldItem item;
        item.plain = true;
        item.text = notice;
        return deliver_locked(to, {item}, reply_context, false);
    }
    HeldItem item;
    item.file = true;
    item.path = path;
    item.name = display;
    item.mime_type = mime_type.empty() ? "image/jpeg" : mime_type;
    return deliver_locked(to, {item}, reply_context, false);
}

std::vector<LineTransport::Piece> LineTransport::build_pieces(const Address& to, const std::vector<HeldItem>& items,
                                                              const nlohmann::json& reply_context, bool flushing) {
    std::vector<Piece> pieces;
    bool first_text = true;
    std::string quote;
    // 群里引用触发本回合的那条消息,方便大家看出在回答谁(引用令牌长期有效,可重复使用)。
    if (!flushing && to.kind == ChatKind::Group && reply_context.is_object())
        quote = json_string(reply_context, "quote_token");
    for (const auto& item : items) {
        if (item.file) {
            const auto base = public_base();
            const auto relative = base.empty() ? std::string{} : media_.add(item.path, item.name, item.mime_type);
            if (relative.empty()) {
                Piece piece;
                piece.text = "图片「" + item.name + "」暂时无法发送(LINE 回调地址尚未就绪),请在 ACECode 中查看。";
                piece.source.plain = true;
                piece.source.text = piece.text;
                piece.message = text_message(piece.text);
                pieces.push_back(std::move(piece));
                continue;
            }
            Piece piece;
            piece.source = item;
            piece.message = image_message(base + kMediaRoutePrefix + relative);
            pieces.push_back(std::move(piece));
            continue;
        }
        const auto chunks = item.plain ? chunk_text(item.text, kChunkUnits, true) : plain_text_chunks(item.text);
        for (const auto& chunk : chunks) {
            Piece piece;
            piece.text = chunk;
            piece.source.plain = true;
            piece.source.text = chunk;
            const auto shown = flushing && first_text ? "(补发)" + chunk : chunk;
            piece.message = text_message(shown, first_text ? quote : std::string{});
            first_text = false;
            pieces.push_back(std::move(piece));
        }
    }
    return pieces;
}

std::optional<ReplyToken> LineTransport::usable_reply_token_locked(const Address& to,
                                                                   const nlohmann::json& reply_context) {
    const auto now = now_wall_ms();
    const auto from_context = reply_token_of(reply_context);
    if (from_context && reply_token_fresh(*from_context, now) && !used_tokens_.count(from_context->token))
        return from_context;
    // 没有可用的上下文令牌(Desktop 里输入的回复、或令牌已用):用该会话最近一条消息的令牌。
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = latest_reply_.find(to.key());
    if (it != latest_reply_.end() && reply_token_fresh(it->second, now) && !used_tokens_.count(it->second.token))
        return it->second;
    return std::nullopt;
}

void LineTransport::mark_token_used_locked(const std::string& token) {
    if (!used_tokens_.insert(token).second) return;
    used_order_.push_back(token);
    while (used_order_.size() > kUsedTokenLimit) {
        used_tokens_.erase(used_order_.front());
        used_order_.pop_front();
    }
}

void LineTransport::after_sent(const Address& to, const std::vector<Piece>& pieces, std::size_t begin,
                               std::size_t count, const ApiResult& result) {
    // 记下发出的消息 id,对方引用机器人的消息时能认出来。
    if (result.body.is_object() && result.body.contains("sentMessages") && result.body["sentMessages"].is_array()) {
        const auto& sent = result.body["sentMessages"];
        for (std::size_t i = 0; i < sent.size() && i < count; ++i) {
            const auto& piece = pieces[begin + i];
            if (!sent[i].is_object() || piece.text.empty()) continue;
            const auto id = json_string(sent[i], "id");
            if (!id.empty()) remember_message(id, piece.text, true);
        }
    }
    // 官方账号发出消息后“正在输入”动画会消失:仍在忙碌时稍后重显(见 typing_resume_delay)。
    bool typing = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = typing_.find(to.chat);
        if (it != typing_.end()) {
            it->second = Clock::now() + options_.typing_resume_delay;
            typing = true;
        }
    }
    if (typing) wake_typer();
}

SendResult LineTransport::deliver_locked(const Address& to, std::vector<HeldItem> items,
                                         const nlohmann::json& reply_context, bool flushing) {
    const auto key = to.key();
    if (!flushing && held_size(key) > 0) {
        // 已有暂存时新的输出排在后面,保证顺序。
        hold(key, std::move(items));
        return {SendOutcome::Held, {}};
    }
    const auto pieces = build_pieces(to, items, reply_context, flushing);
    if (pieces.empty()) return {SendOutcome::Sent, {}};
    std::vector<nlohmann::json> messages;
    messages.reserve(pieces.size());
    for (const auto& piece : pieces) messages.push_back(piece.message);

    std::size_t index = 0;
    if (const auto token = usable_reply_token_locked(to, reply_context)) {
        const auto count = (std::min)(messages.size(), kMaxMessagesPerCall);
        // 回复令牌只能用一次:无论结果如何都记为已用。
        mark_token_used_locked(token->token);
        ApiResult result;
        for (std::size_t attempt = 0;; ++attempt) {
            result = api_.reply(token->token, messages_of(messages, 0, count));
            const bool rate_limited = result.status == 429 && !is_monthly_limit(result.status, result.message);
            if (!rate_limited || attempt >= options_.send_retry.size() ||
                !reply_token_fresh(*token, now_wall_ms()))
                break;
            if (!wait_for(options_.send_retry[attempt])) break;
        }
        if (result.ok) {
            after_sent(to, pieces, 0, count, result);
            index = count;
        } else if (result.auth_failed) {
            return {SendOutcome::Failed, result.message};
        } else if (result.status >= 400 && result.status < 500) {
            // 令牌失效或已用(400 Invalid reply token)等明确拒收:平台什么都没发,改用推送。
            LOG_INFO("[channels/line] reply refused (HTTP " + std::to_string(result.status) + "), falling back to push");
        } else {
            // 超时 / 5xx:平台可能已经发出,不能重发。
            LOG_WARN("[channels/line] reply failed with unknown outcome: " + result.message);
            return {SendOutcome::Failed, "LINE 回复结果未知(网络或平台错误),未重发:" + result.message};
        }
    }

    while (index < messages.size()) {
        const auto count = (std::min)(messages.size() - index, kMaxMessagesPerCall);
        // 同一批重试必须沿用同一个重试键,LINE 据此去重(重复返回 409)。
        const auto retry_key = new_retry_key();
        ApiResult result;
        for (std::size_t attempt = 0;; ++attempt) {
            result = api_.push(to.chat, messages_of(messages, index, count), retry_key);
            if (result.ok || result.status == 409 || is_monthly_limit(result.status, result.message)) break;
            const bool retryable = !result.auth_failed && !retry_key.empty() &&
                                   (result.status == 429 || result.status >= 500 || result.status == 0);
            if (!retryable || attempt >= options_.send_retry.size()) break;
            LOG_INFO("[channels/line] push failed (HTTP " + std::to_string(result.status) + "), retrying with the same key");
            if (!wait_for(options_.send_retry[attempt])) break;
        }
        if (result.ok || result.status == 409) {
            quota_exhausted_ = false;
            after_sent(to, pieces, index, count, result);
            index += count;
            continue;
        }
        if (is_monthly_limit(result.status, result.message)) {
            quota_exhausted_ = true;
            std::vector<HeldItem> rest;
            for (std::size_t i = index; i < pieces.size(); ++i) rest.push_back(pieces[i].source);
            const auto n = rest.size();
            hold(key, std::move(rest));
            LOG_WARN("[channels/line] monthly push quota exhausted, holding " + std::to_string(n) +
                     " message(s) until the contact's next message");
            return {SendOutcome::Held, "LINE 推送额度已用完,将在对方下一条消息时补发"};
        }
        LOG_WARN("[channels/line] push failed (HTTP " + std::to_string(result.status) + "): " + result.message);
        return {SendOutcome::Failed, push_error(result)};
    }
    return {SendOutcome::Sent, {}};
}

void LineTransport::hold(const std::string& key, std::vector<HeldItem> items) {
    std::lock_guard<std::mutex> lock(held_mu_);
    auto& queue = held_[key];
    for (auto& item : items) queue.push_back(std::move(item));
    std::size_t dropped = 0;
    while (queue.size() > kMaxHeldPerConversation) {
        queue.pop_front();
        ++dropped;
    }
    if (dropped) LOG_WARN("[channels/line] held queue full, dropped " + std::to_string(dropped) + " oldest output(s)");
}

std::deque<HeldItem> LineTransport::take_held(const std::string& key) {
    std::lock_guard<std::mutex> lock(held_mu_);
    const auto it = held_.find(key);
    if (it == held_.end()) return {};
    auto items = std::move(it->second);
    held_.erase(it);
    return items;
}

std::size_t LineTransport::held_size(const std::string& key) const {
    std::lock_guard<std::mutex> lock(held_mu_);
    const auto it = held_.find(key);
    return it == held_.end() ? 0 : it->second.size();
}

void LineTransport::flush_held(const Address& address, const nlohmann::json& reply_context) {
    std::lock_guard<std::mutex> lock(send_mu_);
    auto held = take_held(address.key());
    if (held.empty()) return;
    LOG_INFO("[channels/line] resending " + std::to_string(held.size()) + " held output(s) before the new message");
    deliver_locked(address, std::vector<HeldItem>(held.begin(), held.end()), reply_context, true);
}

void LineTransport::send_unidentified_notice(const std::string& reply_token) {
    std::lock_guard<std::mutex> lock(send_mu_);
    if (used_tokens_.count(reply_token)) return;
    mark_token_used_locked(reply_token);
    const auto result = api_.reply(
        reply_token, nlohmann::json::array({text_message("无法识别发言人:电脑版 LINE 在群里不提供用户 ID,请用手机版 LINE 发送。")}));
    if (!result.ok) LOG_DEBUG("[channels/line] unidentified-sender notice not sent (HTTP " + std::to_string(result.status) + ")");
}

void LineTransport::set_typing(const Address& to, bool on) {
    // LINE 只允许对一对一聊天的用户显示动画(群 / 多人聊天会 400)。
    if (to.kind != ChatKind::Private || !is_user_id(to.chat)) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (on) typing_.emplace(to.chat, Clock::now());
        else typing_.erase(to.chat);
    }
    if (on) wake_typer();
}

void LineTransport::wake_typer() {
    {
        // 在锁内置位:输入线程检查条件与进入等待之间不会漏掉这次唤醒。
        std::lock_guard<std::mutex> lock(wake_mu_);
        typing_wake_ = true;
    }
    wake_.notify_all();
}

void LineTransport::typing_loop() {
    while (!stopping_) {
        std::vector<std::string> due;
        auto next = Clock::now() + options_.typing_refresh;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const auto now = Clock::now();
            for (auto& [chat, at] : typing_) {
                if (at <= now) {
                    due.push_back(chat);
                    at = now + options_.typing_refresh;
                }
                next = (std::min)(next, at);
            }
        }
        for (const auto& chat : due) {
            // 动画最长 60 秒;每 50 秒续一次,忙碌期间不会断。
            const auto result = api_.start_loading(chat, 60);
            if (!result.ok)
                LOG_DEBUG("[channels/line] loading animation not shown (HTTP " + std::to_string(result.status) + ")");
        }
        std::unique_lock<std::mutex> lock(wake_mu_);
        wake_.wait_until(lock, next, [this] { return stopping_.load() || typing_wake_.exchange(false); });
    }
}

bool LineTransport::download(const Attachment& attachment, const std::filesystem::path& dest, std::string* error) {
    if (attachment.remote_ref.empty()) {
        if (error) *error = "该附件无法下载";
        return false;
    }
    if (attachment.size > options_.max_download_bytes) {
        if (error) *error = "附件超过大小限制";
        return false;
    }
    if (attachment.remote_ref.rfind("https://", 0) == 0 || attachment.remote_ref.rfind("http://", 0) == 0)
        return api_.download_url(attachment.remote_ref, dest, options_.max_download_bytes, error, cancel_fn());
    return api_.download_content(attachment.remote_ref, dest, options_.max_download_bytes, error, cancel_fn());
}

} // namespace acecode::im::line
