#include <gtest/gtest.h>

#include "channels/core/projection.hpp"
#include "session/attachment_store.hpp"
#include "session/session_storage.hpp"
#include "test_support/channels/core_fakes.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

// channels/core/projection:一个绑定的出站投影。订阅会话事件,把助手文本、附件、
// 权限请求与提问发到 IM;发送在 RemoteControlHub 的后台线程进行,断言前等待。

namespace acecode::channels::core {
namespace {

using test::FakeSessions;
using test::FakeTransport;
using test::private_address;
using test::wait_until;

class ChannelOutbound : public ::testing::Test {
protected:
    void SetUp() override {
        home_dir = channels::test::temporary("outbound");
        std::filesystem::create_directories(home_dir);
        home = std::make_unique<channels::test::Home>(home_dir);
        sessions.add_session("s-1", cwd, "", true, "测试会话");
    }
    void TearDown() override {
        if (projection) projection->stop();
        projection.reset();
        home.reset();
        std::error_code ec;
        std::filesystem::remove_all(home_dir, ec);
    }
    void start(im::Capabilities caps = {}) {
        transport = std::make_shared<FakeTransport>("telegram", caps);
        slot->set(transport);
        BindingRecord record;
        record.address = private_address("1");
        record.session_id = "s-1";
        record.cwd = cwd;
        record.no_workspace = true;
        projection = std::make_shared<Projection>(
            record, ProjectionDeps{&sessions, slot, [this](const std::string&) { return pending; }});
        projection->start();
    }
    void user(const nlohmann::json& metadata = nlohmann::json::object()) {
        sessions.emit("s-1", SessionEventKind::Message, {{"role", "user"}, {"content", "问题"}, {"metadata", metadata}});
    }

