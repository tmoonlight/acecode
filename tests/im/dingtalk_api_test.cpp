#include <gtest/gtest.h>

#include "im/dingtalk/dingtalk_api.hpp"
#include "test_support/im/fake_dingtalk_server.hpp"

#include <fstream>

// im/dingtalk/dingtalk_api:钉钉 HTTP 客户端对本机假开放平台的端到端测试 —— 令牌缓存与失效重试、
// 凭据校验的三种失败、两套错误形态、webhook 白名单、媒体上传与 OSS 下载。

namespace acecode::im::dingtalk {
namespace {

ApiOptions local_options(test::FakeDingTalkServer& server) {
    ApiOptions options;
    options.client_id = server.client_id;
    options.client_secret = server.client_secret;
    options.api_base = server.base();
    options.oapi_base = server.base();
    options.use_proxy = false;
    return options;
}

// 场景:连续两次调用群发接口,之后令牌在平台侧被作废,再调用一次。
// 期望:前两次复用同一个令牌(只换一次);作废后第一次请求返回 InvalidAuthentication,
// 客户端清掉缓存换新令牌并重试一次成功;令牌放在 x-acs-dingtalk-access-token 头里。
TEST(DingTalkApi, CachesTokenAndRefreshesOnceWhenInvalid) {
    test::FakeDingTalkServer server;
    Api api(local_options(server));
    EXPECT_TRUE(api.send_group("dingR", "cid1", "sampleText", {{"content", "a"}}).ok);
    EXPECT_TRUE(api.send_group("dingR", "cid1", "sampleText", {{"content", "b"}}).ok);
    EXPECT_EQ(server.token_calls(), 1);
    server.revoke_token("tok-1");
    const auto result = api.send_group("dingR", "cid1", "sampleText", {{"content", "c"}});
    EXPECT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(server.token_calls(), 2);
    const auto sent = server.requests_to("/v1.0/robot/groupMessages/send");
    ASSERT_EQ(sent.size(), 4u);
    EXPECT_EQ(sent[2].token, "tok-1");
    EXPECT_EQ(sent[3].token, "tok-2");
    // msgParam 是 JSON 字符串而不是对象(平台要求二次编码)。
    ASSERT_TRUE(sent[3].body["msgParam"].is_string());
    EXPECT_EQ(nlohmann::json::parse(sent[3].body["msgParam"].get<std::string>())["content"], "c");
    EXPECT_EQ(sent[3].body["openConversationId"], "cid1");
}

// 场景:用户填错了 Client Secret 后点“保存”。
// 期望:校验失败且标记为凭据无效(不是网络问题);中文原因指向“凭证与基础信息”;不含密钥原文。
TEST(DingTalkApi, VerifyReportsInvalidCredentials) {
    test::FakeDingTalkServer server;
    auto options = local_options(server);
    options.client_secret = "wrong-secret-value-999";
    const auto result = Api(options).verify();
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.auth_failed);
    EXPECT_FALSE(result.network);
    EXPECT_NE(result.error.find(u8"Client Secret"), std::string::npos);
    EXPECT_EQ(result.error.find("wrong-secret-value-999"), std::string::npos);
}

// 场景:凭据正确、应用已发布,用户点“保存”。
// 期望:校验成功,返回 Client ID 与默认机器人编码(与 Client ID 相同);做过一次 Stream 注册
// 但不建立 WebSocket 连接(ticket 90 秒后自然失效)。
TEST(DingTalkApi, VerifySucceedsWithoutOpeningAConnection) {
    test::FakeDingTalkServer server;
    const auto result = Api(local_options(server)).verify();
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.client_id, server.client_id);
    EXPECT_EQ(result.robot_code, server.client_id);
    EXPECT_EQ(server.issued_tickets().size(), 1u);
    EXPECT_EQ(server.connections(), 0);
    const auto open = server.requests_to("/v1.0/gateway/connections/open");
    ASSERT_EQ(open.size(), 1u);
    EXPECT_EQ(open[0].body["subscriptions"][0]["topic"], kBotMessageTopic);
    EXPECT_EQ(open[0].body["subscriptions"][0]["type"], "CALLBACK");
}

// 场景:凭据正确,但应用未发布 / 机器人没开 Stream 模式,Stream 注册返回 400。
// 期望:标记为 rejected(既不是凭据错误也不是网络问题),原因提示检查发布与 Stream 模式。
TEST(DingTalkApi, VerifyReportsUnpublishedApp) {
    test::FakeDingTalkServer server;
    server.open_handler = [](const test::FakeDingTalkServer::Request&) {
        return test::FakeDingTalkServer::Reply{400, {{"code", "invalidParameter"}, {"requestid", "R"},
                                                      {"message", "robot not found"}}};
    };
    const auto result = Api(local_options(server)).verify();
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.rejected);
    EXPECT_FALSE(result.auth_failed);
    EXPECT_NE(result.error.find(u8"Stream 模式"), std::string::npos) << result.error;
}

// 场景:钉钉开放平台连不上(端口无人监听)。
// 期望:标记为网络问题,而不是凭据无效。
TEST(DingTalkApi, VerifyDistinguishesNetworkFailure) {
    std::string dead_base;
    {
        test::FakeDingTalkServer server;
        dead_base = server.base();
    }
    ApiOptions options;
    options.client_id = "dingX";
    options.client_secret = "secret-value-xyz";
    options.api_base = dead_base;
    options.oapi_base = dead_base;
    options.use_proxy = false;
    const auto result = Api(options).verify();
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.network);
    EXPECT_FALSE(result.auth_failed);
}

