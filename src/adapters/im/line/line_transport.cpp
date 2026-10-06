#include "im/line/line_transport.hpp"

#include "im/http.hpp"
#include "im/redact.hpp"
#include "platform/crypto/secure_random.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace acecode::im::line {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kSeenLimit = 4096;
constexpr std::size_t kRecentLimit = 512;
constexpr std::size_t kNameCacheLimit = 512;
constexpr auto kNameTtl = std::chrono::hours(1);

constexpr const char* kUseWebhookOff =
    "请在 LINE Developers Console 的 Messaging API 页开启 Use webhook,开启后自动连上";
constexpr const char* kSetupHint =
    "需在 LINE Developers Console → Messaging API 开启 Use webhook;并在 LINE Official Account Manager "
    "关闭“自动回复”和“欢迎消息”,否则官方账号会同时自动应答";

std::int64_t now_wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string trim_slash(std::string base) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base;
}

ListenerReply plain_reply(int status, const std::string& body) {
    ListenerReply reply;
    reply.status = status;
    reply.body = body;
    reply.headers = {{"Cache-Control", "no-store"}};
    return reply;
}

} // namespace

LineTransport::LineTransport(LineTransportOptions options)
    : options_(std::move(options)), api_(options_.api), tunnel_(options_.tunnel), media_(options_.media_ttl) {
    options_.public_url = trim_slash(options_.public_url);
    status_.extra["setup_hint"] = kSetupHint;
    status_.extra["tunnel"] = tunnel_mode() ? "cloudflare" : "custom";
}

LineTransport::~LineTransport() { stop(); }

Capabilities LineTransport::capabilities() const {
    Capabilities caps;
    caps.max_text_units = kChunkUnits;
    caps.count_utf16 = true;
    caps.batch_turn_output = true;  // 一个回合合并成一次回复,尽量用免费的回复令牌
    caps.supports_typing = true;
    caps.max_upload_bytes = options_.max_image_bytes;
    caps.max_download_bytes = options_.max_download_bytes;
    return caps;
}

void LineTransport::start(TransportCallbacks callbacks) {
    if (running_.exchange(true)) return;
    callbacks_ = std::move(callbacks);
    stopping_ = false;
    cancel_flag_->store(false);
    {
        std::lock_guard<std::mutex> lock(mu_);
        instance_id_ = platform::secure_random_token(24);
        if (instance_id_.empty()) instance_id_ = "line-" + std::to_string(now_wall_ms());
    }
    link_ = acecode::JoiningThread(&LineTransport::link_loop, this);
    events_ = acecode::JoiningThread(&LineTransport::event_loop, this);
    typer_ = acecode::JoiningThread(&LineTransport::typing_loop, this);
}

void LineTransport::stop() {
    if (!running_.exchange(false)) return;
    stopping_ = true;
    cancel_flag_->store(true);
    // 先进出一次各自的锁再通知:等待方要么还没检查条件(会看到 stopping_),要么已在等待,
    // 不会丢失这次唤醒(事件线程的等待没有超时,丢了就永远停不下来)。
    { std::lock_guard<std::mutex> lock(wake_mu_); }
    wake_.notify_all();
    { std::lock_guard<std::mutex> lock(queue_mu_); }
    queue_cv_.notify_all();
    if (link_.joinable()) link_.join();
    // 回调端口停下后不会再进来新的请求;之后才能收尾事件线程。
    if (options_.listener) options_.listener->stop();
    listener_port_ = 0;
    if (events_.joinable()) events_.join();
    if (typer_.joinable()) typer_.join();
    drop_tunnel();
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        queue_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        typing_.clear();
        public_base_.clear();
    }
    set_status(LinkState::Stopped, {});
}

TransportStatus LineTransport::status() const {
    TransportStatus copy;
    {
        std::lock_guard<std::mutex> lock(mu_);
        copy = status_;
    }
    copy.extra["held"] = held_count();
    copy.extra["push_quota_exhausted"] = quota_exhausted_.load();
    return copy;
}

std::size_t LineTransport::held_count() const {
    std::lock_guard<std::mutex> lock(held_mu_);
    std::size_t sum = 0;
    for (const auto& entry : held_) sum += entry.second.size();
    return sum;
}

