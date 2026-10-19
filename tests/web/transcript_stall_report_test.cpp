// 覆盖 src/apps/web/handlers/transcript_stall_report.cpp:前端「实时状态卡住」诊断
// 上报(POST /api/diagnostics/transcript-stall)的白名单校验。这份上报会原样写进
// daemon 日志,所以只允许已知字段、字符串限字符集和长度、数值必须有限且非负;
// 任何一项不合规整条拒收,而不是挑出合规的部分记下来。

#include <gtest/gtest.h>

#include "web/handlers/transcript_stall_report.hpp"

#include <nlohmann/json.hpp>

#include <string>

using acecode::web::sanitize_transcript_stall_report;
using nlohmann::json;

namespace {

json full_report() {
    return json{
        {"kind", "stall_probe"},
        {"session_id", "20261019-101010-abcd"},
        {"reason", "server_idle"},
        {"action", "resync"},
        {"trigger", "visible"},
        {"client_epoch", "1b2c3d4e-0000-4000-8000-123456789abc"},
        {"server_epoch", "1b2c3d4e-0000-4000-8000-123456789abc"},
        {"client_active_turn_id", "turn-1"},
        {"ws_state", "open"},
        {"visibility", "visible"},
        {"ms_since_event", 21000},
        {"client_last_seq", 120},
        {"server_event_seq", 180},
        {"heartbeat_timeouts", 0},
        {"server_busy", false},
        {"server_active", true},
        {"client_running", true},
    };
}

} // namespace

// 场景:页面卡在「正在推理」,探针发现服务端已空闲,上报完整现场。
// 期望:全部白名单字段原样保留(数值转成整数),kind 必填且被保留。
TEST(TranscriptStallReport, KeepsAllowlistedFields) {
    const auto report = sanitize_transcript_stall_report(full_report());
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ((*report)["kind"], "stall_probe");
    EXPECT_EQ((*report)["reason"], "server_idle");
    EXPECT_EQ((*report)["server_event_seq"], 180);
    EXPECT_EQ((*report)["server_busy"], false);
    EXPECT_EQ((*report)["client_running"], true);
}

// 场景:客户端多带了不认识的字段(例如整段会话正文)。
// 期望:未知字段被丢弃,不进日志;其余字段照常保留,不因此拒收。
TEST(TranscriptStallReport, DropsUnknownFields) {
    auto body = full_report();
    body["transcript"] = "user secret text";
    const auto report = sanitize_transcript_stall_report(body);
    ASSERT_TRUE(report.has_value());
    EXPECT_FALSE(report->contains("transcript"));
    EXPECT_EQ((*report)["session_id"], "20261019-101010-abcd");
}

// 场景:心跳超时上报只有连接层的几个字段,没有会话 id。
// 期望:除 kind 外都是可选的,最小上报也被接受。
TEST(TranscriptStallReport, AcceptsMinimalHeartbeatReport) {
    const auto report = sanitize_transcript_stall_report(
        json{{"kind", "heartbeat_timeout"}, {"reason", "heartbeat_timeout"}, {"ws_ms_since_message", 31000}});
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->size(), 3u);
}

// 场景:kind 缺失或不是已知取值。
// 期望:拒收 —— 路由回 400,日志里不会出现来路不明的类别。
TEST(TranscriptStallReport, RejectsMissingOrUnknownKind) {
    auto body = full_report();
    body.erase("kind");
    EXPECT_FALSE(sanitize_transcript_stall_report(body).has_value());
    body["kind"] = "anything";
    EXPECT_FALSE(sanitize_transcript_stall_report(body).has_value());
    EXPECT_FALSE(sanitize_transcript_stall_report(json::array()).has_value());
}

// 场景:字符串里带换行 / 空格 / 超长,企图在日志里伪造新的一行。
// 期望:整条拒收,而不是截断或转义后记录。
TEST(TranscriptStallReport, RejectsLogInjectionInStrings) {
    auto body = full_report();
    body["session_id"] = "abc\n[ERROR] forged";
    EXPECT_FALSE(sanitize_transcript_stall_report(body).has_value());
    body = full_report();
    body["reason"] = "server idle";
    EXPECT_FALSE(sanitize_transcript_stall_report(body).has_value());
    body = full_report();
    body["ws_state"] = std::string(17, 'a');
    EXPECT_FALSE(sanitize_transcript_stall_report(body).has_value());
}

// 场景:数值字段为负、类型不对,或布尔字段传了字符串。
// 期望:拒收。null 视同未提供,不算错误(前端拿不到值时会省略或置空)。
TEST(TranscriptStallReport, RejectsInvalidNumbersAndBooleans) {
    auto body = full_report();
    body["client_last_seq"] = -1;
    EXPECT_FALSE(sanitize_transcript_stall_report(body).has_value());
    body = full_report();
    body["ms_since_event"] = "21000";
    EXPECT_FALSE(sanitize_transcript_stall_report(body).has_value());
    body = full_report();
    body["server_busy"] = "false";
    EXPECT_FALSE(sanitize_transcript_stall_report(body).has_value());
    body = full_report();
    body["server_event_seq"] = nullptr;
    const auto report = sanitize_transcript_stall_report(body);
    ASSERT_TRUE(report.has_value());
    EXPECT_FALSE(report->contains("server_event_seq"));
}
