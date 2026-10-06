#include <gtest/gtest.h>

#include "im/telegram/tg_api.hpp"
#include "test_support/im/fake_telegram_server.hpp"

// im/telegram/tg_api:Bot API 客户端,对着本机假 Bot API 跑。

namespace acecode::im::telegram {
namespace {

ApiOptions local_options(const test::FakeTelegramServer& server) {
    ApiOptions options;
    options.token = server.token();
    options.api_base = server.base();
    options.use_proxy = false;
    return options;
}

// 场景:用正确 token 调 getMe,以及用错误 token 调 getMe。
// 期望:前者成功并拿到机器人信息;后者返回 401 失败,错误信息里不含 token。
TEST(TelegramApi, CallsMethodsWithTokenInPath) {
    test::FakeTelegramServer server;
    Api api(local_options(server));
    const auto me = api.call("getMe", nlohmann::json::object());
    ASSERT_TRUE(me.ok) << me.description;
    EXPECT_EQ(me.result.value("username", ""), "AceTestBot");
    auto bad_options = local_options(server);
    bad_options.token = "999999:WRONG-TOKEN-zyxwvutsrqponmlkjihgfedcba";
    Api bad(bad_options);
    const auto denied = bad.call("getMe", nlohmann::json::object());
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.status, 401);
    EXPECT_EQ(denied.description.find("WRONG-TOKEN"), std::string::npos);
}

// 场景:getUpdates 返回两种 409(配置了 webhook / 另一个程序在轮询),以及 429。
// 期望:409 按描述区分成两类;429 读出 retry_after。
TEST(TelegramApi, ClassifiesConflictsAndReadsRetryAfter) {
    test::FakeTelegramServer server;
    std::string mode = "webhook";
    server.override = [&](const test::FakeTelegramServer::Call& call) -> nlohmann::json {
        if (call.method != "getUpdates" && call.method != "sendMessage") return nullptr;
        if (call.method == "sendMessage")
            return {{"ok", false}, {"error_code", 429}, {"description", "Too Many Requests: retry after 3"},
                    {"parameters", {{"retry_after", 3}}}};
        if (mode == "webhook")
            return {{"ok", false}, {"error_code", 409},
                    {"description", "Conflict: can't use getUpdates method while webhook is active"}};
        return {{"ok", false}, {"error_code", 409},
                {"description", "Conflict: terminated by other getUpdates request"}};
    };
    Api api(local_options(server));
    EXPECT_EQ(conflict_kind(api.call("getUpdates", {{"offset", 0}})), ConflictKind::Webhook);
    mode = "other";
    EXPECT_EQ(conflict_kind(api.call("getUpdates", {{"offset", 0}})), ConflictKind::OtherPoller);
    const auto limited = api.call("sendMessage", {{"chat_id", 1}, {"text", "x"}});
    EXPECT_EQ(limited.status, 429);
    EXPECT_EQ(limited.retry_after, 3);
    EXPECT_EQ(conflict_kind(limited), ConflictKind::None);
}

// 场景:连接一个没有服务的地址,错误文本里本会带出完整 URL(含 token)。
// 期望:请求失败且错误信息不含 token。
TEST(TelegramApi, TransportErrorsDoNotLeakToken) {
    ApiOptions options;
    options.token = "123456:SECRET-TOKEN-abcdefghijklmnopqrstuvwxyz";
    options.api_base = "http://127.0.0.1:1";
    options.use_proxy = false;
    Api api(options);
    const auto result = api.call("getMe", nlohmann::json::object(), std::chrono::seconds(2));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.status, 0);
    EXPECT_EQ(result.description.find("SECRET-TOKEN"), std::string::npos) << result.description;
}

} // namespace
} // namespace acecode::im::telegram