    const std::string cwd = "C:/fake-no-workspace/s-1";
    std::filesystem::path home_dir;
    std::unique_ptr<channels::test::Home> home;
    FakeSessions sessions;
    std::shared_ptr<FakeTransport> transport;
    std::shared_ptr<TransportSlot> slot = std::make_shared<TransportSlot>();
    std::shared_ptr<Projection> projection;
    std::vector<nlohmann::json> pending;
};

// 场景:一个回合里先有工具调用过程,最后是助手答复。
// 期望:只发送助手文本,工具开始/进度不发送。
TEST_F(ChannelOutbound, SendsAssistantTextButNotToolProgress) {
    start();
    user();
    sessions.emit("s-1", SessionEventKind::ToolStart, {{"tool", "bash"}, {"args", {{"command", "ls"}}}});
    sessions.emit("s-1", SessionEventKind::ToolUpdate, {{"tool", "bash"}, {"partial", "a.txt"}});
    sessions.assistant("s-1", "最终答复");
    ASSERT_TRUE(transport->wait_said("最终答复"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(transport->texts(), std::vector<std::string>{"最终答复"});
}

// 场景:平台声明按回合合并输出(QQ),一个回合内产生两段助手文本。
// 期望:回合结束(忙碌变为空闲)前不发送;结束时合并成一条发出。
TEST_F(ChannelOutbound, BatchesTurnOutputWhenPlatformAsks) {
    im::Capabilities caps;
    caps.batch_turn_output = true;
    start(caps);
    user();
    sessions.assistant("s-1", "第一段");
    sessions.assistant("s-1", "第二段");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(transport->sent_count(), 0u);
    sessions.emit("s-1", SessionEventKind::BusyChanged, {{"busy", false}});
    ASSERT_TRUE(wait_until([&] { return transport->sent_count() == 1; }));
    EXPECT_EQ(transport->texts()[0], "第一段\n\n第二段");
}

// 场景:用户消息来自 IM(带回复上下文),随后在 Desktop 里输入一条(不带通道信息)。
// 期望:IM 那一回合的输出引用原消息;Desktop 输入那一回合的回复同样发到 IM,但不带引用。
TEST_F(ChannelOutbound, ReplyContextFollowsTheTriggeringMessage) {
    start();
    user({{"channel", {{"platform", "telegram"}, {"reply_context", {{"msg_id", "m1"}}}}}});
    sessions.assistant("s-1", "回复 IM");
    ASSERT_TRUE(transport->wait_said("回复 IM"));
    user();
    sessions.assistant("s-1", "回复 Desktop");
    ASSERT_TRUE(transport->wait_said("回复 Desktop"));
    for (const auto& sent : transport->sent()) {
        if (sent.text == "回复 IM") EXPECT_EQ(sent.context.value("msg_id", ""), "m1");
        if (sent.text == "回复 Desktop") EXPECT_TRUE(sent.context.empty());
    }
}

// 场景:投影启动前会话已有一个待确认的权限请求;之后又来一个请求并先在 Desktop 被处理。
// 期望:启动时补发已有请求的提示;Desktop 处理后 IM 收到已处理提示,该编号不能再被 IM 取走。
TEST_F(ChannelOutbound, PermissionPromptsAndFirstAnswerWins) {
    pending = {{{"request_id", "p0"}, {"tool", "file_write"}, {"args", {{"path", "a.txt"}}}}};
    start();
    ASSERT_TRUE(transport->wait_said("/approve p0"));
    sessions.emit("s-1", SessionEventKind::PermissionRequest, {{"request_id", "p1"}, {"tool", "bash"}});
    ASSERT_TRUE(transport->wait_said("/approve p1"));
    EXPECT_EQ(projection->pending_permission_ids(), (std::vector<std::string>{"p0", "p1"}));
    sessions.emit("s-1", SessionEventKind::PermissionClosed, {{"request_id", "p1"}, {"choice", "allow"}});
    EXPECT_TRUE(transport->wait_said("权限请求 p1 已处理"));
    EXPECT_FALSE(projection->take_permission("p1"));
    EXPECT_TRUE(projection->take_permission("p0"));
    // 已关闭的请求重放不会再次提示
    sessions.emit("s-1", SessionEventKind::PermissionRequest, {{"request_id", "p1"}, {"tool", "bash"}});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::size_t prompts = 0;
    for (const auto& text : transport->texts())
        if (text.find("/approve p1") != std::string::npos) ++prompts;
    EXPECT_EQ(prompts, 1u);
}

// 场景:会话发起 AskUserQuestion。
// 期望:按 /aq 语法发到 IM。
TEST_F(ChannelOutbound, QuestionsAreProjectedWithAqSyntax) {
    start();
    sessions.emit("s-1", SessionEventKind::QuestionRequest,
                  {{"request_id", "q1"},
                   {"questions",
                    {{{"id", "target"},
                      {"header", "部署"},
                      {"text", "部署到哪?"},
                      {"options", {{{"label", "测试"}}, {{"label", "正式"}}}}}}}});
    EXPECT_TRUE(transport->wait_said("/aq"));
    EXPECT_TRUE(transport->said("部署到哪?"));
}

// 场景:平台拒绝了一条输出,下一条正常。
// 期望:失败计入统计,不重发;后续输出照常发送。
TEST_F(ChannelOutbound, SendFailuresAreCountedNotRetried) {
    start();
    transport->fail_sends = 1;
    sessions.assistant("s-1", "会失败");
    ASSERT_TRUE(wait_until([&] { return projection->stats().outbound_failed == 1; }));
    sessions.assistant("s-1", "会成功");
    ASSERT_TRUE(transport->wait_said("会成功"));
    EXPECT_FALSE(transport->said("会失败"));
    EXPECT_EQ(projection->stats().outbound_failed, 1u);
}

// 场景:工具产出两个附件:一个在会话自己的附件目录里,一个是指向外部文件的引用;
// 同一个附件在一个回合里出现两次。
// 期望:前者以文件发送且只发一次;外部引用被拒绝并计入失败。
TEST_F(ChannelOutbound, AttachmentsAreRevalidatedBeforeSending) {
    start();
    const auto project = SessionStorage::get_project_dir(cwd);
    std::string error;
    const auto stored = save_attachment(project, "s-1", "out.png", "image/png", "png-bytes", &error);
    ASSERT_TRUE(stored.has_value()) << error;
    const auto outside = home_dir / "outside.txt";
    std::ofstream(outside, std::ios::binary) << "secret";
    const auto reference =
        save_attachment_reference(project, "s-1", "outside.txt", "text/plain", path_to_utf8(outside), &error);
    ASSERT_TRUE(reference.has_value()) << error;
    user();
    sessions.emit("s-1", SessionEventKind::ToolEnd,
                  {{"tool", "image_generate"},
                   {"attachments", {{{"id", stored->id}}, {{"id", stored->id}}, {{"id", reference->id}}}}});
    ASSERT_TRUE(wait_until([&] { return projection->stats().outbound_failed == 1; }));
    ASSERT_TRUE(wait_until([&] { return transport->sent_count() == 1; }));
    const auto sent = transport->sent();
    EXPECT_EQ(sent[0].file, "out.png");
}

// 场景:平台支持“正在输入”,会话进入忙碌后又回到空闲。
// 期望:忙碌时打开输入状态,空闲时关闭。
TEST_F(ChannelOutbound, TypingFollowsBusyState) {
    im::Capabilities caps;
    caps.supports_typing = true;
    start(caps);
    sessions.emit("s-1", SessionEventKind::BusyChanged, {{"busy", true}});
    sessions.emit("s-1", SessionEventKind::BusyChanged, {{"busy", false}});
    ASSERT_GE(transport->typing.size(), 2u);
    EXPECT_TRUE(transport->typing[0]);
    EXPECT_FALSE(transport->typing.back());
}

// 场景:投影被停止(绑定转移或通道关闭)后,会话又产生输出。
// 期望:已退订,不再发送任何东西。
TEST_F(ChannelOutbound, StoppedProjectionSendsNothing) {
    start();
    projection->stop();
    EXPECT_EQ(sessions.listener_count("s-1"), 0u);
    sessions.assistant("s-1", "停止之后");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(transport->sent_count(), 0u);
}

} // namespace
} // namespace acecode::channels::core
