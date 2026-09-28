// 覆盖 resume 回放对工具前言(openspec add-tool-preamble)的处理:
//   1. assistant 正文里的 <text_preamble> 标签回放时剥掉 —— 标签只在实时期间进
//      loading,落定后不显示(用户决定),落盘正文却保留原文(模型会模仿自己的
//      历史输出);整段都是标签的 assistant 消息不推正文行
//   2. metadata.tool_preamble 只是记录,不还原任何伪行(第一版的「● 标题」伪行
//      与参数版的 tool_call 行前言都已废)
//   3. 没有标签 / 没有 metadata 的老会话与改动前完全一致

#include <gtest/gtest.h>

#include "tui/resume/session_replay.hpp"
#include "tool/tool_executor.hpp"
#include "llm/llm_provider.hpp"
#include "tui/tui_state.hpp"

#include <nlohmann/json.hpp>

using acecode::ChatMessage;
using acecode::ToolExecutor;
using acecode::TuiState;
using acecode::replay_session_messages;

namespace {

nlohmann::json one_tool_call(const std::string& id,
                             const std::string& name,
                             const std::string& args_json) {
    nlohmann::json tc;
    tc["id"] = id;
    tc["type"] = "function";
    tc["function"]["name"] = name;
    tc["function"]["arguments"] = args_json;
    return nlohmann::json::array({tc});
}

ChatMessage assistant_with_tool(const std::string& content,
                                const nlohmann::json& preamble_metadata = nullptr) {
    ChatMessage m;
    m.role = "assistant";
    m.content = content;
    m.tool_calls = one_tool_call("call-1", "file_read", R"({"file_path":"a.txt"})");
    if (!preamble_metadata.is_null()) {
        m.metadata = {{"tool_preamble", preamble_metadata}};
    }
    return m;
}

ChatMessage tool_result(const std::string& content) {
    ChatMessage t;
    t.role = "tool";
    t.content = content;
    t.tool_call_id = "call-1";
    return t;
}

std::vector<std::string> roles_of(const std::vector<TuiState::Message>& rows) {
    std::vector<std::string> out;
    for (const auto& row : rows) out.push_back(row.role);
    return out;
}

} // namespace

// 场景:assistant 正文只有一个标签 "<text_preamble type=\"read\">Reading the loader</text_preamble>\n\n"
// + 1 个 tool_call,metadata 带 {source:prompt, title, kind:read},后跟结果。
// 期望:没有 assistant 正文行、没有任何前言伪行,只有 tool_call → tool_result。
TEST(SessionReplayToolPreamble, TagOnlyAssistantTextProducesNoRow) {
    ToolExecutor tools;
    const auto rows = replay_session_messages({
        assistant_with_tool(
            "<text_preamble type=\"read\">Reading the loader</text_preamble>\n\n",
            {{"source", "prompt"}, {"title", "Reading the loader"}, {"kind", "read"}}),
        tool_result("ok"),
    }, tools);
    EXPECT_EQ(roles_of(rows), (std::vector<std::string>{"tool_call", "tool_result"}));
}

// 场景:标签后面还有一句真正的正文 "Checking the loader first."(模型混用)。
// 期望:正文行只剩那句话,标签与紧跟的空行都不在里面。
TEST(SessionReplayToolPreamble, TagIsStrippedFromAssistantRow) {
    ToolExecutor tools;
    const auto rows = replay_session_messages({
        assistant_with_tool(
            "<text_preamble type=\"write\">Editing config</text_preamble>\n\nChecking the loader first.",
            {{"source", "prompt"}, {"title", "Editing config"}, {"kind", "write"}}),
        tool_result("ok"),
    }, tools);
    ASSERT_EQ(roles_of(rows), (std::vector<std::string>{
        "assistant", "tool_call", "tool_result"}));
    EXPECT_EQ(rows[0].content, "Checking the loader first.");
}

// 场景:reasoning 来源的 metadata(只有 title,没有标签)与没有 metadata 的老会话。
// 期望:两者都与改动前一致 —— assistant → tool_call → tool_result,不多不少。
TEST(SessionReplayToolPreamble, MetadataAloneAddsNoRows) {
    ToolExecutor tools;
    const auto reasoning = replay_session_messages({
        assistant_with_tool("Let me look.",
                            {{"source", "reasoning"}, {"title", "Reading registry sections"}}),
        tool_result("ok"),
    }, tools);
    EXPECT_EQ(roles_of(reasoning), (std::vector<std::string>{
        "assistant", "tool_call", "tool_result"}));
    EXPECT_EQ(reasoning[0].content, "Let me look.");

    const auto legacy = replay_session_messages({
        assistant_with_tool("Let me look."),
        tool_result("ok"),
    }, tools);
    EXPECT_EQ(roles_of(legacy), (std::vector<std::string>{
        "assistant", "tool_call", "tool_result"}));
}
