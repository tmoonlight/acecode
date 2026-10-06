#include <gtest/gtest.h>

#include "im/dingtalk/dingtalk_protocol.hpp"
#include "im/dingtalk/dingtalk_stream.hpp"

// im/dingtalk/dingtalk_stream:Stream 帧解析、回执构造、断开指令、空闲检测与重连退避。
// 全部用假时间驱动,不碰网络。

namespace acecode::im::dingtalk {
namespace {

using Clock = StreamSession::Clock;
using std::chrono::milliseconds;

nlohmann::json parse(const std::string& text) { return nlohmann::json::parse(text); }

std::string callback_frame(const std::string& id, const std::string& topic, const nlohmann::json& data) {
    return nlohmann::json{{"specVersion", "1.0"},
                          {"type", "CALLBACK"},
                          {"headers", {{"messageId", id}, {"topic", topic}, {"contentType", "application/json"},
                                       {"time", "1690106592000"}}},
                          {"data", data.dump()}}
        .dump();
}

// 场景:帧头的值有的是字符串、有的是数字(协议文档示例里 time 是数字),data 是 JSON 字符串。
// 期望:type 统一为大写;topic / messageId 照常读出;数字头部不导致解析失败;data 保留原文。
TEST(DingTalkStream, ParsesFramesWithMixedHeaderTypes) {
    const auto frame = parse_stream_frame(
        R"({"specVersion":"1.0","type":"system","headers":{"topic":"ping","messageId":12345,"time":1690106592000},"data":"{\"opaque\":\"abc\"}"})");
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->type, "SYSTEM");
    EXPECT_EQ(frame->topic, "ping");
    EXPECT_EQ(frame->message_id, "12345");
    EXPECT_EQ(frame->data, R"({"opaque":"abc"})");
    EXPECT_FALSE(parse_stream_frame("not json"));
    EXPECT_FALSE(parse_stream_frame(R"({"headers":{}})"));
}

// 场景:平台发来 SYSTEM ping(带 opaque)。
// 期望:回一帧 code 200 的回执,messageId 相同,data 逐字回显(平台靠 opaque 对账);不派发任何消息。
TEST(DingTalkStream, PingIsEchoedVerbatim) {
    StreamSession session;
    const auto t0 = Clock::now();
    session.on_connected(t0);
    const auto out = session.on_frame(
        R"({"type":"SYSTEM","headers":{"topic":"ping","messageId":"p1","contentType":"application/json"},"data":"{\"opaque\":\"123-dsfs\"}"})",
        t0);
    ASSERT_EQ(out.replies.size(), 1u);
    const auto reply = parse(out.replies[0]);
    EXPECT_EQ(reply["code"], 200);
    EXPECT_EQ(reply["headers"]["messageId"], "p1");
    EXPECT_EQ(reply["headers"]["contentType"], "application/json");
    EXPECT_EQ(reply["data"], R"({"opaque":"123-dsfs"})");
    EXPECT_TRUE(out.messages.empty());
    EXPECT_FALSE(out.disconnect);
}

// 场景:收到机器人消息回调。
// 期望:先产生 code 200 的回执(data 为 {"response":null}),同时把消息交给上层;回执在派发之前构造好。
TEST(DingTalkStream, RobotCallbackIsAcknowledgedAndDispatched) {
    StreamSession session;
    const auto t0 = Clock::now();
    session.on_connected(t0);
    const auto out = session.on_frame(callback_frame("h1", kBotMessageTopic, {{"msgId", "m1"}}), t0);
    ASSERT_EQ(out.replies.size(), 1u);
    const auto reply = parse(out.replies[0]);
    EXPECT_EQ(reply["code"], 200);
    EXPECT_EQ(reply["headers"]["messageId"], "h1");
    EXPECT_EQ(reply["data"], kCallbackAckData);
    ASSERT_EQ(out.messages.size(), 1u);
    EXPECT_EQ(out.messages[0].message_id, "h1");
    EXPECT_EQ(parse(out.messages[0].data)["msgId"], "m1");
}

// 场景:本地待处理队列已满时又来了一条机器人消息。
// 期望:不回执、不派发(deferred=true),让平台约 60 秒后重投,而不是回执后丢掉。
TEST(DingTalkStream, FullQueueLeavesCallbackUnacknowledged) {
    StreamSession session;
    const auto t0 = Clock::now();
    session.on_connected(t0);
    const auto out = session.on_frame(callback_frame("h2", kBotMessageTopic, {{"msgId", "m2"}}), t0, false);
    EXPECT_TRUE(out.deferred);
    EXPECT_TRUE(out.replies.empty());
    EXPECT_TRUE(out.messages.empty());
}

