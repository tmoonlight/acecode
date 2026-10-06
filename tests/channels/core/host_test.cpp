#include <gtest/gtest.h>

#include "channels/core/host.hpp"
#include "test_support/channels/core_fakes.hpp"
#include "test_support/im/fake_qq_server.hpp"

#include <filesystem>
#include <fstream>
#include <memory>

// channels/core/host + platform_runtime:平台宿主的生命周期。传输层用假实现(由工厂注入),
// QQ 扫码流程对接本机假门户;两个宿主在同一临时目录里争用归属锁。

namespace acecode::channels::core {
namespace {

using test::FakeSessions;
using test::FakeTransport;
using test::message;
using test::private_address;
using test::wait_until;

constexpr const char* kSecretB64 = "ZGVmZ2hpamtsbW5vOWrzEhyaIrNNBzyavxFHGfpv4JwMQcJAlM16v321wbiQ0njyP4fhqnk=";

std::string fixed_key() {
    std::string key;
    for (int i = 0; i < 32; ++i) key.push_back(static_cast<char>(i));
    return key;
}

class ChannelHostTest : public ::testing::Test {
protected:
    void SetUp() override {
        home_dir = channels::test::temporary("host");
        std::filesystem::create_directories(home_dir);
        home = std::make_unique<channels::test::Home>(home_dir);
        root = home_dir / "channels";
    }
    void TearDown() override {
        home.reset();
        std::error_code ec;
        std::filesystem::remove_all(home_dir, ec);
    }

    HostServices services(std::int64_t pid = 1111) {
        HostServices s;
        s.conversation.sessions = &sessions;
        s.conversation.pending_permissions = [](const std::string&) { return std::vector<nlohmann::json>{}; };
        s.conversation.session_cwd = [this](const std::string& id) {
            std::lock_guard<std::mutex> lock(sessions.mu);
            return sessions.sessions[id].cwd;
        };
        s.conversation.catalog = [this](const std::optional<std::string>&) { return sessions.catalog(); };
        s.conversation.resume_target = [this](const rc::RcSessionTarget& target) {
            return sessions.resume_session(target.session_id, {});
        };
        s.broadcast = [this](const std::string& type, const nlohmann::json& payload) {
            std::lock_guard<std::mutex> lock(mu);
            events.emplace_back(type, payload);
        };
        s.current_pid = [pid] { return pid; };
        s.standby_retry = std::chrono::milliseconds(30);
        s.make_transport = [this](const std::string& platform, const PlatformConfig& config, ChannelStore&) {
            auto transport = std::make_shared<FakeTransport>(platform);
            transport->account = platform == "qq" ? config.credentials.value("app_id", std::string{}) : "100";
            if (platform == "telegram") transport->extra = {{"username", "ace_bot"}};
            std::lock_guard<std::mutex> lock(mu);
            made.push_back(transport);
            return transport;
        };
        s.validate_credentials = [this](const std::string&, const nlohmann::json& credentials, std::string* error) {
            std::lock_guard<std::mutex> lock(mu);
            validated.push_back(credentials);
            if (reject_credentials) {
                *error = "凭据无效";
                return nlohmann::json(nullptr);
            }
            return credentials;
        };
        s.bind.portal_base = qq.base();
        s.bind.connect_base = "https://q.qq.com";
        s.bind.use_proxy = false;
        s.bind.poll_interval = std::chrono::milliseconds(20);
        s.bind.total_timeout = std::chrono::seconds(5);
        s.bind.key_provider = fixed_key;
        return s;
    }

