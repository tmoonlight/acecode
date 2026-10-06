#include <gtest/gtest.h>

#include "im/discord/discord_gateway.hpp"

// im/discord/discord_gateway:Discord 网关会话状态机与登录预算。全部用假时间驱动,不碰网络。

namespace acecode::im::discord {
namespace {

using Clock = GatewaySession::Clock;
using std::chrono::milliseconds;

nlohmann::json parse(const std::string& text) { return nlohmann::json::parse(text); }

std::string hello(int interval_ms) {
    return nlohmann::json{{"op", 10}, {"d", {{"heartbeat_interval", interval_ms}}}}.dump();
}

std::string ready(int seq) {
    return nlohmann::json{{"op", 0},
                          {"t", "READY"},
                          {"s", seq},
                          {"d",
                           {{"v", 10},
                            {"user", {{"id", "900"}, {"username", "AceBot"}, {"bot", true}}},
                            {"guilds", nlohmann::json::array({{{"id", "1"}, {"unavailable", true}},
                                                              {{"id", "2"}, {"unavailable", true}}})},
                            {"session_id", "S1"},
                            {"resume_gateway_url", "wss://resume.discord.gg"},
                            {"application", {{"id", "901"}, {"flags", 1 << 19}}}}}}
        .dump();
}

// 场景:新连接收到 Hello(心跳间隔 40 秒),本次连接的抖动系数为 0.5。
// 期望:立即回 Identify:token 是原始 token(不带 "Bot " 前缀)、intents=37377、properties 用新写法
// (os/browser/device,不带 $);首拍心跳在 40×0.5=20 秒时,之前不发。
TEST(DiscordGateway, HelloSendsIdentifyAndSchedulesJitteredFirstBeat) {
    GatewaySession session;
    const auto t0 = Clock::now();
    session.on_connected("raw-token", t0, 0.5);
    const auto out = session.on_frame(hello(40000), t0);
    ASSERT_EQ(out.frames.size(), 1u);
    EXPECT_TRUE(out.identified);
    const auto identify = parse(out.frames[0]);
    EXPECT_EQ(identify["op"], 2);
    EXPECT_EQ(identify["d"]["token"], "raw-token");
    EXPECT_EQ(identify["d"]["intents"], kIntents);
    EXPECT_EQ(identify["d"]["intents"], 37377);
    EXPECT_EQ(identify["d"]["properties"]["browser"], "acecode");
    EXPECT_FALSE(identify["d"]["properties"].contains("$os"));
    EXPECT_FALSE(identify["d"].contains("compress"));
    EXPECT_EQ(session.heartbeat_interval(), milliseconds(40000));
    EXPECT_FALSE(session.on_tick(t0 + milliseconds(19999)).heartbeat);
    EXPECT_TRUE(session.on_tick(t0 + milliseconds(20000)).heartbeat);
}

// 场景:收到 READY,随后一条 MESSAGE_CREATE。
// 期望:READY 让连接可用,记下 session_id、恢复地址、机器人 id/名、应用 id/flags、服务器数量;
// 消息事件作为派发交给上层(READY 本身不派发);seq 跟着更新,可以恢复。
TEST(DiscordGateway, ReadyRecordsSessionAndDispatchesEvents) {
    GatewaySession session;
    const auto t0 = Clock::now();
    session.on_connected("tok", t0, 0.0);
    session.on_frame(hello(40000), t0);
    const auto r = session.on_frame(ready(1), t0);
    EXPECT_TRUE(r.ready);
    EXPECT_FALSE(r.resumed);
    EXPECT_TRUE(r.dispatches.empty());
    ASSERT_TRUE(r.ready_info);
    EXPECT_EQ(r.ready_info->bot_id, "900");
    EXPECT_EQ(r.ready_info->bot_name, "AceBot");
    EXPECT_EQ(r.ready_info->application_id, "901");
    EXPECT_EQ(r.ready_info->application_flags, 1 << 19);
    EXPECT_EQ(r.ready_info->guild_count, 2u);
    EXPECT_EQ(session.session_id(), "S1");
    EXPECT_EQ(session.resume_url(), "wss://resume.discord.gg");
    const auto m = session.on_frame(
        nlohmann::json{{"op", 0}, {"t", "MESSAGE_CREATE"}, {"s", 2}, {"d", {{"id", "M1"}}}}.dump(), t0);
    ASSERT_EQ(m.dispatches.size(), 1u);
    EXPECT_EQ(m.dispatches[0].first, "MESSAGE_CREATE");
    EXPECT_EQ(session.last_seq(), 2);
    EXPECT_TRUE(session.can_resume());
}

// 场景:首拍心跳发出后一直没有 ACK,又到了下一拍;另一种情况是收到 ACK 后照常继续。
// 期望:心跳帧带最新 seq;没等到 ACK 就到期时判定假死(应以非 1000 关闭码断开并恢复);
// 收到 op 11 后恢复正常节奏。还没收到任何 Dispatch 时心跳的 d 为 null。
TEST(DiscordGateway, HeartbeatCarriesSequenceAndDetectsZombie) {
    GatewaySession session;
    const auto t0 = Clock::now();
    session.on_connected("tok", t0, 0.0);
    session.on_frame(hello(10000), t0);
    const auto first = session.on_tick(t0);
    ASSERT_TRUE(first.heartbeat);
    EXPECT_TRUE(parse(*first.heartbeat)["d"].is_null());
    session.on_frame(R"({"op":11})", t0);
    session.on_frame(ready(5), t0);
    const auto beat = session.on_tick(t0 + milliseconds(10000));
    ASSERT_TRUE(beat.heartbeat);
    EXPECT_EQ(parse(*beat.heartbeat)["d"], 5);
    EXPECT_TRUE(session.on_tick(t0 + milliseconds(20000)).zombie);
    session.on_frame(R"({"op":11})", t0 + milliseconds(20000));
    EXPECT_TRUE(session.on_tick(t0 + milliseconds(20001)).heartbeat);
}

// 场景:连上之后 20 秒(默认超时)还没收到 Hello。
// 期望:on_tick 报告 hello_timeout,传输层据此断开重连;收到 Hello 前不发心跳。
TEST(DiscordGateway, ReportsMissingHello) {
    GatewaySession session;
    const auto t0 = Clock::now();
    session.on_connected("tok", t0, 0.0);
    EXPECT_FALSE(session.on_tick(t0 + milliseconds(19000)).hello_timeout);
    const auto tick = session.on_tick(t0 + milliseconds(20000));
    EXPECT_TRUE(tick.hello_timeout);
    EXPECT_FALSE(tick.heartbeat);
}

// 场景:READY 之后普通断线(1006),重连后再次收到 Hello;以及恢复成功收到 RESUMED。
// 期望:用 Resume(op 6)带 token、原 session_id 与最新 seq,不发 Identify(不消耗登录预算);
// RESUMED 让连接可用且标记为恢复。
TEST(DiscordGateway, ResumesAfterAbnormalClose) {
    GatewaySession session;
    const auto t0 = Clock::now();
    session.on_connected("tok", t0, 0.0);
    session.on_frame(hello(40000), t0);
    session.on_frame(ready(7), t0);
    EXPECT_EQ(session.on_closed(1006).action, CloseAction::Resume);
    EXPECT_TRUE(session.can_resume());
    session.on_connected("tok", t0, 0.3);
    const auto out = session.on_frame(hello(40000), t0);
    ASSERT_EQ(out.frames.size(), 1u);
    EXPECT_FALSE(out.identified);
    const auto resume = parse(out.frames[0]);
    EXPECT_EQ(resume["op"], 6);
    EXPECT_EQ(resume["d"]["token"], "tok");
    EXPECT_EQ(resume["d"]["session_id"], "S1");
    EXPECT_EQ(resume["d"]["seq"], 7);
    const auto resumed = session.on_frame(R"({"op":0,"t":"RESUMED","s":8,"d":null})", t0);
    EXPECT_TRUE(resumed.ready);
    EXPECT_TRUE(resumed.resumed);
    EXPECT_FALSE(resumed.ready_info);
}

// 场景:服务端发 op 7(可能在 Hello 之前);发 op 1 要求立即心跳。
// 期望:op 7 返回重连指令并保留会话(之后 Resume);op 1 立即回一帧心跳。
TEST(DiscordGateway, ServerReconnectAndHeartbeatRequests) {
    GatewaySession session;
    const auto t0 = Clock::now();
    session.on_connected("tok", t0, 0.0);
    const auto early = session.on_frame(R"({"op":7,"d":null})", t0);
    EXPECT_TRUE(early.reconnect);
    session.on_frame(hello(40000), t0);
    session.on_frame(ready(2), t0);
    const auto reconnect = session.on_frame(R"({"op":7,"d":null})", t0);
    EXPECT_TRUE(reconnect.reconnect);
    EXPECT_FALSE(reconnect.invalid_session);
    EXPECT_TRUE(session.can_resume());
    const auto beat = session.on_frame(R"({"op":1,"d":null})", t0);
    ASSERT_EQ(beat.frames.size(), 1u);
    EXPECT_EQ(parse(beat.frames[0])["op"], 1);
    EXPECT_EQ(parse(beat.frames[0])["d"], 2);
}

// 场景:服务端发 op 9;d=true 表示还能恢复,d=false 表示会话作废(Resume 窗口过期也是这样)。
// 期望:d=true 只要求重连、会话保留;d=false 清掉会话(含恢复地址),标记 invalid_session,
// 下一次连接改发 Identify。
TEST(DiscordGateway, InvalidSessionKeepsOrClearsSession) {
    GatewaySession session;
    const auto t0 = Clock::now();
    session.on_connected("tok", t0, 0.0);
    session.on_frame(hello(40000), t0);
    session.on_frame(ready(3), t0);
    const auto resumable = session.on_frame(R"({"op":9,"d":true})", t0);
    EXPECT_TRUE(resumable.reconnect);
    EXPECT_FALSE(resumable.invalid_session);
    EXPECT_TRUE(session.can_resume());
    const auto dead = session.on_frame(R"({"op":9,"d":false})", t0);
    EXPECT_TRUE(dead.reconnect);
    EXPECT_TRUE(dead.invalid_session);
    EXPECT_FALSE(session.can_resume());
    EXPECT_TRUE(session.resume_url().empty());
    session.on_connected("tok", t0, 0.0);
    EXPECT_TRUE(session.on_frame(hello(40000), t0).identified);
}

// 场景:网关以各种关闭码断开。
// 期望:4004(token 无效)与 4010–4014 是致命错误,不再重连,4014 的原因指向开启 Message Content
// Intent;4003/4007/4009 要重新登录且清掉会话;4008 单独按频控处理;其余(1000/1001/1006/4000/
// 4001/4002/4005 以及未知码)按普通断线恢复。
TEST(DiscordGateway, ClassifiesCloseCodes) {
    for (int code : {4004, 4010, 4011, 4012, 4013, 4014})
        EXPECT_EQ(classify_close(code).action, CloseAction::Fatal) << code;
    EXPECT_NE(classify_close(4014).reason.find("Message Content Intent"), std::string::npos);
    EXPECT_NE(classify_close(4004).reason.find("Token"), std::string::npos);
    for (int code : {4003, 4007, 4009})
        EXPECT_EQ(classify_close(code).action, CloseAction::Identify) << code;
    EXPECT_EQ(classify_close(4008).action, CloseAction::RateLimited);
    for (int code : {1000, 1001, 1006, 4000, 4001, 4002, 4005, 4999})
        EXPECT_EQ(classify_close(code).action, CloseAction::Resume) << code;

    GatewaySession session;
    const auto t0 = Clock::now();
    session.on_connected("tok", t0, 0.0);
    session.on_frame(hello(40000), t0);
    session.on_frame(ready(4), t0);
    EXPECT_EQ(session.on_closed(4009).action, CloseAction::Identify);
    EXPECT_FALSE(session.can_resume());
}

// 场景:两次 Identify 之间,以及新会话连续失败(Identify 后没等到 READY)。
// 期望:两次 Identify 至少间隔 5 秒;失败后按 5s→10s→20s 指数退避,封顶 5 分钟;
// 收到 READY 后清零,回到 5 秒最小间隔。
TEST(DiscordGateway, IdentifyBudgetEnforcesIntervalAndBackoff) {
    IdentifyBudget budget;
    const std::int64_t t = 1'700'000'000'000;
    EXPECT_EQ(budget.next_allowed_ms(t), t);
    budget.record_identify(t);
    EXPECT_EQ(budget.next_allowed_ms(t), t + 5000);
    budget.record_failure();
    EXPECT_EQ(budget.next_allowed_ms(t), t + 5000);
    budget.record_failure();
    EXPECT_EQ(budget.next_allowed_ms(t), t + 10000);
    budget.record_failure();
    EXPECT_EQ(budget.next_allowed_ms(t), t + 20000);
    for (int i = 0; i < 20; ++i) budget.record_failure();
    EXPECT_EQ(budget.next_allowed_ms(t), t + 5 * 60 * 1000);
    budget.record_ready();
    EXPECT_EQ(budget.next_allowed_ms(t), t + 5000);
}

// 场景:24 小时窗口内 Identify 次数达到上限(测试里上限设为 3;真实值 900,硬上限 1000,
// 超出会被 Discord 重置 token)。
// 期望:达到上限后暂停到窗口结束;窗口过后重新计数。
TEST(DiscordGateway, IdentifyBudgetPausesAtDailyCap) {
    IdentifyBudget::Limits limits;
    limits.daily_cap = 3;
    IdentifyBudget budget(limits, 0, 0);
    const std::int64_t t = 1'700'000'000'000;
    for (int i = 0; i < 3; ++i) budget.record_identify(t + i * 10000);
    const auto now = t + 60000;
    EXPECT_TRUE(budget.daily_cap_reached(now));
    EXPECT_EQ(budget.next_allowed_ms(now), t + 24LL * 3600 * 1000);
    const auto later = t + 24LL * 3600 * 1000 + 1;
    EXPECT_FALSE(budget.daily_cap_reached(later));
    budget.record_identify(later);
    EXPECT_EQ(budget.count(), 1);
    EXPECT_EQ(budget.window_start_ms(), later);
}

// 场景:进程重启,从持久化的计数恢复(窗口开始于 1 小时前,已用 900 次);
// 另有 /gateway/bot 报告 session_start_limit.remaining=0,要求等 reset_after。
// 期望:恢复的计数立即生效(暂停到窗口结束);block_until 的时刻同样被遵守。
TEST(DiscordGateway, IdentifyBudgetHonoursPersistedCountAndStartLimit) {
    const std::int64_t now = 1'700'000'000'000;
    IdentifyBudget restored(IdentifyBudget::Limits{}, now - 3600 * 1000, 900);
    EXPECT_TRUE(restored.daily_cap_reached(now));
    EXPECT_EQ(restored.next_allowed_ms(now), now - 3600 * 1000 + 24LL * 3600 * 1000);

    IdentifyBudget blocked;
    blocked.block_until(now + 90000);
    EXPECT_EQ(blocked.next_allowed_ms(now), now + 90000);
    EXPECT_EQ(blocked.next_allowed_ms(now + 90000), now + 90000);
}

} // namespace
} // namespace acecode::im::discord
