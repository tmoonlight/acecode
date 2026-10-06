// 覆盖 SessionActivityState(桌面像素办公室读取的会话活动快照)的状态折叠规则:
//  1. 回合完成在事件环被挤掉后仍可读,新回合开始即唤醒;
//  2. 等待授权 / 等待回答优先于其它活动,全部关闭后回到工具执行;
//  3. 取消与失败不会被记成成功完成;
//  4. 不缓冲的进度事件与压缩完成同样进入快照;
//  5. 网状消息只保留身份、不保留正文,且有条数上限;
//  6. 流式正文让快照进入「撰写回复」并只保留有限尾巴;
//  7. 推理与正文各自成段,换阶段时旧尾巴清空;
//  8. 工具调用带一行调用预览,工具结束后预览清空。
#include <gtest/gtest.h>
#include "session/event_dispatcher.hpp"
#include "session/session_activity_state.hpp"

using namespace acecode;
using nlohmann::json;

// 场景:事件环容量只有 1,回合完成后又来了 5 个 token 把 Done 挤出环。
// 期望:快照仍报 outcome=completed / busy=false;下一回合 busy=true 后 outcome 清空。
TEST(SessionActivityState, CompletionSurvivesReplayEvictionAndNewTurnWakes) {
    EventDispatcher events(1);
    events.emit(SessionEventKind::BusyChanged, {{"busy", true}, {"turn_id", "first"}});
    events.emit(SessionEventKind::Done, {{"outcome", "completed"}});
    for (int i = 0; i < 5; ++i) events.emit(SessionEventKind::Token, {{"text", "x"}});
    EXPECT_EQ(events.activity_snapshot()["outcome"], "completed");
    EXPECT_FALSE(events.activity_snapshot()["busy"].get<bool>());
    events.emit(SessionEventKind::BusyChanged, {{"busy", true}, {"turn_id", "next"}});
    EXPECT_EQ(events.activity_snapshot()["outcome"], "");
    EXPECT_TRUE(events.activity_snapshot()["busy"].get<bool>());
}

// 场景:工具执行中先后来两个授权请求和一个提问,中间夹一条普通进度。
// 期望:只要还有未关闭的授权就是 permission_waiting,其次 question_waiting,
// 都关闭后回到 tool_running;工具结束后 tool 清空,输出正文不进快照。
TEST(SessionActivityState, PendingInteractionsOutrankOtherActivityUntilAllClose) {
    EventDispatcher events;
    events.emit(SessionEventKind::BusyChanged, {{"busy", true}});
    events.emit(SessionEventKind::ToolStart, {{"tool", "bash"}, {"tool_call_id", "t"}});
    events.emit(SessionEventKind::PermissionRequest, {{"request_id", "one"}});
    events.emit(SessionEventKind::PermissionRequest, {{"request_id", "two"}});
    events.emit(SessionEventKind::AgentProgress, {{"phase", "thinking"}, {"label", "Thinking"}});
    events.emit(SessionEventKind::PermissionClosed, {{"request_id", "one"}});
    EXPECT_EQ(events.activity_snapshot()["phase"], "permission_waiting");
    events.emit(SessionEventKind::QuestionRequest, {{"request_id", "q"}});
    events.emit(SessionEventKind::PermissionClosed, {{"request_id", "two"}});
    EXPECT_EQ(events.activity_snapshot()["phase"], "question_waiting");
    events.emit(SessionEventKind::QuestionClosed, {{"request_id", "q"}});
    EXPECT_EQ(events.activity_snapshot()["phase"], "tool_running");
    events.emit(SessionEventKind::ToolEnd, {{"tool", "bash"}, {"tool_call_id", "t"}, {"output", "private output"}});
    EXPECT_EQ(events.activity_snapshot()["tool"], "");
    EXPECT_EQ(events.activity_snapshot().dump().find("private output"), std::string::npos);
}

// 场景:回合以 aborted / error 结束,或 busy=false 却没有 outcome。
// 期望:outcome 原样保留为 aborted / error;没有 outcome 时保持空串,不能当成 completed。
TEST(SessionActivityState, CancellationAndFailureAreNeverSuccessfulCompletion) {
    for (const auto* outcome : {"aborted", "error"}) {
        EventDispatcher events;
        events.emit(SessionEventKind::BusyChanged, {{"busy", true}});
        events.emit(SessionEventKind::BusyChanged, {{"busy", false}, {"outcome", outcome}});
        EXPECT_EQ(events.activity_snapshot()["outcome"], outcome);
    }
    EventDispatcher events;
    events.emit(SessionEventKind::BusyChanged, {{"busy", false}});
    EXPECT_EQ(events.activity_snapshot()["outcome"], "");
}

// 场景:不进回放环的 compacting 进度,以及压缩通知先「未完成」后「完成」。
// 期望:phase=compacting 可见;只有 compact_notice_complete=true 才记 compact_id。
TEST(SessionActivityState, TransientProgressAndCompactionAreSnapshotState) {
    EventDispatcher events;
    EventDispatcher::EmitOptions transient;
    transient.buffered = false;
    events.emit(SessionEventKind::AgentProgress, {{"phase", "compacting"}}, transient);
    EXPECT_EQ(events.activity_snapshot()["phase"], "compacting");
    events.emit(SessionEventKind::Message, {{"metadata", {{"compact_notice_id", "c1"}, {"compact_notice_complete", false}}}});
    EXPECT_EQ(events.activity_snapshot()["compact_id"], "");
    events.emit(SessionEventKind::Message, {{"metadata", {{"compact_notice_id", "c1"}, {"compact_notice_complete", true}}}});
    EXPECT_EQ(events.activity_snapshot()["compact_id"], "c1");
}

