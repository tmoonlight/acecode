#include <gtest/gtest.h>

#include "channels/core/conversations.hpp"
#include "test_support/channels/core_fakes.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>

// channels/core/conversations:IM 会话与 ACECode 会话的绑定路由。用内存版 SessionClient
// 与假传输层驱动;回复经出站投影的后台线程发送,断言前等待。
// 默认身份:Telegram 机器人 100,机主 user:1,已授权联系人 user:2,已批准群 -100。

namespace acecode::channels::core {
namespace {

using test::FakeSessions;
using test::FakeTransport;
using test::group_address;
using test::message;
using test::private_address;
using test::wait_until;

class ChannelRouter : public ::testing::Test {
protected:
    void SetUp() override {
        home_dir = channels::test::temporary("router");
        std::filesystem::create_directories(home_dir);
        home = std::make_unique<channels::test::Home>(home_dir);
        transport = std::make_shared<FakeTransport>("telegram");
        slot->set(transport);
        open_store();
        store->update_config([](PlatformConfig& config) {
            config.enabled = true;
            config.owner = "user:1";
            config.access = {{"user:1", "", 0}, {"user:2", "", 0}, {"group:-100", "", 0}};
        });
        make_conversations();
    }

    void TearDown() override {
        conversations.reset();
        home.reset();
        std::error_code ec;
        std::filesystem::remove_all(home_dir, ec);
    }

    void open_store() {
        store = std::make_unique<ChannelStore>(home_dir / "telegram");
        store->load();
    }

    void make_conversations() {
        ConversationDeps deps;
        deps.sessions = &sessions;
        deps.pending_permissions = [](const std::string&) { return std::vector<nlohmann::json>{}; };
        deps.session_cwd = [this](const std::string& id) {
            std::lock_guard<std::mutex> lock(sessions.mu);
            return sessions.sessions[id].cwd;
        };
        deps.catalog = [this](const std::optional<std::string>& query) {
            auto all = sessions.catalog();
            if (!query) return all;
            std::vector<rc::RcSessionTarget> hits;
            for (const auto& target : all)
                if (target.title.find(*query) != std::string::npos) hits.push_back(target);
            return hits;
        };
        deps.resume_target = [this](const rc::RcSessionTarget& target) {
            return sessions.resume_session(target.session_id, {});
        };
        deps.model_names = [] { return std::vector<std::string>{"fast", "smart"}; };
        deps.switch_model = [this](const std::string& id, const std::string& name, std::string*) {
            switched.emplace_back(id, name);
            return true;
        };
        deps.set_permission_mode = [this](const std::string& id, const std::string& mode) {
            mode_changes.emplace_back(id, mode);
            if (fail_controls) return false;
            std::lock_guard<std::mutex> lock(sessions.mu);
            sessions.sessions.at(id).permission_mode = mode;
            return true;
        };
        deps.model_state = [this](const std::string& id) -> std::optional<SessionModelState> {
            return model_states[id];
        };
        deps.set_reasoning_effort = [this](const std::string& id, const std::optional<std::string>& effort,
                                          std::string* error) -> std::optional<SessionModelState> {
            effort_changes.emplace_back(id, effort);
            if (fail_controls || (effort && *effort != "low" && *effort != "high")) {
                *error = "测试更新失败";
                return std::nullopt;
            }
            model_states[id].reasoning_effort = effort;
            return model_states[id];
        };
        deps.expand_skill = [](const std::string&, const std::string& text) { return "[展开]" + text; };
        deps.list_skills = [this](const std::string&) { return skills; };
        deps.side_question = [this](const std::string& id, const std::string& question,
                                    SideChatCancellation& cancellation) {
            return side ? side(id, question, cancellation) : SideQuestionResult{};
        };
        deps.reference_sessions = [this](const std::string&, const std::vector<rc::RcSessionTarget>& targets,
                                          const std::string& text,
                                          std::string*) -> std::optional<SessionReferenceExpansion> {
            referenced.push_back(targets);
            SessionReferenceExpansion expansion;
            expansion.text = "[引用 " + std::to_string(targets.size()) + " 个会话]\n" + text;
            for (const auto& target : targets) expansion.meta.push_back({{"session_id", target.session_id}});
            return expansion;
        };
        deps.utc_offset_minutes = [] { return 480; };
        deps.release_session = [this](const std::string& id, const std::string& key, const std::string& label) {
            if (conversations) conversations->release_session(id, key, label);
        };
        deps.on_bindings_changed = [this] { ++binding_changes; };
        deps.media_dir = home_dir / "media";
        conversations = std::make_unique<Conversations>("telegram", *store, access, slot, deps);
    }

    // 模拟 daemon 重启:丢弃内存状态,重新读盘。
    void restart() {
        conversations.reset();
        open_store();
        make_conversations();
    }

    HandleOutcome send(const im::Inbound& inbound) { return conversations->handle(inbound); }
    HandleOutcome send(const std::string& user, const std::string& text, const std::string& id) {
        return send(message(private_address(user), text, id));
    }
    std::string bound(const std::string& user) {
        const auto record = store->binding(private_address(user).key());
        return record ? record->session_id : std::string{};
    }

