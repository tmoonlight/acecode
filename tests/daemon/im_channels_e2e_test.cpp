#include <gtest/gtest.h>

#include "channels/core/line_webhook_server.hpp"
#include "config/config.hpp"
#include "daemon/im_channels.hpp"
#include "daemon/rc_session_catalog.hpp"
#include "permissions/permissions.hpp"
#include "session_host/local_session_client.hpp"
#include "session/session_storage.hpp"
#include "session_host/session_registry.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "test_support/im/fake_dingtalk_server.hpp"
#include "test_support/im/fake_discord_server.hpp"
#include "test_support/im/fake_feishu_server.hpp"
#include "test_support/im/fake_line_server.hpp"
#include "test_support/im/fake_qq_server.hpp"
#include "test_support/im/fake_telegram_server.hpp"
#include "test_support/im/fake_weixin_server.hpp"
#include "tool/tool_executor.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

// 消息通道端到端:daemon 的生产装配(make_im_channel_host)+ 真实 SessionRegistry / AgentLoop
// (脚本化模型)+ 真实各平台传输层,平台换成本机假服务。覆盖真实账号验收清单里
// 不依赖真实账号的部分:凭据校验、机主绑定、私聊与群 @ 的完整往返、审批、切到工作区会话、
// /new、非机主受限、开关即时性、图片入站、QQ 扫码、语音识别文字与超出回复窗口后的补发;
// 微信扫码登录,飞书、钉钉、Discord、LINE 的 6 位绑定码与私聊往返。
// 这五个平台的假服务只在用到的用例里创建(各自要起一个本机端口)。

namespace acecode::daemon {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using std::chrono::milliseconds;

constexpr const char* kSecretB64 = "ZGVmZ2hpamtsbW5vOWrzEhyaIrNNBzyavxFHGfpv4JwMQcJAlM16v321wbiQ0njyP4fhqnk=";

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

bool wait_until(const std::function<bool()>& ready, milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) return true;
        std::this_thread::sleep_for(milliseconds(20));
    }
    return ready();
}

std::string fixed_key() {
    std::string key;
    for (int i = 0; i < 32; ++i) key.push_back(static_cast<char>(i));
    return key;
}

json telegram_message(std::int64_t chat, const std::string& chat_type, std::int64_t from, std::int64_t id,
                      const std::string& text) {
    return {{"message", {{"message_id", id},
                         {"from", {{"id", from}, {"is_bot", false}, {"first_name", "U" + std::to_string(from)}}},
                         {"chat", {{"id", chat}, {"type", chat_type}}},
                         {"text", text}}}};
}

class ImChannelsEndToEnd : public testing::Test {
protected:
    void SetUp() override {
        home = fs::temp_directory_path() / ("acecode_im_e2e_" + std::to_string(std::random_device{}()));
        fs::create_directories(home / "ws");
        workspace = path_to_utf8(home / "ws");
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
        deps.cwd = workspace;
        deps.template_permissions = &permissions;
        deps.no_workspace_cache_root = path_to_utf8(home / "no-workspace");
        deps.auto_title_generator = [](const std::string&) { return std::optional<std::string>{"title"}; };
        registry = std::make_unique<SessionRegistry>(std::move(deps));
        client = std::make_unique<LocalSessionClient>(*registry);
    }

    void TearDown() override {
        if (host) host->stop();
        host.reset();
        client.reset();
        if (registry) registry->shutdown_all();
        registry.reset();
        set_env(home_key(), had_home ? previous_home : std::string{});
        override_run_mode_for_test(previous_mode);
        reset_data_dir_cache_for_test();
        std::error_code ec;
        fs::remove_all(home, ec);
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
        deps.broadcast = [this](const std::string& type, const json& payload) {
            std::lock_guard<std::mutex> lock(events_mu);
            events.emplace_back(type, payload);
        };
        deps.endpoints.telegram_api_base = telegram.base();
        deps.endpoints.qq_api_base = qq.base();
        deps.endpoints.qq_token_url = qq.base() + "/app/getAppAccessToken";
        if (weixin) {
            deps.endpoints.weixin_api_base = weixin->base();
            deps.endpoints.weixin_cdn_base = weixin->cdn_base();
            deps.weixin_login.poll_interval = milliseconds(20);
            deps.weixin_login.total_timeout = std::chrono::seconds(5);
            deps.weixin_login.redirect_scheme = "http";
        }
        if (dingtalk) {
            deps.endpoints.dingtalk_api_base = dingtalk->base();
            deps.endpoints.dingtalk_oapi_base = dingtalk->base();
        }
        if (discord) deps.endpoints.discord_api_base = discord->api_base();
        if (feishu) {
            deps.endpoints.feishu_base = feishu->base();
            deps.tune_services = [](channels::core::HostServices& services) {
                services.tune_feishu = [](im::feishu::FeishuTransportOptions& options) {
                    options.backoff = {milliseconds(50)};
                    options.receive_poll = milliseconds(50);
                    options.send_gap = milliseconds(1);
                };
            };
        }
        if (line) {
            deps.endpoints.line_api_base = line->base();
            deps.endpoints.line_data_api_base = line->base();
            // 自有公网地址模式:公网地址就是本机回调端口本身(真实环境里要求 https,
            // 这里绕过保存前的校验直接改传输层参数),不启动 cloudflared。
            deps.tune_services = [port = line_port](channels::core::HostServices& services) {
                services.tune_line = [port](im::line::LineTransportOptions& options) {
                    options.listen_port = port;
                    options.public_url = "http://127.0.0.1:" + std::to_string(port);
                    options.backoff = {milliseconds(50)};
                    options.public_check_timeout = std::chrono::seconds(3);
                    options.public_check_interval = milliseconds(50);
                    options.webhook_recheck = milliseconds(100);
                };
            };
        }
        deps.endpoints.use_proxy = false;
        deps.bind.portal_base = qq.base();
        deps.bind.connect_base = "https://q.qq.com";
        deps.bind.use_proxy = false;
        deps.bind.poll_interval = milliseconds(20);
        deps.bind.total_timeout = std::chrono::seconds(5);
        deps.bind.key_provider = fixed_key;
        deps.root = home / "channels";
        host = make_im_channel_host(std::move(deps));
        host->start();
    }