    std::shared_ptr<FakeTransport> latest(const std::string& platform) {
        std::lock_guard<std::mutex> lock(mu);
        for (auto it = made.rbegin(); it != made.rend(); ++it)
            if ((*it)->platform() == platform) return *it;
        return nullptr;
    }
    std::size_t made_count() {
        std::lock_guard<std::mutex> lock(mu);
        return made.size();
    }
    std::vector<nlohmann::json> events_of(const std::string& type) {
        std::lock_guard<std::mutex> lock(mu);
        std::vector<nlohmann::json> out;
        for (const auto& [name, payload] : events)
            if (name == type) out.push_back(payload);
        return out;
    }
    static nlohmann::json platform_of(const ChannelHost& host, const std::string& name) {
        const auto snapshot = host.snapshot();  // 先存下来:range-for 不延长临时对象子对象的生命周期
        for (const auto& item : snapshot["platforms"])
            if (item.value("platform", "") == name) return item;
        return nlohmann::json::object();
    }
    // 快照里可为 null 的字段(如 owner)按空串读。
    static std::string text(const nlohmann::json& object, const char* key) {
        return object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : std::string{};
    }
    // Telegram 配好凭据、设机主 user:1 并打开开关。
    void connect_telegram(ChannelHost& host) {
        host.platform("telegram").set_credentials({{"token", "100:abcdefgh1234"}});
        host.platform("telegram").set_owner("user:1", "机主");
        host.platform("telegram").set_enabled(true);
    }

