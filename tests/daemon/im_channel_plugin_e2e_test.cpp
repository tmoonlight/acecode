#include <gtest/gtest.h>

#include "channels/core/group_chat_tool.hpp"
#include "channels/core/platforms.hpp"
#include "config/config.hpp"
#include "daemon/im_channels.hpp"
#include "daemon/rc_session_catalog.hpp"
#include "permissions/permissions.hpp"
#include "session_host/local_session_client.hpp"
#include "session_host/session_registry.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "tool/tool_executor.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

// 外部平台插件端到端(openspec add-channel-platform-plugins):daemon 的生产装配
// (register_im_channel_plugins + make_im_channel_host)+ 真实 SessionRegistry / AgentLoop(脚本化模型)
// + 真实插件子进程(tests/channels/helpers/fake_channel_plugin)。覆盖:连接器目录里的清单被注册、
// 自己发给自己的消息往返、群消息到点汇总进代答会话、代答会话只能经 send_to_group_chat 发群
// (带前缀)、AI 的普通回复不进群、插件报告需要登录时卡片能触发登录。

namespace acecode::daemon {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using std::chrono::milliseconds;

const char* home_key() {
#ifdef _WIN32
    return "USERPROFILE";
#else
    return "HOME";
#endif
}

void set_env(const char* key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    if (value.empty()) unsetenv(key);
    else setenv(key, value.c_str(), 1);
#endif
}

bool wait_until(const std::function<bool()>& ready, milliseconds timeout = std::chrono::seconds(15)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) return true;
        std::this_thread::sleep_for(milliseconds(20));
    }
    return ready();
}

class ImChannelPluginEndToEnd : public testing::Test {
protected:
    void SetUp() override {
        home = fs::temp_directory_path() / ("acecode_plugin_e2e_" + std::to_string(std::random_device{}()));
        fs::create_directories(home);
        if (const char* previous = std::getenv(home_key())) {
            previous_home = previous;
            had_home = true;
        }
        set_env(home_key(), path_to_utf8(home));
        previous_mode = override_run_mode_for_test(RunMode::User);
        reset_data_dir_cache_for_test();

        SessionRegistryDeps deps;
        deps.provider_accessor = [p = provider] { return std::shared_ptr<LlmProvider>(p); };
        deps.tools = &tools;
        deps.cwd = path_to_utf8(home);
        deps.template_permissions = &permissions;
        deps.no_workspace_cache_root = path_to_utf8(home / "no-workspace");
        deps.auto_title_generator = [](const std::string&) { return std::optional<std::string>{"title"}; };
        registry = std::make_unique<SessionRegistry>(std::move(deps));
        client = std::make_unique<LocalSessionClient>(*registry);
        tools.register_tool(channels::core::create_send_to_group_chat_tool(
            [this](const std::string& id, const std::string& text) { return host->send_to_group(id, text); }));
    }

    void TearDown() override {
        if (host) host->stop();
        host.reset();
        client.reset();
        if (registry) registry->shutdown_all();
        registry.reset();
        channels::core::register_plugin_platforms({});
        set_env(home_key(), had_home ? previous_home : std::string{});
        override_run_mode_for_test(previous_mode);
        reset_data_dir_cache_for_test();
        std::error_code ec;
        fs::remove_all(home, ec);
    }

    // 在 <data_dir>/connectors/example 放一份清单,指向假插件。
    void install_plugin(std::vector<std::string> extra_args = {}) {
        const auto dir = im_channel_plugins_root() / "example";
        fs::create_directories(dir);
        log = dir / "frames.log";
        std::vector<std::string> args{"--log", "${manifest_dir}/frames.log", "--account", "me", "--name", "Zhang San"};
        for (auto& arg : extra_args) args.push_back(std::move(arg));
        const json manifest{{"schema", "acecode.channel-platform.v1"},
                            {"name", "example"},
                            {"label", "Example IM"},
                            {"command", ACECODE_FAKE_CHANNEL_PLUGIN_PATH},
                            {"args", args},
                            {"account", "self"},
                            {"features", {"watched_groups"}}};
        std::ofstream(dir / "channel-platform.json", std::ios::binary) << manifest.dump();
        const auto names = register_im_channel_plugins(im_channel_plugins_root());
        ASSERT_EQ(names, std::vector<std::string>{"example"});
    }