    json platform(const std::string& name) {
        const auto snapshot = host->snapshot();
        for (const auto& item : snapshot["platforms"])
            if (item.value("platform", "") == name) return item;
        return json::object();
    }
    static std::string text(const json& object, const char* key) {
        return object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : std::string{};
    }
    // 某个 IM 会话(chat + sender)当前绑定的会话 id。
    std::string bound(const std::string& name, const std::string& chat, const std::string& sender) {
        const auto snapshot = platform(name);  // 先存下来:range-for 不延长临时对象子对象的生命周期
        for (const auto& binding : snapshot["bindings"])
            if (binding.value("chat", "") == chat && binding.value("sender", "") == sender)
                return binding.value("session_id", "");
        return {};
    }
    std::vector<json> events_of(const std::string& type) {
        std::lock_guard<std::mutex> lock(events_mu);
        std::vector<json> out;
        for (const auto& [name, payload] : events)
            if (name == type) out.push_back(payload);
        return out;
    }
    // Telegram 发到某个聊天的消息里是否有一条包含 needle。
    bool telegram_sent(std::int64_t chat, const std::string& needle) {
        for (const auto& call : telegram.calls_to("sendMessage")) {
            if (call.body.value("chat_id", std::int64_t{0}) == chat &&
                call.body.value("text", std::string{}).find(needle) != std::string::npos)
                return true;
        }
        return false;
    }
    // 用户消息是否出现在模型收到的某次请求里(证明它进入了会话)。
    bool model_saw(const std::string& needle) {
        for (int i = 0; i < provider->turn_count(); ++i)
            for (const auto& message : provider->messages_for_turn(i))
                if (message.role == "user" && message.content.find(needle) != std::string::npos) return true;
        return false;
    }
    void connect_telegram_as_owner() {
        auto& runtime = host->platform("telegram");
        runtime.set_credentials({{"token", telegram.token()}});
        runtime.set_enabled(true);
        ASSERT_TRUE(wait_until([&] { return platform("telegram").value("state", "") == "connected"; }));
        const auto link = host->owner_link("telegram").value("link", std::string{});
        const auto code = link.substr(link.find("start=") + 6);
        telegram.push_update(telegram_message(42, "private", 42, 1, "/start " + code));
        ASSERT_TRUE(wait_until([&] { return text(platform("telegram"), "owner") == "user:42"; }));
    }

    fs::path home;
    std::string workspace;
    ToolExecutor tools;
    PermissionManager permissions;
    AppConfig config;
    std::shared_mutex config_mu;
    std::shared_ptr<acecode_test::StubLlmProvider> provider = std::make_shared<acecode_test::StubLlmProvider>();
    std::unique_ptr<SessionRegistry> registry;
    std::unique_ptr<LocalSessionClient> client;
    acecode::test::FakeTelegramServer telegram;
    acecode::test::FakeQqServer qq;
    std::unique_ptr<acecode::test::FakeWeixinServer> weixin;
    std::unique_ptr<acecode::test::FakeDingTalkServer> dingtalk;
    std::unique_ptr<acecode::test::FakeDiscordServer> discord;
    std::unique_ptr<acecode::test::FakeFeishuServer> feishu;
    std::unique_ptr<acecode::test::FakeLineServer> line;
    std::uint16_t line_port = 0;
    std::unique_ptr<channels::core::ChannelHost> host;
    std::mutex events_mu;
    std::vector<std::pair<std::string, json>> events;
    std::string previous_home;
    bool had_home = false;
    RunMode previous_mode = RunMode::User;
};

// 场景:设置页填错 token 后改成正确的;用机主链接里的 /start 码绑定机主;机主私聊提问。
// 期望:错误 token 不保存;正确 token 经 getMe 校验后连接;/start 发送者成为机主并收到确认;
// 私聊消息新建无项目会话、进入模型,助手答复按 Telegram HTML 发回同一私聊。
TEST_F(ImChannelsEndToEnd, TelegramValidatesTokenBindsOwnerAndChats) {
    start_host();
    EXPECT_THROW(host->platform("telegram").set_credentials({{"token", "999:wrong-token"}}), std::runtime_error);
    EXPECT_FALSE(platform("telegram").value("configured", true));
    connect_telegram_as_owner();
    EXPECT_TRUE(wait_until([&] { return telegram_sent(42, "已绑定为机主"); }));

    provider->push_text("**收到**,我是 ACECode");
    telegram.push_update(telegram_message(42, "private", 42, 2, "你好"));
    ASSERT_TRUE(wait_until([&] { return telegram_sent(42, "<b>收到</b>,我是 ACECode"); }));
    EXPECT_TRUE(model_saw("你好"));
    const auto session = bound("telegram", "42", "42");
    ASSERT_FALSE(session.empty()) << platform("telegram").dump(2);
    EXPECT_EQ(host->bound_sessions().at(session), "telegram");
    for (const auto& info : client->list_sessions())
        if (info.id == session) EXPECT_TRUE(info.no_workspace);
}

