#include <gtest/gtest.h>

#include "im/weixin/weixin_login.hpp"
#include "test_support/im/fake_weixin_server.hpp"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

// im/weixin/weixin_login:微信 ClawBot 扫码登录流程。用本机假 iLink 服务代替 ilinkai.weixin.qq.com;
// 真实服务的状态查询会挂起约 30 秒,假服务立即应答,轮询间隔缩短到 20ms。

namespace acecode::im::weixin {
namespace {

LoginOptions local_options(const test::FakeWeixinServer& server) {
    LoginOptions options;
    options.api_base = server.base();
    options.use_proxy = false;
    options.poll_interval = std::chrono::milliseconds(20);
    options.total_timeout = std::chrono::seconds(5);
    options.redirect_scheme = "http";
    return options;
}

nlohmann::json confirmed(const std::string& base_url = "https://ilinkai.weixin.qq.com") {
    return {{"ret", 0},
            {"status", "confirmed"},
            {"bot_token", "bot-token-SECRET-123456"},
            {"ilink_bot_id", "e06c1ceea05e@im.bot"},
            {"baseurl", base_url},
            {"ilink_user_id", "o9cq800kum_owner@im.wechat"}};
}

struct ProgressLog {
    std::mutex mu;
    std::vector<LoginProgress> steps;
    LoginProgressFn fn() {
        return [this](const LoginProgress& step) {
            std::lock_guard<std::mutex> lock(mu);
            steps.push_back(step);
        };
    }
};

// 场景:用户扫码,状态依次为 wait → scaned → confirmed。
// 期望:先回调一次二维码(内容是 qrcode_img_content 的 liteapp 网址,不是 32 位令牌),扫码后回调一次
// Scanned;结果带回 bot_token、ilink_bot_id、baseurl 与扫码人;状态查询带上令牌,
// 并只带 iLink-App-Id: bot 与 ClientVersion 131584 两个协议头(没有 Authorization)。
TEST(WeixinLogin, CompletesAfterScanAndConfirm) {
    test::FakeWeixinServer server;
    std::atomic<int> polls{0};
    server.qr_status_handler = [&](const test::FakeWeixinServer::Call&) {
        const int n = ++polls;
        if (n == 1) return nlohmann::json{{"ret", 0}, {"status", "wait"}};
        if (n == 2) return nlohmann::json{{"ret", 0}, {"status", "scaned"}};
        if (n == 3) return nlohmann::json{{"ret", 0}, {"status", "scaned"}};
        return confirmed();
    };
    ProgressLog log;
    std::atomic<bool> cancelled{false};
    const auto result = run_login(local_options(server), cancelled, log.fn());
    ASSERT_EQ(result.phase, LoginPhase::Completed) << result.error;
    EXPECT_EQ(result.bot_token, "bot-token-SECRET-123456");
    EXPECT_EQ(result.bot_id, "e06c1ceea05e@im.bot");
    EXPECT_EQ(result.base_url, "https://ilinkai.weixin.qq.com");
    EXPECT_EQ(result.user_id, "o9cq800kum_owner@im.wechat");
    EXPECT_EQ(result.refreshes, 0);

    ASSERT_EQ(log.steps.size(), 2u);
    EXPECT_EQ(log.steps[0].phase, LoginPhase::WaitingScan);
    EXPECT_EQ(log.steps[0].qr_url, "https://liteapp.weixin.qq.com/q/7GiQu1?qrcode=qr-1&bot_type=3");
    EXPECT_EQ(log.steps[1].phase, LoginPhase::Scanned);
    EXPECT_EQ(log.steps[1].qr_url, log.steps[0].qr_url);

    const auto qr = server.calls_to("get_bot_qrcode");
    ASSERT_EQ(qr.size(), 1u);
    EXPECT_EQ(qr[0].method, "GET");
    EXPECT_EQ(qr[0].query.at("bot_type"), "3");
    const auto status = server.calls_to("get_qrcode_status");
    ASSERT_FALSE(status.empty());
    EXPECT_EQ(status[0].query.at("qrcode"), "qr-1");
    EXPECT_EQ(status[0].headers.at("iLink-App-Id"), "bot");
    EXPECT_EQ(status[0].headers.at("iLink-App-ClientVersion"), "131584");
    EXPECT_EQ(status[0].headers.count("Authorization"), 0u);
}

// 场景:第一张二维码过期(expired),平台再发一张新的,用户扫第二张。
// 期望:自动重新申请二维码并回调新地址(两次地址不同),之后用新令牌查询,最终完成且 refreshes = 1。
TEST(WeixinLogin, RefreshesExpiredQrCode) {
    test::FakeWeixinServer server;
    server.qr_status_handler = [&](const test::FakeWeixinServer::Call& call) {
        if (call.query.at("qrcode") == "qr-1") return nlohmann::json{{"ret", 0}, {"status", "expired"}};
        return confirmed();
    };
    ProgressLog log;
    std::atomic<bool> cancelled{false};
    const auto result = run_login(local_options(server), cancelled, log.fn());
    ASSERT_EQ(result.phase, LoginPhase::Completed) << result.error;
    EXPECT_EQ(result.refreshes, 1);
    ASSERT_EQ(log.steps.size(), 2u);
    EXPECT_NE(log.steps[0].qr_url, log.steps[1].qr_url);
    EXPECT_EQ(log.steps[1].refreshes, 1);
    EXPECT_EQ(server.calls_to("get_bot_qrcode").size(), 2u);
}

// 场景:二维码一直过期(用户迟迟不扫),换码次数超过上限。
// 期望:返回 Failed 并说明“多次过期”,不会无限换码。
TEST(WeixinLogin, GivesUpAfterTooManyExpiredCodes) {
    test::FakeWeixinServer server;
    server.qr_status_handler = [](const test::FakeWeixinServer::Call&) {
        return nlohmann::json{{"ret", 0}, {"status", "expired"}};
    };
    auto options = local_options(server);
    options.max_refreshes = 2;
    std::atomic<bool> cancelled{false};
    const auto result = run_login(options, cancelled, {});
    EXPECT_EQ(result.phase, LoginPhase::Failed);
    EXPECT_NE(result.error.find(u8"多次过期"), std::string::npos) << result.error;
    EXPECT_EQ(server.calls_to("get_bot_qrcode").size(), 3u);
}

// 场景:扫码后平台要求把轮询换到另一个机房(scaned_but_redirect + redirect_host)。
// 期望:之后用同一个令牌到新地址轮询,在那里拿到确认;没有返回 baseurl 时沿用默认接口地址。
TEST(WeixinLogin, FollowsRedirectHost) {
    test::FakeWeixinServer primary;
    test::FakeWeixinServer redirected;
    primary.qr_status_handler = [&](const test::FakeWeixinServer::Call&) {
        return nlohmann::json{{"ret", 0}, {"status", "scaned_but_redirect"}, {"redirect_host", redirected.host()}};
    };
    redirected.qr_status_handler = [](const test::FakeWeixinServer::Call&) {
        auto done = confirmed();
        done.erase("baseurl");
        return done;
    };
    ProgressLog log;
    std::atomic<bool> cancelled{false};
    const auto result = run_login(local_options(primary), cancelled, log.fn());
    ASSERT_EQ(result.phase, LoginPhase::Completed) << result.error;
    EXPECT_EQ(result.base_url, primary.base());
    EXPECT_EQ(primary.calls_to("get_qrcode_status").size(), 1u);
    const auto moved = redirected.calls_to("get_qrcode_status");
    ASSERT_FALSE(moved.empty());
    EXPECT_EQ(moved[0].query.at("qrcode"), "qr-1");
    ASSERT_EQ(log.steps.size(), 2u);
    EXPECT_EQ(log.steps[1].phase, LoginPhase::Scanned);
}

// 场景:新版协议要求在电脑上输入手机显示的数字(need_verifycode),ACECode 不支持这一步。
// 期望:不挂起等待,立即返回 Failed,原因用中文说明需要验证数字。
TEST(WeixinLogin, VerificationCodeFailsWithClearMessage) {
    test::FakeWeixinServer server;
    server.qr_status_handler = [](const test::FakeWeixinServer::Call&) {
        return nlohmann::json{{"ret", 0}, {"status", "need_verifycode"}};
    };
    std::atomic<bool> cancelled{false};
    const auto result = run_login(local_options(server), cancelled, {});
    EXPECT_EQ(result.phase, LoginPhase::Failed);
    EXPECT_NE(result.error.find(u8"验证数字"), std::string::npos) << result.error;
    EXPECT_TRUE(result.bot_token.empty());
}

// 场景:申请二维码时平台报错;以及确认结果里缺少机器人 id。
// 期望:前者 Failed 并带平台原因;后者 Failed,且不返回任何凭据。
TEST(WeixinLogin, ReportsQrFailureAndIncompleteConfirmation) {
    test::FakeWeixinServer server;
    server.qr_handler = [](const test::FakeWeixinServer::Call&) {
        return nlohmann::json{{"ret", -1}, {"errmsg", "bot_type invalid"}};
    };
    std::atomic<bool> cancelled{false};
    const auto failed = run_login(local_options(server), cancelled, {});
    EXPECT_EQ(failed.phase, LoginPhase::Failed);
    EXPECT_NE(failed.error.find("bot_type invalid"), std::string::npos) << failed.error;

    test::FakeWeixinServer partial;
    partial.qr_status_handler = [](const test::FakeWeixinServer::Call&) {
        auto done = confirmed();
        done.erase("ilink_bot_id");
        return done;
    };
    const auto incomplete = run_login(local_options(partial), cancelled, {});
    EXPECT_EQ(incomplete.phase, LoginPhase::Failed);
    EXPECT_TRUE(incomplete.bot_token.empty());
}

// 场景:二维码已显示,用户点了取消;以及一直没人扫码直到总超时。
// 期望:取消后很快返回 Cancelled;超时返回 TimedOut。均不产生凭据。
TEST(WeixinLogin, CancelAndTimeout) {
    test::FakeWeixinServer server;
    std::atomic<bool> cancelled{false};
    auto options = local_options(server);
    options.poll_interval = std::chrono::seconds(10);
    LoginResult result;
    std::thread runner([&] { result = run_login(options, cancelled, {}); });
    ASSERT_TRUE(test::FakeWeixinServer::wait_until([&] { return !server.calls_to("get_qrcode_status").empty(); }));
    const auto start = std::chrono::steady_clock::now();
    cancelled = true;
    runner.join();
    EXPECT_EQ(result.phase, LoginPhase::Cancelled);
    EXPECT_TRUE(result.bot_token.empty());
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));

    std::atomic<bool> idle{false};
    auto quick = local_options(server);
    quick.total_timeout = std::chrono::milliseconds(300);
    EXPECT_EQ(run_login(quick, idle, {}).phase, LoginPhase::TimedOut);
}

} // namespace
} // namespace acecode::im::weixin
