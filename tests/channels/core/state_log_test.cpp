#include <gtest/gtest.h>

#include "channels/core/state_log.hpp"

#include <algorithm>

// channels/core/state_log:设置页只显示简要状态(2026-10-05 验收反馈),连接原因、隐私模式、
// 暂存待补发、机主、授权名单、待批准请求、会话绑定与收发计数都在快照变化时写进日志。
// 这里只测“前后两份快照 -> 日志行”的纯函数,不碰真实日志文件。

namespace acecode::channels::core {
namespace {

nlohmann::json telegram_snapshot() {
    return {
        {"platform", "telegram"},
        {"configured", true},
        {"enabled", true},
        {"state", "connected"},
        {"detail", ""},
        {"retry_stopped", false},
        {"account", "100"},
        {"display_name", "@ace_bot"},
        {"credential_hint", "****wxyz"},
        {"extra", {{"username", "ace_bot"}, {"privacy_mode", true}}},
        {"owner", "user:42"},
        {"owner_window", false},
        {"contacts", nlohmann::json::array({{{"principal", "user:42"}, {"kind", "user"}, {"name", "我"}, {"owner", true}}})},
        {"pending", nlohmann::json::array()},
        {"bindings", nlohmann::json::array({{{"key", "k1"},
                                             {"label", "Telegram 私聊"},
                                             {"chat", "42"},
                                             {"sender", "42"},
                                             {"session_id", "s-1"},
                                             {"no_workspace", false},
                                             {"cwd", "C:/work/项目A"},
                                             {"sent", 3},
                                             {"failed", 1},
                                             {"dropped", 0}}})},
    };
}

const StateLogLine* find_line(const std::vector<StateLogLine>& lines, const std::string& text) {
    const auto it = std::find_if(lines.begin(), lines.end(),
                                 [&text](const StateLogLine& line) { return line.text == text; });
    return it == lines.end() ? nullptr : &*it;
}

bool any_line_contains(const std::vector<StateLogLine>& lines, const std::string& needle) {
    return std::any_of(lines.begin(), lines.end(),
                       [&needle](const StateLogLine& line) { return line.text.find(needle) != std::string::npos; });
}

// 场景:daemon 启动后第一次发布 Telegram 快照(此前没有写过日志)。
// 期望:写出当前状态的摘要——状态与机器人名、附加信息、隐私模式的含义、机主、授权名单、
// 已有绑定及其收发计数;凭据尾号不出现在任何一行里。
TEST(ChannelStateLog, FirstPublishSummarizesCurrentState) {
    const auto lines = describe_state_changes(nullptr, telegram_snapshot());
    EXPECT_NE(find_line(lines, "state connected @ace_bot"), nullptr);
    EXPECT_TRUE(any_line_contains(lines, "status {"));
    EXPECT_TRUE(any_line_contains(lines, "\"privacy_mode\":true"));
    EXPECT_NE(find_line(lines, "group privacy mode on: groups deliver only @mentions, replies to the bot and commands"),
              nullptr);
    EXPECT_NE(find_line(lines, "owner user:42 (我)"), nullptr);
    EXPECT_NE(find_line(lines, "authorized user:42 (我)"), nullptr);
    EXPECT_NE(find_line(lines,
                        "binding Telegram 私聊 chat 42 -> session s-1 in C:/work/项目A (sent 3, failed 1, dropped 0)"),
              nullptr);
    EXPECT_FALSE(any_line_contains(lines, "wxyz"));
}

// 场景:同一份快照被连续发布两次(例如别的平台的绑定变化触发 publish_all)。
// 期望:第二次没有任何日志,避免每次推送都刷屏。
TEST(ChannelStateLog, UnchangedSnapshotWritesNothing) {
    const auto state = telegram_snapshot();
    EXPECT_TRUE(describe_state_changes(state, state).empty());
}

// 场景:连接中断后重试,随后鉴权失败停止重试。
// 期望:状态行写出前后状态与原因(设置页只显示“连接失败,详情见日志”),失败类状态用警告级别。
TEST(ChannelStateLog, StateChangesCarryTheReason) {
    const auto connected = telegram_snapshot();
    auto retrying = connected;
    retrying["state"] = "retrying";
    retrying["detail"] = "网络中断";
    auto lines = describe_state_changes(connected, retrying);
    const auto* line = find_line(lines, "state connected @ace_bot -> retrying @ace_bot: 网络中断");
    ASSERT_NE(line, nullptr);
    EXPECT_EQ(line->level, LogLevel::Warn);

    auto failed = retrying;
    failed["state"] = "failed";
    failed["retry_stopped"] = true;
    failed["detail"] = "token 无效或已被吊销";
    lines = describe_state_changes(retrying, failed);
    EXPECT_NE(find_line(lines, "state retrying @ace_bot: 网络中断 -> failed (retry stopped) @ace_bot: token 无效或已被吊销"),
              nullptr);

    auto standby = connected;
    standby["state"] = "standby";
    standby["display_name"] = "";
    standby["hosted_by_pid"] = 4321;
    lines = describe_state_changes(connected, standby);
    const auto* held = find_line(lines, "state connected @ace_bot -> standby (held by pid 4321)");
    ASSERT_NE(held, nullptr);
    EXPECT_EQ(held->level, LogLevel::Info);
}

// 场景:陌生人私聊产生待批准请求,机主批准后对方进入授权名单;之后撤销授权、机主被移除。
// 期望:请求出现与关闭、授权加入与移除、机主变化各写一行,带身份与展示名。
TEST(ChannelStateLog, AccessChangesAreLogged) {
    const auto base = telegram_snapshot();
    auto pending = base;
    pending["pending"] = nlohmann::json::array(
        {{{"id", "r1"}, {"kind", "user"}, {"name", "Alice"}, {"principal", "user:7"}, {"label", "Telegram 私聊"},
          {"expires_in_s", 600}}});
    auto lines = describe_state_changes(base, pending);
    EXPECT_NE(find_line(lines, "request r1 pending: user user:7 (Alice) via Telegram 私聊, expires in 600s"), nullptr);

    auto approved = base;
    approved["contacts"].push_back({{"principal", "user:7"}, {"kind", "user"}, {"name", "Alice"}, {"owner", false}});
    lines = describe_state_changes(pending, approved);
    EXPECT_NE(find_line(lines, "request r1 closed (approved, rejected or expired): user:7 (Alice)"), nullptr);
    EXPECT_NE(find_line(lines, "access granted: user:7 (Alice)"), nullptr);

    auto revoked = approved;
    revoked["owner"] = nullptr;
    revoked["contacts"] = nlohmann::json::array({approved["contacts"][1]});
    lines = describe_state_changes(approved, revoked);
    EXPECT_NE(find_line(lines, "access removed: user:42 (我)"), nullptr);
    EXPECT_NE(find_line(lines, "owner none"), nullptr);
}

// 场景:QQ 扫码后打开机主窗口;同一 IM 会话从会话 s-1 切到 s-2;投递失败、成功各一次;最后绑定被释放。
// 期望:窗口开合、换绑、失败(警告)、成功(调试级别)、释放各一行。
TEST(ChannelStateLog, OwnerWindowAndBindingChangesAreLogged) {
    const auto base = telegram_snapshot();
    auto window = base;
    window["owner_window"] = true;
    EXPECT_NE(find_line(describe_state_changes(base, window),
                        "owner window open: the first private chat within 10 minutes becomes the owner"),
              nullptr);

    auto moved = base;
    moved["bindings"][0]["session_id"] = "s-2";
    moved["bindings"][0]["no_workspace"] = true;
    moved["bindings"][0]["failed"] = 2;
    moved["bindings"][0]["sent"] = 4;
    const auto lines = describe_state_changes(base, moved);
    EXPECT_NE(find_line(lines, "binding Telegram 私聊 chat 42 -> session s-2 (no workspace)"), nullptr);
    const auto* failed = find_line(lines, "delivery to Telegram 私聊 chat 42 failed (sent 4, failed 2, dropped 0)");
    ASSERT_NE(failed, nullptr);
    EXPECT_EQ(failed->level, LogLevel::Warn);
    const auto* sent = find_line(lines, "delivered 1 to Telegram 私聊 chat 42 (sent 4, failed 2, dropped 0)");
    ASSERT_NE(sent, nullptr);
    EXPECT_EQ(sent->level, LogLevel::Dbg);

    auto released = base;
    released["bindings"] = nlohmann::json::array();
    EXPECT_NE(find_line(describe_state_changes(base, released),
                        "binding removed: Telegram 私聊 chat 42 (was session s-1 in C:/work/项目A)"),
              nullptr);
}

// 场景:更换 Telegram token;QQ 改了 AppID;QQ 有回复被平台拒收而暂存;Telegram 机器人被配置了 webhook。
// 期望:凭据只记“已更新”(QQ 附 AppID,它不是密钥),不写任何凭据片段;暂存数量与 webhook 用警告级别说明后果。
TEST(ChannelStateLog, CredentialsAndPlatformExtras) {
    const auto base = telegram_snapshot();
    auto rotated = base;
    rotated["credential_hint"] = "****abcd";
    auto lines = describe_state_changes(base, rotated);
    EXPECT_NE(find_line(lines, "credentials updated"), nullptr);
    EXPECT_FALSE(any_line_contains(lines, "abcd"));

    nlohmann::json qq{{"platform", "qq"}, {"configured", true}, {"state", "connected"}, {"app_id", "1020"},
                      {"credential_hint", "****9999"}, {"extra", {{"held", 0}}}};
    auto qq_next = qq;
    qq_next["app_id"] = "2030";
    qq_next["extra"]["held"] = 2;
    lines = describe_state_changes(qq, qq_next);
    EXPECT_NE(find_line(lines, "credentials updated (AppID 2030)"), nullptr);
    const auto* held =
        find_line(lines, "2 output(s) held after QQ refused them; they are resent before the contact's next message");
    ASSERT_NE(held, nullptr);
    EXPECT_EQ(held->level, LogLevel::Warn);
    EXPECT_FALSE(any_line_contains(lines, "9999"));

    auto webhook = base;
    webhook["extra"]["webhook"] = true;
    const auto* blocked = find_line(describe_state_changes(base, webhook),
                                    "a webhook is set for this bot; long polling stays blocked until it is removed in "
                                    "the connect dialog");
    ASSERT_NE(blocked, nullptr);
    EXPECT_EQ(blocked->level, LogLevel::Warn);
}

} // namespace
} // namespace acecode::channels::core