    std::filesystem::path home_dir;
    std::unique_ptr<channels::test::Home> home;
    FakeSessions sessions;
    std::shared_ptr<FakeTransport> transport;
    std::shared_ptr<TransportSlot> slot = std::make_shared<TransportSlot>();
    std::unique_ptr<ChannelStore> store;
    AccessControl access;
    std::unique_ptr<Conversations> conversations;
    std::vector<std::pair<std::string, std::string>> switched;
    std::vector<std::pair<std::string, std::string>> mode_changes;
    std::vector<std::pair<std::string, std::optional<std::string>>> effort_changes;
    std::map<std::string, SessionModelState> model_states;
    bool fail_controls = false;
    std::vector<SkillListing> skills;
    std::function<SideQuestionResult(const std::string&, const std::string&, SideChatCancellation&)> side;
    std::vector<std::vector<rc::RcSessionTarget>> referenced;
    int binding_changes = 0;
};

// 场景:已授权联系人第一次发普通消息。
// 期望:新建无项目会话(default 权限、不继承 daemon 危险标志)并持久化绑定;
// 消息作为用户输入提交,metadata.channel 带平台、地址与回复上下文。
TEST_F(ChannelRouter, FirstMessageCreatesNoWorkspaceSession) {
    send("1", "你好", "m1");
    const auto created = sessions.created_log();
    ASSERT_EQ(created.size(), 1u);
    EXPECT_TRUE(created[0].second.no_workspace);
    EXPECT_EQ(created[0].second.permission_mode, "default");
    EXPECT_FALSE(created[0].second.inherit_dangerous_mode);
    EXPECT_EQ(bound("1"), created[0].first);
    const auto inputs = sessions.input_log();
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs[0].first, created[0].first);
    EXPECT_EQ(inputs[0].second.text, "你好");
    const auto channel = inputs[0].second.metadata["channel"];
    EXPECT_EQ(channel.value("platform", ""), "telegram");
    EXPECT_EQ(channel["reply_context"].value("msg_id", ""), "m1");
    EXPECT_GE(binding_changes, 1);
}

// 场景:daemon 重启后同一联系人再次发消息。
// 期望:不新建会话,消息进入原绑定会话(先恢复该会话)。
TEST_F(ChannelRouter, BindingSurvivesRestart) {
    send("1", "第一条", "m1");
    const auto id = bound("1");
    restart();
    send("1", "第二条", "m2");
    EXPECT_EQ(sessions.created_log().size(), 1u);
    const auto inputs = sessions.input_log();
    ASSERT_EQ(inputs.size(), 2u);
    EXPECT_EQ(inputs[1].first, id);
    EXPECT_NE(std::find(sessions.resumed.begin(), sessions.resumed.end(), id), sessions.resumed.end());
}

// 场景:平台重复投递同一条消息(同一 message_id)。
// 期望:只提交一次。
TEST_F(ChannelRouter, DuplicateDeliveryIsIgnored) {
    send("1", "你好", "m1");
    send("1", "你好", "m1");
    EXPECT_EQ(sessions.input_log().size(), 1u);
}

// 场景:同一个群里两个已授权成员分别 @机器人。
// 期望:各自绑定独立的会话,上下文不混合。
TEST_F(ChannelRouter, GroupMembersGetSeparateSessions) {
    send(message(group_address("-100", "1"), "成员一", "g1"));
    send(message(group_address("-100", "2"), "成员二", "g2"));
    const auto inputs = sessions.input_log();
    ASSERT_EQ(inputs.size(), 2u);
    EXPECT_NE(inputs[0].first, inputs[1].first);
}

// 场景:绑定在工作区 W 的会话上时发送 /new。
// 期望:在 W 新建会话(同 cwd 与 workspace_hash)并改绑,回复里带工作区名。
TEST_F(ChannelRouter, NewSessionFollowsCurrentWorkspace) {
    sessions.add_session("w-1", "C:/work/proj", "hash-w", false, "工作区会话");
    send("1", "/resume w-1", "m1");
    ASSERT_EQ(bound("1"), "w-1");
    send("1", "/new", "m2");
    const auto created = sessions.created_log();
    ASSERT_EQ(created.size(), 1u);
    EXPECT_FALSE(created[0].second.no_workspace);
    EXPECT_EQ(created[0].second.cwd, "C:/work/proj");
    EXPECT_EQ(created[0].second.workspace_hash, "hash-w");
    EXPECT_EQ(bound("1"), created[0].first);
    EXPECT_TRUE(transport->wait_said("已新建会话(proj)"));
    EXPECT_TRUE(sessions.input_log().empty());  // 命令本身不进会话
}

// 场景:绑定会话正在执行时发送 /new、/resume,以及 /status。
// 期望:/new 与 /resume 被拒绝且绑定不变;/status 立即回复并显示执行中。
TEST_F(ChannelRouter, BusySessionRejectsSwitching) {
    sessions.add_session("w-1", "C:/work/proj", "hash-w", false, "工作区会话");
    send("1", "开始", "m1");
    const auto id = bound("1");
    sessions.set_busy(id, true);
    send("1", "/new", "m2");
    send("1", "/resume w-1", "m3");
    EXPECT_EQ(sessions.created_log().size(), 1u);
    EXPECT_EQ(bound("1"), id);
    EXPECT_TRUE(transport->wait_said("请先 /stop"));
    send("1", "/status", "m4");
    EXPECT_TRUE(transport->wait_said("状态:执行中"));
}

TEST_F(ChannelRouter, ModeCommandsStayInBoundSessionWhileBusy) {
    send("1", "/new", "m1");
    send("2", "/new", "m2");
    const auto id = bound("2");
    sessions.set_busy(id, true);
    send("2", "/mode", "m3");
    EXPECT_TRUE(transport->wait_said("权限模式:默认权限 (default)"));
    send("2", "/mode auto", "m4");
    EXPECT_TRUE(transport->wait_said("已切换权限模式为 自动模式 (auto)"));
    send("2", "/yolo", "m5");
    send("2", "/yolo", "m5");
    EXPECT_TRUE(transport->wait_said("已切换权限模式为 完全访问权限 (yolo)"));
    send("2", "/status", "m6");
    EXPECT_TRUE(transport->wait_said("权限模式:完全访问权限 (yolo)"));
    send("2", "/mode default", "m7");
    ASSERT_EQ(mode_changes.size(), 3u);
    EXPECT_EQ(mode_changes[0], std::make_pair(id, std::string("auto")));
    EXPECT_EQ(mode_changes[1], std::make_pair(id, std::string("yolo")));
    EXPECT_EQ(mode_changes[2], std::make_pair(id, std::string("default")));
    EXPECT_TRUE(sessions.input_log().empty());
    std::lock_guard<std::mutex> lock(sessions.mu);
    EXPECT_EQ(sessions.sessions.at(bound("1")).permission_mode, "default");
    EXPECT_TRUE(sessions.sessions.at(id).busy);
}

TEST_F(ChannelRouter, EffortQueriesChangesResetsAndReportsFailures) {
    send("1", "/new", "m1");
    const auto id = bound("1");
    auto& model = model_states[id];
    model.provider = "openai";
    model.reasoning = ModelReasoningOptions{};
    model.reasoning->supported = true;
    model.reasoning->enabled = true;
    model.reasoning->supported_efforts = {"low", "high"};
    model.reasoning->effort = "low";
    sessions.set_busy(id, true);
    send("1", "/effort", "m2");
    EXPECT_TRUE(transport->wait_said("思考深度:low (模型预设)"));
    EXPECT_TRUE(transport->wait_said("可用档位: low high"));
    send("1", "/effort high", "m3");
    send("1", "/effort high", "m3");
    EXPECT_TRUE(transport->wait_said("从下一次模型请求起生效"));
    EXPECT_EQ(model.reasoning_effort, "high");
    send("1", "/effort invalid", "m4");
    EXPECT_TRUE(transport->wait_said("测试更新失败"));
    EXPECT_EQ(model.reasoning_effort, "high");
    send("1", "/status", "m5");
    EXPECT_TRUE(transport->wait_said("思考深度:high"));
    send("1", "/effort default", "m6");
    EXPECT_TRUE(transport->wait_said("已恢复模型预设思考深度"));
    EXPECT_FALSE(model.reasoning_effort);
    ASSERT_EQ(effort_changes.size(), 3u);
    EXPECT_EQ(effort_changes.back().first, id);
    EXPECT_FALSE(effort_changes.back().second);
    EXPECT_TRUE(sessions.input_log().empty());
    model.reasoning->enabled = false;
    send("1", "/effort", "m7");
    EXPECT_TRUE(transport->wait_said("当前模型未启用或不支持 effort 档位"));
}

TEST_F(ChannelRouter, ControlsRejectUnboundUnauthorizedMalformedAndDeletedSessions) {
    for (const auto* command : {"/mode auto", "/yolo", "/effort high"}) {
        send("1", command, std::string("unbound-") + command);
        send("9", command, std::string("unauthorized-") + command);
    }
    EXPECT_TRUE(transport->wait_said("当前还没有会话"));
    EXPECT_TRUE(sessions.created_log().empty());
    send("1", "/new", "new");
    send("1", "/mode bad", "bad-mode");
    send("1", "/yolo extra", "bad-yolo");
    send("1", "/effort high low", "bad-effort");
    const auto id = bound("1");
    restart();
    sessions.remove(id);
    send("1", "/yolo", "deleted");
    EXPECT_TRUE(transport->wait_said("无法打开当前绑定的会话"));
    EXPECT_TRUE(mode_changes.empty());
    EXPECT_TRUE(effort_changes.empty());
    EXPECT_TRUE(sessions.input_log().empty());
}

TEST_F(ChannelRouter, FailedModeUpdateDoesNotReportSuccess) {
    send("1", "/new", "m1");
    fail_controls = true;
    send("1", "/yolo", "m2");
    EXPECT_TRUE(transport->wait_said("切换权限模式失败"));
    EXPECT_FALSE(transport->said("已切换权限模式"));
    send("1", "/mode", "m3");
    EXPECT_TRUE(transport->wait_said("权限模式:默认权限 (default)"));
    EXPECT_TRUE(sessions.input_log().empty());
}

// 场景:机主 /sessions 列出全部会话,然后目录里新增一个排在最前面的会话,再 /resume 1。
// 期望:编号按最近一次列表的快照解析,选中的是列表里的第 1 项而不是新会话。
TEST_F(ChannelRouter, ResumeIndexUsesLatestListingSnapshot) {
    sessions.add_session("b-1", "C:/work/b", "hash-b", false, "会话 B");
    sessions.add_session("c-1", "C:/work/c", "hash-c", false, "会话 C");
    send("1", "/sessions", "m1");
    EXPECT_TRUE(transport->wait_said("会话 B"));
    sessions.add_session("a-1", "C:/work/a", "hash-a", false, "会话 A");
    send("1", "/resume 1", "m2");
    EXPECT_EQ(bound("1"), "b-1");
    EXPECT_TRUE(transport->wait_said("已切换到会话:会话 B"));
}

// 场景:非机主联系人 /sessions 后尝试切到别人的会话,再切回自己创建的会话。
// 期望:列表只含自己创建的会话;切别人的会话被拒绝且绑定不变。
TEST_F(ChannelRouter, NonOwnerOnlySeesOwnSessions) {
    sessions.add_session("w-1", "C:/work/proj", "hash-w", false, "别人的会话");
    send("2", "你好", "m1");
    const auto own = bound("2");
    send("2", "/sessions", "m2");
    EXPECT_TRUE(wait_until([&] { return transport->said("可切换的会话"); }));
    EXPECT_FALSE(transport->said("别人的会话"));
    send("2", "/resume w-1", "m3");
    EXPECT_TRUE(transport->wait_said("只能切换到你自己"));
    EXPECT_EQ(bound("2"), own);
}

// 场景:机主在群里用 /resume 选中一个当前绑定在自己私聊上的会话。
// 期望:绑定转到群;私聊收到“已转到其他通道”的提示并解除绑定;
// 此后该会话的输出只发到群里,私聊的旧投影已退订。
TEST_F(ChannelRouter, ResumeTransfersBindingBetweenConversations) {
    send("1", "私聊开始", "m1");
    const auto id = bound("1");
    send(message(group_address("-100", "1"), "/resume " + id, "g1"));
    EXPECT_TRUE(bound("1").empty());
    const auto group_binding = store->binding(group_address("-100", "1").key());
    ASSERT_TRUE(group_binding.has_value());
    EXPECT_EQ(group_binding->session_id, id);
    EXPECT_TRUE(transport->wait_said("当前会话已转到Telegram 群聊"));
    EXPECT_TRUE(wait_until([&] { return sessions.listener_count(id) == 1; }));
    sessions.assistant(id, "只给群的回复");
    ASSERT_TRUE(transport->wait_said("只给群的回复"));
    for (const auto& sent : transport->sent()) {
        if (sent.text == "只给群的回复") EXPECT_EQ(sent.to.kind, im::ChatKind::Group);
    }
}

// 场景:绑定的会话已被永久删除,联系人在重启后继续发消息。
// 期望:明确提示会话无法打开并引导 /new 或 /sessions;不静默新建会话顶替。
TEST_F(ChannelRouter, DeletedSessionIsReportedNotReplaced) {
    send("1", "你好", "m1");
    const auto id = bound("1");
    restart();
    sessions.remove(id);
    send("1", "还在吗", "m2");
    EXPECT_TRUE(transport->wait_said("无法打开当前绑定的会话"));
    EXPECT_EQ(sessions.created_log().size(), 1u);
    EXPECT_EQ(bound("1"), id);
}

// 场景:普通文本以 / 开头但不是通道命令(技能命令)。
// 期望:像 Web 输入一样展开后提交,显示文本保留原文。
TEST_F(ChannelRouter, SkillCommandsAreExpandedLikeWebInput) {
    send("1", "/review 这个文件", "m1");
    const auto inputs = sessions.input_log();
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs[0].second.text, "[展开]/review 这个文件");
    EXPECT_EQ(inputs[0].second.display_text, "/review 这个文件");
}

