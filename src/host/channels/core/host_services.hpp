#pragma once

// 消息通道宿主的外部依赖:各平台的接入地址(测试指向本机假服务)、扫码流程参数、
// 以及测试用的传输层参数调整钩子。真实运行时 daemon 只填会话依赖与广播。

#include "channels/core/platform_runtime.hpp"
#include "im/dingtalk/dingtalk_transport.hpp"
#include "im/discord/discord_transport.hpp"
#include "im/feishu/feishu_transport.hpp"
#include "im/line/line_transport.hpp"
#include "im/qqbot/qq_bind.hpp"
#include "im/qqbot/qq_transport.hpp"
#include "im/telegram/tg_transport.hpp"
#include "im/weixin/weixin_login.hpp"
#include "im/weixin/weixin_transport.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace acecode::channels::core {

// 真实平台的接入地址;测试指向本机假服务。
struct HostEndpoints {
    std::string qq_api_base = im::qqbot::kApiBase;
    std::string qq_token_url = im::qqbot::kTokenUrl;
    std::string telegram_api_base = im::telegram::kApiBase;
    // 微信:扫码登录用 weixin_api_base;登录后使用登录结果里的 base_url(为空时退回它)。
    std::string weixin_api_base = im::weixin::kApiBase;
    std::string weixin_cdn_base = im::weixin::kCdnBase;
    std::string dingtalk_api_base = im::dingtalk::kApiBase;
    std::string dingtalk_oapi_base = im::dingtalk::kOapiBase;
    std::string discord_api_base = im::discord::kApiBase;
    std::string feishu_base;  // 非空时覆盖按 domain(飞书 / Lark)选出的地址
    std::string line_api_base = im::line::kApiBase;
    std::string line_data_api_base = im::line::kDataApiBase;
    bool use_proxy = true;
};

struct HostServices {
    // 会话依赖;其中 release_session / on_bindings_changed / media_dir 由宿主填写。
    ConversationDeps conversation;
    Broadcast broadcast;
    std::function<std::int64_t()> current_pid;
    std::chrono::milliseconds standby_retry{std::chrono::seconds(5)};
    HostEndpoints endpoints;
    im::qqbot::BindOptions bind;
    // 微信扫码登录参数;api_base / use_proxy 以 endpoints 为准。
    im::weixin::LoginOptions weixin_login;
    // 为空时按 endpoints 创建真实传输层 / 联网校验凭据。
    TransportFactory make_transport;
    CredentialValidator validate_credentials;
    // 测试用:调整默认传输层的重试间隔等参数。
    std::function<void(im::qqbot::QqTransportOptions&)> tune_qq;
    std::function<void(im::telegram::TelegramTransportOptions&)> tune_telegram;
    std::function<void(im::weixin::WeixinTransportOptions&)> tune_weixin;
    std::function<void(im::dingtalk::DingTalkTransportOptions&)> tune_dingtalk;
    std::function<void(im::discord::DiscordTransportOptions&)> tune_discord;
    std::function<void(im::feishu::FeishuTransportOptions&)> tune_feishu;
    // LINE 测试还要在这里换掉 cloudflared(tunnel.locate / launch)等。
    std::function<void(im::line::LineTransportOptions&)> tune_line;
};

} // namespace acecode::channels::core