// 场景:机器人在一个未批准的群里被 @,机主在设置页批准该群后再次 @ 提问;群里不 @ 的消息。
// 期望:未批准时只产生群的待批准请求、不回复;批准后 @ 的提问进入会话并在群里得到答复;
// 不 @ 的消息被忽略。隐私模式(默认开启)反映在快照里。
TEST_F(ImChannelsEndToEnd, TelegramGroupNeedsApprovalAndMention) {
    start_host();
    connect_telegram_as_owner();
    EXPECT_TRUE(platform("telegram")["extra"].value("privacy_mode", false));
    auto mention = telegram_message(-100, "group", 42, 10, "@AceTestBot 帮我看看");
    mention["message"]["entities"] = json::array({{{"type", "mention"}, {"offset", 0}, {"length", 11}}});
    telegram.push_update(mention);
    ASSERT_TRUE(wait_until([&] { return !events_of("channels_request").empty(); }));
    const auto request = events_of("channels_request").back();
    EXPECT_EQ(request.value("kind", ""), "group");
    host->platform("telegram").approve(request.value("id", ""), true);

    telegram.push_update(telegram_message(-100, "group", 42, 11, "大家好"));  // 不 @,应忽略
    provider->push_text("群里的答复");
    mention["message"]["message_id"] = 12;
    telegram.push_update(mention);
    ASSERT_TRUE(wait_until([&] { return telegram_sent(-100, "群里的答复"); }));
    EXPECT_FALSE(model_saw("大家好"));
    EXPECT_FALSE(bound("telegram", "-100", "42").empty());
}

// 场景:机主先在私聊里开始,然后 /sessions 列出会话、/resume 切到一个工作区会话继续提问,
// 再 /new。
// 期望:切换后提问进入工作区会话并收到答复;/new 在同一工作区新建会话并改绑。
TEST_F(ImChannelsEndToEnd, TelegramOwnerSwitchesIntoWorkspaceSessionAndNew) {
    start_host();
    connect_telegram_as_owner();
    SessionOptions options;
    options.cwd = workspace;
    const auto workspace_session = client->create_session(options);
    ASSERT_FALSE(workspace_session.empty());

    telegram.push_update(telegram_message(42, "private", 42, 2, "/sessions"));
    ASSERT_TRUE(wait_until([&] { return telegram_sent(42, "可切换的会话"); }));
    telegram.push_update(telegram_message(42, "private", 42, 3, "/resume " + workspace_session));
    ASSERT_TRUE(wait_until([&] { return bound("telegram", "42", "42") == workspace_session; }));
    EXPECT_TRUE(wait_until([&] { return telegram_sent(42, "已切换到会话"); }));

    provider->push_text("工作区里的答复");
    telegram.push_update(telegram_message(42, "private", 42, 4, "看看工作区"));
    ASSERT_TRUE(wait_until([&] { return telegram_sent(42, "工作区里的答复"); }));
    EXPECT_TRUE(model_saw("看看工作区"));

    telegram.push_update(telegram_message(42, "private", 42, 5, "/new"));
    ASSERT_TRUE(wait_until([&] {
        const auto id = bound("telegram", "42", "42");
        return !id.empty() && id != workspace_session;
    }));
    const auto fresh = bound("telegram", "42", "42");
    bool in_workspace = false;
    for (const auto& info : client->list_sessions())
        if (info.id == fresh) in_workspace = !info.no_workspace && path_from_utf8(info.cwd) == path_from_utf8(workspace);
    EXPECT_TRUE(in_workspace);
}

// 场景:机主批准一位普通联系人;该联系人试图 /resume 机主的会话。
// 期望:请求被拒绝且其绑定不变;对方仍可正常和自己的会话对话。
TEST_F(ImChannelsEndToEnd, TelegramNonOwnerCannotResumeOthersSessions) {
    start_host();
    connect_telegram_as_owner();
    provider->push_text("机主的答复");
    telegram.push_update(telegram_message(42, "private", 42, 2, "机主的问题"));
    ASSERT_TRUE(wait_until([&] { return telegram_sent(42, "机主的答复"); }));
    const auto owner_session = bound("telegram", "42", "42");

    telegram.push_update(telegram_message(7, "private", 7, 3, "你好"));
    ASSERT_TRUE(wait_until([&] { return !events_of("channels_request").empty(); }));
    EXPECT_TRUE(wait_until([&] { return telegram_sent(7, "还没有被授权"); }));
    host->platform("telegram").approve(events_of("channels_request").back().value("id", ""), true);
    EXPECT_EQ(text(platform("telegram"), "owner"), "user:42");  // 已有机主,不会被替换

    provider->push_text("给联系人的答复");
    telegram.push_update(telegram_message(7, "private", 7, 4, "我的问题"));
    ASSERT_TRUE(wait_until([&] { return telegram_sent(7, "给联系人的答复"); }));
    const auto contact_session = bound("telegram", "7", "7");
    telegram.push_update(telegram_message(7, "private", 7, 5, "/resume " + owner_session));
    EXPECT_TRUE(wait_until([&] { return telegram_sent(7, "只能切换到你自己"); }));
    EXPECT_EQ(bound("telegram", "7", "7"), contact_session);
    EXPECT_EQ(bound("telegram", "42", "42"), owner_session);
}

