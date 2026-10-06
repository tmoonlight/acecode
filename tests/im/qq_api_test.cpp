#include <gtest/gtest.h>

#include "im/qqbot/qq_api.hpp"
#include "test_support/im/fake_qq_server.hpp"

#include <fstream>

// im/qqbot/qq_api:QQ OpenAPI 客户端,对着本机假开放平台跑。

namespace acecode::im::qqbot {
namespace {

ApiOptions local_options(const test::FakeQqServer& server) {
    ApiOptions options;
    options.app_id = "APP";
    options.app_secret = "secret-value-123";
    options.api_base = server.base();
    options.token_url = server.base() + "/app/getAppAccessToken";
    options.use_proxy = false;
    return options;
}

// 场景:连续两次取令牌。
// 期望:第一次向平台换取,第二次直接复用缓存;请求体带 appId 与 clientSecret。
TEST(QqApi, FetchesAndCachesAccessToken) {
    test::FakeQqServer server;
    nlohmann::json seen;
    server.token_handler = [&](const nlohmann::json& body) {
        seen = body;
        return nlohmann::json{{"access_token", "tok-A"}, {"expires_in", "7200"}};
    };
    Api api(local_options(server));
    std::string error;
    EXPECT_EQ(api.access_token(&error), "tok-A") << error;
    EXPECT_EQ(api.access_token(&error), "tok-A");
    EXPECT_EQ(server.token_calls(), 1);
    EXPECT_EQ(seen.value("appId", ""), "APP");
    EXPECT_EQ(seen.value("clientSecret", ""), "secret-value-123");
}

// 场景:凭据错误时平台仍返回 HTTP 200,只是带 code=100007。
// 期望:取令牌失败、标记为凭据被拒(调用方据此停止重试),错误信息里不含 AppSecret。
TEST(QqApi, InvalidCredentialsAreReportedAsAuthFailure) {
    test::FakeQqServer server;
    server.token_handler = [](const nlohmann::json&) {
        return nlohmann::json{{"code", 100007}, {"message", "appid invalid"}};
    };
    Api api(local_options(server));
    std::string error;
    EXPECT_TRUE(api.access_token(&error).empty());
    EXPECT_TRUE(api.last_token_auth_failed());
    EXPECT_NE(error.find("appid invalid"), std::string::npos) << error;
    EXPECT_EQ(error.find("secret-value-123"), std::string::npos) << error;
    EXPECT_FALSE(api.verify(&error));
}

// 场景:取网关地址,以及发一条单聊消息。
// 期望:请求都带 "QQBot <令牌>" 鉴权头;网关地址原样返回;消息体原样送达。
TEST(QqApi, GatewayAndSendUseBotAuthorization) {
    test::FakeQqServer server;
    Api api(local_options(server));
    std::string error;
    const auto url = api.gateway_url(&error);
    EXPECT_NE(url.find("/websocket"), std::string::npos) << error;
    const auto result = api.send_message("c2c", "U1", {{"content", "hi"}, {"msg_type", 0}});
    EXPECT_TRUE(result.ok) << result.error.message;
    const auto sent = server.requests_to("/v2/users/U1/messages");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].authorization, "QQBot tok-1");
    EXPECT_EQ(sent[0].body.value("content", ""), "hi");
}

// 场景:平台对第一次发送返回 401(令牌在本地缓存期内被吊销)。
// 期望:客户端作废令牌、重新换取后重发一次,最终成功;令牌接口被调用两次。
TEST(QqApi, RetriesOnceAfterUnauthorized) {
    test::FakeQqServer server;
    int calls = 0;
    server.message_handler = [&](const test::FakeQqServer::Request&) {
        ++calls;
        if (calls == 1) return std::make_pair(401, nlohmann::json{{"message", "token expired"}});
        return std::make_pair(200, nlohmann::json{{"id", "R"}});
    };
    Api api(local_options(server));
    const auto result = api.send_message("group", "G1", {{"content", "x"}, {"msg_type", 0}});
    EXPECT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(server.token_calls(), 2);
    const auto sent = server.requests_to("/v2/groups/G1/messages");
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[1].authorization, "QQBot tok-2");
}

// 场景:平台以 400 拒绝发送。
// 期望:返回失败并带平台给的错误码与原因,不重试。
TEST(QqApi, ClientErrorsAreReturnedWithoutRetry) {
    test::FakeQqServer server;
    server.message_handler = [](const test::FakeQqServer::Request&) {
        return std::make_pair(400, nlohmann::json{{"code", 40034}, {"message", "msg_id expired"}});
    };
    Api api(local_options(server));
    const auto result = api.send_message("c2c", "U1", {{"content", "x"}});
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.status, 400);
    EXPECT_EQ(result.error.code, 40034);
    EXPECT_EQ(server.requests_to("/v2/users/U1/messages").size(), 1u);
}

// 场景:上传一个普通文件到群。
// 期望:请求体包含 file_type=4、base64 数据、文件名,且不直接发送(srv_send_msg=false);返回 file_info。
TEST(QqApi, UploadsFileAsBase64) {
    test::FakeQqServer server;
    Api api(local_options(server));
    const auto result = api.upload_file("group", "G1", kFileTypeFile, "aGVsbG8=", "r.pdf");
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.response.value("file_info", ""), "INFO-1");
    const auto uploads = server.requests_to("/v2/groups/G1/files");
    ASSERT_EQ(uploads.size(), 1u);
    EXPECT_EQ(uploads[0].body.value("file_type", 0), kFileTypeFile);
    EXPECT_EQ(uploads[0].body.value("file_data", ""), "aGVsbG8=");
    EXPECT_EQ(uploads[0].body.value("file_name", ""), "r.pdf");
    EXPECT_FALSE(uploads[0].body.value("srv_send_msg", true));
}

// 场景:下载附件,一个在上限内,一个超过上限。
// 期望:前者写入目标文件并带鉴权头;后者失败且不留下半截文件。
TEST(QqApi, DownloadsWithAuthAndSizeLimit) {
    test::FakeQqServer server;
    Api api(local_options(server));
    const auto dir = std::filesystem::path(testing::TempDir()) / "acecode-qq-api-download";
    std::filesystem::create_directories(dir);
    std::string error;
    ASSERT_TRUE(api.download(server.base() + "/download/small", dir / "small.bin", 1024, &error)) << error;
    EXPECT_EQ(std::filesystem::file_size(dir / "small.bin"), 16u);
    EXPECT_EQ(server.requests_to("/download/small").at(0).authorization, "QQBot tok-1");
    EXPECT_FALSE(api.download(server.base() + "/download/big", dir / "big.bin", 1024, &error));
    EXPECT_FALSE(std::filesystem::exists(dir / "big.bin"));
    std::filesystem::remove_all(dir);
}

} // namespace
} // namespace acecode::im::qqbot