    std::filesystem::path home_dir, root;
    std::unique_ptr<channels::test::Home> home;
    FakeSessions sessions;
    acecode::test::FakeQqServer qq;
    std::mutex mu;
    std::vector<std::pair<std::string, nlohmann::json>> events;
    std::vector<std::shared_ptr<FakeTransport>> made;
    std::vector<nlohmann::json> validated;
    bool reject_credentials = false;
};

// 场景:新平台没有凭据时打开开关;保存凭据后打开、再关闭。
// 期望:无凭据时拒绝打开;打开立即连接(无需保存),关闭立即断开并持久化开关状态;
// 快照里的凭据只显示尾号;每次变化都推送 channels_state。
TEST_F(ChannelHostTest, ToggleConnectsAndDisconnectsImmediately) {
    ChannelHost host(root, services());
    host.start();
    EXPECT_EQ(platform_of(host, "telegram").value("state", ""), "unconfigured");
    EXPECT_THROW(host.platform("telegram").set_enabled(true), std::runtime_error);

    host.platform("telegram").set_credentials({{"token", "100:abcdefgh1234"}});
    auto snapshot = platform_of(host, "telegram");
    EXPECT_EQ(snapshot.value("state", ""), "disabled");
    EXPECT_EQ(snapshot.value("credential_hint", ""), "****1234");
    EXPECT_EQ(host.snapshot().dump().find("abcdefgh1234"), std::string::npos);

    host.platform("telegram").set_enabled(true);
    const auto transport = latest("telegram");
    ASSERT_TRUE(transport);
    EXPECT_EQ(transport->starts, 1);
    EXPECT_EQ(platform_of(host, "telegram").value("state", ""), "connected");

    host.platform("telegram").set_enabled(false);
    EXPECT_EQ(transport->stops, 1);
    EXPECT_EQ(platform_of(host, "telegram").value("state", ""), "disabled");
    EXPECT_FALSE(events_of("channels_state").empty());
    host.stop();

    ChannelStore store(root / "telegram");
    store.load();
    EXPECT_FALSE(store.config().enabled);
    EXPECT_EQ(store.config().credentials.value("token", ""), "100:abcdefgh1234");
}

// 场景:手动保存的凭据校验失败;已保存凭据后只改一个字段。
// 期望:校验失败不保存并带原因;缺失的字段沿用已保存的值再校验。
TEST_F(ChannelHostTest, CredentialsAreValidatedAndMerged) {
    ChannelHost host(root, services());
    host.start();
    host.platform("qq").set_credentials({{"app_id", "A1"}, {"app_secret", "secret-1"}});
    reject_credentials = true;
    try {
        host.platform("qq").set_credentials({{"app_secret", "wrong"}});
        FAIL() << "校验失败应当抛异常";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "凭据无效");
    }
    EXPECT_EQ(platform_of(host, "qq").value("credential_hint", ""), "****et-1");
    reject_credentials = false;
    host.platform("qq").set_credentials({{"app_secret", "secret-2"}});
    {
        std::lock_guard<std::mutex> lock(mu);
        EXPECT_EQ(validated.back().value("app_id", ""), "A1");
    }
    EXPECT_EQ(platform_of(host, "qq").value("app_id", ""), "A1");
    host.stop();
}

// 场景:传输层报告网络中断后进入重试,随后报告鉴权失败并停止重试。
// 期望:页面状态依次为 retrying(附原因)与 failed(retry_stopped),并推送给页面。
TEST_F(ChannelHostTest, TransportStatusIsPublished) {
    ChannelHost host(root, services());
    host.start();
    connect_telegram(host);
    const auto transport = latest("telegram");
    transport->report(im::LinkState::Retrying, "网络中断");
    auto snapshot = platform_of(host, "telegram");
    EXPECT_EQ(snapshot.value("state", ""), "retrying");
    EXPECT_EQ(snapshot.value("detail", ""), "网络中断");
    bool pushed = false;
    for (const auto& event : events_of("channels_state"))
        if (event.value("state", "") == "retrying") pushed = true;
    EXPECT_TRUE(pushed);
    host.stop();
}

// 场景:平台明确拒绝凭据,传输层停止重试;用户再次打开开关。
// 期望:页面显示 failed 与原因;再次打开开关会重建连接。
TEST_F(ChannelHostTest, ReenablingAfterFatalErrorReconnects) {
    ChannelHost host(root, services());
    host.start();
    connect_telegram(host);
    latest("telegram")->report(im::LinkState::Failed, "Token 无效");
    auto snapshot = platform_of(host, "telegram");
    EXPECT_EQ(snapshot.value("state", ""), "failed");
    EXPECT_EQ(snapshot.value("detail", ""), "Token 无效");
    EXPECT_EQ(made_count(), 1u);
    host.platform("telegram").set_enabled(true);
    EXPECT_EQ(made_count(), 2u);
    EXPECT_EQ(platform_of(host, "telegram").value("state", ""), "connected");
    host.stop();
}

// 场景:两个 daemon 进程(这里是两个宿主)都开启了同一个 Telegram 机器人。
// 期望:先取得归属锁的宿主连接;另一个进入待命并显示托管进程的 PID;
// 持有者退出后,待命宿主在有限时间内接管连接。
TEST_F(ChannelHostTest, SecondHostWaitsAndTakesOver) {
    auto first = std::make_unique<ChannelHost>(root, services(1111));
    first->start();
    connect_telegram(*first);
    ASSERT_EQ(platform_of(*first, "telegram").value("state", ""), "connected");

    ChannelHost second(root, services(2222));
    second.start();
    auto waiting = platform_of(second, "telegram");
    EXPECT_EQ(waiting.value("state", ""), "standby");
    EXPECT_EQ(waiting.value("hosted_by_pid", 0), 1111);
    EXPECT_EQ(made_count(), 1u);  // 待命期间不建立第二个连接

    first->stop();
    first.reset();
    EXPECT_TRUE(wait_until([&] { return platform_of(second, "telegram").value("state", "") == "connected"; }));
    EXPECT_EQ(made_count(), 2u);
    second.stop();
}

// 场景:开启状态下 daemon 重启;重启前已有一个会话绑定。
// 期望:新宿主启动后自动连接,绑定保留,并为绑定恢复出站投影(先恢复会话)。
TEST_F(ChannelHostTest, RestartReconnectsAndRestoresBindings) {
    std::string bound;
    {
        ChannelHost host(root, services());
        host.start();
        connect_telegram(host);
        latest("telegram")->inject(message(private_address("1"), "你好", "m1"));
        ASSERT_TRUE(wait_until([&] { return !sessions.input_log().empty(); }));
        bound = sessions.input_log()[0].first;
        host.stop();
    }
    sessions.resumed.clear();
    ChannelHost host(root, services());
    host.start();
    EXPECT_EQ(platform_of(host, "telegram").value("state", ""), "connected");
    EXPECT_EQ(host.bound_sessions().at(bound), "telegram");
    EXPECT_TRUE(wait_until([&] {
        std::lock_guard<std::mutex> lock(sessions.mu);
        return std::find(sessions.resumed.begin(), sessions.resumed.end(), bound) != sessions.resumed.end();
    }));
    // 恢复会话之后才订阅事件:等订阅建立再模拟 Desktop 里产生的回复。
    ASSERT_TRUE(wait_until([&] { return sessions.listener_count(bound) == 1; }));
    sessions.assistant(bound, "Desktop 里输入后的回复");
    EXPECT_TRUE(latest("telegram")->wait_said("Desktop 里输入后的回复"));
    host.stop();
}

// 场景:Telegram 的 config.json 被写坏。
// 期望:该平台显示 error 与原因,开关操作被拒绝;另一个平台不受影响。
TEST_F(ChannelHostTest, CorruptConfigBlocksOnlyThatPlatform) {
    std::filesystem::create_directories(root / "telegram");
    std::ofstream(root / "telegram" / "config.json", std::ios::binary) << "{oops";
    ChannelHost host(root, services());
    host.start();
    const auto broken = platform_of(host, "telegram");
    EXPECT_EQ(broken.value("state", ""), "error");
    EXPECT_NE(broken.value("detail", "").find("损坏"), std::string::npos);
    EXPECT_THROW(host.platform("telegram").set_enabled(true), std::runtime_error);
    EXPECT_EQ(platform_of(host, "qq").value("state", ""), "unconfigured");
    host.stop();
}

// 场景:平台还没有机主,陌生人私聊;机主在设置页批准。
// 期望:推送 channels_request 并在快照里列出请求;批准后对方进入授权名单并成为机主。
TEST_F(ChannelHostTest, PendingRequestsAreBroadcastAndApproved) {
    ChannelHost host(root, services());
    host.start();
    host.platform("telegram").set_credentials({{"token", "100:abcdefgh1234"}});
    host.platform("telegram").set_enabled(true);
    latest("telegram")->inject(message(private_address("9"), "你好", "m1"));
    ASSERT_TRUE(wait_until([&] { return !events_of("channels_request").empty(); }));
    const auto request = events_of("channels_request")[0];
    EXPECT_EQ(request.value("kind", ""), "user");
    const auto pending = platform_of(host, "telegram")["pending"];
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].value("label", ""), "Telegram 私聊");
    EXPECT_THROW(host.platform("telegram").approve("no-such-id", true), std::runtime_error);
    host.platform("telegram").approve(request.value("id", ""), true);
    const auto snapshot = platform_of(host, "telegram");
    EXPECT_EQ(text(snapshot, "owner"), "user:9");
    EXPECT_TRUE(snapshot["pending"].empty());
    ASSERT_EQ(snapshot["contacts"].size(), 1u);
    EXPECT_TRUE(snapshot["contacts"][0].value("owner", false));
    host.stop();
}