// 场景:关闭开关期间机主发来消息,随后重新打开开关。
// 期望:关闭立即停止轮询,期间不处理任何消息;重新打开后从保存的位置继续,补处理那条消息。
TEST_F(ImChannelsEndToEnd, TelegramSwitchTakesEffectImmediately) {
    start_host();
    connect_telegram_as_owner();
    host->platform("telegram").set_enabled(false);
    EXPECT_EQ(platform("telegram").value("state", ""), "disabled");
    telegram.push_update(telegram_message(42, "private", 42, 2, "关着的时候发的"));
    std::this_thread::sleep_for(milliseconds(800));
    EXPECT_FALSE(model_saw("关着的时候发的"));

    provider->push_text("恢复后的答复");
    host->platform("telegram").set_enabled(true);
    ASSERT_TRUE(wait_until([&] { return telegram_sent(42, "恢复后的答复"); }));
    EXPECT_TRUE(model_saw("关着的时候发的"));
}

// 场景:机主发来一张带说明文字的图片。
// 期望:图片经 getFile 下载并导入会话附件,说明文字与图片一起进入会话,答复发回私聊。
TEST_F(ImChannelsEndToEnd, TelegramPhotoBecomesSessionAttachment) {
    start_host();
    connect_telegram_as_owner();
    provider->push_text("看到图片了");
    json photo = {{"message", {{"message_id", 20},
                               {"from", {{"id", 42}, {"is_bot", false}, {"first_name", "Ann"}}},
                               {"chat", {{"id", 42}, {"type", "private"}}},
                               {"caption", "看看这张图"},
                               {"photo", json::array({{{"file_id", "p1"}, {"file_unique_id", "u1"},
                                                       {"width", 90}, {"height", 90}, {"file_size", 32}}})}}}};
    telegram.push_update(photo);
    ASSERT_TRUE(wait_until([&] { return telegram_sent(42, "看到图片了"); }));
    EXPECT_FALSE(telegram.calls_to("download:documents/p1.bin").empty());
    EXPECT_TRUE(model_saw("看看这张图"));
    const auto session = bound("telegram", "42", "42");
    std::string error;
    bool has_attachment = false;
    for (const auto& info : client->list_sessions()) {
        if (info.id != session) continue;
        const auto dir = path_from_utf8(SessionStorage::get_project_dir(info.cwd)) / "attachments" / session;
        has_attachment = fs::exists(dir) && !fs::is_empty(dir);
    }
    EXPECT_TRUE(has_attachment);
}

// 场景:模型要调用一个需要确认的工具。第一次机主在 Telegram 里 /approve;第二次先在 Desktop
// 里批准,之后机主再发 /approve。
// 期望:IM 收到带编号的确认请求;IM 批准后工具执行、答复发回;Desktop 先批准时 IM 收到
// “已处理”提示,随后的 /approve 得到“已处理或不存在”,工具不会因此多执行一次。
TEST_F(ImChannelsEndToEnd, TelegramPermissionApprovalAndFirstAnswerWins) {
    std::atomic<int> deploys{0};
    ToolDef def;
    def.name = "deploy_preview";
    def.description = "deploy a preview build";
    def.parameters = json({{"type", "object"}, {"properties", {{"target", {{"type", "string"}}}}}});
    ASSERT_TRUE(tools.register_tool(ToolImpl{def, [&](const std::string&, const ToolContext&) {
        ++deploys;
        return ToolResult{"deployed", true};
    }, false}));
    start_host();
    connect_telegram_as_owner();
    // 从发到私聊的最后一条确认请求里取出编号。
    auto latest_request_id = [&]() {
        std::string id;
        for (const auto& call : telegram.calls_to("sendMessage")) {
            const auto body = call.body.value("text", std::string{});
            const auto at = body.find("/approve ");
            if (call.body.value("chat_id", std::int64_t{0}) != 42 || at == std::string::npos) continue;
            const auto start = at + 9;
            const auto end = body.find_first_of(" \n<", start);
            id = body.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }
        return id;
    };

    provider->push_tool_call("deploy_preview", json{{"target", "staging"}}.dump(), "call-1");
    provider->push_text("第一次部署完成");
    telegram.push_update(telegram_message(42, "private", 42, 2, "部署一下"));
    ASSERT_TRUE(wait_until([&] { return !latest_request_id().empty(); }));
    const auto first = latest_request_id();
    telegram.push_update(telegram_message(42, "private", 42, 3, "/approve " + first));
    ASSERT_TRUE(wait_until([&] { return telegram_sent(42, "第一次部署完成"); }));
    EXPECT_EQ(deploys.load(), 1);
    EXPECT_TRUE(telegram_sent(42, "已允许权限请求 " + first));

    provider->push_tool_call("deploy_preview", json{{"target", "prod"}}.dump(), "call-2");
    provider->push_text("第二次部署完成");
    telegram.push_update(telegram_message(42, "private", 42, 4, "再部署一次"));
    ASSERT_TRUE(wait_until([&] { const auto id = latest_request_id(); return !id.empty() && id != first; }));
    const auto second = latest_request_id();
    client->respond_permission(bound("telegram", "42", "42"), {second, PermissionDecisionChoice::Allow});
    ASSERT_TRUE(wait_until([&] { return telegram_sent(42, "权限请求 " + second + " 已处理"); }));
    ASSERT_TRUE(wait_until([&] { return telegram_sent(42, "第二次部署完成"); }));
    telegram.push_update(telegram_message(42, "private", 42, 5, "/approve " + second));
    EXPECT_TRUE(wait_until([&] { return telegram_sent(42, "该权限请求已处理或不存在"); }));
    EXPECT_EQ(deploys.load(), 2);
}

