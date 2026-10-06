#include <gtest/gtest.h>

#include "im/discord/discord_api.hpp"
#include "test_support/im/fake_discord_server.hpp"

#include <atomic>
#include <future>

// im/discord/discord_api:Discord REST 客户端与凭据校验(本机假 Discord,不连真实平台)。
// 覆盖:鉴权头与 User-Agent、token 校验三种结果(成功 / token 无效 / 网络不通)、intent 开关、
// 发消息的 nonce 幂等重发、429 等待重发、等待过长放弃、停机打断等待。

namespace acecode::im::discord {
namespace {

ApiOptions local_options(const test::FakeDiscordServer& server) {
    ApiOptions options;
    options.token = server.token;
    options.api_base = server.api_base();
    options.use_proxy = false;
    return options;
}

// 场景:用户在设置页填入正确的 bot token,保存前联网校验。
// 期望:成功;给出机器人 id、用户名、应用 id、Message Content Intent 已开启,以及邀请链接;
// 请求带 "Authorization: Bot <token>" 与 "DiscordBot (...)" 格式的 User-Agent。
TEST(DiscordApi, ValidatesTokenAndReadsApplication) {
    test::FakeDiscordServer server;
    const auto result = validate_credentials(local_options(server));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.profile.user_id, server.bot_id);
    EXPECT_EQ(result.profile.username, "AceBot");
    EXPECT_EQ(result.profile.application_id, server.application_id);
    EXPECT_EQ(result.profile.application_name, "Ace App");
    EXPECT_EQ(result.profile.message_content, IntentState::Enabled);
    EXPECT_NE(result.profile.invite_url.find("client_id=" + server.application_id), std::string::npos);
    const auto me = server.requests_to("GET", "/users/@me");
    ASSERT_EQ(me.size(), 1u);
    EXPECT_EQ(me[0].authorization, "Bot " + server.token);
    EXPECT_EQ(me[0].user_agent.rfind("DiscordBot (", 0), 0u) << me[0].user_agent;
}

// 场景:用户从别处复制时带上了 "Bot " 前缀和首尾空白;应用没有开启 Message Content Intent。
// 期望:前缀与空白被去掉后校验成功;intent 状态为“未开启”(设置向导据此提示去后台打开开关),
// 但不影响校验通过。
TEST(DiscordApi, AcceptsBotPrefixAndReportsDisabledIntent) {
    test::FakeDiscordServer server;
    server.application_flags = 0;
    auto options = local_options(server);
    options.token = "  Bot " + server.token + "  ";
    const auto result = validate_credentials(options);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.profile.message_content, IntentState::Disabled);
}

// 场景:token 错误(已重置或抄错),平台返回 401。
// 期望:失败且标记为凭据无效(不要重试);原因是一句明确的中文;错误文本里没有 token。
TEST(DiscordApi, RejectsInvalidTokenWithoutLeakingIt) {
    test::FakeDiscordServer server;
    auto options = local_options(server);
    options.token = "OTk5OTk5.wrong.token-value-that-should-never-be-logged";
    const auto result = validate_credentials(options);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.auth_failed);
    EXPECT_NE(result.error.find(u8"无效"), std::string::npos) << result.error;
    EXPECT_EQ(result.error.find("token-value-that-should-never-be-logged"), std::string::npos);
}

// 场景:网络不通(连接被拒)。
// 期望:失败但不标记为凭据无效(换网络后可以重试);原因说明无法连接 Discord。
TEST(DiscordApi, DistinguishesNetworkFailureFromInvalidToken) {
    ApiOptions options;
    options.token = "OTk5OTk5.some.token";
    options.api_base = "http://127.0.0.1:1/api/v10";
    options.use_proxy = false;
    const auto result = validate_credentials(options);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.auth_failed);
    EXPECT_NE(result.error.find(u8"无法连接"), std::string::npos) << result.error;
}