    void start_host() {
        ImChannelDeps deps;
        deps.registry = registry.get();
        deps.client = client.get();
        deps.config = &config;
        deps.config_mu = &config_mu;
        const auto projects = path_to_utf8(path_from_utf8(get_acecode_dir()) / "projects");
        deps.catalog = [this, projects](const std::optional<std::string>& query) {
            return build_rc_session_catalog(projects, *client, query);
        };
        deps.broadcast = [](const std::string&, const json&) {};
        deps.root = home / "channels";
        deps.tune_services = [this](channels::core::HostServices& services) {
            services.standby_retry = milliseconds(50);
            services.conversation.clock = [this] {
                return std::chrono::steady_clock::now() + std::chrono::seconds(clock_offset_s.load());
            };
            services.tune_plugin = [](im::plugin::PluginTransportOptions& options) {
                options.initial_backoff = milliseconds(50);
            };
        };
        host = make_im_channel_host(std::move(deps));
        host->start();
    }

    json platform() {
        const auto snapshot = host->snapshot();
        for (const auto& item : snapshot["platforms"])
            if (item.value("platform", "") == "example") return item;
        return json::object();
    }
    std::vector<json> sends() {
        std::vector<json> out;
        std::ifstream in(log, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            const auto frame = json::parse(line, nullptr, false);
            if (frame.is_object() && frame.value("method", "") == "send_text") out.push_back(frame["params"]);
        }
        return out;
    }
    bool sent(const std::string& kind, const std::string& chat, const std::string& text) {
        for (const auto& params : sends()) {
            const auto address = params.value("address", json::object());
            if (address.value("kind", "") == kind && address.value("chat", "") == chat &&
                params.value("text", "") == text)
                return true;
        }
        return false;
    }
    std::string frames() {
        std::ifstream in(log, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    bool model_saw(const std::string& needle) {
        for (int i = 0; i < provider->turn_count(); ++i)
            for (const auto& message : provider->messages_for_turn(i))
                if (message.role == "user" && message.content.find(needle) != std::string::npos) return true;
        return false;
    }

    fs::path home;
    fs::path log;
    ToolExecutor tools;
    PermissionManager permissions;
    AppConfig config;
    std::shared_mutex config_mu;
    std::shared_ptr<acecode_test::StubLlmProvider> provider = std::make_shared<acecode_test::StubLlmProvider>();
    std::unique_ptr<SessionRegistry> registry;
    std::unique_ptr<LocalSessionClient> client;
    std::unique_ptr<channels::core::ChannelHost> host;
    std::atomic<int> clock_offset_s{0};
    std::string previous_home;
    bool had_home = false;
    RunMode previous_mode = RunMode::User;
};

// 场景:连接器目录装好插件清单,在卡片里设置一个关注的群(前缀 [AI] )后连接;插件随即推来
// 一条自己发给自己的消息和一条群消息;时间推进过轮询间隔。
// 期望:自聊消息进会话、回复经插件发回自聊;群消息到点汇总进单独的代答会话,该会话的工具表
// 只有白名单工具、系统提示带 <group-delegate>;模型调用 send_to_group_chat 后插件收到带前缀的群消息,
// 模型的普通回复不进群。
TEST_F(ImChannelPluginEndToEnd, SelfChatAndGroupDelegateRoundTrip) {
    install_plugin();
    start_host();
    ASSERT_TRUE(platform().value("plugin", false));
    ASSERT_EQ(platform().value("label", ""), "Example IM");
    auto& runtime = host->platform("example");
    runtime.set_settings({{"groups", "g1"}, {"poll_interval_s", 30}, {"prefix", "[AI] "}, {"prompt", "只回答排期问题"}});

    provider->push_text("你好,我在");                               // 自聊回复
    provider->push_tool_call("send_to_group_chat", R"({"text":"收到"})");  // 代答:发群
    provider->push_text("已经在群里回复了");                           // 代答会话的正文,不进群
    runtime.set_enabled(true);

    ASSERT_TRUE(wait_until([&] { return platform().value("state", "") == "connected"; })) << platform().dump() << frames();
    EXPECT_EQ(platform().value("display_name", ""), "Zhang San");
    // 用户在卡片上点了连接:插件收到的 initialize 带 reason=user。
    EXPECT_NE(frames().find(R"("reason":"user")"), std::string::npos) << frames();
    ASSERT_TRUE(wait_until([&] { return model_saw("hello"); }));
    ASSERT_TRUE(wait_until([&] { return sent("private", "me", "你好,我在"); }));

    clock_offset_s = 31;
    ASSERT_TRUE(wait_until([&] { return model_saw("[群聊新消息] 群「测试群」"); }));
    ASSERT_TRUE(wait_until([&] { return sent("group", "g1", "[AI] 收到"); }));
    ASSERT_TRUE(wait_until([&] { return provider->turn_count() >= 3; }));
    std::this_thread::sleep_for(milliseconds(200));
    for (const auto& params : sends()) EXPECT_EQ(params.value("text", "").find("已经在群里回复了"), std::string::npos);

    // 代答会话(第 2 次模型请求)的工具表与系统提示。
    bool delegate_turn_checked = false;
    for (int i = 0; i < provider->turn_count(); ++i) {
        const auto messages = provider->messages_for_turn(i);
        if (messages.empty() || messages[0].content.find("<group-delegate>") == std::string::npos) continue;
        std::vector<std::string> names;
        for (const auto& tool : provider->tools_for_turn(i)) names.push_back(tool.name);
        EXPECT_NE(std::find(names.begin(), names.end(), "send_to_group_chat"), names.end());
        EXPECT_NE(messages[0].content.find("只回答排期问题"), std::string::npos);
        delegate_turn_checked = true;
        break;
    }
    EXPECT_TRUE(delegate_turn_checked);
    // 自聊会话看不到发群工具。
    std::vector<std::string> first;
    for (const auto& tool : provider->tools_for_turn(0)) first.push_back(tool.name);
    EXPECT_EQ(std::find(first.begin(), first.end(), "send_to_group_chat"), first.end());

    bool delegate_binding = false;
    const auto snapshot = platform();  // 先存下来:range-for 不延长临时对象子对象的生命周期
    for (const auto& binding : snapshot["bindings"])
        if (binding.value("delegate", false) && binding.value("chat", "") == "g1") delegate_binding = true;
    EXPECT_TRUE(delegate_binding);
}

// 场景:插件一启动就报告需要登录(账号在别处登录被踢下线后重启的情形),用户点了卡片上的「登录」。
// 期望:快照 login_required 为真、不算已连接;login 操作转给插件后变为已连接。
TEST_F(ImChannelPluginEndToEnd, LoginRequiredIsSurfacedAndLoginReconnects) {
    install_plugin({"--login-required", "--no-inbound"});
    start_host();
    host->platform("example").set_enabled(true);
    ASSERT_TRUE(wait_until([&] { return platform().value("login_required", false); }));
    EXPECT_EQ(platform().value("state", ""), "failed");
    host->platform("example").action("login", json::object());
    ASSERT_TRUE(wait_until([&] { return platform().value("state", "") == "connected"; })) << platform().dump() << frames();
    EXPECT_FALSE(platform().value("login_required", true));
}

} // namespace
} // namespace acecode::daemon