// 场景:用手机 QQ 扫码(门户返回扫码人 openid),扫码人私聊提问,再发一段带识别文字的语音。
// 期望:扫码完成后凭据保存、开关自动打开并连上网关,扫码人成为机主;提问进入会话,
// 答复以被动回复(带原消息 msg_id)发回;语音以“[语音转写] …”进入会话。
TEST_F(ImChannelsEndToEnd, QqScanConnectsAndChatsWithVoice) {
    int polls = 0;
    qq.poll_bind_handler = [&](const json&) {
        if (++polls < 2) return json{{"retcode", 0}, {"data", {{"status", 1}}}};
        return json{{"retcode", 0},
                    {"data", {{"status", 2}, {"bot_appid", "102030405"}, {"bot_encrypt_secret", kSecretB64},
                              {"user_openid", "OWNER1"}}}};
    };
    start_host();
    host->start_bind("qq");
    ASSERT_TRUE(wait_until([&] { return host->bind_state("qq").value("phase", "") == "completed"; }));
    ASSERT_TRUE(wait_until([&] { return platform("qq").value("state", "") == "connected"; }));
    EXPECT_EQ(text(platform("qq"), "owner"), "user:OWNER1");
    EXPECT_EQ(text(platform("qq"), "app_id"), "102030405");

    provider->push_text("你好呀,我在");
    qq.push_c2c("M1", "OWNER1", "你好");
    ASSERT_TRUE(wait_until([&] {
        for (const auto& request : qq.requests_to("/v2/users/OWNER1/messages"))
            if (request.body.value("msg_id", "") == "M1" && request.body.dump().find("我在") != std::string::npos)
                return true;
        return false;
    }));
    EXPECT_TRUE(model_saw("你好"));

    provider->push_text("好的,记下了");
    qq.push_c2c("M2", "OWNER1", "", json::array({{{"content_type", "voice"},
                                                   {"url", "https://example.invalid/voice.silk"},
                                                   {"asr_refer_text", "明天上午开会"}}}));
    ASSERT_TRUE(wait_until([&] { return model_saw("[语音转写] 明天上午开会"); }));
}

// 场景:扫码绑定 QQ 后,机器人在一个未批准的群里被成员 @;机主在设置页先批准群,再批准该成员。
// 期望:群与成员分别产生待批准请求(QQ 群成员身份是 member:<群>:<成员>,与机主的私聊身份不同);
// 两者都批准后成员的 @ 提问进入会话,答复以被动回复发回该群。
TEST_F(ImChannelsEndToEnd, QqGroupAndMemberNeedSeparateApproval) {
    qq.poll_bind_handler = [](const json&) {
        return json{{"retcode", 0},
                    {"data", {{"status", 2}, {"bot_appid", "102030405"}, {"bot_encrypt_secret", kSecretB64},
                              {"user_openid", "OWNER1"}}}};
    };
    start_host();
    host->start_bind("qq");
    ASSERT_TRUE(wait_until([&] { return platform("qq").value("state", "") == "connected"; }));

    qq.push_group_at("G1", "G1", "MEMBER1", " 帮我看看日志");
    ASSERT_TRUE(wait_until([&] { return events_of("channels_request").size() == 1; }));
    EXPECT_EQ(events_of("channels_request")[0].value("kind", ""), "group");
    host->platform("qq").approve(events_of("channels_request")[0].value("id", ""), true);

    qq.push_group_at("G2", "G1", "MEMBER1", " 帮我看看日志");
    ASSERT_TRUE(wait_until([&] { return events_of("channels_request").size() == 2; }));
    EXPECT_EQ(events_of("channels_request")[1].value("kind", ""), "member");
    host->platform("qq").approve(events_of("channels_request")[1].value("id", ""), true);
    EXPECT_EQ(text(platform("qq"), "owner"), "user:OWNER1");

    provider->push_text("日志看过了");
    qq.push_group_at("G3", "G1", "MEMBER1", " 帮我看看日志");
    ASSERT_TRUE(wait_until([&] {
        for (const auto& request : qq.requests_to("/v2/groups/G1/messages"))
            if (request.body.value("msg_id", "") == "G3" && request.body.dump().find("日志看过了") != std::string::npos)
                return true;
        return false;
    }));
    EXPECT_FALSE(bound("qq", "G1", "MEMBER1").empty());
}