// 场景:连续 20 条网状 agent 间消息,正文是私有内容。
// 期望:只保留最近 16 条的发送方 / 接收方身份,快照里找不到正文。
TEST(SessionActivityState, MeshTransfersRetainIdentityWithoutPrivateBodyAndAreBounded) {
    EventDispatcher events;
    for (int i = 0; i < 20; ++i) events.emit(SessionEventKind::Message, {
        {"content", "private content"},
        {"metadata", {{"inter_agent", {{"type", "MESSAGE"}, {"sender", "/root/a"},
            {"recipient", "/root/b"}, {"sender_session_id", "child-a"}}}}}});
    const auto snapshot = events.activity_snapshot();
    ASSERT_EQ(snapshot["transfers"].size(), 16u);
    EXPECT_EQ(snapshot["transfers"].back()["sender_session_id"], "child-a");
    EXPECT_EQ(snapshot.dump().find("private content"), std::string::npos);
}

// 场景:默认「用于编程」模式下模型开始输出正文 —— 这时服务端不会发 responding 进度,
// 只有一串 token。共推 400 个「字」(1200 字节),远超气泡需要的长度。
// 期望:快照 phase=responding,text 是最后 160 字节以内、以最后一个字结尾的合法 UTF-8;
// 进度里遗留的「正在等待模型响应」不再当作当前文案。
// 回归:修复前快照一直停在 model_waiting,办公室里正在飙字的 agent 只显示一个问号。
TEST(SessionActivityState, StreamingReplyIsVisibleWithBoundedTail) {
    EventDispatcher events;
    events.emit(SessionEventKind::BusyChanged, {{"busy", true}});
    events.emit(SessionEventKind::AgentProgress, {{"phase", "model_waiting"}, {"label", "正在等待模型响应"}});
    for (int i = 0; i < 399; ++i) events.emit(SessionEventKind::Token, {{"text", "字"}});
    events.emit(SessionEventKind::Token, {{"text", "尾"}});
    const auto snapshot = events.activity_snapshot();
    EXPECT_EQ(snapshot["phase"], "responding");
    EXPECT_EQ(snapshot["label"], "");
    const auto tail = snapshot["text"].get<std::string>();
    EXPECT_LE(tail.size(), 160u);
    EXPECT_GE(tail.size(), 150u);
    EXPECT_EQ(tail.size() % 3, 0u) << "每个汉字 3 字节,截断点必须落在字符边界";
    EXPECT_EQ(tail.substr(tail.size() - 3), "尾");
}

// 场景:先推理(reasoning 进度 + 推理片段),再开始输出正文。
// 期望:推理阶段 text 是推理尾巴;转入正文后旧推理内容清空,只剩正文。
TEST(SessionActivityState, ReasoningAndReplyAreSeparateBubbles) {
    EventDispatcher events;
    events.emit(SessionEventKind::BusyChanged, {{"busy", true}});
    events.emit(SessionEventKind::AgentProgress, {{"phase", "reasoning"}, {"label", "正在推理"}});
    events.emit(SessionEventKind::Reasoning, {{"text", "check config"}});
    EXPECT_EQ(events.activity_snapshot()["phase"], "reasoning");
    EXPECT_EQ(events.activity_snapshot()["text"], "check config");
    events.emit(SessionEventKind::Token, {{"text", "好的"}});
    EXPECT_EQ(events.activity_snapshot()["phase"], "responding");
    EXPECT_EQ(events.activity_snapshot()["text"], "好的");
}

// 场景:正文写到一半开始调用工具,工具带调用预览(display_override);随后工具结束。
// 期望:工具期间 phase=tool_running、detail=预览、text 为空(正文气泡结束);
// 工具结束后 tool / detail 都清空;没有 display_override 时退回 command_preview。
TEST(SessionActivityState, ToolCallCarriesOneLinePreviewUntilItEnds) {
    EventDispatcher events;
    events.emit(SessionEventKind::BusyChanged, {{"busy", true}});
    events.emit(SessionEventKind::Token, {{"text", "我先看看"}});
    events.emit(SessionEventKind::ToolStart, {{"tool", "file_read"}, {"tool_call_id", "a"},
        {"display_override", "src/main.cpp"}, {"args", {{"path", "secret-arg"}}}});
    auto snapshot = events.activity_snapshot();
    EXPECT_EQ(snapshot["phase"], "tool_running");
    EXPECT_EQ(snapshot["detail"], "src/main.cpp");
    EXPECT_EQ(snapshot["text"], "");
    EXPECT_EQ(snapshot.dump().find("secret-arg"), std::string::npos);
    events.emit(SessionEventKind::ToolEnd, {{"tool", "file_read"}, {"tool_call_id", "a"}});
    snapshot = events.activity_snapshot();
    EXPECT_EQ(snapshot["tool"], "");
    EXPECT_EQ(snapshot["detail"], "");
    events.emit(SessionEventKind::ToolStart, {{"tool", "bash"}, {"tool_call_id", "b"},
        {"display_override", ""}, {"command_preview", "npm test"}});
    EXPECT_EQ(events.activity_snapshot()["detail"], "npm test");
}
