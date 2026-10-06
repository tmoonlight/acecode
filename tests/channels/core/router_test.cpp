#include <gtest/gtest.h>

#include "channels/core/conversations.hpp"
#include "test_support/channels/core_fakes.hpp"

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
        deps.expand_skill = [](const std::string&, const std::string& text) { return "[展开]" + text; };
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
// 期望:图片下载后导入会话附件,与文字一起提交(content_parts 含文字与图片,metadata 记录附件)。
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
    EXPECT_EQ(inputs[0].second.content_parts.size(), 2u);
    ASSERT_TRUE(inputs[0].second.metadata.contains("attachments"));
    EXPECT_EQ(inputs[0].second.metadata["attachments"].size(), 1u);
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

} // namespace
} // namespace acecode::channels::core