// 场景:发消息时平台先返回 502(请求可能其实已送达),第二次成功。
// 期望:用同一个 nonce(且 enforce_nonce=true)重发,平台据此去重,不会出现两条相同消息。
TEST(DiscordApi, CreateMessageRetriesTransientErrorsWithSameNonce) {
    test::FakeDiscordServer server;
    std::atomic<int> calls{0};
    server.message_handler = [&calls](const test::FakeDiscordServer::Request&) {
        if (calls++ == 0) return std::make_pair(502, nlohmann::json{{"message", "bad gateway"}});
        return std::make_pair(0, nlohmann::json());
    };
    RateLimitPolicy policy;
    policy.send_retry_delay = std::chrono::milliseconds(10);
    Api api(local_options(server), policy);
    const auto result = api.create_message("500000000000000001", {{"content", "hi"}});
    ASSERT_TRUE(result.ok) << result.error.message;
    const auto sent = server.requests_to("POST", "/channels/500000000000000001/messages");
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_FALSE(sent[0].body.value("nonce", "").empty());
    EXPECT_EQ(sent[0].body.value("nonce", ""), sent[1].body.value("nonce", "x"));
    EXPECT_TRUE(sent[1].body.value("enforce_nonce", false));
}

// 场景:发消息遇到 429,响应体 retry_after=0.2(秒,小数)。
// 期望:等待约 0.2 秒后原样重发并成功;总共两次请求。
TEST(DiscordApi, WaitsRetryAfterOn429) {
    test::FakeDiscordServer server;
    std::atomic<int> calls{0};
    server.message_handler = [&calls](const test::FakeDiscordServer::Request&) {
        if (calls++ == 0)
            return std::make_pair(429, nlohmann::json{{"message", "You are being rate limited."},
                                                      {"retry_after", 0.2}, {"global", false}, {"code", 20028}});
        return std::make_pair(0, nlohmann::json());
    };
    Api api(local_options(server));
    const auto begin = std::chrono::steady_clock::now();
    const auto result = api.create_message("500000000000000001", {{"content", "hi"}});
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(calls.load(), 2);
    EXPECT_GE(elapsed, std::chrono::milliseconds(150));
}

// 场景:429 要求等待 120 秒,超过允许的单次等待上限(测试里设为 1 秒)。
// 期望:不等待、不重发,直接以 429 失败返回,调用方给用户“限流,稍后再试”。
TEST(DiscordApi, GivesUpWhenRetryAfterExceedsLimit) {
    test::FakeDiscordServer server;
    server.message_handler = [](const test::FakeDiscordServer::Request&) {
        return std::make_pair(429, nlohmann::json{{"message", "You are being rate limited."},
                                                  {"retry_after", 120.0}, {"global", false}});
    };
    RateLimitPolicy policy;
    policy.max_wait = std::chrono::seconds(1);
    Api api(local_options(server), policy);
    const auto result = api.create_message("500000000000000001", {{"content", "hi"}});
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.status, 429);
    EXPECT_EQ(server.requests_to("POST", "/channels/").size(), 1u);
}

// 场景:发消息正在等一个 30 秒的 429,此时通道被关闭(stop 调用 cancel)。
// 期望:等待立即被打断,调用在 3 秒内以 cancelled 返回,不会把停机卡住。
TEST(DiscordApi, CancelInterruptsRateLimitWait) {
    test::FakeDiscordServer server;
    server.message_handler = [](const test::FakeDiscordServer::Request&) {
        return std::make_pair(429, nlohmann::json{{"message", "You are being rate limited."},
                                                  {"retry_after", 30.0}, {"global", true}});
    };
    Api api(local_options(server));
    auto pending = std::async(std::launch::async, [&api] {
        return api.create_message("500000000000000001", {{"content", "hi"}});
    });
    ASSERT_TRUE(test::FakeDiscordServer::wait_until(
        [&server] { return server.requests_to("POST", "/channels/").size() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    api.cancel();
    ASSERT_EQ(pending.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    const auto result = pending.get();
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.cancelled);
}

} // namespace
} // namespace acecode::im::discord