// 场景:扫码人私聊发来一张图片。
// 期望:图片按附件地址带 QQBot 令牌下载,导入会话附件,并与文字说明一起进入会话。
TEST_F(ImChannelsEndToEnd, QqImageBecomesSessionAttachment) {
    qq.poll_bind_handler = [](const json&) {
        return json{{"retcode", 0},
                    {"data", {{"status", 2}, {"bot_appid", "102030405"}, {"bot_encrypt_secret", kSecretB64},
                              {"user_openid", "OWNER1"}}}};
    };
    start_host();
    host->start_bind("qq");
    ASSERT_TRUE(wait_until([&] { return platform("qq").value("state", "") == "connected"; }));

    provider->push_text("图片收到了");
    qq.push_c2c("M1", "OWNER1", "看看这张截图",
                json::array({{{"content_type", "image/png"}, {"filename", "shot.png"},
                              {"url", qq.base() + "/download/shot"}, {"size", 16}}}));
    ASSERT_TRUE(wait_until([&] {
        for (const auto& request : qq.requests_to("/v2/users/OWNER1/messages"))
            if (request.body.dump().find("图片收到了") != std::string::npos) return true;
        return false;
    }));
    const auto downloads = qq.requests_to("/download/shot");
    ASSERT_EQ(downloads.size(), 1u);
    EXPECT_EQ(downloads[0].authorization.rfind("QQBot ", 0), 0u);
    EXPECT_TRUE(model_saw("看看这张截图"));
    const auto session = bound("qq", "OWNER1", "OWNER1");
    bool has_attachment = false;
    for (const auto& info : client->list_sessions()) {
        if (info.id != session) continue;
        const auto dir = path_from_utf8(SessionStorage::get_project_dir(info.cwd)) / "attachments" / session;
        has_attachment = fs::exists(dir) && !fs::is_empty(dir);
    }
    EXPECT_TRUE(has_attachment);
}

// 场景:QQ 被动与主动回复都被平台拒绝(超出回复窗口且主动消息被关),之后对方又发来一条消息。
// 期望:那一回合的答复先暂存(快照显示 1 条待补发);下一条消息到达时先以“(补发)”补发,
// 再处理新消息。
TEST_F(ImChannelsEndToEnd, QqHeldOutputIsResentOnNextMessage) {
    qq.poll_bind_handler = [](const json&) {
        return json{{"retcode", 0},
                    {"data", {{"status", 2}, {"bot_appid", "102030405"}, {"bot_encrypt_secret", kSecretB64},
                              {"user_openid", "OWNER1"}}}};
    };
    std::atomic<bool> reject{false};
    qq.message_handler = [&](const acecode::test::FakeQqServer::Request&) {
        if (reject) return std::make_pair(403, json{{"code", 304}, {"message", "reply window closed"}});
        return std::make_pair(200, json{{"id", "R"}});
    };
    start_host();
    host->start_bind("qq");
    ASSERT_TRUE(wait_until([&] { return platform("qq").value("state", "") == "connected"; }));

    reject = true;
    provider->push_text("迟到的答复");
    qq.push_c2c("M1", "OWNER1", "第一个问题");
    ASSERT_TRUE(wait_until([&] { return platform("qq")["extra"].value("held", 0) == 1; }));

    reject = false;
    provider->push_text("新的答复");
    qq.push_c2c("M2", "OWNER1", "第二个问题");
    ASSERT_TRUE(wait_until([&] {
        bool resent = false, fresh = false;
        for (const auto& request : qq.requests_to("/v2/users/OWNER1/messages")) {
            const auto body = request.body.dump();
            if (body.find("(补发)") != std::string::npos && body.find("迟到的答复") != std::string::npos) resent = true;
            if (body.find("新的答复") != std::string::npos) fresh = true;
        }
        return resent && fresh;
    }));
    EXPECT_EQ(platform("qq")["extra"].value("held", -1), 0);
}

// 场景:设置页点「连接」开始微信扫码,用户扫码并在手机上确认;机主随后私聊提问。
// 期望:扫码过程中快照给出二维码内容;确认后保存 bot_token / bot_id / baseurl、扫码人成为机主、
// 自动启用并连接;私聊消息进入会话,答复用该用户的 context_token 发回(纯文本,不带 Markdown)。
TEST_F(ImChannelsEndToEnd, WeixinScanLoginBindsOwnerAndChats) {
    weixin = std::make_unique<acecode::test::FakeWeixinServer>();
    std::atomic<int> polls{0};
    weixin->qr_status_handler = [&](const acecode::test::FakeWeixinServer::Call&) {
        if (++polls < 3) return json{{"ret", 0}, {"status", "wait"}};
        return json{{"ret", 0},
                    {"status", "confirmed"},
                    {"bot_token", weixin->token()},
                    {"ilink_bot_id", "e06c1ceea05e@im.bot"},
                    {"baseurl", weixin->base()},
                    {"ilink_user_id", "owner@im.wechat"}};
    };
    start_host();
    host->start_bind("weixin");
    ASSERT_TRUE(wait_until([&] {
        return host->bind_state("weixin").value("qr_url", std::string{}).find("liteapp.weixin.qq.com") !=
               std::string::npos;
    })) << host->bind_state("weixin").dump();
    ASSERT_TRUE(wait_until([&] { return platform("weixin").value("state", "") == "connected"; }))
        << platform("weixin").dump(2);
    EXPECT_EQ(text(platform("weixin"), "owner"), "user:owner@im.wechat");
    EXPECT_EQ(platform("weixin").value("account", ""), "e06c1ceea05e@im.bot");

    provider->push_text("**收到**,我是 ACECode");
    weixin->push_text("owner@im.wechat", "你好", 7001, "ctx-owner");
    ASSERT_TRUE(wait_until([&] {
        for (const auto& call : weixin->calls_to("sendmessage")) {
            const auto& msg = call.body["msg"];
            if (msg.value("to_user_id", "") != "owner@im.wechat") continue;
            const auto said = msg["item_list"][0]["text_item"].value("text", "");
            if (said.find("收到,我是 ACECode") != std::string::npos && msg.value("context_token", "") == "ctx-owner")
                return true;
        }
        return false;
    }));
    EXPECT_TRUE(model_saw("你好"));
    EXPECT_FALSE(bound("weixin", "owner@im.wechat", "owner@im.wechat").empty());
}