std::uint16_t LineTransport::listen_port() const { return listener_port_.load(); }

std::string LineTransport::public_base() const {
    std::lock_guard<std::mutex> lock(mu_);
    return public_base_;
}

std::string LineTransport::account() const {
    std::lock_guard<std::mutex> lock(mu_);
    return account_;
}

void LineTransport::set_status(LinkState state, const std::string& detail, bool retry_stopped) {
    TransportStatus snapshot;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto clean = redact_secrets(detail, {options_.api.channel_secret, options_.api.access_token});
        if (status_.state == state && status_.detail == clean && status_.retry_stopped == retry_stopped) return;
        status_.state = state;
        status_.detail = clean;
        status_.retry_stopped = retry_stopped;
        snapshot = status_;
    }
    if (state == LinkState::Failed || state == LinkState::Retrying)
        LOG_INFO(std::string("[channels/line] link ") + link_state_name(state) +
                 (retry_stopped ? " (retry stopped)" : ""));
    if (callbacks_.on_status) callbacks_.on_status(snapshot);
}

void LineTransport::drop_tunnel() {
    // 隧道一停,旧的公网地址(含图片临时链接)就失效了;重新登记前不再对外给出它。
    tunnel_.stop();
    if (!tunnel_mode()) return;
    std::lock_guard<std::mutex> lock(mu_);
    public_base_.clear();
}

void LineTransport::set_extra(const std::string& key, nlohmann::json value) {
    std::lock_guard<std::mutex> lock(mu_);
    status_.extra[key] = std::move(value);
}

void LineTransport::progress(std::size_t attempt, const std::string& detail) {
    set_status(attempt == 0 ? LinkState::Connecting : LinkState::Retrying, detail);
}

bool LineTransport::wait_for(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(wake_mu_);
    wake_.wait_for(lock, duration, [this] { return stopping_.load(); });
    return !stopping_;
}

CancelFn LineTransport::cancel_fn() const {
    return [flag = cancel_flag_] { return flag->load(); };
}

ListenerHandler LineTransport::listener_handler() {
    return [ref = lifetime_.ref(*this)](const ListenerRequest& request) {
        ListenerReply reply = plain_reply(503, "unavailable");
        ref.with([&reply, &request](LineTransport& transport) { reply = transport.handle_request(request); });
        return reply;
    };
}

// ---------------------------------------------------------------- 连接与看守

void LineTransport::link_loop() {
    std::size_t attempt = 0;
    set_status(LinkState::Connecting, "正在连接 LINE");
    while (!stopping_) {
        bool reached = false;
        LinkStep step = LinkStep::Retry;
        try {
            step = connect_and_watch(attempt, &reached);
        } catch (const std::exception& e) {
            // 线程函数不能抛出异常(会终止进程):记日志后按普通临时错误重试。
            LOG_ERROR(std::string("[channels/line] unexpected error while connecting: ") + e.what());
            set_status(LinkState::Retrying, "连接 LINE 时出现内部错误,稍后重试");
        }
        if (step == LinkStep::Stopped || stopping_) break;
        if (step == LinkStep::Fatal) {
            // 致命错误不会自己好:不再保留公网隧道,等用户处理后重新打开。
            drop_tunnel();
            break;
        }
        if (reached) attempt = 0;
        const auto& steps = options_.backoff;
        const auto delay = steps.empty() ? std::chrono::milliseconds(1000) : steps[(std::min)(attempt, steps.size() - 1)];
        ++attempt;
        if (!wait_for(delay)) break;
    }
    if (!stopping_) {
        std::unique_lock<std::mutex> lock(wake_mu_);
        wake_.wait(lock, [this] { return stopping_.load(); });
    }
}