// 场景:尚未绑定时 /status;绑定后 /stop。
// 期望:命令不进入对话;未绑定时提示直接发消息开始;/stop 中止当前回合并确认。
TEST_F(ChannelRouter, CommandsAreHandledOutsideTheConversation) {
    send("1", "/status", "m1");
    EXPECT_TRUE(transport->wait_said("当前还没有会话"));
    send("1", "开始", "m2");
    const auto id = bound("1");
    send("1", "/stop", "m3");
    EXPECT_EQ(sessions.abort_log(), std::vector<std::string>{id});
    EXPECT_TRUE(transport->wait_said("已请求停止"));
    EXPECT_EQ(sessions.input_log().size(), 1u);
}

// 场景:/model 不带参数、带已保存的模型名、带不存在的名字。
// 期望:列出模型;切换绑定会话的模型;未知名字报错并列出可用模型。
TEST_F(ChannelRouter, ModelCommandListsAndSwitches) {
    send("1", "开始", "m1");
    const auto id = bound("1");
    send("1", "/model", "m2");
    EXPECT_TRUE(transport->wait_said("可用模型"));
    send("1", "/model smart", "m3");
    ASSERT_EQ(switched.size(), 1u);
    EXPECT_EQ(switched[0], std::make_pair(id, std::string("smart")));
    EXPECT_TRUE(transport->wait_said("已切换模型为 smart"));
    send("1", "/model nope", "m4");
    EXPECT_TRUE(transport->wait_said("没有名为 nope 的模型"));
    EXPECT_EQ(switched.size(), 1u);
}

