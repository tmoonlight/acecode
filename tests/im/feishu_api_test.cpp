#include <gtest/gtest.h>

#include "im/feishu/feishu_api.hpp"
#include "test_support/im/fake_feishu_server.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>

// im/feishu/feishu_api:飞书 OpenAPI 客户端对本机假开放平台的端到端测试 ——
// 令牌缓存与失效重取、凭据校验(成功 / 凭据错误 / 网络不通 / 机器人未就绪)、
// 长连接地址请求、消息资源下载。

namespace acecode::im::feishu {
namespace {

using test::FakeFeishuServer;

ApiOptions local_options(const FakeFeishuServer& server) {
    ApiOptions options;
    options.app_id = "cli_app";
    options.app_secret = "secret-value-123";
    options.base = server.base();
    options.use_proxy = false;
    return options;
}

std::filesystem::path temp_file(const char* name) { return std::filesystem::path(testing::TempDir()) / name; }

// 场景:连续两次调用需要令牌的接口。
// 期望:只换一次令牌并复用缓存;请求带 "Bearer <令牌>" 与含独立 channel 标记的 User-Agent。
TEST(FeishuApi, CachesTenantTokenAcrossCalls) {
    FakeFeishuServer server;
    Api api(local_options(server));
    std::string error;
    EXPECT_TRUE(api.bot_info(&error).ok) << error;
    EXPECT_TRUE(api.bot_info(&error).ok) << error;
    EXPECT_EQ(server.token_calls(), 1);
    const auto info = server.requests_to("/open-apis/bot/v3/info");
    ASSERT_EQ(info.size(), 2u);
    EXPECT_EQ(info[1].authorization, "Bearer t-1");
    EXPECT_NE(info[1].user_agent.find(" channel"), std::string::npos) << info[1].user_agent;
    const auto token = server.requests_to("/open-apis/auth/v3/tenant_access_token/internal");
    ASSERT_EQ(token.size(), 1u);
    EXPECT_EQ(token[0].body.value("app_id", ""), "cli_app");
    EXPECT_EQ(token[0].body.value("app_secret", ""), "secret-value-123");
}

// 场景:发消息时平台返回 99991663(令牌失效,例如被另一个客户端换了新令牌后旧的到期)。
// 期望:丢弃缓存、换新令牌并用新令牌重试一次,最终成功;不把错误交给调用方。
TEST(FeishuApi, RefreshesTokenOnInvalidTokenCode) {
    FakeFeishuServer server;
    std::atomic<int> calls{0};
    server.message_handler = [&calls](const FakeFeishuServer::Request&) {
        if (calls++ == 0)
            return std::make_pair(400, nlohmann::json{{"code", 99991663}, {"msg", "Invalid access token"}});
        return std::make_pair(200, nlohmann::json{{"code", 0}, {"data", {{"message_id", "om_ok"}}}});
    };
    Api api(local_options(server));
    const auto result = api.call("POST", "/open-apis/im/v1/messages?receive_id_type=chat_id",
                                 {{"receive_id", "oc_1"}, {"msg_type", "text"}, {"content", R"({"text":"hi"})"}});
    EXPECT_TRUE(result.ok) << describe_error(result);
    EXPECT_EQ(result.data.value("message_id", ""), "om_ok");
    EXPECT_EQ(server.token_calls(), 2);
    const auto sends = server.sends();
    ASSERT_EQ(sends.size(), 2u);
    EXPECT_EQ(sends[1].authorization, "Bearer t-2");
    EXPECT_EQ(sends[1].query, "chat_id");
}

// 场景:发消息被限流,响应头带 x-ogw-ratelimit-reset 与 X-Tt-Logid。
// 期望:ApiResult 判为限流,带出平台要求的等待秒数与请求 id(只用于日志排障)。
TEST(FeishuApi, ReadsRateLimitHeaders) {
    FakeFeishuServer server;
    server.message_handler = [](const FakeFeishuServer::Request&) {
        return std::make_pair(429, nlohmann::json{{"code", 99991400}, {"msg", "request trigger frequency limit"}});
    };
    server.message_headers = {{"x-ogw-ratelimit-reset", "2"}, {"X-Tt-Logid", "log-123"}};
    Api api(local_options(server));
    const auto result = api.call("POST", "/open-apis/im/v1/messages?receive_id_type=chat_id", {{"receive_id", "oc"}});
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(is_rate_limited(result));
    EXPECT_EQ(result.retry_after, std::chrono::seconds(2));
    EXPECT_EQ(result.log_id, "log-123");
}

// 场景:保存凭据前联网校验,凭据正确且机器人已发布启用。
// 期望:ok,返回机器人 open_id 与名称,机器人就绪、没有提示。
TEST(FeishuApi, VerifyReturnsBotIdentity) {
    FakeFeishuServer server;
    const auto result = verify_credentials(local_options(server));
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.auth_failed);
    EXPECT_EQ(result.bot_open_id, "ou_bot");
    EXPECT_EQ(result.bot_name, "TestBot");
    EXPECT_TRUE(result.bot_ready);
    EXPECT_TRUE(result.bot_warning.empty());
}

// 场景:App Secret 填错,换令牌接口返回 HTTP 200 + code 10015。
// 期望:判为凭据被拒(不是网络问题),给出“App Secret 不正确”,错误文本不含密钥。
TEST(FeishuApi, VerifyReportsWrongSecret) {
    FakeFeishuServer server;
    server.token_handler = [](const FakeFeishuServer::Request&) {
        return std::make_pair(200, nlohmann::json{{"code", 10015}, {"msg", "wrong app secret"}});
    };
    const auto result = verify_credentials(local_options(server));
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.auth_failed);
    EXPECT_FALSE(result.network_error);
    EXPECT_NE(result.error.find("App Secret"), std::string::npos) << result.error;
    EXPECT_EQ(result.error.find("secret-value-123"), std::string::npos);
}

