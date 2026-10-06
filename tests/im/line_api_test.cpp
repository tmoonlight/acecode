#include <gtest/gtest.h>

#include "im/line/line_api.hpp"
#include "im/line/line_tunnel.hpp"
#include "test_support/im/fake_line_server.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>

// im/line/line_api:LINE API 客户端(本机假 LINE 服务)。
// 覆盖:无状态令牌换取与缓存、401 后重换一次、凭据无效、长期令牌、联网校验三种结果、
// webhook 地址未设置、附件下载(正常 / 超限 / 撤回 / 转码中)、重试键格式。

namespace acecode::im::line {
namespace {

using test::FakeLineServer;

ApiOptions local_options(const FakeLineServer& server) {
    ApiOptions options;
    options.channel_id = FakeLineServer::kChannelId;
    options.channel_secret = FakeLineServer::kSecret;
    options.api_base = server.base();
    options.data_api_base = server.base();
    options.use_proxy = false;
    options.timeout = std::chrono::seconds(5);
    options.transcoding_poll = std::chrono::milliseconds(20);
    return options;
}

std::filesystem::path temp_file(const std::string& name) {
    return std::filesystem::temp_directory_path() / ("acecode-line-api-" + name);
}

// 场景:连续两次调用接口。
// 期望:第一次用 Channel ID + secret 换一个无状态令牌(表单字段正确),第二次直接复用缓存,
// 不会每次都换令牌。
TEST(LineApi, MintsStatelessTokenOnceAndCachesIt) {
    FakeLineServer server;
    Api api(local_options(server));
    BotInfo bot;
    ASSERT_TRUE(api.bot_info(&bot).ok);
    EXPECT_EQ(bot.user_id, FakeLineServer::kBotId);
    EXPECT_EQ(bot.basic_id, "@216ruabc");
    EXPECT_EQ(bot.display_name, u8"ACE 测试机器人");
    ASSERT_TRUE(api.bot_info(&bot).ok);
    EXPECT_EQ(server.mint_count(), 1);
    const auto mints = server.calls_to("POST", "/oauth2/v3/token");
    ASSERT_EQ(mints.size(), 1u);
    EXPECT_EQ(mints[0].body.value("grant_type", ""), "client_credentials");
    EXPECT_EQ(mints[0].body.value("client_id", ""), FakeLineServer::kChannelId);
    EXPECT_EQ(server.calls_to("GET", "/v2/bot/info")[0].authorization, "Bearer stateless-1");
}

// 场景:平台提前作废了缓存的无状态令牌,下一次调用收到 401。
// 期望:自动重换一次令牌并重试成功,调用方看不到 401。
TEST(LineApi, RemintsOnceAfter401) {
    FakeLineServer server;
    Api api(local_options(server));
    BotInfo bot;
    ASSERT_TRUE(api.bot_info(&bot).ok);
    server.revoke_tokens();
    const auto result = api.bot_info(&bot);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(server.mint_count(), 2);
}

// 场景:Channel secret 填错。
// 期望:换令牌被 400 拒绝,结果标 auth_failed,给出中文原因且不含 secret。
TEST(LineApi, InvalidChannelCredentialsAreAuthFailures) {
    FakeLineServer server;
    auto options = local_options(server);
    options.channel_secret = "ffffffffffffffffffffffffffffffff";
    Api api(options);
    BotInfo bot;
    const auto result = api.bot_info(&bot);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.auth_failed);
    EXPECT_NE(result.message.find("Channel ID 或 Channel secret 无效"), std::string::npos);
    EXPECT_EQ(result.message.find(options.channel_secret), std::string::npos);
}

// 场景:用户粘贴了长期令牌;之后该令牌被重新签发(旧的失效)。
// 期望:直接用长期令牌,不换取无状态令牌;失效后 401 即判凭据失效,不重试换令牌。
TEST(LineApi, LongLivedTokenIsUsedDirectly) {
    FakeLineServer server;
    server.long_lived_token = "long-lived-token-abcdef";
    auto options = local_options(server);
    options.access_token = server.long_lived_token;
    Api api(options);
    BotInfo bot;
    ASSERT_TRUE(api.bot_info(&bot).ok);
    EXPECT_EQ(server.mint_count(), 0);
    EXPECT_EQ(server.calls_to("GET", "/v2/bot/info")[0].authorization, "Bearer long-lived-token-abcdef");

    Api stale([&options] {
        auto copy = options;
        copy.access_token = "revoked-token-123456";
        return copy;
    }());
    const auto result = stale.bot_info(&bot);
    EXPECT_TRUE(result.auth_failed);
    EXPECT_NE(result.message.find("Channel access token"), std::string::npos);
    EXPECT_EQ(server.mint_count(), 0);
}

// 场景:保存凭据前的联网校验:正确凭据、错误 secret、缺 secret、服务不可达。
// 期望:正确时给出机器人 userId / basicId / 名称;错误时是明确的中文原因;
// 网络不通时标 network_error(界面据此区分“凭据错”和“连不上”)。
TEST(LineApi, ValidateCredentialsDistinguishesFailures) {
    FakeLineServer server;
    auto ok = validate_credentials(local_options(server));
    ASSERT_TRUE(ok.ok) << ok.error;
    EXPECT_EQ(ok.bot.user_id, FakeLineServer::kBotId);
    EXPECT_EQ(ok.bot.basic_id, "@216ruabc");

    auto wrong = local_options(server);
    wrong.channel_secret = "ffffffffffffffffffffffffffffffff";
    auto bad = validate_credentials(wrong);
    EXPECT_FALSE(bad.ok);
    EXPECT_FALSE(bad.network_error);
    EXPECT_NE(bad.error.find("Channel secret"), std::string::npos);

    auto missing = local_options(server);
    missing.channel_secret.clear();
    EXPECT_NE(validate_credentials(missing).error.find("Channel secret"), std::string::npos);

    auto offline = local_options(server);
    offline.api_base = "http://127.0.0.1:" + std::to_string(pick_free_loopback_port());
    auto down = validate_credentials(offline);
    EXPECT_FALSE(down.ok);
    EXPECT_TRUE(down.network_error);
}

// 场景:长期令牌 + Channel ID + secret 同时填写,但 secret 不对。
// 期望:仍用 Channel ID + secret 换一次令牌来验证 secret(回调签名要用它),校验失败。
TEST(LineApi, ValidateChecksSecretEvenWithLongLivedToken) {
    FakeLineServer server;
    server.long_lived_token = "long-lived-token-abcdef";
    auto options = local_options(server);
    options.access_token = server.long_lived_token;
    EXPECT_TRUE(validate_credentials(options).ok);
    options.channel_secret = "ffffffffffffffffffffffffffffffff";
    const auto result = validate_credentials(options);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.network_error);
}

