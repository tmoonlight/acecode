#include <gtest/gtest.h>

#include "im/qqbot/qq_gateway.hpp"

// im/qqbot/qq_gateway:QQ 网关会话状态机。全部用假时间驱动,不碰网络。

namespace acecode::im::qqbot {
namespace {

using Clock = GatewaySession::Clock;
using std::chrono::milliseconds;

nlohmann::json parse(const std::string& text) { return nlohmann::json::parse(text); }

std::string hello(int interval_ms) {
    return nlohmann::json{{"op", 10}, {"d", {{"heartbeat_interval", interval_ms}}}}.dump();
}

std::string ready(int seq) {
    return nlohmann::json{{"op", 0}, {"t", "READY"}, {"s", seq},
                          {"d", {{"session_id", "S1"}, {"user", {{"id", "B1"}, {"username", "Bot"}}}}}}
        .dump();
}

// 场景:新连接收到 Hello(心跳间隔 10 秒)。
// 期望:回 Identify,带 "QQBot <令牌>" 与只订阅单聊/群 @ 的 intents;心跳按 80% 即 8 秒发送。
TEST(QqGateway, HelloTriggersIdentifyAndHeartbeatInterval) {
    GatewaySession session;
    const auto t0 = Clock::now();
    session.on_connected("tok", t0);
    const auto out = session.on_frame(hello(10000), t0);
    ASSERT_EQ(out.frames.size(), 1u);
    const auto identify = parse(out.frames[0]);
    EXPECT_EQ(identify["op"], 2);
    EXPECT_EQ(identify["d"]["token"], "QQBot tok");
    EXPECT_EQ(identify["d"]["intents"], kIntents);
    EXPECT_EQ(session.heartbeat_interval(), milliseconds(8000));
}

// 场景:收到 READY 与之后的一条单聊消息事件。
// 期望:READY 让连接可用并记下 session_id 与机器人名;消息事件作为派发交给上层;seq 跟着更新。
TEST(QqGateway, ReadyAndDispatchUpdateSessionAndSequence) {
    GatewaySession session;
    const auto t0 = Clock::now();
    session.on_connected("tok", t0);
    session.on_frame(hello(10000), t0);
    const auto r = session.on_frame(ready(1), t0);
    EXPECT_TRUE(r.ready);
    EXPECT_EQ(r.bot_name, "Bot");
    EXPECT_EQ(session.session_id(), "S1");
    const auto m = session.on_frame(
        nlohmann::json{{"op", 0}, {"t", "C2C_MESSAGE_CREATE"}, {"s", 2}, {"d", {{"id", "M1"}}}}.dump(), t0);
    ASSERT_EQ(m.dispatches.size(), 1u);
    EXPECT_EQ(m.dispatches[0].first, "C2C_MESSAGE_CREATE");
    EXPECT_EQ(session.last_seq(), 2);
}

// 场景:心跳到期前、到期时、以及发出心跳后一直没收到 ACK 又到下一次到期。
// 期望:到期前不发;到期发出带最新 seq 的心跳;没等到 ACK 再到期时判定连接假死;
// 收到 ACK 后恢复正常心跳。
TEST(QqGateway, HeartbeatScheduleAndZombieDetection) {
    GatewaySession session;
    const auto t0 = Clock::now();
    session.on_connected("tok", t0);
    session.on_frame(hello(10000), t0);
    session.on_frame(ready(5), t0);
    EXPECT_FALSE(session.on_tick(t0 + milliseconds(7000)).heartbeat);
    const auto beat = session.on_tick(t0 + milliseconds(8000));
    ASSERT_TRUE(beat.heartbeat);
    EXPECT_EQ(parse(*beat.heartbeat)["d"], 5);
    EXPECT_TRUE(session.on_tick(t0 + milliseconds(16000)).zombie);
    session.on_frame(R"({"op":11})", t0 + milliseconds(16000));
    EXPECT_TRUE(session.on_tick(t0 + milliseconds(16001)).heartbeat);
}

// 场景:普通断线(1006)后重连,再次收到 Hello。
// 期望:用 Resume(op 6)带上原 session_id 与 seq 恢复会话,而不是重新登录。
TEST(QqGateway, ResumesAfterOrdinaryDisconnect) {
    GatewaySession session;
    const auto t0 = Clock::now();
    session.on_connected("tok", t0);
    session.on_frame(hello(10000), t0);
    session.on_frame(ready(7), t0);
    EXPECT_EQ(session.on_closed(1006).action, CloseAction::Resume);
    session.on_connected("tok2", t0);
    const auto out = session.on_frame(hello(10000), t0);
    ASSERT_EQ(out.frames.size(), 1u);
    const auto resume = parse(out.frames[0]);
    EXPECT_EQ(resume["op"], 6);
    EXPECT_EQ(resume["d"]["session_id"], "S1");
    EXPECT_EQ(resume["d"]["seq"], 7);
    EXPECT_EQ(resume["d"]["token"], "QQBot tok2");
}

// 场景:因会话失效(4009)断开,或服务端发来 op 9(d=false)。
// 期望:会话被清掉,下一次连接重新 Identify。
TEST(QqGateway, InvalidSessionForcesFreshIdentify) {
    GatewaySession session;
    const auto t0 = Clock::now();
    session.on_connected("tok", t0);
    session.on_frame(hello(10000), t0);
    session.on_frame(ready(3), t0);
    EXPECT_EQ(session.on_closed(4009).action, CloseAction::Identify);
    EXPECT_FALSE(session.can_resume());

    session.on_connected("tok", t0);
    session.on_frame(hello(10000), t0);
    session.on_frame(ready(4), t0);
    const auto invalid = session.on_frame(R"({"op":9,"d":false})", t0);
    EXPECT_TRUE(invalid.reconnect);
    EXPECT_FALSE(session.can_resume());
}

// 场景:服务端发 op 7 要求重连;发 op 1 要求立即心跳。
// 期望:op 7 返回重连指令且保留会话;op 1 立即回一帧心跳。
TEST(QqGateway, ServerReconnectAndHeartbeatRequests) {
    GatewaySession session;
    const auto t0 = Clock::now();
    session.on_connected("tok", t0);
    session.on_frame(hello(10000), t0);
    session.on_frame(ready(2), t0);
    const auto reconnect = session.on_frame(R"({"op":7})", t0);
    EXPECT_TRUE(reconnect.reconnect);
    EXPECT_TRUE(session.can_resume());
    const auto beat = session.on_frame(R"({"op":1})", t0);
    ASSERT_EQ(beat.frames.size(), 1u);
    EXPECT_EQ(parse(beat.frames[0])["op"], 1);
}

} // namespace
} // namespace acecode::im::qqbot
