#include "test_support/agent_loop/characterization_fixture.hpp"

#include "session/channel_delegate.hpp"
#include "session/session_storage.hpp"

#include <algorithm>
#include <set>

// 群聊代答会话(openspec add-channel-platform-plugins D5):AgentLoop 按会话 meta 的 channel_delegate
// 收窄工具并追加 <group-delegate> 系统提示;普通会话看不到 send_to_group_chat。

namespace {
using namespace acecode;
using namespace acecode_test::characterization;

std::set<std::string> names(const std::vector<ToolDef>& tools) {
    std::set<std::string> out;
    for (const auto& tool : tools) out.insert(tool.name);
    return out;
}

ChannelDelegate sample_delegate() {
    ChannelDelegate delegate;
    delegate.platform = "example";
    delegate.chat = "g1";
    delegate.chat_name = "项目群";
    delegate.user_name = "张三";
    delegate.prompt = "只回答排期问题";
    return delegate;
}

// 场景:会话绑定为某个群的代答会话,注册了读文件、bash 和发群工具。
// 期望:请求里只剩白名单内的工具(读文件、发群),bash 不出现;系统提示末尾带 <group-delegate>,
// 写明代表谁、哪个群和用户的代答约定。回归保护:群成员的发言进入会话后不能诱导执行命令。
TEST(AgentLoopChannelDelegate, DelegateSessionKeepsOnlyReadOnlyToolsAndGroupSender) {
    Isolation isolation;
    Harness h(isolation);
    h.tools.register_tool(h.probe("file_read", true));
    h.tools.register_tool(h.probe("bash", false));
    h.tools.register_tool(h.probe(kSendToGroupChatToolName, true));
    h.session->set_channel_delegate(sample_delegate());
    h.provider->push_text("不需要回复");
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("[群聊新消息] 群「项目群」"); }));

    const auto tools = names(h.provider->tools_for_turn(0));
    EXPECT_TRUE(tools.count("file_read"));
    EXPECT_TRUE(tools.count(kSendToGroupChatToolName));
    EXPECT_FALSE(tools.count("bash"));
    const auto messages = h.provider->messages_for_turn(0);
    ASSERT_FALSE(messages.empty());
    EXPECT_EQ(messages[0].role, "system");
    const auto& system = messages[0].content;
    EXPECT_NE(system.find("<group-delegate>"), std::string::npos);
    EXPECT_NE(system.find("「张三」"), std::string::npos);
    EXPECT_NE(system.find("群聊「项目群」"), std::string::npos);
    EXPECT_NE(system.find("只回答排期问题"), std::string::npos);
}

// 场景:普通会话注册了发群工具(daemon 对所有会话注册它)。
// 期望:模型侧工具表里没有 send_to_group_chat,其它工具照常;系统提示没有 <group-delegate>。
TEST(AgentLoopChannelDelegate, OrdinarySessionHidesGroupSender) {
    Isolation isolation;
    Harness h(isolation);
    h.tools.register_tool(h.probe("bash", false));
    h.tools.register_tool(h.probe(kSendToGroupChatToolName, true));
    h.provider->push_text("ok");
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("hello"); }));
    const auto tools = names(h.provider->tools_for_turn(0));
    EXPECT_TRUE(tools.count("bash"));
    EXPECT_FALSE(tools.count(kSendToGroupChatToolName));
    EXPECT_EQ(h.provider->messages_for_turn(0)[0].content.find("<group-delegate>"), std::string::npos);
}

// 场景:代答身份写进 meta 后读回;旧 meta 没有这个字段;字段不全。
// 期望:完整身份逐字段往返;缺失或不全时读成不活动(普通会话)。
TEST(ChannelDelegateMeta, RoundTripsAndToleratesLegacyMeta) {
    Isolation isolation;
    SessionMeta meta;
    meta.id = "s-1";
    meta.channel_delegate = sample_delegate();
    const auto path = acecode::path_to_utf8(isolation.directory.path / "s-1.meta.json");
    ASSERT_TRUE(SessionStorage::write_meta(path, meta));
    const auto read = SessionStorage::read_meta(path);
    EXPECT_EQ(read.channel_delegate, sample_delegate());

    EXPECT_FALSE(channel_delegate_from_json(nlohmann::json::object()).active());
    EXPECT_FALSE(channel_delegate_from_json({{"platform", "example"}}).active());
    EXPECT_EQ(build_channel_delegate_system_context(ChannelDelegate{}), "");
}
} // namespace