// 场景:绑定会话发出权限请求,IM 里 /approve;随后另一个请求先在 Desktop 被处理。
// 期望:IM 的批准提交给会话;同一编号再次 /approve 得到“已处理或不存在”;
// Desktop 先处理的请求在 IM 里收到已处理提示,之后 /approve 同样无效。
TEST_F(ChannelRouter, PermissionRequestsFirstAnswerWins) {
    send("1", "开始", "m1");
    const auto id = bound("1");
    sessions.emit(id, SessionEventKind::PermissionRequest,
                  {{"request_id", "p1"}, {"tool", "bash"}, {"args", {{"command", "ls"}}}});
    ASSERT_TRUE(transport->wait_said("/approve p1"));
    send("1", "/approve p1", "m2");
    ASSERT_EQ(sessions.decisions.size(), 1u);
    EXPECT_EQ(sessions.decisions[0].second.choice, PermissionDecisionChoice::Allow);
    EXPECT_TRUE(transport->wait_said("已允许权限请求 p1"));
    send("1", "/approve p1", "m3");
    EXPECT_TRUE(transport->wait_said("该权限请求已处理或不存在"));

    sessions.emit(id, SessionEventKind::PermissionRequest, {{"request_id", "p2"}, {"tool", "bash"}});
    ASSERT_TRUE(transport->wait_said("/approve p2"));
    sessions.emit(id, SessionEventKind::PermissionClosed, {{"request_id", "p2"}, {"choice", "allow"}});
    EXPECT_TRUE(transport->wait_said("权限请求 p2 已处理"));
    send("1", "/deny p2", "m4");
    EXPECT_EQ(sessions.decisions.size(), 1u);
}