// 场景:机器人单聊接口 HTTP 200 返回,但该用户在 invalidStaffIdList 里。
// 期望:视为未送达(ok=false),原因说明对方不在可见范围;不会被当成成功吞掉。
TEST(DingTalkApi, OtoListedUserIsNotDelivered) {
    test::FakeDingTalkServer server;
    server.oto_handler = [](const test::FakeDingTalkServer::Request&) {
        return test::FakeDingTalkServer::Reply{200, {{"processQueryKey", "PQK"},
                                                      {"invalidStaffIdList", {"staff01"}}}};
    };
    Api api(local_options(server));
    const auto result = api.send_oto("dingR", "staff01", "sampleMarkdown", {{"title", "t"}, {"text", "x"}});
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.message.find(u8"可见范围"), std::string::npos);
    const auto sent = server.requests_to("/v1.0/robot/oToMessages/batchSend");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].body["userIds"], nlohmann::json::array({"staff01"}));
}

// 场景:会话 webhook 返回 HTTP 200 + errcode 300001;另有一个指向外部主机的 webhook。
// 期望:HTTP 200 但 errcode 非 0 判为失败并识别为 webhook 失效(Hermes 只看状态码的 bug 不复现);
// 外部主机的地址直接拒绝,不发出任何请求。
TEST(DingTalkApi, WebhookChecksErrcodeAndAllowlist) {
    test::FakeDingTalkServer server;
    server.webhook_handler = [](const test::FakeDingTalkServer::Request&) {
        return test::FakeDingTalkServer::Reply{200, {{"errcode", 300001}, {"errmsg", "session 不存在"}}};
    };
    Api api(local_options(server));
    const nlohmann::json body{{"msgtype", "text"}, {"text", {{"content", "hi"}}}};
    const auto gone = api.send_webhook(server.webhook("S9"), body);
    EXPECT_FALSE(gone.ok);
    EXPECT_TRUE(is_webhook_gone(gone.error));
    EXPECT_EQ(gone.error.message.find("S9"), std::string::npos);

    const auto foreign = api.send_webhook("https://evil.example.com/robot/sendBySession?session=x", body);
    EXPECT_FALSE(foreign.ok);
    EXPECT_EQ(server.requests_to("/robot/sendBySession").size(), 1u);
}

// 场景:上传一张图片作为媒体文件。
// 期望:multipart 带 type 字段与 media 文件(文件名、内容原样),type 与令牌也放在查询参数里;
// 返回带 @ 前缀的 media_id;令牌在 OAPI 上失效(errcode 40014)时换新令牌重试一次。
TEST(DingTalkApi, UploadsMediaWithTypeFieldAndQuery) {
    test::FakeDingTalkServer server;
    std::atomic<int> uploads{0};
    server.upload_handler = [&uploads](const test::FakeDingTalkServer::Request&) {
        if (uploads++ == 0)
            return test::FakeDingTalkServer::Reply{200, {{"errcode", 40014}, {"errmsg", "不合法的access_token"}}};
        return test::FakeDingTalkServer::Reply{0, nullptr};
    };
    const auto file = std::filesystem::path(testing::TempDir()) / "acecode-dingtalk-shot.png";
    { std::ofstream(file, std::ios::binary) << "PNGDATA"; }
    Api api(local_options(server));
    const auto result = api.upload_media("image", file, "shot.png", "image/png");
    EXPECT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.response.value("media_id", ""), "@media-1");
    const auto requests = server.requests_to("/media/upload");
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[0].token, "tok-1");
    EXPECT_EQ(requests[1].token, "tok-2");
    EXPECT_EQ(requests[1].query, "image");
    EXPECT_EQ(requests[1].body.value("type", ""), "image");
    EXPECT_EQ(requests[1].file_name, "shot.png");
    EXPECT_EQ(requests[1].file_body, "PNGDATA");
    std::filesystem::remove(file);
}

// 场景:下载入站附件:先用 downloadCode 换下载地址,再 GET 该地址;另一个附件超过本地上限。
// 期望:换地址时带 downloadCode 与 robotCode;GET 不带 Content-Type(否则 OSS 签名校验失败);
// 超限时返回 false、中文原因,并删掉半截文件。
TEST(DingTalkApi, DownloadsWithoutContentTypeAndHonoursLimit) {
    test::FakeDingTalkServer server;
    Api api(local_options(server));
    const auto dest = std::filesystem::path(testing::TempDir()) / "acecode-dingtalk-dl.bin";
    std::string error;
    ASSERT_TRUE(api.download("code1", "dingR", dest, 1024, &error)) << error;
    EXPECT_EQ(std::filesystem::file_size(dest), 16u);
    const auto resolve = server.requests_to("/v1.0/robot/messageFiles/download");
    ASSERT_EQ(resolve.size(), 1u);
    EXPECT_EQ(resolve[0].body.value("downloadCode", ""), "code1");
    EXPECT_EQ(resolve[0].body.value("robotCode", ""), "dingR");
    const auto oss = server.requests_to("/oss/code1");
    ASSERT_EQ(oss.size(), 1u);
    EXPECT_TRUE(oss[0].content_type.empty());

    EXPECT_FALSE(api.download("big", "dingR", dest, 100, &error));
    EXPECT_NE(error.find(u8"超过"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(dest));
}

} // namespace
} // namespace acecode::im::dingtalk
