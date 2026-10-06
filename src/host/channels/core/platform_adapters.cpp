#include "channels/core/platform_adapters.hpp"

#include "im/dingtalk/dingtalk_api.hpp"
#include "channels/core/line_webhook_server.hpp"
#include "im/discord/discord_api.hpp"
#include "im/feishu/feishu_api.hpp"
#include "im/line/line_api.hpp"
#include "im/qqbot/qq_api.hpp"
#include "im/telegram/tg_api.hpp"
#include "im/weixin/weixin_api.hpp"
#include "utils/logger.hpp"

#include <mutex>

namespace acecode::channels::core {
namespace {

std::string trimmed(const nlohmann::json& credentials, const char* key) {
    if (!credentials.is_object() || !credentials.contains(key) || !credentials[key].is_string()) return {};
    const auto value = credentials[key].get<std::string>();
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

const char* qq_bind_phase(im::qqbot::BindPhase phase) {
    switch (phase) {
        case im::qqbot::BindPhase::WaitingScan: return "waiting";
        case im::qqbot::BindPhase::Completed: return "completed";
        case im::qqbot::BindPhase::Failed: return "failed";
        case im::qqbot::BindPhase::Cancelled: return "cancelled";
        case im::qqbot::BindPhase::TimedOut: return "timed_out";
    }
    return "failed";
}

std::shared_ptr<im::Transport> make_qq(const PlatformConfig& config, const HostServices& services) {
    const auto& endpoints = services.endpoints;
    im::qqbot::QqTransportOptions options;
    options.api.app_id = trimmed(config.credentials, "app_id");
    options.api.app_secret = trimmed(config.credentials, "app_secret");
    options.api.api_base = endpoints.qq_api_base;
    options.api.token_url = endpoints.qq_token_url;
    options.api.use_proxy = endpoints.use_proxy;
    if (services.tune_qq) services.tune_qq(options);
    return std::make_shared<im::qqbot::QqTransport>(std::move(options));
}

std::shared_ptr<im::Transport> make_telegram(const PlatformConfig& config, ChannelStore& store,
                                             const HostServices& services) {
    im::telegram::TelegramTransportOptions options;
    options.api.token = trimmed(config.credentials, "token");
    options.api.api_base = services.endpoints.telegram_api_base;
    options.api.use_proxy = services.endpoints.use_proxy;
    options.initial_offset = store.cursor("telegram_offset");
    options.on_offset = [&store](std::int64_t offset) {
        try {
            store.set_cursor("telegram_offset", offset);
        } catch (const std::exception& e) {
            LOG_WARN(std::string("[channels/telegram] save offset failed: ") + e.what());
        }
    };
    if (services.tune_telegram) services.tune_telegram(options);
    return std::make_shared<im::telegram::TelegramTransport>(std::move(options));
}

// 微信的轮询游标与每个联系人的 context_token 都存在通道的 values 里;换机器人时随账号状态一起清掉。
constexpr const char* kWeixinCursor = "weixin_cursor";
constexpr const char* kWeixinContextTokens = "weixin_context_tokens";

std::shared_ptr<im::Transport> make_weixin(const PlatformConfig& config, ChannelStore& store,
                                           const HostServices& services) {
    const auto& endpoints = services.endpoints;
    im::weixin::WeixinTransportOptions options;
    options.api.token = trimmed(config.credentials, "bot_token");
    options.api.base_url = trimmed(config.credentials, "base_url");
    if (options.api.base_url.empty()) options.api.base_url = endpoints.weixin_api_base;
    options.api.cdn_base = endpoints.weixin_cdn_base;
    options.api.use_proxy = endpoints.use_proxy;
    options.bot_id = trimmed(config.credentials, "bot_id");

    const auto cursor = store.value(kWeixinCursor);
    if (cursor.is_string()) options.initial_cursor = cursor.get<std::string>();
    const auto tokens = store.value(kWeixinContextTokens);
    if (tokens.is_object()) {
        for (const auto& [peer, token] : tokens.items())
            if (token.is_string()) options.initial_context_tokens[peer] = token.get<std::string>();
    }
    options.on_cursor = [&store](const std::string& value) {
        try {
            store.set_value(kWeixinCursor, value.empty() ? nlohmann::json() : nlohmann::json(value));
        } catch (const std::exception& e) {
            LOG_WARN(std::string("[channels/weixin] save cursor failed: ") + e.what());
        }
    };
    // 读改写整个对象,多个发送 / 轮询线程同时回调时要串行。
    auto tokens_mu = std::make_shared<std::mutex>();
    options.on_context_token = [&store, tokens_mu](const std::string& peer, const std::string& token) {
        try {
            std::lock_guard<std::mutex> lock(*tokens_mu);
            auto all = store.value(kWeixinContextTokens);
            if (!all.is_object()) all = nlohmann::json::object();
            if (token.empty()) all.erase(peer);
            else all[peer] = token;
            store.set_value(kWeixinContextTokens, all.empty() ? nlohmann::json() : all);
        } catch (const std::exception& e) {
            LOG_WARN(std::string("[channels/weixin] save context token failed: ") + e.what());
        }
    };
    if (services.tune_weixin) services.tune_weixin(options);
    return std::make_shared<im::weixin::WeixinTransport>(std::move(options));
}

im::dingtalk::ApiOptions dingtalk_api_options(const nlohmann::json& credentials, const HostServices& services) {
    im::dingtalk::ApiOptions options;
    options.client_id = trimmed(credentials, "client_id");
    options.client_secret = trimmed(credentials, "client_secret");
    options.robot_code = trimmed(credentials, "robot_code");
    options.api_base = services.endpoints.dingtalk_api_base;
    options.oapi_base = services.endpoints.dingtalk_oapi_base;
    options.use_proxy = services.endpoints.use_proxy;
    return options;
}

std::shared_ptr<im::Transport> make_dingtalk(const PlatformConfig& config, const HostServices& services) {
    im::dingtalk::DingTalkTransportOptions options;
    options.api = dingtalk_api_options(config.credentials, services);
    if (services.tune_dingtalk) services.tune_dingtalk(options);
    return std::make_shared<im::dingtalk::DingTalkTransport>(std::move(options));
}

im::feishu::ApiOptions feishu_api_options(const nlohmann::json& credentials, const HostServices& services) {
    im::feishu::ApiOptions options;
    options.app_id = trimmed(credentials, "app_id");
    options.app_secret = trimmed(credentials, "app_secret");
    options.base = services.endpoints.feishu_base.empty() ? im::feishu::base_for_domain(trimmed(credentials, "domain"))
                                                          : services.endpoints.feishu_base;
    options.use_proxy = services.endpoints.use_proxy;
    return options;
}

std::shared_ptr<im::Transport> make_feishu(const PlatformConfig& config, const HostServices& services) {
    im::feishu::FeishuTransportOptions options;
    options.api = feishu_api_options(config.credentials, services);
    if (services.tune_feishu) services.tune_feishu(options);
    return std::make_shared<im::feishu::FeishuTransport>(std::move(options));
}

im::line::ApiOptions line_api_options(const nlohmann::json& credentials, const HostServices& services) {
    im::line::ApiOptions options;
    options.channel_id = trimmed(credentials, "channel_id");
    options.channel_secret = trimmed(credentials, "channel_secret");
    options.access_token = trimmed(credentials, "access_token");
    options.api_base = services.endpoints.line_api_base;
    options.data_api_base = services.endpoints.line_data_api_base;
    options.use_proxy = services.endpoints.use_proxy;
    return options;
}

// LINE 回调端口:第一次由系统分配,之后沿用(用户自己的反向代理指向它)。
constexpr const char* kLineListenPort = "line_listen_port";

std::shared_ptr<im::Transport> make_line(const PlatformConfig& config, ChannelStore& store,
                                         const HostServices& services) {
    im::line::LineTransportOptions options;
    options.api = line_api_options(config.credentials, services);
    options.public_url = trimmed(config.credentials, "public_url");
    const auto port = store.cursor(kLineListenPort);
    if (port > 0 && port <= 65535) options.listen_port = static_cast<std::uint16_t>(port);
    options.on_listen_port = [&store](std::uint16_t value) {
        try {
            store.set_cursor(kLineListenPort, value);
        } catch (const std::exception& e) {
            LOG_WARN(std::string("[channels/line] save listen port failed: ") + e.what());
        }
    };
    // 独立的 Crow 应用,只监听 127.0.0.1,绝不复用 daemon 的主 Web 端口。
    options.listener = std::make_shared<LineWebhookServer>();
    if (services.tune_line) services.tune_line(options);
    return std::make_shared<im::line::LineTransport>(std::move(options));
}

// Discord 的 24 小时登录计数要跨重启保留(超额会被平台重置 token)。
constexpr const char* kDiscordIdentifyWindow = "discord_identify_window_ms";
constexpr const char* kDiscordIdentifyCount = "discord_identify_count";

std::shared_ptr<im::Transport> make_discord(const PlatformConfig& config, ChannelStore& store,
                                            const HostServices& services) {
    im::discord::DiscordTransportOptions options;
    options.api.token = trimmed(config.credentials, "token");
    options.api.api_base = services.endpoints.discord_api_base;
    options.api.use_proxy = services.endpoints.use_proxy;
    options.identify_window_start_ms = store.cursor(kDiscordIdentifyWindow);
    options.identify_count = store.cursor(kDiscordIdentifyCount);
    options.on_identify_ledger = [&store](std::int64_t window_start_ms, std::int64_t count) {
        try {
            store.set_cursor(kDiscordIdentifyWindow, window_start_ms);
            store.set_cursor(kDiscordIdentifyCount, count);
        } catch (const std::exception& e) {
            LOG_WARN(std::string("[channels/discord] save identify ledger failed: ") + e.what());
        }
    };
    if (services.tune_discord) services.tune_discord(options);
    return std::make_shared<im::discord::DiscordTransport>(std::move(options));
}

nlohmann::json validate_qq(const nlohmann::json& credentials, const HostServices& services, std::string* error) {
    im::qqbot::ApiOptions options;
    options.app_id = trimmed(credentials, "app_id");
    options.app_secret = trimmed(credentials, "app_secret");
    if (options.app_id.empty() || options.app_secret.empty()) {
        if (error) *error = "请填写 AppID 和 AppSecret";
        return nullptr;
    }
    options.api_base = services.endpoints.qq_api_base;
    options.token_url = services.endpoints.qq_token_url;
    options.use_proxy = services.endpoints.use_proxy;
    im::qqbot::Api api(options);
    std::string reason;
    if (!api.verify(&reason)) {
        if (error) *error = reason.empty() ? std::string("凭据无效") : reason;
        return nullptr;
    }
    return {{"app_id", options.app_id}, {"app_secret", options.app_secret}};
}

nlohmann::json validate_telegram(const nlohmann::json& credentials, const HostServices& services,
                                 std::string* error) {
    const auto token = trimmed(credentials, "token");
    if (token.find(':') == std::string::npos) {
        if (error) *error = "Token 格式不对,应形如 123456:ABC-DEF…(在 @BotFather 中获取)";
        return nullptr;
    }
    im::telegram::ApiOptions options;
    options.token = token;
    options.api_base = services.endpoints.telegram_api_base;
    options.use_proxy = services.endpoints.use_proxy;
    const auto result = im::telegram::Api(options).call("getMe", nlohmann::json::object(), std::chrono::seconds(15));
    if (!result.ok) {
        if (error) {
            if (result.status == 401 || result.status == 404 || result.error_code == 401)
                *error = "Token 无效,请在 @BotFather 中确认后重试";
            else if (result.status == 0)
                *error = "无法连接 Telegram" + (result.description.empty() ? std::string{} : ":" + result.description);
            else
                *error = result.description.empty() ? std::string("Token 校验失败") : result.description;
        }
        return nullptr;
    }
    return {{"token", token}};
}

// 微信凭据只来自扫码登录;这里只检查齐全,不联网(登录失效由传输层报告)。
nlohmann::json validate_weixin(const nlohmann::json& credentials, std::string* error) {
    const auto token = trimmed(credentials, "bot_token");
    const auto bot_id = trimmed(credentials, "bot_id");
    if (token.empty() || bot_id.empty()) {
        if (error) *error = "请在设置页扫码登录微信";
        return nullptr;
    }
    nlohmann::json saved{{"bot_token", token}, {"bot_id", bot_id}};
    const auto base_url = trimmed(credentials, "base_url");
    if (!base_url.empty()) saved["base_url"] = base_url;
    return saved;
}

// 取一次 access token 并试注册一次 Stream 连接(票据随即丢弃),能区分凭据错误与应用未发布等。
nlohmann::json validate_dingtalk(const nlohmann::json& credentials, const HostServices& services,
                                 std::string* error) {
    auto options = dingtalk_api_options(credentials, services);
    if (options.client_id.empty() || options.client_secret.empty()) {
        if (error) *error = "请填写 Client ID 和 Client Secret";
        return nullptr;
    }
    const auto result = im::dingtalk::Api(options).verify();
    if (!result.ok) {
        if (error) *error = result.error.empty() ? std::string("凭据校验失败") : result.error;
        return nullptr;
    }
    nlohmann::json saved{{"client_id", options.client_id}, {"client_secret", options.client_secret}};
    if (!options.robot_code.empty()) saved["robot_code"] = options.robot_code;
    return saved;
}

// 换 tenant 令牌并读机器人信息。机器人未就绪(还没发布版本)不算失败:飞书要求长连接在线
// 才能保存订阅方式,发布往往在连上之后,连接步骤的清单会提示。
nlohmann::json validate_feishu(const nlohmann::json& credentials, const HostServices& services,
                               std::string* error) {
    const auto options = feishu_api_options(credentials, services);
    if (options.app_id.empty() || options.app_secret.empty()) {
        if (error) *error = "请填写 App ID 和 App Secret";
        return nullptr;
    }
    const auto result = im::feishu::verify_credentials(options);
    if (!result.ok) {
        if (error) *error = result.error.empty() ? std::string("凭据校验失败") : result.error;
        return nullptr;
    }
    nlohmann::json saved{{"app_id", options.app_id}, {"app_secret", options.app_secret}};
    const auto domain = trimmed(credentials, "domain");
    if (!domain.empty()) saved["domain"] = domain;
    return saved;
}

nlohmann::json validate_line(const nlohmann::json& credentials, const HostServices& services, std::string* error) {
    const auto options = line_api_options(credentials, services);
    if (options.channel_id.empty() || options.channel_secret.empty()) {
        if (error) *error = "请填写 Channel ID 和 Channel secret";
        return nullptr;
    }
    const auto public_url = trimmed(credentials, "public_url");
    if (!public_url.empty() && public_url.rfind("https://", 0) != 0) {
        if (error) *error = "公网地址必须以 https:// 开头(LINE 只回调 HTTPS)";
        return nullptr;
    }
    const auto result = im::line::validate_credentials(options);
    if (!result.ok) {
        if (error) *error = result.error.empty() ? std::string("凭据校验失败") : result.error;
        return nullptr;
    }
    nlohmann::json saved{{"channel_id", options.channel_id},
                         {"channel_secret", options.channel_secret},
                         {"bot_id", result.bot.user_id}};
    if (!options.access_token.empty()) saved["access_token"] = options.access_token;
    if (!public_url.empty()) saved["public_url"] = public_url;
    return saved;
}

nlohmann::json validate_discord(const nlohmann::json& credentials, const HostServices& services,
                                std::string* error) {
    im::discord::ApiOptions options;
    options.token = trimmed(credentials, "token");
    if (options.token.rfind("Bot ", 0) == 0) options.token = options.token.substr(4);
    if (options.token.empty()) {
        if (error) *error = "请填写 Bot Token";
        return nullptr;
    }
    options.api_base = services.endpoints.discord_api_base;
    options.use_proxy = services.endpoints.use_proxy;
    const auto result = im::discord::validate_credentials(options);
    if (!result.ok) {
        if (error) *error = result.error.empty() ? std::string("Token 校验失败") : result.error;
        return nullptr;
    }
    // 关着 Message Content Intent 时网关会以 4014 拒绝连接,保存前就说清楚。
    if (result.profile.message_content == im::discord::IntentState::Disabled) {
        if (error) *error = "请先在 Discord 开发者后台的 Bot 页打开 Message Content Intent,保存后再连接";
        return nullptr;
    }
    nlohmann::json saved{{"token", options.token}, {"bot_id", result.profile.user_id}};
    if (!result.profile.application_id.empty()) saved["application_id"] = result.profile.application_id;
    return saved;
}

const char* weixin_bind_phase(im::weixin::LoginPhase phase) {
    switch (phase) {
        case im::weixin::LoginPhase::WaitingScan:
        case im::weixin::LoginPhase::Scanned: return "waiting";
        case im::weixin::LoginPhase::Completed: return "completed";
        case im::weixin::LoginPhase::Failed: return "failed";
        case im::weixin::LoginPhase::Cancelled: return "cancelled";
        case im::weixin::LoginPhase::TimedOut: return "timed_out";
    }
    return "failed";
}

BindRunner weixin_bind_runner(const HostServices& services) {
    auto options = services.weixin_login;
    options.api_base = services.endpoints.weixin_api_base;
    options.use_proxy = services.endpoints.use_proxy;
    return [options](const std::atomic<bool>& cancelled, const std::function<void(const BindProgress&)>& progress) {
        const auto result = im::weixin::run_login(options, cancelled,
                                                  [&progress](const im::weixin::LoginProgress& update) {
            BindProgress step;
            step.qr_url = update.qr_url;
            step.refreshes = update.refreshes;
            progress(step);
        });
        BindOutcome outcome;
        outcome.phase = weixin_bind_phase(result.phase);
        outcome.error = result.error;
        if (result.phase == im::weixin::LoginPhase::Completed) {
            outcome.credentials = {{"bot_token", result.bot_token}, {"bot_id", result.bot_id}};
            if (!result.base_url.empty()) outcome.credentials["base_url"] = result.base_url;
            outcome.account = result.bot_id;
            if (!result.user_id.empty()) outcome.owner = "user:" + result.user_id;
        }
        return outcome;
    };
}

BindRunner qq_bind_runner(const HostServices& services) {
    return [options = services.bind](const std::atomic<bool>& cancelled,
                                     const std::function<void(const BindProgress&)>& progress) {
        const auto result = im::qqbot::run_bind(options, cancelled, [&progress](const im::qqbot::BindUpdate& update) {
            BindProgress step;
            step.qr_url = update.qr_url;
            step.refreshes = update.refreshes;
            progress(step);
        });
        BindOutcome outcome;
        outcome.phase = qq_bind_phase(result.phase);
        outcome.error = result.error;
        if (result.phase == im::qqbot::BindPhase::Completed) {
            outcome.credentials = {{"app_id", result.app_id}, {"app_secret", result.app_secret}};
            outcome.account = result.app_id;
            if (!result.user_openid.empty()) outcome.owner = "user:" + result.user_openid;
        }
        return outcome;
    };
}

} // namespace

std::shared_ptr<im::Transport> make_platform_transport(const std::string& platform, const PlatformConfig& config,
                                                       ChannelStore& store, const HostServices& services) {
    if (platform == "qq") return make_qq(config, services);
    if (platform == "weixin") return make_weixin(config, store, services);
    if (platform == "feishu") return make_feishu(config, services);
    if (platform == "dingtalk") return make_dingtalk(config, services);
    if (platform == "telegram") return make_telegram(config, store, services);
    if (platform == "discord") return make_discord(config, store, services);
    if (platform == "line") return make_line(config, store, services);
    return nullptr;
}

nlohmann::json validate_platform_credentials(const std::string& platform, const nlohmann::json& credentials,
                                             const HostServices& services, std::string* error) {
    if (platform == "qq") return validate_qq(credentials, services, error);
    if (platform == "weixin") return validate_weixin(credentials, error);
    if (platform == "feishu") return validate_feishu(credentials, services, error);
    if (platform == "dingtalk") return validate_dingtalk(credentials, services, error);
    if (platform == "telegram") return validate_telegram(credentials, services, error);
    if (platform == "discord") return validate_discord(credentials, services, error);
    if (platform == "line") return validate_line(credentials, services, error);
    if (error) *error = "未知的消息通道平台";
    return nullptr;
}

BindRunner make_bind_runner(const std::string& platform, const HostServices& services) {
    if (platform == "qq") return qq_bind_runner(services);
    if (platform == "weixin") return weixin_bind_runner(services);
    return {};
}

} // namespace acecode::channels::core