// 场景:会话在提问(AskUserQuestion)时,联系人没有用 /aq 而是直接发了文字。
// 期望:作为插话交给当前提问,不作为新的普通输入排队。
TEST_F(ChannelRouter, PlainTextDuringQuestionInterjects) {
    send("1", "开始", "m1");
    const auto id = bound("1");
    sessions.emit(id, SessionEventKind::QuestionRequest,
                  {{"request_id", "q1"},
                   {"questions",
                    {{{"id", "pick"},
                      {"header", "选择"},
                      {"text", "选哪个?"},
                      {"options", {{{"label", "A"}}, {{"label", "B"}}}}}}}});
    ASSERT_TRUE(transport->wait_said("/aq"));
    send("1", "我想要 C", "m2");
    ASSERT_EQ(sessions.interjections.size(), 1u);
    EXPECT_EQ(sessions.interjections[0].request_id, "q1");
    EXPECT_EQ(sessions.interjections[0].input.text, "我想要 C");
    EXPECT_EQ(sessions.input_log().size(), 1u);
}

// 场景:联系人发来带识别文字的语音,以及一段视频。
// 期望:语音按“[语音转写] …”提交;视频得到“暂不支持”的回复,会话不收到输入。
TEST_F(ChannelRouter, VoiceTranscriptsAndUnsupportedMedia) {
    auto voice = message(private_address("1"), "", "m1");
    im::Attachment clip;
    clip.kind = im::AttachmentKind::Voice;
    clip.transcript = "明天开会";
    voice.attachments.push_back(clip);
    send(voice);
    auto inputs = sessions.input_log();
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs[0].second.text, "[语音转写] 明天开会");

    auto video = message(private_address("1"), "", "m2");
    im::Attachment movie;
    movie.kind = im::AttachmentKind::Video;
    video.attachments.push_back(movie);
    send(video);
    EXPECT_TRUE(transport->wait_said("暂不支持视频消息"));
    EXPECT_EQ(sessions.input_log().size(), 1u);
}

// 场景:联系人发来一张图片并附带文字。
// 期望:图片下载后导入会话附件,与文字一起提交(content_parts 依次为文字、图片、图片本地路径的说明,
// metadata 记录附件);界面显示的仍是用户原话。路径说明让能看图的模型可以用 show_image 截取局部发回。
TEST_F(ChannelRouter, ImagesAreImportedAsSessionAttachments) {
    auto photo = message(private_address("1"), "看看这张图", "m1");
    im::Attachment image;
    image.kind = im::AttachmentKind::Image;
    image.name = "screen.png";
    image.mime_type = "image/png";
    image.remote_ref = "file-1";
    photo.attachments.push_back(image);
    send(photo);
    const auto inputs = sessions.input_log();
    ASSERT_EQ(inputs.size(), 1u);
    const auto& parts = inputs[0].second.content_parts;
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[1].value("type", ""), "image");
    const auto saved_path = parts[1]["attachment"].value("path", std::string{});
    ASSERT_FALSE(saved_path.empty());
    EXPECT_NE(parts[2].value("text", "").find("Image saved at " + saved_path), std::string::npos);
    EXPECT_EQ(inputs[0].second.metadata.value("display_text", std::string{}), "看看这张图");
    ASSERT_TRUE(inputs[0].second.metadata.contains("attachments"));
    EXPECT_EQ(inputs[0].second.metadata["attachments"].size(), 1u);
}

// 场景:平台把一张 PNG 报成 photo.jpg / image/jpeg(插件只能从对象 key 猜类型,手机端发的图常没有扩展名)。
// 期望:下载后按文件头识别,附件存为 image/png、名字改成 photo.png,模型收到的图片类型与内容一致。
// 回归:以前直接用平台给的类型,类型与内容不符的图片发给模型时会被服务端拒收。
TEST_F(ChannelRouter, ImageTypeFollowsTheFileSignature) {
    transport->download_bytes = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(32, '\0');
    auto photo = message(private_address("1"), "", "m1");
    im::Attachment image;
    image.kind = im::AttachmentKind::Image;
    image.name = "photo.jpg";
    image.mime_type = "image/jpeg";
    image.remote_ref = "file-1";
    photo.attachments.push_back(image);
    send(photo);
    const auto inputs = sessions.input_log();
    ASSERT_EQ(inputs.size(), 1u);
    ASSERT_TRUE(inputs[0].second.metadata.contains("attachments"));
    const auto& saved = inputs[0].second.metadata["attachments"][0];
    EXPECT_EQ(saved.value("mime_type", std::string{}), "image/png");
    EXPECT_EQ(saved.value("name", std::string{}), "photo.png");
}