// 场景:网络不通(端口 1 上没有服务);以及没填 App Secret。
// 期望:前者判为网络错误而非凭据错误;后者直接提示填写,不发请求。
TEST(FeishuApi, VerifyDistinguishesNetworkFailure) {
    ApiOptions options;
    options.app_id = "cli_app";
    options.app_secret = "secret-value-123";
    options.base = "http://127.0.0.1:1";
    options.use_proxy = false;
    const auto result = verify_credentials(options);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.network_error);
    EXPECT_FALSE(result.auth_failed);
    EXPECT_NE(result.error.find("无法连接"), std::string::npos) << result.error;

    options.app_secret.clear();
    const auto empty = verify_credentials(options);
    EXPECT_TRUE(empty.auth_failed);
    EXPECT_FALSE(empty.error.empty());
}

// 场景:凭据正确,但机器人还没就绪 —— 一次 activate_status=0(待安装),一次 bot/v3/info 直接报错
// (未添加机器人能力 / 未发布版本)。
// 期望:凭据仍判为有效(长连接不依赖它,飞书后台要先有在线长连接才能保存订阅方式),
// 但 bot_ready=false 并给出中文提示。
TEST(FeishuApi, VerifyWarnsWhenBotIsNotReady) {
    FakeFeishuServer server;
    std::atomic<int> calls{0};
    server.bot_handler = [&calls](const FakeFeishuServer::Request&) {
        if (calls++ == 0)
            return std::make_pair(200, nlohmann::json{{"code", 0},
                                                      {"bot", {{"activate_status", 0}, {"open_id", "ou_bot"}}}});
        return std::make_pair(400, nlohmann::json{{"code", 230006}, {"msg", "bot ability is not activated"}});
    };
    const auto inactive = verify_credentials(local_options(server));
    EXPECT_TRUE(inactive.ok);
    EXPECT_FALSE(inactive.bot_ready);
    EXPECT_EQ(inactive.activate_status, 0);
    EXPECT_FALSE(inactive.bot_warning.empty());
    const auto missing = verify_credentials(local_options(server));
    EXPECT_TRUE(missing.ok);
    EXPECT_FALSE(missing.bot_ready);
    EXPECT_NE(missing.bot_warning.find("机器人能力"), std::string::npos) << missing.bot_warning;
}

// 场景:请求长连接地址;另一次平台以 1000040345 拒绝凭据。
// 期望:请求体用 PascalCase 的 AppID / AppSecret,带 locale: zh 与 channel UA;解析出地址、
// service_id、device_id。被拒时判为致命,原因不含密钥。
TEST(FeishuApi, RequestsWebSocketEndpoint) {
    FakeFeishuServer server;
    Api api(local_options(server));
    const auto info = api.ws_endpoint();
    ASSERT_TRUE(info.ok) << info.message;
    EXPECT_EQ(info.service_id, 1234);
    EXPECT_EQ(info.device_id, "dev-7");
    const auto requests = server.requests_to("/callback/ws/endpoint");
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].body.value("AppID", ""), "cli_app");
    EXPECT_EQ(requests[0].body.value("AppSecret", ""), "secret-value-123");
    EXPECT_EQ(requests[0].locale, "zh");
    EXPECT_NE(requests[0].user_agent.find(" channel"), std::string::npos);
    EXPECT_EQ(server.token_calls(), 0);  // 长连接不用 tenant_access_token

    server.endpoint_handler = [](const FakeFeishuServer::Request&) {
        return std::make_pair(200, nlohmann::json{{"code", 1000040345}, {"msg", "app_id or app_secret is invalid"},
                                                  {"data", {{"URL", ""}}}});
    };
    const auto rejected = api.ws_endpoint();
    EXPECT_TRUE(rejected.fatal);
    EXPECT_EQ(rejected.message.find("secret-value-123"), std::string::npos);
}

// 场景:下载消息里的图片;下载超过本地上限的文件;下载合并转发里的附件(平台拒绝);
// 下载时令牌恰好失效。
// 期望:正常下载写出完整文件并带 type=image;超限失败、删除半截文件并提示“超过大小限制”;
// 平台拒绝时给出中文原因;令牌失效时换新令牌重试一次后成功。
TEST(FeishuApi, DownloadsMessageResources) {
    FakeFeishuServer server;
    Api api(local_options(server));
    const auto dest = temp_file("acecode-feishu-download.bin");
    std::string error;
    ASSERT_TRUE(api.download_resource({"om_1", "img_k", "image"}, dest, 1024, &error)) << error;
    EXPECT_EQ(std::filesystem::file_size(dest), 16u);
    const auto first = server.requests_to("/open-apis/im/v1/messages/om_1/resources/img_k");
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].query, "image");

    EXPECT_FALSE(api.download_resource({"om_1", "big", "file"}, dest, 1024, &error));
    EXPECT_NE(error.find("超过"), std::string::npos) << error;
    EXPECT_FALSE(std::filesystem::exists(dest));

    EXPECT_FALSE(api.download_resource({"om_1", "gone", "file"}, dest, 1024, &error));
    EXPECT_NE(error.find("合并转发"), std::string::npos) << error;
    EXPECT_FALSE(std::filesystem::exists(dest));

    const auto before = server.token_calls();
    ASSERT_TRUE(api.download_resource({"om_1", "expired", "file"}, dest, 1024, &error)) << error;
    EXPECT_EQ(server.token_calls(), before + 1);
    std::filesystem::remove(dest);
}

} // namespace
} // namespace acecode::im::feishu