LineTransport::LinkStep LineTransport::connect_and_watch(std::size_t attempt, bool* reached) {
    const auto cancel = cancel_fn();
    progress(attempt, attempt == 0 ? "正在连接 LINE" : "正在重新连接 LINE");

    BotInfo bot;
    const auto info = api_.bot_info(&bot, cancel);
    if (stopping_ || info.cancelled) return LinkStep::Stopped;
    if (!info.ok) {
        if (info.auth_failed) {
            set_status(LinkState::Failed, info.message, true);
            return LinkStep::Fatal;
        }
        set_status(LinkState::Retrying, info.status == 0 ? info.message : "LINE 暂时不可用:" + info.message);
        return LinkStep::Retry;
    }
    if (bot.user_id.empty()) {
        set_status(LinkState::Retrying, "LINE 返回的机器人信息无效");
        return LinkStep::Retry;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        account_ = bot.user_id;
        status_.account = bot.user_id;
        status_.display_name = bot.display_name;
        status_.extra["basic_id"] = bot.basic_id;
        status_.extra["add_friend_url"] = add_friend_url(bot.basic_id);
        status_.extra["chat_mode"] = bot.chat_mode;
    }

    LinkStep step = LinkStep::Retry;
    if (!ensure_listener(&step)) return step;
    std::string base;
    if (!resolve_public_base(attempt, &base, &step)) return step;

    if (options_.verify_public_url) {
        progress(attempt, "正在检查公网地址能否访问本机");
        if (!check_public(base, options_.public_check_timeout)) {
            if (stopping_) return LinkStep::Stopped;
            if (tunnel_mode()) {
                drop_tunnel();  // 换一条隧道(新主机名)再试
                set_status(LinkState::Retrying, "Cloudflare 隧道暂时无法从公网访问,正在重建");
            } else {
                set_status(LinkState::Retrying, "无法通过 " + base + " 访问本机 LINE 回调端口,请确认反向代理指向 http://127.0.0.1:" +
                                                    std::to_string(listener_port_.load()));
            }
            return LinkStep::Retry;
        }
    }

    const auto desired = base + kWebhookRoute;
    WebhookInfo webhook;
    if (!register_webhook(desired, &webhook, &step)) return step;
    {
        std::lock_guard<std::mutex> lock(mu_);
        public_base_ = base;
        status_.extra["public_url"] = base;
        status_.extra["webhook_url"] = desired;
    }
    log_quota();

    while (!webhook.active) {
        set_extra("webhook_active", false);
        set_status(LinkState::Retrying, kUseWebhookOff);
        if (!wait_for(options_.webhook_recheck)) return LinkStep::Stopped;
        if (tunnel_mode() && !tunnel_.alive()) {
            drop_tunnel();
            return LinkStep::Retry;
        }
        const auto result = api_.webhook_info(&webhook, cancel);
        if (stopping_) return LinkStep::Stopped;
        if (result.auth_failed) {
            set_status(LinkState::Failed, result.message, true);
            return LinkStep::Fatal;
        }
        if (result.ok && webhook.endpoint != desired) {
            LOG_WARN("[channels/line] webhook endpoint was changed elsewhere, registering it again");
            api_.set_webhook(desired, cancel);
        }
    }
    set_extra("webhook_active", true);
    *reached = true;
    LOG_INFO("[channels/line] connected, webhook " + desired);
    set_status(LinkState::Connected, {});
    return watch(base, desired);
}

bool LineTransport::ensure_listener(LinkStep* step) {
    if (!options_.listener) {
        set_status(LinkState::Failed, "LINE 回调端口组件缺失", true);
        *step = LinkStep::Fatal;
        return false;
    }
    if (listener_port_.load() != 0 && options_.listener->port() == listener_port_.load()) return true;
    std::string error;
    auto port = options_.listener->start(options_.listen_port, listener_handler(), &error);
    if (port == 0 && options_.listen_port != 0 && tunnel_mode()) {
        // 隧道模式下端口号无关紧要:指定端口被占用时退回系统分配。
        LOG_WARN("[channels/line] listener port " + std::to_string(options_.listen_port) +
                 " unavailable, falling back to an OS-assigned port");
        port = options_.listener->start(0, listener_handler(), &error);
    }
    if (port == 0) {
        set_status(LinkState::Retrying, "无法在本机 127.0.0.1:" + std::to_string(options_.listen_port) +
                                            " 上接收 LINE 回调:" + error);
        *step = LinkStep::Retry;
        return false;
    }
    const auto previous = listener_port_.exchange(port);
    if (previous != 0 && previous != port) drop_tunnel();  // 旧隧道指向旧端口
    set_extra("listen_port", port);
    LOG_INFO("[channels/line] webhook listener on 127.0.0.1:" + std::to_string(port));
    if (port != options_.listen_port && options_.on_listen_port) options_.on_listen_port(port);
    return true;
}

