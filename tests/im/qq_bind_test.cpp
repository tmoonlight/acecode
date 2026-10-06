#include <gtest/gtest.h>

#include "im/qqbot/qq_bind.hpp"
#include "test_support/im/fake_qq_server.hpp"

#include <thread>

// im/qqbot/qq_bind:QQ 机器人扫码配置流程。用本机假门户代替 q.qq.com;
// 加密向量用固定密钥(0x00..0x1f)与 IV,由 Python cryptography 预先算出。

namespace acecode::im::qqbot {
namespace {

constexpr const char* kKeyB64 = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";
constexpr const char* kSecretB64 = "ZGVmZ2hpamtsbW5vOWrzEhyaIrNNBzyavxFHGfpv4JwMQcJAlM16v321wbiQ0njyP4fhqnk=";

std::string fixed_key() {
    std::string key;
    for (int i = 0; i < 32; ++i) key.push_back(static_cast<char>(i));
    return key;
}

BindOptions local_options(const test::FakeQqServer& server) {
    BindOptions options;
    options.portal_base = server.base();
    options.connect_base = "https://q.qq.com";
    options.use_proxy = false;
    options.poll_interval = std::chrono::milliseconds(20);
    options.total_timeout = std::chrono::seconds(5);
    options.key_provider = fixed_key;
    return options;
}

// 场景:用户扫码,门户第一次轮询返回“进行中”,第二次返回完成并带加密密钥与扫码人。
// 期望:先回调一次显示二维码(指向正式门户、来源 acecode);最终返回完成,
// 解密出的 AppSecret 与预设明文一致;申请任务时上传的是本地生成的密钥。
TEST(QqBind, CompletesAndDecryptsSecret) {
    test::FakeQqServer server;
    int polls = 0;
    server.poll_bind_handler = [&](const nlohmann::json&) {
        ++polls;
        if (polls < 2) return nlohmann::json{{"retcode", 0}, {"data", {{"status", 1}}}};
        return nlohmann::json{{"retcode", 0},
                              {"data", {{"status", 2}, {"bot_appid", 102030405},
                                        {"bot_encrypt_secret", kSecretB64}, {"user_openid", "OWNER1"}}}};
    };
    std::vector<BindUpdate> updates;
    std::atomic<bool> cancelled{false};
    const auto result = run_bind(local_options(server), cancelled,
                                 [&](const BindUpdate& u) { updates.push_back(u); });
    ASSERT_EQ(result.phase, BindPhase::Completed) << result.error;
    EXPECT_EQ(result.app_id, "102030405");
    EXPECT_EQ(result.app_secret, u8"qq-test-secret-中文-123");
    EXPECT_EQ(result.user_openid, "OWNER1");
    ASSERT_EQ(updates.size(), 1u);
    EXPECT_EQ(updates[0].qr_url, "https://q.qq.com/qqbot/openclaw/connect.html?task_id=task-1&source=acecode&_wv=2");
    const auto created = server.requests_to("/lite/create_bind_task");
    ASSERT_EQ(created.size(), 1u);
    EXPECT_EQ(created[0].body.value("key", ""), kKeyB64);
}

// 场景:第一张二维码过期(status 3),门户再给一个新任务。
// 期望:自动换新任务,二维码回调两次且地址不同,最终完成且 refreshes = 1。
TEST(QqBind, RefreshesExpiredQrCode) {
    test::FakeQqServer server;
    int created = 0;
    server.create_bind_handler = [&](const nlohmann::json&) {
        ++created;
        return nlohmann::json{{"retcode", 0}, {"data", {{"task_id", "task-" + std::to_string(created)}}}};
    };
    server.poll_bind_handler = [&](const nlohmann::json& body) {
        if (body.value("task_id", "") == "task-1") return nlohmann::json{{"retcode", 0}, {"data", {{"status", 3}}}};
        return nlohmann::json{{"retcode", 0},
                              {"data", {{"status", 2}, {"bot_appid", "APP"}, {"bot_encrypt_secret", kSecretB64}}}};
    };
    std::vector<std::string> urls;
    std::atomic<bool> cancelled{false};
    const auto result = run_bind(local_options(server), cancelled,
                                 [&](const BindUpdate& u) { urls.push_back(u.qr_url); });
    ASSERT_EQ(result.phase, BindPhase::Completed) << result.error;
    EXPECT_EQ(result.refreshes, 1);
    ASSERT_EQ(urls.size(), 2u);
    EXPECT_NE(urls[0], urls[1]);
    EXPECT_TRUE(result.user_openid.empty());
}

// 场景:二维码已显示,用户在扫码前点了取消。
// 期望:流程很快返回 Cancelled,不产生任何凭据。
TEST(QqBind, CancelStopsQuickly) {
    test::FakeQqServer server;
    std::atomic<bool> cancelled{false};
    auto options = local_options(server);
    options.poll_interval = std::chrono::seconds(10);
    BindUpdate result;
    std::thread runner([&] { result = run_bind(options, cancelled, {}); });
    ASSERT_TRUE(test::FakeQqServer::wait_until([&] { return !server.requests_to("/lite/create_bind_task").empty(); }));
    const auto start = std::chrono::steady_clock::now();
    cancelled = true;
    runner.join();
    EXPECT_EQ(result.phase, BindPhase::Cancelled);
    EXPECT_TRUE(result.app_secret.empty());
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
}

// 场景:门户拒绝创建任务(retcode 非 0),以及一直没人扫码直到总超时。
// 期望:前者返回 Failed 并带门户给的原因;后者返回 TimedOut。
TEST(QqBind, ReportsPortalFailureAndTimeout) {
    test::FakeQqServer server;
    server.create_bind_handler = [](const nlohmann::json&) {
        return nlohmann::json{{"retcode", 1001}, {"msg", "source not allowed"}};
    };
    std::atomic<bool> cancelled{false};
    const auto failed = run_bind(local_options(server), cancelled, {});
    EXPECT_EQ(failed.phase, BindPhase::Failed);
    EXPECT_NE(failed.error.find("source not allowed"), std::string::npos) << failed.error;

    test::FakeQqServer idle;
    auto options = local_options(idle);
    options.total_timeout = std::chrono::milliseconds(300);
    EXPECT_EQ(run_bind(options, cancelled, {}).phase, BindPhase::TimedOut);
}

// 场景:扫码结果里的密文被篡改,或用错了密钥。
// 期望:解密失败,不返回任何明文。
TEST(QqBind, RejectsTamperedSecret) {
    std::string secret, error;
    EXPECT_TRUE(decrypt_bind_secret(kSecretB64, kKeyB64, secret, &error)) << error;
    std::string tampered = kSecretB64;
    tampered[20] = tampered[20] == 'A' ? 'B' : 'A';
    secret.clear();
    EXPECT_FALSE(decrypt_bind_secret(tampered, kKeyB64, secret, &error));
    EXPECT_TRUE(secret.empty());
    EXPECT_FALSE(decrypt_bind_secret("not base64!", kKeyB64, secret, &error));
}

} // namespace
} // namespace acecode::im::qqbot