// 场景:机主撤销某联系人的授权时,该联系人绑定的会话正在执行。
// 期望:该会话的当前回合被中止。
TEST_F(ChannelRouter, RevokingAccessAbortsTheirTurn) {
    send("2", "开始", "m1");
    const auto id = bound("2");
    conversations->abort_principal("user:2");
    EXPECT_EQ(sessions.abort_log(), std::vector<std::string>{id});
}

// 场景:陌生人私聊。
// 期望:返回新建的待批准请求供设置页通知,对方收到配对提示,不建会话。
TEST_F(ChannelRouter, StrangerGetsPairingNotice) {
    const auto outcome = send("9", "你好", "m1");
    ASSERT_TRUE(outcome.pending_created.has_value());
    EXPECT_EQ(outcome.pending_created->principal, "user:9");
    EXPECT_TRUE(transport->wait_said("还没有被授权"));
    EXPECT_TRUE(sessions.created_log().empty());
}

// ---- 与 Desktop 对齐的命令、分页与 @编号 引用 ----

std::string two_digits(int value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02d", value);
    return buffer;
}

// 场景:机主有 25 个会话,用简写 /s 列出,/mr 翻页,/s 3 直接看第三页,再 /r 15 切换。
// 期望:每页 10 条、编号跨页连续;/r 15 切到列表里第 15 个会话(不是当前页的第 5 个)。
// 回归:旧版 /sessions 只列 10 条,/sessions more 一次性列 50 条,超出单条消息长度。
TEST_F(ChannelRouter, SessionListPaginatesWithContinuousNumbers) {
    for (int i = 1; i <= 25; ++i)
        sessions.add_session("s-" + two_digits(i), "C:/work/p", "hash-p", false, "会话 " + two_digits(i));
    send("1", "/s", "m1");
    ASSERT_TRUE(transport->wait_said("第 1/3 页"));
    EXPECT_TRUE(transport->said("1. 会话 01"));
    EXPECT_FALSE(transport->said("11. 会话 11"));
    send("1", "/mr", "m2");
    ASSERT_TRUE(transport->wait_said("11. 会话 11"));
    EXPECT_TRUE(transport->said("第 2/3 页"));
    send("1", "/s 3", "m3");
    ASSERT_TRUE(transport->wait_said("21. 会话 21"));
    EXPECT_TRUE(transport->said("(最后一页)"));
    send("1", "/mr", "m4");
    EXPECT_TRUE(transport->wait_said("已经是最后一页"));
    send("1", "/r 15", "m5");
    EXPECT_EQ(bound("1"), "s-15");
}

// 场景:/s 后面直接跟关键词(不再需要 search),以及旧写法 /sessions search。
// 期望:两种写法都按关键词搜索,标题里标明搜索词,/r 编号对应搜索结果。
TEST_F(ChannelRouter, SessionSearchAcceptsBareKeyword) {
    sessions.add_session("a-1", "C:/work/a", "hash-a", false, "登录页面");
    sessions.add_session("b-1", "C:/work/b", "hash-b", false, "支付接口");
    send("1", "/s 支付", "m1");
    ASSERT_TRUE(transport->wait_said("匹配「支付」的会话"));
    EXPECT_TRUE(transport->said("1. 支付接口"));
    EXPECT_FALSE(transport->said("登录页面"));
    send("1", "/r 1", "m2");
    EXPECT_EQ(bound("1"), "b-1");
    send("1", "/sessions search 登录", "m3");
    EXPECT_TRUE(transport->wait_said("1. 登录页面"));
}

// 场景:没有任何可翻页的输出时发送 /mr。
// 期望:回复「没有可以翻页的内容」,不新建会话。
TEST_F(ChannelRouter, MoreWithoutPagedOutput) {
    send("1", "/mr", "m1");
    EXPECT_TRUE(transport->wait_said("没有可以翻页的内容"));
    EXPECT_TRUE(sessions.created_log().empty());
}

// 场景:会话里有 13 个技能,发送 /sk、/sk 关键词、/sk 9。
// 期望:按名称排序分页列出(每行 /技能名 — 描述);关键词同时匹配名称和描述;页码越界时提示总页数。
TEST_F(ChannelRouter, SkillsListPaginatesAndFilters) {
    for (int i = 1; i <= 12; ++i) skills.push_back({"skill-" + two_digits(i), "描述 " + two_digits(i)});
    skills.push_back({"review", "代码审查"});
    send("1", "/new", "m0");
    send("1", "/sk", "m1");
    ASSERT_TRUE(transport->wait_said("可用技能"));
    EXPECT_TRUE(transport->said("1. /review — 代码审查"));
    EXPECT_TRUE(transport->said("第 1/2 页"));
    send("1", "/sk 审查", "m2");
    ASSERT_TRUE(transport->wait_said("匹配「审查」的技能"));
    send("1", "/sk 9", "m3");
    EXPECT_TRUE(transport->wait_said("只有 2 页"));
}