bool LineTransport::resolve_public_base(std::size_t attempt, std::string* base, LinkStep* step) {
    if (!tunnel_mode()) {
        *base = options_.public_url;
        return true;
    }
    if (tunnel_.alive() && !tunnel_.public_url().empty()) {
        *base = tunnel_.public_url();
        return true;
    }
    drop_tunnel();
    progress(attempt, "正在建立 Cloudflare 隧道");
    TunnelFailure failure = TunnelFailure::None;
    std::string error;
    *base = tunnel_.start(listener_port_.load(), cancel_fn(), &failure, &error);
    if (failure == TunnelFailure::Missing) {
        set_extra("cloudflared_missing", true);  // 设置页据此提示安装,不必解析文字
        set_status(LinkState::Failed, kCloudflaredInstallHint, true);
        *step = LinkStep::Fatal;
        return false;
    }
    if (failure == TunnelFailure::Cancelled || stopping_) {
        *step = LinkStep::Stopped;
        return false;
    }
    if (base->empty()) {
        set_status(LinkState::Retrying, "Cloudflare 隧道启动失败:" + error);
        *step = LinkStep::Retry;
        return false;
    }
    return true;
}

bool LineTransport::check_public(const std::string& base, std::chrono::milliseconds timeout) {
    std::string instance;
    {
        std::lock_guard<std::mutex> lock(mu_);
        instance = instance_id_;
    }
    const auto deadline = Clock::now() + timeout;
    bool warned = false;
    while (!stopping_) {
        HttpRequest request;
        request.url = base + kHealthRoute;
        request.timeout = std::chrono::seconds(10);
        request.use_proxy = options_.api.use_proxy;
        request.cancel = cancel_fn();
        const auto response = http_send(request);
        if (response.status == 200 && response.body.find(instance) != std::string::npos) return true;
        if (response.status == 200 && !warned) {
            // 有应答但不是本进程的回调端口:公网地址指错了地方。
            LOG_WARN("[channels/line] public URL answered, but not from this ACECode listener");
            warned = true;
        }
        if (Clock::now() >= deadline) break;
        if (!wait_for(options_.public_check_interval)) break;
    }
    LOG_WARN("[channels/line] public URL self-check failed");
    return false;
}

bool LineTransport::register_webhook(const std::string& desired, WebhookInfo* info, LinkStep* step) {
    const auto cancel = cancel_fn();
    // 每次连上都重新登记一次(便宜且可靠:快速隧道每次重启主机名都会变,别人也可能在控制台改过)。
    const auto put = api_.set_webhook(desired, cancel);
    if (stopping_) {
        *step = LinkStep::Stopped;
        return false;
    }
    if (put.auth_failed) {
        set_status(LinkState::Failed, put.message, true);
        *step = LinkStep::Fatal;
        return false;
    }
    if (!put.ok) {
        if (put.status == 400 && !tunnel_mode()) {
            set_status(LinkState::Failed, "LINE 不接受这个 webhook 地址(需要可从公网访问的 https 地址):" + put.message,
                       true);
            *step = LinkStep::Fatal;
        } else {
            set_status(LinkState::Retrying, "设置 LINE webhook 地址失败:" + put.message);
            *step = LinkStep::Retry;
        }
        return false;
    }
    LOG_INFO("[channels/line] webhook endpoint registered (LINE may take up to a minute to switch)");
    // 改地址不影响 “Use webhook” 开关(只能在控制台改),读出来决定是否算已连接。
    const auto result = api_.webhook_info(info, cancel);
    if (stopping_) {
        *step = LinkStep::Stopped;
        return false;
    }
    if (result.auth_failed) {
        set_status(LinkState::Failed, result.message, true);
        *step = LinkStep::Fatal;
        return false;
    }
    if (!result.ok && result.status != 404) {
        set_status(LinkState::Retrying, "读取 LINE webhook 设置失败:" + result.message);
        *step = LinkStep::Retry;
        return false;
    }
    // 404:LINE 侧缓存还没反映刚登记的地址(最多 1 分钟),按未开启处理,稍后复查。
    if (!result.ok) info->active = false;
    if (options_.test_webhook && desired != last_tested_endpoint_ && !run_webhook_test(desired, step)) return false;
    return true;
}

