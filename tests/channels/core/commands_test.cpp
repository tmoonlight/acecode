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
    EXPECT_EQ(parse_command("/mode").kind, CommandKind::Mode);
    EXPECT_EQ(parse_command(" /MODE AUTO ").argument, "auto");
    EXPECT_EQ(parse_command("/mode accept-edits").argument, "auto");
    EXPECT_EQ(parse_command("/mode acceptEdits").argument, "auto");
    EXPECT_EQ(parse_command("/mode default").argument, "default");
    EXPECT_EQ(parse_command("/mode yolo").argument, "yolo");
    EXPECT_EQ(parse_command(" /YOLO ").kind, CommandKind::Yolo);
    EXPECT_EQ(parse_command("/effort").kind, CommandKind::Effort);
    EXPECT_EQ(parse_command(" /EFFORT HIGH ").argument, "high");
    EXPECT_EQ(parse_command("/effort default").argument, "default");
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
    EXPECT_EQ(parse_command("/yolo-extra").kind, CommandKind::None);
}

// 场景:命令缺参数或参数格式不对。
// 期望:返回中文用法提示,由调用方直接回复给用户。
TEST(ChannelCommands, ReportsUsageErrors) {
    EXPECT_FALSE(parse_command("/resume").error.empty());
    EXPECT_FALSE(parse_command("/approve").error.empty());
    EXPECT_FALSE(parse_command("/deny a b").error.empty());
    // /sessions 后面跟任意文字都是搜索关键词(旧版这里报用法错误)。
    EXPECT_TRUE(parse_command("/sessions everything").error.empty());
    EXPECT_TRUE(parse_command("/sessions all").error.empty());
    EXPECT_FALSE(parse_command("/mode invalid").error.empty());
    EXPECT_FALSE(parse_command("/mode auto yolo").error.empty());
    EXPECT_FALSE(parse_command("/yolo extra").error.empty());
    EXPECT_FALSE(parse_command("/effort high low").error.empty());
    EXPECT_FALSE(parse_command("/effort high\tlow").error.empty());
    EXPECT_TRUE(parse_command("/effort xhigh").error.empty());
}

// 场景:会话正在执行时收到各种命令。
// 期望:只有 /new 与 /resume 需要等待或先 /stop,其余命令立即处理。
TEST(ChannelCommands, OnlyNewAndResumeWaitForIdle) {
    EXPECT_FALSE(runs_while_busy(CommandKind::New));
    EXPECT_FALSE(runs_while_busy(CommandKind::Resume));
    for (const auto kind : {CommandKind::Help, CommandKind::Status, CommandKind::Stop, CommandKind::Sessions,
                            CommandKind::Model, CommandKind::Mode, CommandKind::Yolo, CommandKind::Effort,
                            CommandKind::Approve, CommandKind::Deny, CommandKind::Question})
        EXPECT_TRUE(runs_while_busy(kind));
}

// 场景:机主与普通联系人分别发送 /help。
// 期望:普通联系人的帮助额外说明只能切换自己创建的会话;权限提示附带 /approve 与 /deny 用法。
TEST(ChannelCommands, HelpAndPromptsAreChinese) {
    EXPECT_EQ(texts::help(true).find("只能在自己创建的会话"), std::string::npos);
    EXPECT_NE(texts::help(false).find("只能在自己创建的会话"), std::string::npos);
    for (const auto* command : {"/mode", "/yolo", "/effort"}) {
        EXPECT_NE(texts::help(true).find(command), std::string::npos);
        EXPECT_NE(texts::help(false).find(command), std::string::npos);
    }
    const auto prompt = texts::permission_prompt("p1", "bash", "{\"command\":\"ls\"}");
    EXPECT_NE(prompt.find("/approve p1"), std::string::npos);
    EXPECT_NE(prompt.find("/deny p1"), std::string::npos);
}

// 场景:用户在手机上用简写代替全名(/s、/r、/m、/pm……)。
// 期望:每个简写都解析成与全名相同的命令,参数照常带上;简写都不超过两个字母。
TEST(ChannelCommands, ShortAliasesMatchFullNames) {
    const std::pair<const char*, const char*> pairs[] = {
        {"/h", "/help"},     {"/st", "/status"},  {"/x", "/stop"},     {"/n", "/new"},
        {"/s", "/sessions"}, {"/r 3", "/resume 3"}, {"/m", "/model"},  {"/pm", "/mode"},
        {"/y", "/yolo"},     {"/e", "/effort"},   {"/a p1", "/approve p1"}, {"/d p1", "/deny p1"},
        {"/c", "/compact"},  {"/i", "/init"},     {"/g", "/goal"},     {"/p", "/plan"},
        {"/t 补充", "/turn 补充"}, {"/b 问题", "/btw 问题"}, {"/l", "/lsp"}, {"/sb", "/sandbox"},
        {"/me", "/memory"},  {"/sw", "/swarm"},   {"/sk", "/skills"},  {"/mr", "/more"},
        {"@r", "@ref"},
    };
    for (const auto& [shorthand, full] : pairs) {
        const auto a = parse_command(shorthand);
        const auto b = parse_command(full);
        EXPECT_NE(a.kind, CommandKind::None) << shorthand;
        EXPECT_EQ(a.kind, b.kind) << shorthand;
        EXPECT_EQ(a.argument, b.argument) << shorthand;
        const std::string name = std::string(shorthand).substr(0, std::string(shorthand).find(' '));
        EXPECT_LE(name.size(), 3u) << "简写超过两个字母:" << shorthand;
    }
    EXPECT_EQ(parse_command("/side 问题").kind, CommandKind::Side);
    EXPECT_EQ(parse_command("/ref 登录").kind, CommandKind::RefList);
}

