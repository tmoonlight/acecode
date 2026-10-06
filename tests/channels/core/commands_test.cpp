#include <gtest/gtest.h>

#include "channels/core/commands.hpp"

// channels/core/commands:IM 内命令解析(纯逻辑)。识别的命令不进入对话;
// 不认识的 /xxx 当作普通文本(可能是技能命令)。

namespace acecode::channels::core {
namespace {

// 场景:用户发送各种已知命令,大小写混用、前后带空格、带参数。
// 期望:识别为对应命令,参数去掉两端空白;/start 等同 /help,/session 与 /models 是别名。
TEST(ChannelCommands, ParsesKnownCommands) {
    EXPECT_EQ(parse_command("/help").kind, CommandKind::Help);
    EXPECT_EQ(parse_command("/start").kind, CommandKind::Help);
    EXPECT_EQ(parse_command("  /STATUS  ").kind, CommandKind::Status);
    EXPECT_EQ(parse_command("/stop").kind, CommandKind::Stop);
    EXPECT_EQ(parse_command("/new").kind, CommandKind::New);
    EXPECT_EQ(parse_command("/session").kind, CommandKind::Sessions);
    const auto search = parse_command("/sessions search  登录 bug ");
    EXPECT_EQ(search.kind, CommandKind::Sessions);
    EXPECT_EQ(search.argument, "search  登录 bug");
    EXPECT_TRUE(search.error.empty());
    EXPECT_EQ(parse_command("/sessions more").kind, CommandKind::Sessions);
    const auto resume = parse_command("/resume 3");
    EXPECT_EQ(resume.kind, CommandKind::Resume);
    EXPECT_EQ(resume.argument, "3");
    EXPECT_EQ(parse_command("/models").kind, CommandKind::Model);
    EXPECT_EQ(parse_command("/model gpt-5").argument, "gpt-5");
    EXPECT_EQ(parse_command("/approve abc").kind, CommandKind::Approve);
    EXPECT_EQ(parse_command("/deny abc").kind, CommandKind::Deny);
    EXPECT_EQ(parse_command("/aq 1 2").kind, CommandKind::Question);
}

// 场景:普通文字与不认识的斜杠命令(例如技能命令 /review)。
// 期望:都不是通道命令,交给会话(技能命令随后按 Web 输入的规则展开)。
TEST(ChannelCommands, UnknownSlashTextIsPlainInput) {
    EXPECT_EQ(parse_command("你好").kind, CommandKind::None);
    EXPECT_EQ(parse_command("/review 这个文件").kind, CommandKind::None);
    EXPECT_EQ(parse_command("").kind, CommandKind::None);
    EXPECT_EQ(parse_command("/statusbar").kind, CommandKind::None);
}

// 场景:命令缺参数或参数格式不对。
// 期望:返回中文用法提示,由调用方直接回复给用户。
TEST(ChannelCommands, ReportsUsageErrors) {
    EXPECT_FALSE(parse_command("/resume").error.empty());
    EXPECT_FALSE(parse_command("/approve").error.empty());
    EXPECT_FALSE(parse_command("/deny a b").error.empty());
    EXPECT_FALSE(parse_command("/sessions everything").error.empty());
    EXPECT_TRUE(parse_command("/sessions all").error.empty());
}

// 场景:会话正在执行时收到各种命令。
// 期望:只有 /new 与 /resume 需要等待或先 /stop,其余命令立即处理。
TEST(ChannelCommands, OnlyNewAndResumeWaitForIdle) {
    EXPECT_FALSE(runs_while_busy(CommandKind::New));
    EXPECT_FALSE(runs_while_busy(CommandKind::Resume));
    for (const auto kind : {CommandKind::Help, CommandKind::Status, CommandKind::Stop, CommandKind::Sessions,
                            CommandKind::Model, CommandKind::Approve, CommandKind::Deny, CommandKind::Question})
        EXPECT_TRUE(runs_while_busy(kind));
}

// 场景:机主与普通联系人分别发送 /help。
// 期望:普通联系人的帮助额外说明只能切换自己创建的会话;权限提示附带 /approve 与 /deny 用法。
TEST(ChannelCommands, HelpAndPromptsAreChinese) {
    EXPECT_EQ(texts::help(true).find("只能在自己创建的会话"), std::string::npos);
    EXPECT_NE(texts::help(false).find("只能在自己创建的会话"), std::string::npos);
    const auto prompt = texts::permission_prompt("p1", "bash", "{\"command\":\"ls\"}");
    EXPECT_NE(prompt.find("/approve p1"), std::string::npos);
    EXPECT_NE(prompt.find("/deny p1"), std::string::npos);
}

} // namespace
} // namespace acecode::channels::core