// 场景:设置页填写钉钉 Client ID / Secret 并连接;向导给出 6 位绑定码,用户私聊机器人发送它,
// 然后提问。期望:凭据经取 token 与试注册 Stream 校验后保存并连接;绑定码的发送者成为机主、
// 收到确认且这条消息不进会话;提问进入会话,答复经该消息带来的会话 webhook 以 Markdown 发回。
TEST_F(ImChannelsEndToEnd, DingTalkOwnerCodeBindsAndRepliesThroughWebhook) {
    dingtalk = std::make_unique<acecode::test::FakeDingTalkServer>();
    start_host();
    auto& runtime = host->platform("dingtalk");
    EXPECT_THROW(runtime.set_credentials({{"client_id", dingtalk->client_id}, {"client_secret", "wrong-secret-0"}}),
                 std::runtime_error);
    runtime.set_credentials({{"client_id", dingtalk->client_id}, {"client_secret", dingtalk->client_secret}});
    runtime.set_enabled(true);
    ASSERT_TRUE(wait_until([&] { return platform("dingtalk").value("state", "") == "connected"; }))
        << platform("dingtalk").dump(2);
    const auto code = host->owner_link("dingtalk").value("code", std::string{});
    ASSERT_EQ(code.size(), 6u);

    dingtalk->push_callback("H1", dingtalk->private_text("m1", "staff01", code));
    ASSERT_TRUE(wait_until([&] { return text(platform("dingtalk"), "owner") == "user:staff01"; }));
    const auto sent_through_webhook = [&](const std::string& needle) {
        for (const auto& request : dingtalk->requests_to("/robot/sendBySession"))
            if (request.body.dump().find(needle) != std::string::npos) return true;
        return false;
    };
    EXPECT_TRUE(wait_until([&] { return sent_through_webhook("已绑定为机主"); }));
    EXPECT_FALSE(model_saw(code));

    provider->push_text("**收到**,我是 ACECode");
    dingtalk->push_callback("H2", dingtalk->private_text("m2", "staff01", "你好"));
    ASSERT_TRUE(wait_until([&] { return sent_through_webhook("**收到**,我是 ACECode"); }));
    EXPECT_TRUE(model_saw("你好"));
    EXPECT_TRUE(dingtalk->has_ack("H2"));
}

// 场景:设置页粘贴 Discord Bot Token 并连接;用户私信机器人发送绑定码,再私信提问。
// 期望:token 经 /users/@me 校验后保存,并带回机器人 id 与应用 id(邀请链接要用);网关 Identify
// 后连接;绑定码的发送者成为机主;提问进入会话,答复以 Markdown 原样发到该私信频道。
TEST_F(ImChannelsEndToEnd, DiscordOwnerCodeBindsAndChatsInDm) {
    discord = std::make_unique<acecode::test::FakeDiscordServer>();
    start_host();
    auto& runtime = host->platform("discord");
    EXPECT_THROW(runtime.set_credentials({{"token", "not-a-valid-token"}}), std::runtime_error);
    runtime.set_credentials({{"token", discord->token}});
    const auto saved = platform("discord")["credentials_public"];
    EXPECT_EQ(saved.value("bot_id", ""), discord->bot_id) << saved.dump();
    EXPECT_EQ(saved.value("application_id", ""), discord->application_id);
    runtime.set_enabled(true);
    ASSERT_TRUE(wait_until([&] { return platform("discord").value("state", "") == "connected"; }))
        << platform("discord").dump(2);
    const auto code = host->owner_link("discord").value("code", std::string{});
    ASSERT_EQ(code.size(), 6u);

    const std::string user = "800000000000000042";
    const auto dm = "/channels/" + acecode::test::FakeDiscordServer::dm_channel_for(user) + "/messages";
    const auto sent_to_dm = [&](const std::string& needle) {
        for (const auto& request : discord->requests_to("POST", dm))
            if (request.body.value("content", std::string{}).find(needle) != std::string::npos) return true;
        return false;
    };
    discord->push_dm("100000000000000001", user, code);
    ASSERT_TRUE(wait_until([&] { return text(platform("discord"), "owner") == "user:" + user; }));
    EXPECT_TRUE(wait_until([&] { return sent_to_dm("已绑定为机主"); }));

    provider->push_text("**收到**,我是 ACECode");
    discord->push_dm("100000000000000002", user, "你好");
    ASSERT_TRUE(wait_until([&] { return sent_to_dm("**收到**,我是 ACECode"); }));
    EXPECT_TRUE(model_saw("你好"));
}

