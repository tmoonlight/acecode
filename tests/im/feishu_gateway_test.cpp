#include <gtest/gtest.h>

#include "im/feishu/feishu_gateway.hpp"
#include "im/feishu/feishu_pacer.hpp"

// im/feishu/feishu_gateway:飞书长连接会话状态机(心跳、读超时、ACK、拆包重组、pong 配置)
// 与重连等待计算;im/feishu/feishu_pacer:发送节流。全部用假时间驱动,不碰网络。

namespace acecode::im::feishu {
namespace {

using Clock = LinkSession::Clock;
using std::chrono::milliseconds;
using std::chrono::seconds;

Frame event_frame(const std::string& payload, const std::string& id = "f1", int sum = 1, int seq = 0) {
    Frame frame;
    frame.seq_id = 7;
    frame.log_id = 9;
    frame.service = 1234;
    frame.method = kMethodData;
    frame.headers = {{"type", "event"}, {"message_id", id}, {"sum", std::to_string(sum)},
                     {"seq", std::to_string(seq)}, {"trace_id", "t"}, {"instance_id", "inst"}};
    frame.payload = payload;
    return frame;
}

Frame decode(const std::string& bytes) {
    Frame frame;
    EXPECT_TRUE(decode_frame(bytes, &frame, nullptr));
    return frame;
}

LinkSession connected_session(Clock::time_point t0, int ping_seconds = 10) {
    LinkSession session;
    ClientConfig config;
    config.ping_interval_s = ping_seconds;
    session.set_config(config);
    session.on_connected(1234, t0);
    return session;
}

// 场景:连上长连接后时间推进(心跳间隔 10 秒),期间服务端一直有回应。
// 期望:连上的第一个 tick 立即发心跳(service_id 正确),之后不到 10 秒不发,满 10 秒再发。
TEST(FeishuGateway, PingsImmediatelyThenEveryInterval) {
    const auto t0 = Clock::now();
    auto session = connected_session(t0);
    const auto first = session.on_tick(t0);
    ASSERT_TRUE(first.ping.has_value());
    EXPECT_EQ(*first.ping, encode_frame(make_ping_frame(1234)));
    EXPECT_FALSE(session.on_tick(t0 + seconds(5)).ping.has_value());
    session.on_message(encode_frame(Frame{}), t0 + seconds(6));  // 任意帧刷新读超时
    EXPECT_TRUE(session.on_tick(t0 + seconds(10)).ping.has_value());
}

// 场景:NAT 断开后 read 永远阻塞,心跳写入照样“成功”,但服务端不再有任何帧到达。
// 期望:超过 2 × 心跳间隔 + 5 秒(此处 25 秒)判定假死;期间收到任何帧都会重新计时。
TEST(FeishuGateway, DetectsSilentConnectionAfterReadTimeout) {
    const auto t0 = Clock::now();
    auto session = connected_session(t0);
    EXPECT_EQ(session.read_timeout(), milliseconds(25000));
    EXPECT_FALSE(session.on_tick(t0 + seconds(24)).dead);
    session.on_message(encode_frame(Frame{}), t0 + seconds(24));
    EXPECT_FALSE(session.on_tick(t0 + seconds(40)).dead);
    EXPECT_TRUE(session.on_tick(t0 + seconds(49)).dead);
}

// 场景:收到一个未拆包的事件帧。
// 期望:立即产出一个 ACK —— 回显原帧(SeqID/LogID/service/全部头,含 instance_id),追加 biz_rt,
// payload 为 {"code":200};同时把完整的事件 JSON 交给调用方。
TEST(FeishuGateway, AcksEventFramesAndEmitsPayload) {
    const auto t0 = Clock::now();
    auto session = connected_session(t0);
    const auto out = session.on_message(encode_frame(event_frame(R"({"schema":"2.0"})")), t0);
    ASSERT_EQ(out.frames.size(), 1u);
    ASSERT_EQ(out.events.size(), 1u);
    EXPECT_EQ(out.events[0], R"({"schema":"2.0"})");
    const auto ack = decode(out.frames[0]);
    EXPECT_EQ(ack.seq_id, 7u);
    EXPECT_EQ(ack.log_id, 9u);
    EXPECT_EQ(ack.service, 1234);
    EXPECT_EQ(ack.method, kMethodData);
    EXPECT_EQ(ack.header("instance_id"), "inst");
    EXPECT_EQ(ack.headers.back().first, "biz_rt");
    EXPECT_EQ(ack.payload.value_or(""), R"({"code":200})");
}

// 场景:一个事件被拆成 3 片乱序到达(seq 2、0、1)。
// 期望:前两片不回应、不产出事件;凑齐时按序号拼接出完整 JSON,只用凑齐的那一帧(seq=1)回 ACK。
TEST(FeishuGateway, ReassemblesSplitPayloadsOutOfOrder) {
    const auto t0 = Clock::now();
    auto session = connected_session(t0);
    EXPECT_TRUE(session.on_message(encode_frame(event_frame("C", "big", 3, 2)), t0).frames.empty());
    EXPECT_TRUE(session.on_message(encode_frame(event_frame("A", "big", 3, 0)), t0).events.empty());
    EXPECT_EQ(session.pending_sets(), 1u);
    const auto out = session.on_message(encode_frame(event_frame("B", "big", 3, 1)), t0 + milliseconds(100));
    ASSERT_EQ(out.events.size(), 1u);
    EXPECT_EQ(out.events[0], "ABC");
    ASSERT_EQ(out.frames.size(), 1u);
    EXPECT_EQ(decode(out.frames[0]).header("seq"), "1");
    EXPECT_EQ(session.pending_sets(), 0u);
}

// 场景:拆包的第二片在 6 秒后才到(缓存存活 5 秒,每片刷新)。
// 期望:第一片已过期被丢弃,第二片单独凑不齐,不产出事件也不回 ACK(平台会整体重投)。
TEST(FeishuGateway, ExpiresIncompleteSplitPayloads) {
    const auto t0 = Clock::now();
    auto session = connected_session(t0);
    session.on_message(encode_frame(event_frame("A", "m", 2, 0)), t0);
    const auto out = session.on_message(encode_frame(event_frame("B", "m", 2, 1)), t0 + seconds(6));
    EXPECT_TRUE(out.events.empty());
    EXPECT_TRUE(out.frames.empty());
}

// 场景:非法的拆包头(sum=0、seq 越界、sum 超过 64 片上限)。
// 期望:丢弃并给出日志警告,不 ACK、不产出事件、不分配缓存。
TEST(FeishuGateway, RejectsInvalidSplitHeaders) {
    const auto t0 = Clock::now();
    auto session = connected_session(t0);
    for (const auto& frame : {event_frame("x", "a", 0, 0), event_frame("x", "b", 2, 2), event_frame("x", "c", 65, 0)}) {
        const auto out = session.on_message(encode_frame(frame), t0);
        EXPECT_TRUE(out.frames.empty());
        EXPECT_TRUE(out.events.empty());
        EXPECT_FALSE(out.warning.empty());
    }
    EXPECT_EQ(session.pending_sets(), 0u);
}

// 场景:服务端 pong 带来新的 ClientConfig(心跳改为 30 秒);另有服务端 ping、旧版 card 帧、损坏的帧。
// 期望:pong 配置立即生效;服务端 ping 与 card 帧不回应;损坏的帧只产生警告。
TEST(FeishuGateway, AppliesPongConfigAndIgnoresOtherFrames) {
    const auto t0 = Clock::now();
    auto session = connected_session(t0);
    Frame pong;
    pong.service = 1234;
    pong.headers = {{"type", "pong"}};
    pong.payload = R"({"PingInterval":30,"ReconnectInterval":60})";
    const auto out = session.on_message(encode_frame(pong), t0);
    EXPECT_TRUE(out.config_updated);
    EXPECT_EQ(session.ping_interval(), milliseconds(30000));
    EXPECT_EQ(session.config().reconnect_interval_s, 60);

    Frame ping = make_ping_frame(1234);
    EXPECT_TRUE(session.on_message(encode_frame(ping), t0).frames.empty());
    auto card = event_frame("{}");
    card.headers[0].second = "card";
    const auto card_out = session.on_message(encode_frame(card), t0);
    EXPECT_TRUE(card_out.frames.empty());
    EXPECT_TRUE(card_out.events.empty());
    EXPECT_FALSE(session.on_message(std::string("\x42\x10", 2), t0).warning.empty());
}

// 场景:断开后 tick。
// 期望:不再发心跳也不判定假死(由重连流程接管)。
TEST(FeishuGateway, DisconnectedSessionIsQuiet) {
    const auto t0 = Clock::now();
    auto session = connected_session(t0);
    session.on_disconnected();
    const auto tick = session.on_tick(t0 + seconds(100));
    EXPECT_FALSE(tick.ping.has_value());
    EXPECT_FALSE(tick.dead);
}

// 场景:各种重连时机 —— 还没拿到服务端参数、刚掉线(随机抖动)、连续失败(服务端间隔)、本地退避更大。
// 期望:没有服务端参数时只用本地退避;刚掉线时为 jitter × ReconnectNonce;之后为 ReconnectInterval;
// 与本地退避取大。
TEST(FeishuGateway, ComputesReconnectDelay) {
    ClientConfig config;  // nonce 30 s,interval 120 s
    const std::vector<milliseconds> backoff{milliseconds(1000), milliseconds(5000), milliseconds(200000)};
    EXPECT_EQ(reconnect_delay(config, false, 0, backoff, 0.9), milliseconds(1000));
    EXPECT_EQ(reconnect_delay(config, false, 7, backoff, 0.9), milliseconds(200000));
    EXPECT_EQ(reconnect_delay(config, true, 0, backoff, 0.5), milliseconds(15000));
    EXPECT_EQ(reconnect_delay(config, true, 0, backoff, 0.0), milliseconds(1000));
    EXPECT_EQ(reconnect_delay(config, true, 1, backoff, 0.5), milliseconds(120000));
    EXPECT_EQ(reconnect_delay(config, true, 2, backoff, 0.5), milliseconds(200000));
    config.reconnect_interval_s = 0;
    config.reconnect_nonce_s = 0;
    EXPECT_EQ(reconnect_delay(config, true, 1, {milliseconds(50)}, 0.5), milliseconds(50));
}

// 场景:服务端 ReconnectCount 为 -1(无限)、3,以及还没拿到服务端参数。
// 期望:-1 与无服务端参数时永不停止;为 3 时连续失败 3 次后停止。
TEST(FeishuGateway, StopsAfterServerReconnectCount) {
    ClientConfig config;
    EXPECT_FALSE(reconnect_exhausted(config, true, 1000));
    config.reconnect_count = 3;
    EXPECT_FALSE(reconnect_exhausted(config, true, 2));
    EXPECT_TRUE(reconnect_exhausted(config, true, 3));
    EXPECT_FALSE(reconnect_exhausted(config, false, 3));
}

// 场景:同一接收方连续发三条,另一接收方发一条;随后第一个接收方被平台限流 2 秒。
// 期望:同一接收方相邻两条间隔 250 ms,不同接收方互不影响;限流期内的预约推迟到限流结束。
TEST(FeishuGateway, PacerSpacesSendsPerTarget) {
    SendPacer pacer(milliseconds(250));
    const auto t0 = Clock::now();
    EXPECT_EQ(pacer.reserve("oc_a", t0), t0);
    EXPECT_EQ(pacer.reserve("oc_a", t0), t0 + milliseconds(250));
    EXPECT_EQ(pacer.reserve("oc_a", t0), t0 + milliseconds(500));
    EXPECT_EQ(pacer.reserve("oc_b", t0), t0);
    pacer.penalize("oc_a", seconds(2), t0);
    EXPECT_EQ(pacer.reserve("oc_a", t0), t0 + seconds(2));
    EXPECT_EQ(pacer.tracked(), 2u);
}

} // namespace
} // namespace acecode::im::feishu
