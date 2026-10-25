#include <gtest/gtest.h>

#include "im/plugin/plugin_transport.hpp"
#include "utils/utf8_path.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

// 外部平台插件传输层(openspec add-channel-platform-plugins D3):用 tests/channels/helpers 里的
// fake_channel_plugin 走真实的子进程与 stdio 管道,验证 initialize、状态与入站映射、请求 / 响应、
// 单向通知、需要登录、崩溃后退避重启、协议版本不兼容与正常停止。

namespace acecode::im::plugin {
namespace {

bool wait_until(const std::function<bool()>& ready, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return ready();
}

std::filesystem::path scratch_dir() {
    std::random_device rd;
    const auto dir = std::filesystem::temp_directory_path() / ("acecode-plugin-transport-" + std::to_string(rd()));
    std::filesystem::create_directories(dir);
    return dir;
}

std::vector<nlohmann::json> read_log(const std::filesystem::path& path) {
    std::vector<nlohmann::json> frames;
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        const auto frame = nlohmann::json::parse(line, nullptr, false);
        if (frame.is_object()) frames.push_back(frame);
    }
    return frames;
}

bool logged(const std::filesystem::path& path, const std::string& method) {
    for (const auto& frame : read_log(path))
        if (frame.value("method", std::string{}) == method) return true;
    return false;
}

class PluginTransportTest : public ::testing::Test {
protected:
    void SetUp() override { dir = scratch_dir(); }
    void TearDown() override {
        if (transport) transport->stop();
        transport.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void start(std::vector<std::string> extra_args, nlohmann::json settings = nlohmann::json::object()) {
        PluginTransportOptions options;
        options.platform = "example";
        options.command = ACECODE_FAKE_CHANNEL_PLUGIN_PATH;
        options.args = {"--log", path_to_utf8(dir / "frames.log"), "--counter", path_to_utf8(dir / "starts.log")};
        for (auto& arg : extra_args) options.args.push_back(std::move(arg));
        options.cwd = path_to_utf8(dir);
        options.settings = std::move(settings);
        options.initial_backoff = std::chrono::milliseconds(50);
        options.max_backoff = std::chrono::milliseconds(200);
        options.initialize_timeout = std::chrono::seconds(5);
        transport = std::make_unique<PluginTransport>(std::move(options));
        TransportCallbacks callbacks;
        callbacks.on_inbound = [this](Inbound inbound) {
            std::lock_guard<std::mutex> lock(mu);
            inbound_log.push_back(std::move(inbound));
        };
        callbacks.on_status = [this](const TransportStatus& status) {
            std::lock_guard<std::mutex> lock(mu);
            states.push_back(status.state);
        };
        transport->start(std::move(callbacks));
    }

    std::size_t inbound_count() {
        std::lock_guard<std::mutex> lock(mu);
        return inbound_log.size();
    }
    int start_count() {
        std::ifstream in(dir / "starts.log", std::ios::binary);
        int lines = 0;
        std::string line;
        while (std::getline(in, line)) ++lines;
        return lines;
    }

    std::filesystem::path dir;
    std::unique_ptr<PluginTransport> transport;
    std::mutex mu;
    std::vector<Inbound> inbound_log;
    std::vector<LinkState> states;
};

// 场景:插件正常启动,报告已连接,随后推一条自聊消息和一条关注群的消息。
// 期望:状态带上账号与显示名、能力取自 initialize 结果;入站的 platform / account 由 ACECode 补齐
// (插件伪造的 platform 字段被忽略),群消息带群名、点名标记与发送时间。
TEST_F(PluginTransportTest, InitializeReportsStatusCapabilitiesAndInbound) {
    start({}, {{"watched_groups", {"g1"}}});
    ASSERT_TRUE(wait_until([this] { return transport->status().state == LinkState::Connected; }));
    const auto status = transport->status();
    EXPECT_EQ(status.account, "me");
    EXPECT_EQ(status.display_name, "测试用户");
    EXPECT_EQ(transport->capabilities().max_text_units, 3000u);
    EXPECT_TRUE(transport->capabilities().supports_typing);

    ASSERT_TRUE(wait_until([this] { return inbound_count() >= 2; }));
    std::lock_guard<std::mutex> lock(mu);
    const auto& self = inbound_log[0];
    EXPECT_EQ(self.address.platform, "example");
    EXPECT_EQ(self.address.account, "me");
    EXPECT_EQ(self.address.kind, ChatKind::Private);
    EXPECT_EQ(self.text, "hello");
    EXPECT_EQ(self.timestamp_ms, 1761300000000LL);
    const auto& group = inbound_log[1];
    EXPECT_EQ(group.address.kind, ChatKind::Group);
    EXPECT_EQ(group.address.chat, "g1");
    EXPECT_EQ(group.chat_name, "测试群");
    EXPECT_TRUE(group.mentioned);
    EXPECT_EQ(group.sender_name, "Bob");

    const auto initialize = read_log(dir / "frames.log").front();
    EXPECT_EQ(initialize.value("method", ""), "initialize");
    EXPECT_EQ(initialize["params"].value("protocol_version", 0), kProtocolVersion);
    EXPECT_EQ(initialize["params"]["settings"]["watched_groups"][0], "g1");
}

// 场景:发文字成功 / 被平台拒绝,发文件(插件不支持),设置“正在输入”,配置与自定义操作。
// 期望:结果按 outcome 映射;set_typing 是不带 id 的单向通知;configure 带上新设置。
TEST_F(PluginTransportTest, RequestsMapResultsAndTypingIsANotification) {
    start({"--no-inbound"});
    ASSERT_TRUE(wait_until([this] { return transport->status().state == LinkState::Connected; }));
    Address to;
    to.platform = "example";
    to.account = "me";
    to.chat = "me";
    to.sender = "me";
    EXPECT_EQ(transport->send_text(to, "你好", nlohmann::json::object()).outcome, SendOutcome::Sent);
    const auto rejected = transport->send_text(to, "reject", nlohmann::json::object());
    EXPECT_EQ(rejected.outcome, SendOutcome::Failed);
    EXPECT_EQ(rejected.error, "rejected by platform");
    EXPECT_FALSE(transport->send_file(to, dir / "a.txt", "a.txt", "text/plain", nlohmann::json::object()).ok());

    transport->set_typing(to, true);
    ASSERT_TRUE(wait_until([this] { return logged(dir / "frames.log", "set_typing"); }));
    for (const auto& frame : read_log(dir / "frames.log"))
        if (frame.value("method", std::string{}) == "set_typing") EXPECT_FALSE(frame.contains("id"));

    transport->action("configure", {{"watched_groups", {"g2"}}});
    EXPECT_EQ(transport->action("login", nlohmann::json::object()).value("name", ""), "login");
}

// 场景:插件启动后报告需要登录(例如被别处登录踢下线),用户点了登录。
// 期望:状态是 Failed + retry_stopped,extra.login_required 为真;login 操作之后变回已连接。
TEST_F(PluginTransportTest, LoginRequiredUntilTheLoginAction) {
    start({"--login-required", "--no-inbound"});
    ASSERT_TRUE(wait_until([this] { return transport->status().state == LinkState::Failed; }));
    auto status = transport->status();
    EXPECT_TRUE(status.retry_stopped);
    EXPECT_TRUE(status.extra.value("login_required", false));
    transport->action("login", nlohmann::json::object());
    ASSERT_TRUE(wait_until([this] { return transport->status().state == LinkState::Connected; }));
    EXPECT_FALSE(transport->status().extra.value("login_required", false));
}

// 场景:插件前两次启动都在回复 initialize 后崩溃退出。
// 期望:传输层报告 retrying 并按退避重启子进程,第三次起正常连接。
TEST_F(PluginTransportTest, CrashedPluginIsRestartedWithBackoff) {
    start({"--crash-times", "2", "--no-inbound"});
    ASSERT_TRUE(wait_until([this] { return transport->status().state == LinkState::Connected; }));
    EXPECT_EQ(start_count(), 3);
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_NE(std::find(states.begin(), states.end(), LinkState::Retrying), states.end());
}

// 场景:插件报告了不兼容的协议版本。
// 期望:不进入已连接,状态 retrying 且原因里写明版本不兼容;发消息直接失败而不是挂住。
TEST_F(PluginTransportTest, IncompatibleProtocolVersionNeverConnects) {
    start({"--version", "2", "--no-inbound"});
    ASSERT_TRUE(wait_until([this] { return transport->status().state == LinkState::Retrying; }));
    EXPECT_NE(transport->status().detail.find("版本"), std::string::npos) << transport->status().detail;
    Address to;
    to.platform = "example";
    to.account = "me";
    to.chat = "me";
    to.sender = "me";
    EXPECT_FALSE(transport->send_text(to, "hi", nlohmann::json::object()).ok());
}

// 场景:正常连接后停止。
// 期望:先发 shutdown,stop 在超时之内返回,之后状态为 Stopped。
TEST_F(PluginTransportTest, StopSendsShutdownAndReturnsPromptly) {
    start({"--no-inbound"});
    ASSERT_TRUE(wait_until([this] { return transport->status().state == LinkState::Connected; }));
    const auto began = std::chrono::steady_clock::now();
    transport->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::seconds(5));
    EXPECT_EQ(transport->status().state, LinkState::Stopped);
    EXPECT_TRUE(logged(dir / "frames.log", "shutdown"));
}

// 场景:插件给的入站地址不合法(私聊 chat 与 sender 不同、缺 message_id)。
// 期望:解析拒绝,不交给核心。
TEST(PluginInboundParse, RejectsInvalidAddresses) {
    Inbound out;
    EXPECT_FALSE(parse_plugin_inbound({{"address", {{"kind", "private"}, {"chat", "a"}, {"sender", "b"}}},
                                       {"message_id", "1"}},
                                      "example", "me", out));
    EXPECT_FALSE(parse_plugin_inbound({{"address", {{"kind", "group"}, {"chat", "g"}, {"sender", "b"}}}}, "example",
                                      "me", out));
    EXPECT_TRUE(parse_plugin_inbound({{"address", {{"kind", "group"}, {"chat", "g"}, {"sender", "b"}}},
                                      {"message_id", "1"},
                                      {"from_self", true}},
                                     "example", "me", out));
    EXPECT_TRUE(out.from_self);
    EXPECT_FALSE(out.mentioned);
}

} // namespace
} // namespace acecode::im::plugin