// 场景:机主撤销一位联系人的授权时,对方的会话正在执行,随后对方又发消息。
// 期望:当前回合被中止;之后的消息只得到配对提示,不再进入会话。
TEST_F(ChannelHostTest, RevokeAbortsAndStopsProcessing) {
    ChannelHost host(root, services());
    host.start();
    connect_telegram(host);
    const auto transport = latest("telegram");
    transport->inject(message(private_address("2"), "申请", "m1"));
    ASSERT_TRUE(wait_until([&] { return !events_of("channels_request").empty(); }));
    host.platform("telegram").approve(events_of("channels_request")[0].value("id", ""), true);
    EXPECT_EQ(text(platform_of(host, "telegram"), "owner"), "user:1");  // 已有机主,不会被替换
    transport->inject(message(private_address("2"), "开始干活", "m2"));
    ASSERT_TRUE(wait_until([&] { return sessions.input_log().size() == 1; }));
    const auto id = sessions.input_log()[0].first;
    host.platform("telegram").revoke("user:2");
    EXPECT_EQ(sessions.abort_log(), std::vector<std::string>{id});
    transport->inject(message(private_address("2"), "还能用吗", "m3"));
    EXPECT_TRUE(wait_until([&] {
        std::size_t notices = 0;
        for (const auto& text : transport->texts())
            if (text.find("还没有被授权") != std::string::npos) ++notices;
        return notices == 2;
    }));
    EXPECT_EQ(sessions.input_log().size(), 1u);
    host.stop();
}

