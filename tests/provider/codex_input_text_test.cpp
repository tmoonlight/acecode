#include <gtest/gtest.h>

#include "provider/codex_provider.hpp"
#include "llm/tool_protocol_names.hpp"
#include "session/request_context_record.hpp"

#include <string>
#include <vector>

namespace {

acecode::ChatMessage message(std::string role, std::string content) {
    acecode::ChatMessage out;
    out.role = std::move(role);
    out.content = std::move(content);
    return out;
}

nlohmann::json pasted_file_part() {
    return {
        {"type", "file"},
        {"attachment", {
            {"id", "att_paste"},
            {"session_id", "session-codex"},
            {"name", "pasted-text.txt"},
            {"kind", "file"},
            {"mime_type", "text/plain"},
            {"path", "C:/acecode/attachments/session-codex/att_paste.txt"},
            {"size_bytes", 400000},
            {"metadata", {
                {"origin", "pasted_text"},
                {"pasted_text", {{"chars", 300000}, {"lines", 9220}}},
            }},
        }},
    };
}

} // namespace

// 触发场景:Codex 模型下,用户只粘贴了一大段文本(>= 128 KiB 落成附件文件),
// 编辑器里什么都没打 —— user 消息 content 为空,content_parts 只有一个 file 部件;
// 另一条 user 消息既有文字又带文件,还有一个 attachment 元数据损坏的 file 部件。
// 期望行为:Codex 输入文本里出现这条 user 消息,带 [Attached file reference]、
// read_path 与粘贴来源说明;有文字的消息先写文字再写引用;损坏的部件输出
// 与 OpenAI provider 相同的 [Attached file unavailable: invalid metadata]。
// 回归:build_codex_input_text 只写 content,content 为空的消息整条跳过,
// 模型完全看不到用户粘贴的内容(也不知道有这么个文件可读)。
TEST(CodexInputText, FileOnlyUserMessageReachesCodexInput) {
    acecode::ScopedModelToolNameMappings no_rewrite{
        acecode::ToolProtocolNameMappings{}};

    auto file_only = message("user", "");
    file_only.content_parts = nlohmann::json::array({pasted_file_part()});
    auto with_text = message("user", "summarize it");
    with_text.content_parts = nlohmann::json::array({
        {{"type", "text"}, {"text", "summarize it"}},
        pasted_file_part(),
        {{"type", "file"}, {"attachment", "not an object"}},
    });

    const std::string text = acecode::codex_detail::build_codex_input_text(
        {file_only, message("assistant", "ok"), with_text});

    const std::string first_block =
        "### User\n[Attached file reference]\n";
    const auto first = text.find(first_block);
    ASSERT_NE(first, std::string::npos) << text;
    EXPECT_NE(text.find(
                  R"("read_path": "C:/acecode/attachments/session-codex/att_paste.txt")"),
              std::string::npos);
    EXPECT_NE(text.find(R"("origin": "pasted_text")"), std::string::npos);
    EXPECT_NE(text.find("This is text the user pasted into the message"),
              std::string::npos);

    const auto second = text.find("### User\nsummarize it\n\n[Attached file reference]\n");
    ASSERT_NE(second, std::string::npos) << text;
    EXPECT_LT(first, second);
    EXPECT_NE(text.find("[Attached file unavailable: invalid metadata]\n\n", second),
              std::string::npos);
    // 文字部件不重复输出:content 已经是这段文字。
    EXPECT_EQ(text.find("summarize it", second + 20), std::string::npos);
}

// 触发场景:没有任何 content_parts 的普通对话(含 meta 消息、空 assistant、
// 带 tool_call_id 的 tool 消息、带 tool_calls 的 assistant)。
// 期望行为:输出与支持文件部件之前逐字节相同。assistant / tool 消息即使带
// file 部件也不输出引用(只处理 user 消息,与方案一致)。
TEST(CodexInputText, PlainMessagesAreUnchanged) {
    auto meta = message("user", "hidden meta");
    meta.is_meta = true;
    auto assistant_call = message("assistant", "checking");
    assistant_call.tool_calls = nlohmann::json::array({
        {{"id", "call_1"}, {"type", "function"}},
    });
    auto tool = message("tool", "tool output");
    tool.tool_call_id = "call_1";
    tool.content_parts = nlohmann::json::array({pasted_file_part()});
    const std::vector<acecode::ChatMessage> messages{
        message("system", "sys"),
        meta,
        message("user", "hello"),
        message("assistant", ""),
        assistant_call,
        tool,
    };

    const std::string expected =
        "Continue this ACECode conversation. Preserve the user's latest "
        "request as the active task.\n\n"
        "### System\nsys\n\n"
        "### User\nhello\n\n"
        "### Assistant\nchecking\n\n"
        "Assistant tool calls:\n"
        R"([{"id":"call_1","type":"function"}])"
        "\n\n"
        "### Tool tool_call_id=call_1\ntool output\n\n";
    EXPECT_EQ(acecode::codex_detail::build_codex_input_text(messages), expected);
}

TEST(CodexInputText, PersistedRequestContextReachesModelWithoutBecomingUserSpeech) {
    auto snapshot = message("user", "frozen project instructions");
    snapshot.is_meta = true;
    snapshot.subtype = acecode::kRequestContextSnapshot;
    snapshot.metadata = {{"request_context_version", 1}};
    auto update = message("user", "updated plan instructions");
    update.is_meta = true;
    update.subtype = acecode::kRequestContextUpdate;
    update.metadata = {{"request_context_version", 1}};
    auto unsupported = snapshot;
    unsupported.content = "unrecognized internal record";
    unsupported.metadata["request_context_version"] = 999;
    auto checkpoint = message("system", "transcript-only checkpoint");
    checkpoint.is_meta = true;
    checkpoint.subtype = "compact_checkpoint";

    const auto text = acecode::codex_detail::build_codex_input_text({
        message("system", "base system"), snapshot, message("user", "real user request"),
        update, unsupported, checkpoint});
    const std::string expected =
        "Continue this ACECode conversation. Preserve the user's latest "
        "request as the active task.\n\n"
        "### System\nbase system\n\n"
        "### Context\nfrozen project instructions\n\n"
        "### User\nreal user request\n\n"
        "### Context\nupdated plan instructions\n\n";
    EXPECT_EQ(text, expected);
}