// 场景:收到未订阅的回调主题,以及一个(未订阅却到达的)EVENT。
// 期望:未知回调主题回 404;EVENT 回 SUCCESS 防止平台反复重投;两者都不派发。
TEST(DingTalkStream, UnknownTopicsAreAnsweredButNotDispatched) {
    StreamSession session;
    const auto t0 = Clock::now();
    session.on_connected(t0);
    const auto unknown = session.on_frame(callback_frame("h3", "/v1.0/card/instances/callback", {}), t0);
    ASSERT_EQ(unknown.replies.size(), 1u);
    EXPECT_EQ(parse(unknown.replies[0])["code"], 404);
    EXPECT_TRUE(unknown.messages.empty());
    EXPECT_FALSE(unknown.ignored_topic.empty());

    const auto event = session.on_frame(
        R"({"type":"EVENT","headers":{"topic":"*","messageId":"e1","eventType":"user_add_org"},"data":"{}"})", t0);
    ASSERT_EQ(event.replies.size(), 1u);
    EXPECT_EQ(parse(event.replies[0])["data"], kEventAckData);
    EXPECT_TRUE(event.messages.empty());
}

// 场景:平台发来 SYSTEM disconnect;一次带 messageId,一次不带(协议允许缺省)。
// 期望:带 messageId 时先回执再要求断开;不带时不回执,只要求断开。
TEST(DingTalkStream, DisconnectRequestsReconnect) {
    StreamSession session;
    const auto t0 = Clock::now();
    session.on_connected(t0);
    const auto with_id = session.on_frame(
        R"({"type":"SYSTEM","headers":{"topic":"disconnect","messageId":"d1"},"data":"{\"reason\":\"connection is expired\"}"})",
        t0);
    EXPECT_TRUE(with_id.disconnect);
    ASSERT_EQ(with_id.replies.size(), 1u);
    EXPECT_EQ(parse(with_id.replies[0])["headers"]["messageId"], "d1");

    const auto without_id =
        session.on_frame(R"({"type":"SYSTEM","headers":{"topic":"disconnect"},"data":"{}"})", t0);
    EXPECT_TRUE(without_id.disconnect);
    EXPECT_TRUE(without_id.replies.empty());
}

// 场景:收到一帧无法解析的文本。
// 期望:标记 invalid,不回执、不派发;它仍算作连接存活的证据(帧计数增加)。
TEST(DingTalkStream, InvalidFrameIsReportedButKeepsLiveness) {
    StreamSession session(milliseconds(1000));
    const auto t0 = Clock::now();
    session.on_connected(t0);
    const auto out = session.on_frame("<<garbage>>", t0 + milliseconds(900));
    EXPECT_TRUE(out.invalid);
    EXPECT_TRUE(out.replies.empty());
    EXPECT_EQ(session.frames(), 1u);
    EXPECT_FALSE(session.idle_expired(t0 + milliseconds(1800)));
}

// 场景:空闲检测阈值 1 秒;连接后一直没有帧,然后收到一帧 ping。
// 期望:未满 1 秒不判定失效;满 1 秒判定失效;收到帧后从该时刻重新计时;阈值为 0 时永不判定。
TEST(DingTalkStream, IdleTimeoutTracksLastFrame) {
    StreamSession session(milliseconds(1000));
    const auto t0 = Clock::now();
    session.on_connected(t0);
    EXPECT_FALSE(session.idle_expired(t0 + milliseconds(999)));
    EXPECT_TRUE(session.idle_expired(t0 + milliseconds(1000)));
    session.on_frame(R"({"type":"SYSTEM","headers":{"topic":"ping","messageId":"p"},"data":"{}"})",
                     t0 + milliseconds(1500));
    EXPECT_FALSE(session.idle_expired(t0 + milliseconds(2000)));
    EXPECT_TRUE(session.idle_expired(t0 + milliseconds(2500)));

    StreamSession unlimited(milliseconds(0));
    unlimited.on_connected(t0);
    EXPECT_FALSE(unlimited.idle_expired(t0 + std::chrono::hours(5)));
}

// 场景:连续重连失败时的等待(基数 1 秒、上限 60 秒)。
// 期望:1、2、4…秒翻倍,抖动叠加在上面,总等待不超过上限。
TEST(DingTalkStream, BackoffDoublesUpToCap) {
    const milliseconds base(1000), cap(60000), none(0);
    EXPECT_EQ(stream_backoff(0, base, cap, none), milliseconds(1000));
    EXPECT_EQ(stream_backoff(1, base, cap, none), milliseconds(2000));
    EXPECT_EQ(stream_backoff(3, base, cap, none), milliseconds(8000));
    EXPECT_EQ(stream_backoff(10, base, cap, none), milliseconds(60000));
    EXPECT_EQ(stream_backoff(100, base, cap, none), milliseconds(60000));
    EXPECT_EQ(stream_backoff(1, base, cap, milliseconds(500)), milliseconds(2500));
    EXPECT_EQ(stream_backoff(10, base, cap, milliseconds(500)), milliseconds(60000));
}

} // namespace
} // namespace acecode::im::dingtalk