bool LineTransport::run_webhook_test(const std::string& desired, LinkStep* step) {
    // 测试接口每频道每小时只有 60 次,自己再限到每 10 分钟最多一次。
    const auto now = Clock::now();
    if (last_webhook_test_ != Clock::time_point{} && now - last_webhook_test_ < options_.webhook_test_interval)
        return true;
    last_webhook_test_ = now;
    last_tested_endpoint_ = desired;
    const auto result = api_.test_webhook(desired, cancel_fn());
    if (stopping_) {
        *step = LinkStep::Stopped;
        return false;
    }
    if (!result.ok) {
        LOG_WARN("[channels/line] webhook test call failed: " + result.message);
        return true;
    }
    if (json_bool(result.body, "success")) {
        LOG_INFO("[channels/line] LINE webhook test succeeded");
        return true;
    }
    const auto reason = json_string(result.body, "reason");
    const auto detail = json_string(result.body, "detail");
    LOG_WARN("[channels/line] LINE webhook test failed: reason=" + reason + " detail=" +
             redact_secrets(detail, {options_.api.channel_secret}));
    if (reason == "ERROR_STATUS_CODE" && detail == "401") {
        // LINE 的测试请求带签名,本机回 401 说明签名对不上:Channel secret 填错了。
        set_status(LinkState::Failed, "Channel secret 与该频道不匹配,LINE 发来的消息无法通过签名校验", true);
        *step = LinkStep::Fatal;
        return false;
    }
    return true;
}

LineTransport::LinkStep LineTransport::watch(const std::string& base, const std::string& desired) {
    const auto cancel = cancel_fn();
    int tick = 0;
    int self_failures = 0;
    int not_ready = 0;
    bool inactive = false;
    while (true) {
        if (!wait_for(options_.watch_interval)) return LinkStep::Stopped;
        ++tick;
        if (tunnel_mode()) {
            if (!tunnel_.alive()) {
                LOG_WARN("[channels/line] cloudflared exited, rebuilding the quick tunnel");
                drop_tunnel();
                set_status(LinkState::Retrying, "Cloudflare 隧道已退出,正在重建");
                return LinkStep::Retry;
            }
            if (!tunnel_.ready()) {
                // cloudflared 会自己重连边缘(主机名不变);长时间连不上才换一条隧道。
                if (++not_ready >= 10) {
                    drop_tunnel();
                    set_status(LinkState::Retrying, "Cloudflare 隧道长时间断开,正在重建");
                    return LinkStep::Retry;
                }
                set_status(LinkState::Retrying, "Cloudflare 隧道暂时断开,等待自动重连");
                continue;
            }
            not_ready = 0;
        }
        const bool periodic = options_.self_check_every > 0 && tick % options_.self_check_every == 0;
        if (periodic && options_.verify_public_url) {
            if (!check_public(base, std::chrono::seconds(10))) {
                if (stopping_) return LinkStep::Stopped;
                if (++self_failures >= 3) {
                    if (tunnel_mode()) drop_tunnel();
                    set_status(LinkState::Retrying, "公网地址无法访问本机回调端口,正在重新连接");
                    return LinkStep::Retry;
                }
            } else {
                self_failures = 0;
            }
        }
        if (periodic || inactive) {
            WebhookInfo webhook;
            const auto result = api_.webhook_info(&webhook, cancel);
            if (stopping_) return LinkStep::Stopped;
            if (result.auth_failed) {
                set_status(LinkState::Failed, result.message, true);
                return LinkStep::Fatal;
            }
            if (result.ok) {
                if (webhook.endpoint != desired) {
                    LOG_WARN("[channels/line] webhook endpoint was changed elsewhere, registering it again");
                    api_.set_webhook(desired, cancel);
                }
                inactive = !webhook.active;
                set_extra("webhook_active", webhook.active);
                if (inactive) {
                    set_status(LinkState::Retrying, kUseWebhookOff);
                    continue;
                }
            }
        }
        set_status(LinkState::Connected, {});
    }
}