// 场景:先 @ref 列出可引用的会话,再发「对比@1和@2的结论」(中文常不加空格)。
// 期望:当前会话不出现在可引用列表里;两个会话按列表编号被引用,交给模型的文本带上引用上下文,
// 界面显示的文字把 @编号 换成会话标题,metadata.session_references 记录被引用的会话。
TEST_F(ChannelRouter, RefMentionsExpandReferencedSessions) {
    sessions.add_session("r-1", "C:/work/r", "hash-r", false, "需求讨论");
    sessions.add_session("r-2", "C:/work/r", "hash-r", false, "接口设计");
    send("1", "/new", "m0");
    const auto current = bound("1");
    send("1", "@ref", "m1");
    ASSERT_TRUE(transport->wait_said("可引用的会话"));
    EXPECT_TRUE(transport->said("1. 需求讨论"));
    EXPECT_TRUE(transport->said("2. 接口设计"));
    EXPECT_FALSE(transport->said("会话 " + current));
    send("1", "对比@1和@2的结论", "m2");
    ASSERT_EQ(referenced.size(), 1u);
    ASSERT_EQ(referenced[0].size(), 2u);
    EXPECT_EQ(referenced[0][0].session_id, "r-1");
    EXPECT_EQ(referenced[0][1].session_id, "r-2");
    const auto inputs = sessions.input_log();
    ASSERT_EQ(inputs.size(), 1u);
    const auto& input = inputs[0].second;
    EXPECT_EQ(input.text, "[引用 2 个会话]\n对比@「需求讨论」和@「接口设计」的结论");
    EXPECT_EQ(input.display_text, "对比@「需求讨论」和@「接口设计」的结论");
    ASSERT_TRUE(input.metadata.contains("session_references"));
    EXPECT_EQ(input.metadata["session_references"].size(), 2u);
}

// 场景:没发过 @ref 时消息里出现「@3 点开会」这类文字。
// 期望:当作普通文字原样提交,不报错也不引用任何会话。
TEST_F(ChannelRouter, RefMentionWithoutListingIsPlainText) {
    sessions.add_session("r-1", "C:/work/r", "hash-r", false, "需求讨论");
    send("1", "@3 点开会", "m1");
    EXPECT_TRUE(referenced.empty());
    const auto inputs = sessions.input_log();
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs[0].second.text, "@3 点开会");
}

// 场景:@ref 列表只有 1 个会话,用户却写了 @9;以及一次引用超过 5 个会话。
// 期望:回复说明原因,这条消息不提交给会话(避免模型拿到错误的上下文)。
TEST_F(ChannelRouter, RefMentionOutOfRangeIsRejected) {
    for (int i = 1; i <= 6; ++i)
        sessions.add_session("r-" + std::to_string(i), "C:/work/r", "hash-r", false, "会话 " + two_digits(i));
    send("1", "/new", "m0");
    send("1", "@ref", "m1");
    ASSERT_TRUE(transport->wait_said("可引用的会话"));
    send("1", "看看 @9", "m2");
    EXPECT_TRUE(transport->wait_said("没有第 9 个会话"));
    send("1", "@1 @2 @3 @4 @5 @6 一起看", "m3");
    EXPECT_TRUE(transport->wait_said("一次最多引用 5 个会话"));
    EXPECT_TRUE(sessions.input_log().empty());
    EXPECT_TRUE(referenced.empty());
}

// 场景:/l(/lsp)在 daemon 里执行,结果以会话系统消息同步发出。
// 期望:系统消息作为命令回复发回 IM;请求带命令名与参数。
TEST_F(ChannelRouter, BuiltinOutputIsRepliedFromSystemMessages) {
    send("1", "/new", "m0");
    sessions.builtin = [this](const std::string& id, const BuiltinCommandRequest&) {
        sessions.system(id, "LSP: clangd connected", "lsp_status");
        return BuiltinCommandResult{BuiltinCommandStatus::Accepted, "ok"};
    };
    send("1", "/l", "m1");
    EXPECT_TRUE(transport->wait_said("LSP: clangd connected"));
    ASSERT_EQ(sessions.builtins.size(), 1u);
    EXPECT_EQ(sessions.builtins[0].second.name, "lsp");
    send("1", "/sb off", "m2");
    ASSERT_EQ(sessions.builtins.size(), 2u);
    EXPECT_EQ(sessions.builtins[1].second.name, "sandbox");
    EXPECT_EQ(sessions.builtins[1].second.args, "off");
}

// 场景:内置命令输出很长(例如 /me list 列出大量记忆)。
// 期望:分页回复,第一页带页脚,/mr 看到后面的内容。
TEST_F(ChannelRouter, LongBuiltinOutputIsPaged) {
    send("1", "/new", "m0");
    std::string long_text;
    for (int i = 0; i < 300; ++i) long_text += "记忆条目 " + std::to_string(i) + "\n";
    sessions.builtin = [this, long_text](const std::string& id, const BuiltinCommandRequest&) {
        sessions.system(id, long_text, "memory_status");
        return BuiltinCommandResult{BuiltinCommandStatus::Accepted, "ok"};
    };
    send("1", "/me list", "m1");
    ASSERT_TRUE(transport->wait_said("记忆条目 0"));
    EXPECT_TRUE(transport->said("发送 /mr 看下一页"));
    EXPECT_FALSE(transport->said("记忆条目 299"));
    for (int i = 0; i < 10 && !transport->said("记忆条目 299"); ++i) send("1", "/mr", "more-" + std::to_string(i));
    EXPECT_TRUE(transport->wait_said("记忆条目 299"));
}