// 场景:与 Desktop 同名的命令(/compact、/goal、/swarm 等)带参数或缺参数。
// 期望:参数原样交给 daemon 的内置命令;/turn 与 /btw 必须带内容,/swarm 只认 star|mesh|off,
// /compact、/init 不接受参数,/more 只接受页码。
TEST(ChannelCommands, DesktopCommandsValidateArguments) {
    EXPECT_EQ(parse_command("/goal 修好登录").argument, "修好登录");
    EXPECT_STREQ(builtin_command_name(parse_command("/goal").kind), "goal");
    EXPECT_STREQ(builtin_command_name(parse_command("/sb off").kind), "sandbox");
    EXPECT_STREQ(builtin_command_name(parse_command("/me list").kind), "memory");
    EXPECT_STREQ(builtin_command_name(parse_command("/sw mesh").kind), "swarm");
    EXPECT_STREQ(builtin_command_name(parse_command("/status").kind), "");
    EXPECT_EQ(parse_command("/SW MESH").argument, "mesh");
    EXPECT_FALSE(parse_command("/t").error.empty());
    EXPECT_FALSE(parse_command("/b").error.empty());
    EXPECT_FALSE(parse_command("/sw tree").error.empty());
    EXPECT_FALSE(parse_command("/c now").error.empty());
    EXPECT_FALSE(parse_command("/i x").error.empty());
    EXPECT_FALSE(parse_command("/mr next").error.empty());
    EXPECT_FALSE(parse_command("/mr 12345").error.empty());
    EXPECT_TRUE(parse_command("/mr 2").error.empty());
    EXPECT_TRUE(parse_command("/sk 审查").error.empty());
}

// 场景:@ref 的各种写法,以及形似但不是命令的文字(@refactor、@3)。
// 期望:@ref / @r / /ref 都是列出可引用会话,参数是关键词或页码;其它照普通文字处理。
TEST(ChannelCommands, RefListParsing) {
    EXPECT_EQ(parse_command("@ref").kind, CommandKind::RefList);
    EXPECT_EQ(parse_command("  @REF 登录 bug ").argument, "登录 bug");
    EXPECT_EQ(parse_command("@r 2").argument, "2");
    EXPECT_EQ(parse_command("@refactor 一下").kind, CommandKind::None);
    EXPECT_EQ(parse_command("@3 点开会").kind, CommandKind::None);
    EXPECT_EQ(parse_command("邮件发到 a@b.com").kind, CommandKind::None);
}

// 场景:消息正文里出现 @编号 与形似的写法;中文里常不加空格(「总结@1和@2」)。
// 期望:@ 前不是 ASCII 字母数字(或 . _ - @)、后面 1–3 位数字且再往后不是字母数字才算引用,
// 位置与编号准确;邮箱 a@163.com、x@4、@1234、@12ab 都不算(避免把普通文字误当成引用)。
TEST(ChannelCommands, FindsSessionRefMentions) {
    const auto mentions = find_session_ref_mentions("@1 和 @12,还有@3 a@4 x@163.com @1234 @12ab 总结@5和@6。");
    ASSERT_EQ(mentions.size(), 5u);
    EXPECT_EQ(mentions[0].index, 1u);
    EXPECT_EQ(mentions[0].offset, 0u);
    EXPECT_EQ(mentions[0].length, 2u);
    EXPECT_EQ(mentions[1].index, 12u);
    EXPECT_EQ(mentions[2].index, 3u);
    EXPECT_EQ(mentions[3].index, 5u);
    EXPECT_EQ(mentions[4].index, 6u);
    EXPECT_TRUE(find_session_ref_mentions("没有引用").empty());
}

// 场景:机主发送 /help。
// 期望:帮助里同时列出全名和简写,包括从 Desktop 搬来的命令与 @ref。
TEST(ChannelCommands, HelpListsShorthands) {
    const auto help = texts::help(true);
    for (const auto* item : {"/sessions(/s)", "/resume(/r)", "@ref(@r)", "/more(/mr)", "/compact(/c)",
                             "/btw(/b)", "/turn(/t)", "/skills(/sk)", "/memory(/me)", "/swarm(/sw)"})
        EXPECT_NE(help.find(item), std::string::npos) << item;
}

} // namespace
} // namespace acecode::channels::core