void LineTransport::log_quota() {
    nlohmann::json summary;
    const auto result = api_.quota(&summary, cancel_fn());
    if (!result.ok) return;
    set_extra("push_quota", summary);
    LOG_INFO("[channels/line] push quota: " + summary.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

// ---------------------------------------------------------------- 回调端口

ListenerReply LineTransport::handle_request(const ListenerRequest& request) {
    if (request.path == kWebhookRoute) {
        if (request.method != "POST") return plain_reply(405, "method not allowed");
        return accept_webhook(request.body, request.signature);
    }
    if (request.path == kHealthRoute) {
        if (request.method != "GET" && request.method != "HEAD") return plain_reply(405, "method not allowed");
        std::string instance;
        {
            std::lock_guard<std::mutex> lock(mu_);
            instance = instance_id_;
        }
        auto reply = plain_reply(200, nlohmann::json{{"status", "ok"}, {"platform", "line"}, {"instance", instance}}.dump());
        reply.content_type = "application/json";
        return reply;
    }
    if (request.path.rfind(kMediaRoutePrefix, 0) == 0) {
        if (request.method != "GET" && request.method != "HEAD") return plain_reply(405, "method not allowed");
        return serve_media(request.path, request.method == "HEAD");
    }
    return plain_reply(404, "not found");
}

ListenerReply LineTransport::accept_webhook(const std::string& body, const std::string& signature) {
    if (body.size() > kMaxWebhookBodyBytes) return plain_reply(413, "payload too large");
    // 签名必须在解析之前、按原始字节校验。
    if (!verify_signature(body, signature, options_.api.channel_secret)) {
        const auto count = ++rejected_signatures_;
        if (count == 1 || count % 100 == 0)
            LOG_WARN("[channels/line] rejected webhook with invalid signature (total " + std::to_string(count) + ")");
        return plain_reply(401, "invalid signature");
    }
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        if (queue_.size() >= options_.max_queue) {
            LOG_WARN("[channels/line] webhook queue full, asking LINE to redeliver later");
            return plain_reply(503, "busy");
        }
        queue_.push_back({body, now_wall_ms()});
    }
    queue_cv_.notify_one();
    return plain_reply(200, "OK");
}

ListenerReply LineTransport::serve_media(const std::string& path, bool head) {
    const auto entry = media_.find_path(path);
    if (!entry) return plain_reply(404, "not found");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(entry->path, ec)) return plain_reply(404, "not found");
    const auto size = std::filesystem::file_size(entry->path, ec);
    if (ec || size > options_.max_image_bytes) return plain_reply(404, "not found");
    ListenerReply reply = plain_reply(200, {});
    reply.content_type = entry->mime_type.empty() ? "application/octet-stream" : entry->mime_type;
    reply.headers.emplace_back("X-Content-Type-Options", "nosniff");
    if (head) return reply;
    std::ifstream in(entry->path, std::ios::binary);
    reply.body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (reply.body.size() != size) return plain_reply(404, "not found");
    return reply;
}

// ---------------------------------------------------------------- 收消息

void LineTransport::event_loop() {
    while (true) {
        QueuedWebhook item;
        {
            std::unique_lock<std::mutex> lock(queue_mu_);
            queue_cv_.wait(lock, [this] { return stopping_.load() || !queue_.empty(); });
            if (stopping_) return;
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            process_webhook(item);
        } catch (const std::exception& e) {
            LOG_WARN(std::string("[channels/line] failed to process webhook: ") + e.what());
        }
    }
}

void LineTransport::process_webhook(const QueuedWebhook& item) {
    const auto bot = account();
    auto parsed = parse_webhook(item.body, bot, item.received_at_ms);
    if (!parsed.valid) {
        LOG_WARN("[channels/line] ignored a signed webhook that is not valid JSON");
        return;
    }
    if (!bot.empty() && !parsed.destination.empty() && parsed.destination != bot) {
        LOG_WARN("[channels/line] ignored a webhook addressed to another bot");
        return;
    }
    for (auto& event : parsed.events) {
        if (stopping_) return;
        handle_event(event);
    }
}