// 场景:设置页填写飞书 App ID / Secret 并连接;用户私聊机器人发送绑定码,再提问。
// 期望:凭据经 tenant 令牌与机器人信息校验后保存,长连接建立后连接;绑定码的发送者(open_id)
// 成为机主并收到确认;提问进入会话,答复以回复原消息的方式发回;每个事件帧都被 ACK。
TEST_F(ImChannelsEndToEnd, FeishuOwnerCodeBindsAndRepliesToMessage) {
    feishu = std::make_unique<acecode::test::FakeFeishuServer>();
    start_host();
    auto& runtime = host->platform("feishu");
    runtime.set_credentials({{"app_id", "cli_app"}, {"app_secret", "secret-value-123"}});
    runtime.set_enabled(true);
    ASSERT_TRUE(wait_until([&] { return platform("feishu").value("state", "") == "connected"; }))
        << platform("feishu").dump(2);
    const auto code = host->owner_link("feishu").value("code", std::string{});
    ASSERT_EQ(code.size(), 6u);

    const auto p2p = [](const std::string& id, const std::string& said) {
        return acecode::test::FakeFeishuServer::message_event(id, "p2p", "oc_p2p", "ou_owner", "text",
                                                              {{"text", said}});
    };
    const auto sent = [&](const std::string& needle) {
        for (const auto& request : feishu->sends())
            if (request.raw.find(needle) != std::string::npos || request.body.dump().find(needle) != std::string::npos)
                return true;
        return false;
    };
    feishu->push_event(p2p("om_1", code));
    ASSERT_TRUE(wait_until([&] { return text(platform("feishu"), "owner") == "user:ou_owner"; }));
    EXPECT_TRUE(wait_until([&] { return sent("已绑定为机主"); }));

    provider->push_text("收到,我是 ACECode");
    feishu->push_event(p2p("om_2", "你好"));
    ASSERT_TRUE(wait_until([&] { return sent("收到,我是 ACECode"); }));
    EXPECT_TRUE(model_saw("你好"));
    EXPECT_FALSE(feishu->requests_to("/open-apis/im/v1/messages/om_2/reply").empty());
    EXPECT_TRUE(wait_until([&] { return feishu->frames_of_type("event").size() >= 2; }));
}

// 场景:设置页填写 LINE Channel ID / secret 并连接(公网地址指向本机回调端口);用户私聊官方账号
// 发送绑定码,再提问。期望:凭据经换令牌与机器人信息校验后保存并带回机器人 userId;回调地址
// 登记给 LINE 后连接;带签名的 webhook 里绑定码的发送者成为机主;提问进入会话,答复用这条消息
// 的回复令牌发回(免费的 reply,不用 push)。
TEST_F(ImChannelsEndToEnd, LineOwnerCodeBindsAndRepliesWithReplyToken) {
    using acecode::test::FakeLineServer;
    line = std::make_unique<FakeLineServer>();
    line_port = im::line::pick_free_loopback_port();
    start_host();
    auto& runtime = host->platform("line");
    runtime.set_credentials({{"channel_id", FakeLineServer::kChannelId}, {"channel_secret", FakeLineServer::kSecret}});
    EXPECT_EQ(platform("line")["credentials_public"].value("bot_id", ""), FakeLineServer::kBotId);
    runtime.set_enabled(true);
    ASSERT_TRUE(wait_until([&] { return platform("line").value("state", "") == "connected"; }))
        << platform("line").dump(2);
    EXPECT_EQ(line->endpoint(), "http://127.0.0.1:" + std::to_string(line_port) + "/line/webhook");
    const auto code = host->owner_link("line").value("code", std::string{});
    ASSERT_EQ(code.size(), 6u);

    const std::string user = "U11111111111111111111111111111111";
    const auto webhook = "http://127.0.0.1:" + std::to_string(line_port) + "/line/webhook";
    const auto say = [&](const std::string& said, const std::string& id) {
        const auto token = line->issue_reply_token();
        const auto body = FakeLineServer::webhook_body(
            {FakeLineServer::text_event(FakeLineServer::source("user", "", user), said, id, token, "ev-" + id)});
        EXPECT_EQ(FakeLineServer::post_webhook(webhook, body).status, 200);
    };
    const auto replied = [&](const std::string& needle) {
        for (const auto& call : line->calls_to("POST", "/v2/bot/message/reply"))
            if (call.body.dump().find(needle) != std::string::npos) return true;
        return false;
    };
    say(code, "m1");
    ASSERT_TRUE(wait_until([&] { return text(platform("line"), "owner") == "user:" + user; }));
    EXPECT_TRUE(wait_until([&] { return replied("已绑定为机主"); }));

    provider->push_text("收到,我是 ACECode");
    say("你好", "m2");
    ASSERT_TRUE(wait_until([&] { return replied("收到,我是 ACECode"); }));
    EXPECT_TRUE(model_saw("你好"));
    EXPECT_TRUE(line->calls_to("POST", "/v2/bot/message/push").empty());
}

} // namespace
} // namespace acecode::daemon