// 场景:Telegram 未连接时生成机主链接;连接后生成链接,由某人点开发送 /start <码>。
// 期望:未连接时报错;链接指向机器人用户名并带 24 位一次性码;对方成为机主并收到确认;
// 已有机主后不能再生成链接。
TEST_F(ChannelHostTest, TelegramOwnerLinkClaimsOwner) {
    ChannelHost host(root, services());
    host.start();
    EXPECT_THROW(host.owner_link("telegram"), std::runtime_error);
    host.platform("telegram").set_credentials({{"token", "100:abcdefgh1234"}});
    host.platform("telegram").set_enabled(true);
    const auto link = host.owner_link("telegram");
    const std::string prefix = "https://t.me/ace_bot?start=";
    const auto url = link.value("link", "");
    ASSERT_EQ(url.rfind(prefix, 0), 0u) << url;
    const auto code = url.substr(prefix.size());
    EXPECT_EQ(code.size(), 24u);
    EXPECT_EQ(link.value("expires_in_s", 0), 600);

    auto start = message(private_address("5"), "/start " + code, "m1");
    start.start_code = code;
    latest("telegram")->inject(start);
    ASSERT_TRUE(wait_until([&] { return text(platform_of(host, "telegram"), "owner") == "user:5"; }));
    EXPECT_TRUE(latest("telegram")->wait_said("已绑定为机主"));
    EXPECT_TRUE(sessions.input_log().empty());  // /start 本身不进会话
    EXPECT_THROW(host.owner_link("telegram"), std::runtime_error);
    host.stop();
}

// 场景:页面确认移除 Telegram webhook。
// 期望:操作转交给当前传输层;未连接时报错。
TEST_F(ChannelHostTest, PlatformActionsReachTheTransport) {
    ChannelHost host(root, services());
    host.start();
    EXPECT_THROW(host.platform("telegram").action("remove_webhook", nlohmann::json::object()), std::runtime_error);
    connect_telegram(host);
    host.platform("telegram").action("remove_webhook", nlohmann::json::object());
    EXPECT_EQ(latest("telegram")->actions, std::vector<std::string>{"remove_webhook"});
    EXPECT_THROW(host.platform("wechat"), std::invalid_argument);
    host.stop();
}

// 场景:用户点击“扫码连接”,用手机 QQ 扫码确认,绑定服务返回扫码人身份。
// 期望:页面先收到二维码,完成后保存 AppID/AppSecret、扫码人成为机主、QQ 开关自动打开并连接;
// 快照与推送里都不含 AppSecret。
TEST_F(ChannelHostTest, QqScanBindSavesCredentialsAndConnects) {
    int polls = 0;
    qq.poll_bind_handler = [&](const nlohmann::json&) {
        if (++polls < 2) return nlohmann::json{{"retcode", 0}, {"data", {{"status", 1}}}};
        return nlohmann::json{{"retcode", 0},
                              {"data", {{"status", 2}, {"bot_appid", 102030405},
                                        {"bot_encrypt_secret", kSecretB64}, {"user_openid", "OWNER1"}}}};
    };
    ChannelHost host(root, services());
    host.start();
    EXPECT_EQ(host.start_bind("qq").value("phase", ""), "starting");
    ASSERT_TRUE(wait_until([&] { return host.bind_state("qq").value("phase", "") == "completed"; }));
    const auto snapshot = platform_of(host, "qq");
    EXPECT_TRUE(snapshot.value("enabled", false));
    EXPECT_EQ(snapshot.value("app_id", ""), "102030405");
    EXPECT_EQ(text(snapshot, "owner"), "user:OWNER1");
    EXPECT_EQ(snapshot.value("state", ""), "connected");
    bool saw_qr = false;
    for (const auto& event : events_of("channels_bind")) {
        EXPECT_EQ(event.dump().find("qq-test-secret"), std::string::npos);
        if (event.value("phase", "") == "waiting")
            saw_qr = event.value("qr_url", "").find("source=acecode") != std::string::npos;
    }
    EXPECT_TRUE(saw_qr);
    EXPECT_EQ(host.snapshot().dump().find("qq-test-secret"), std::string::npos);
    host.stop();
}