bool LineTransport::seen_before(const std::string& key) {
    std::lock_guard<std::mutex> lock(seen_mu_);
    if (seen_.count(key)) return true;
    seen_.insert(key);
    seen_order_.push_back(key);
    while (seen_order_.size() > kSeenLimit) {
        seen_.erase(seen_order_.front());
        seen_order_.pop_front();
    }
    return false;
}

void LineTransport::handle_event(WebhookEvent& event) {
    if (!event.event_id.empty() && seen_before("e:" + event.event_id)) {
        LOG_DEBUG("[channels/line] duplicate webhook event skipped");
        return;
    }
    if (event.standby) {
        LOG_INFO("[channels/line] standby event ignored (another module owns this chat)");
        return;
    }
    switch (event.kind) {
        case EventKind::Message:
            break;
        case EventKind::Follow:
        case EventKind::Unfollow:
        case EventKind::Join:
        case EventKind::Leave:
        case EventKind::Unsend:
        case EventKind::MessageEdited:
            LOG_INFO("[channels/line] " + event.type + " event (" + event.source_type + ")");
            return;
        default:
            LOG_DEBUG("[channels/line] ignored event type " + event.type);
            return;
    }
    if (!event.message_id.empty() && seen_before("m:" + event.message_id)) return;
    if (event.redelivery) LOG_INFO("[channels/line] processing a redelivered message event");
    if (!event.inbound) {
        if (event.unidentified_sender) {
            LOG_INFO("[channels/line] group message without userId (LINE desktop sender) ignored");
            if (event.mentioned_self && !event.reply_token.empty()) send_unidentified_notice(event.reply_token);
        }
        return;
    }
    Inbound inbound = std::move(*event.inbound);
    if (!event.quoted_message_id.empty()) {
        // 平台不提供被引用消息的内容:从最近收发的消息里找;引用机器人的消息视同点名。
        if (const auto quoted = recall_message(event.quoted_message_id)) {
            inbound.quote_text = quoted->text;
            if (quoted->from_bot) inbound.mentioned = true;
        }
    }
    if (!inbound.text.empty()) remember_message(inbound.message_id, inbound.text, false);
    if (options_.fetch_profiles) inbound.sender_name = sender_name(inbound.address);
    note_reply_token(inbound.address, inbound.reply_context);
    // 先用这条新消息的回复令牌补发之前暂存的输出,再交给核心处理新消息。
    flush_held(inbound.address, inbound.reply_context);
    if (callbacks_.on_inbound) callbacks_.on_inbound(std::move(inbound));
}

std::string LineTransport::sender_name(const Address& address) {
    const auto key = address.chat + "|" + address.sender;
    const auto now = Clock::now();
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = names_.find(key);
        if (it != names_.end() && now - it->second.second < kNameTtl) return it->second.first;
    }
    std::string name;
    const auto result = api_.display_name(address.chat, address.sender, &name);
    if (!result.ok) {
        LOG_DEBUG("[channels/line] display name unavailable (HTTP " + std::to_string(result.status) + ")");
        name.clear();
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (names_.size() >= kNameCacheLimit) names_.clear();
    names_[key] = {name, now};
    return name;
}

void LineTransport::remember_message(const std::string& id, const std::string& text, bool from_bot) {
    if (id.empty()) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (!recent_.count(id)) recent_order_.push_back(id);
    auto& entry = recent_[id];
    entry.text = text.size() > 4000 ? text.substr(0, 4000) : text;
    entry.from_bot = from_bot;
    while (recent_order_.size() > kRecentLimit) {
        recent_.erase(recent_order_.front());
        recent_order_.pop_front();
    }
}

std::optional<LineTransport::RecentMessage> LineTransport::recall_message(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = recent_.find(id);
    if (it == recent_.end()) return std::nullopt;
    return it->second;
}

void LineTransport::note_reply_token(const Address& address, const nlohmann::json& reply_context) {
    const auto token = reply_token_of(reply_context);
    if (!token) return;
    std::lock_guard<std::mutex> lock(mu_);
    const auto now = now_wall_ms();
    for (auto it = latest_reply_.begin(); it != latest_reply_.end();) {
        if (!reply_token_fresh(it->second, now)) it = latest_reply_.erase(it);
        else ++it;
    }
    latest_reply_[address.key()] = *token;
}

} // namespace acecode::im::line