// 场景:频道从未设置过 webhook 地址;随后设置一个地址。
// 期望:读取时 404 → exists=false;设置走 PUT,之后能读到地址与 “Use webhook” 状态。
// 注:im::http_send 需要支持 PUT,否则 PUT 会被当成 GET 发出(本用例即为守卫)。
TEST(LineApi, WebhookEndpointReadAndWrite) {
    FakeLineServer server;
    server.webhook_active = false;
    Api api(local_options(server));
    WebhookInfo info;
    const auto first = api.webhook_info(&info);
    EXPECT_EQ(first.status, 404);
    EXPECT_FALSE(info.exists);
    ASSERT_TRUE(api.set_webhook("https://abc.trycloudflare.com/line/webhook").ok);
    ASSERT_EQ(server.calls_to("PUT", "/v2/bot/channel/webhook/endpoint").size(), 1u);
    ASSERT_TRUE(api.webhook_info(&info).ok);
    EXPECT_TRUE(info.exists);
    EXPECT_EQ(info.endpoint, "https://abc.trycloudflare.com/line/webhook");
    EXPECT_FALSE(info.active);
}

// 场景:下载图片内容;下载超过上限的内容;下载已被撤回的内容;下载仍在转码的视频。
// 期望:正常内容写入文件;超限失败并删除半截文件;410 给出“已撤回”;202 时轮询转码状态后再下载成功。
TEST(LineApi, DownloadsContentWithLimitsAndTranscoding) {
    FakeLineServer server;
    server.set_content("img1", std::string(64, 'p'));
    server.set_content("big", std::string(4096, 'b'));
    server.set_content("transcode-1", std::string(16, 'v'));
    Api api(local_options(server));
    const auto dest = temp_file("content.bin");
    std::string error;
    ASSERT_TRUE(api.download_content("img1", dest, 1024, &error)) << error;
    {
        std::ifstream in(dest, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_EQ(bytes, std::string(64, 'p'));
    }
    EXPECT_FALSE(api.download_content("big", dest, 1024, &error));
    EXPECT_EQ(error, "附件超过大小限制");
    EXPECT_FALSE(std::filesystem::exists(dest));

    EXPECT_FALSE(api.download_content("gone", dest, 1024, &error));
    EXPECT_EQ(error, "对方已撤回该消息");

    ASSERT_TRUE(api.download_content("transcode-1", dest, 1024, &error)) << error;
    EXPECT_EQ(server.calls_to("GET", "/v2/bot/message/transcode-1/content/transcoding").size(), 1u);
    EXPECT_EQ(server.calls_to("GET", "/v2/bot/message/transcode-1/content").size(), 2u);
    std::error_code ec;
    std::filesystem::remove(dest, ec);
}

// 场景:生成推送重试键。
// 期望:是小写 UUID v4 形式,每次不同。
TEST(LineApi, RetryKeysAreUuidV4) {
    const auto a = new_retry_key();
    const auto b = new_retry_key();
    EXPECT_TRUE(std::regex_match(a, std::regex("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$")))
        << a;
    EXPECT_NE(a, b);
}

} // namespace
} // namespace acecode::im::line