// 场景:扫码流程进行中用户点了取消;以及流程进行中重复发起。
// 期望:重复发起被拒绝;取消后状态为 cancelled,QQ 配置与开关保持原样。
TEST_F(ChannelHostTest, QqScanCancelKeepsExistingConfig) {
    qq.poll_bind_handler = [](const nlohmann::json&) { return nlohmann::json{{"retcode", 0}, {"data", {{"status", 1}}}}; };
    ChannelHost host(root, services());
    host.start();
    host.start_bind("qq");
    ASSERT_TRUE(wait_until([&] { return host.bind_state("qq").value("phase", "") == "waiting"; }));
    EXPECT_THROW(host.start_bind("qq"), std::runtime_error);
    host.cancel_bind("qq");
    ASSERT_TRUE(wait_until([&] { return host.bind_state("qq").value("phase", "") == "cancelled"; }));
    const auto snapshot = platform_of(host, "qq");
    EXPECT_FALSE(snapshot.value("configured", true));
    EXPECT_FALSE(snapshot.value("enabled", true));
    host.stop();
}

// 场景:QQ 换成另一个机器人(AppID 变化);以及同一机器人重新保存凭据。
// 期望:换机器人时清空机主与授权名单(QQ 的用户 openid 按机器人隔离);同一机器人保持不变。
TEST_F(ChannelHostTest, ChangingQqBotClearsPerBotIdentities) {
    ChannelHost host(root, services());
    host.start();
    auto& qq_runtime = host.platform("qq");
    qq_runtime.set_credentials({{"app_id", "A1"}, {"app_secret", "secret-1"}});
    qq_runtime.set_owner("user:OWNER1", "");
    qq_runtime.set_credentials({{"app_id", "A1"}, {"app_secret", "secret-2"}});
    EXPECT_EQ(text(platform_of(host, "qq"), "owner"), "user:OWNER1");
    qq_runtime.set_credentials({{"app_id", "A2"}, {"app_secret", "secret-3"}});
    const auto snapshot = platform_of(host, "qq");
    EXPECT_TRUE(snapshot["owner"].is_null());
    EXPECT_TRUE(snapshot["contacts"].empty());
    host.stop();
}

// 场景:机主在 QQ 里 /resume 一个当前绑定在 Telegram 私聊上的会话。
// 期望:绑定转到 QQ;Telegram 私聊收到转移提示;已绑定会话的归属变为 qq。
TEST_F(ChannelHostTest, ResumeTransfersBindingAcrossPlatforms) {
    ChannelHost host(root, services());
    host.start();
    connect_telegram(host);
    host.platform("qq").set_credentials({{"app_id", "A1"}, {"app_secret", "secret-1"}});
    host.platform("qq").set_owner("user:Q1", "机主");
    host.platform("qq").set_enabled(true);
    latest("telegram")->inject(message(private_address("1"), "你好", "m1"));
    ASSERT_TRUE(wait_until([&] { return sessions.input_log().size() == 1; }));
    const auto id = sessions.input_log()[0].first;
    EXPECT_EQ(host.bound_sessions().at(id), "telegram");
    latest("qq")->inject(message(private_address("Q1", "qq", "A1"), "/resume " + id, "q1"));
    EXPECT_TRUE(latest("telegram")->wait_said("当前会话已转到QQ 私聊"));
    EXPECT_TRUE(wait_until([&] {
        const auto bound = host.bound_sessions();
        return bound.count(id) && bound.at(id) == "qq";
    }));
    EXPECT_TRUE(latest("qq")->wait_said("已切换到会话"));
    host.stop();
}

} // namespace
} // namespace acecode::channels::core