// 场景:/c 开始压缩(结果异步),之后会话先发「压缩中」再发「已压缩」(含很长的摘要),然后又自动压缩一次。
// 期望:命令立刻回复已开始;只转发第一条完成通知,且用简短文字代替摘要原文。
TEST_F(ChannelRouter, CompactRelaysCompletionOnce) {
    send("1", "/new", "m0");
    const auto id = bound("1");
    sessions.builtin = [](const std::string&, const BuiltinCommandRequest&) {
        return BuiltinCommandResult{BuiltinCommandStatus::Accepted, "queued"};
    };
    send("1", "/c", "m1");
    ASSERT_TRUE(transport->wait_said("已开始压缩对话上下文"));
    sessions.system(id, "Compacting conversation...", "context_compacting");
    sessions.system(id, "很长的摘要正文", "context_compacted");
    ASSERT_TRUE(transport->wait_said("已压缩对话上下文。"));
    sessions.system(id, "第二次摘要", "context_compacted");
    sessions.assistant(id, "后续回复");
    ASSERT_TRUE(transport->wait_said("后续回复"));
    int relayed = 0;
    for (const auto& text : transport->texts()) relayed += text == "已压缩对话上下文。";
    EXPECT_EQ(relayed, 1);
    EXPECT_FALSE(transport->said("很长的摘要正文"));
    EXPECT_FALSE(transport->said("Compacting conversation"));
}

// 场景:/g 设置目标(没有同步输出),/sw 收到当前 daemon 不支持的回复。
// 期望:参数原样交给 daemon;无输出时给出简短确认;不支持时如实告知。
TEST_F(ChannelRouter, BuiltinAcksAndUnsupportedCommands) {
    send("1", "/new", "m0");
    sessions.builtin = [](const std::string&, const BuiltinCommandRequest& request) {
        if (request.name == "swarm") return BuiltinCommandResult{BuiltinCommandStatus::UnsupportedCommand, ""};
        return BuiltinCommandResult{BuiltinCommandStatus::Accepted, "completed"};
    };
    send("1", "/g 修好登录", "m1");
    EXPECT_TRUE(transport->wait_said("已更新线程目标"));
    ASSERT_FALSE(sessions.builtins.empty());
    EXPECT_EQ(sessions.builtins[0].second.args, "修好登录");
    send("1", "/sw mesh", "m2");
    EXPECT_TRUE(transport->wait_said("当前 ACECode 不支持 /swarm"));
}

// 场景:已授权的普通联系人发送 /me(全局记忆属于机主)。
// 期望:拒绝且不调用 daemon。
TEST_F(ChannelRouter, MemoryIsOwnerOnly) {
    send("2", "你好", "m1");
    send("2", "/me", "m2");
    EXPECT_TRUE(transport->wait_said("只有机主可以管理记忆"));
    EXPECT_TRUE(sessions.builtins.empty());
}

// 场景:会话空闲时发 /t 补充说明,随后会话开始执行一个回合时再发。
// 期望:空闲时提示直接发消息;执行中按当前回合 id 加入补充说明,不新开回合。
TEST_F(ChannelRouter, TurnSteersOnlyTheRunningTurn) {
    send("1", "/new", "m0");
    const auto id = bound("1");
    send("1", "/t 先别改测试", "m1");
    EXPECT_TRUE(transport->wait_said("当前没有正在执行的回合"));
    EXPECT_TRUE(sessions.steers.empty());
    sessions.set_active_turn(id, "turn-9");
    send("1", "/t 先别改测试", "m2");
    ASSERT_EQ(sessions.steers.size(), 1u);
    EXPECT_EQ(sessions.steers[0].turn_id, "turn-9");
    EXPECT_EQ(sessions.steers[0].input.text, "先别改测试");
    EXPECT_TRUE(transport->wait_said("已把补充说明加入当前回合"));
    EXPECT_TRUE(sessions.input_log().empty());
}

// 场景:/b 旁问。
// 期望:在后台回答并发回 IM,不作为对话消息提交给会话。
TEST_F(ChannelRouter, SideQuestionIsAnsweredInBackground) {
    side = [](const std::string&, const std::string& question, SideChatCancellation&) {
        SideQuestionResult result;
        result.status = SideQuestionStatus::Ok;
        result.question = question;
        result.answer = "答案是 42";
        return result;
    };
    send("1", "/new", "m0");
    send("1", "/b 现在进度如何", "m1");
    EXPECT_TRUE(transport->wait_said("答案是 42"));
    EXPECT_TRUE(transport->said("旁问:现在进度如何"));
    EXPECT_TRUE(sessions.input_log().empty());
}

// 场景:旁问还在等模型时,通道断开(用户在设置页取消连接)。
// 期望:正在进行的旁问被取消,断开不会卡住等它答完,也不再往 IM 发回答。
// 回归:旁问若在平台工作线程上同步执行,断开连接要等到模型返回(可能几十秒)。
TEST_F(ChannelRouter, ShutdownCancelsRunningSideQuestion) {
    std::atomic<bool> started{false};
    side = [&started](const std::string&, const std::string&, SideChatCancellation& cancellation) {
        started = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!cancellation.aborted.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        SideQuestionResult result;
        result.status = SideQuestionStatus::Ok;
        result.answer = "不该出现的回答";
        return result;
    };
    send("1", "/new", "m0");
    send("1", "/b 慢问题", "m1");
    ASSERT_TRUE(wait_until([&] { return started.load(); }));
    const auto begin = std::chrono::steady_clock::now();
    conversations->shutdown();
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(3));
    EXPECT_FALSE(transport->said("不该出现的回答"));
}

} // namespace
} // namespace acecode::channels::core
