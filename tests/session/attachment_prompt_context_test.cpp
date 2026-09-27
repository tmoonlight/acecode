#include <gtest/gtest.h>

#include "session/attachment_prompt_context.hpp"
#include "llm/tool_protocol_names.hpp"

#include <string>

namespace {

acecode::AttachmentRecord base_record() {
    acecode::AttachmentRecord record;
    record.id = "att_reference";
    record.session_id = "session-reference";
    record.name = "notes.txt";
    record.kind = "file";
    record.mime_type = "text/plain";
    record.path = "C:/acecode/sessions/att_reference.txt";
    record.size_bytes = 321;
    return record;
}

} // namespace

TEST(AttachmentPromptContext, SnapshotOnlyAttachmentUsesSnapshotAsReadPath) {
    const auto record = base_record();

    const std::string text = acecode::file_attachment_reference_text(record);

    EXPECT_NE(text.find("[Attached file reference]"), std::string::npos);
    EXPECT_NE(text.find(R"("attachment_id": "att_reference")"),
              std::string::npos);
    EXPECT_NE(text.find(R"("snapshot_path": "C:/acecode/sessions/att_reference.txt")"),
              std::string::npos);
    EXPECT_NE(text.find(R"("read_path": "C:/acecode/sessions/att_reference.txt")"),
              std::string::npos);
    EXPECT_EQ(text.find("source_path"), std::string::npos);
    EXPECT_NE(text.find("not included in this message"), std::string::npos);
    EXPECT_NE(text.find("never modify it"), std::string::npos);
}

TEST(AttachmentPromptContext, SourceBackedAttachmentUsesSourceAndKeepsSnapshot) {
    auto record = base_record();
    record.metadata = {
        {"source_path", "D:/outside/source notes.txt"},
    };

    const std::string text = acecode::file_attachment_reference_text(record);

    ASSERT_TRUE(acecode::attachment_source_path(record).has_value());
    EXPECT_EQ(*acecode::attachment_source_path(record),
              "D:/outside/source notes.txt");
    EXPECT_NE(text.find(R"("source_path": "D:/outside/source notes.txt")"),
              std::string::npos);
    EXPECT_NE(text.find(R"("snapshot_path": "C:/acecode/sessions/att_reference.txt")"),
              std::string::npos);
    EXPECT_NE(text.find(R"("read_path": "D:/outside/source notes.txt")"),
              std::string::npos);
    EXPECT_NE(text.find("read `snapshot_path` instead"), std::string::npos);
    EXPECT_NE(text.find("never modify `snapshot_path`"), std::string::npos);
}

TEST(AttachmentPromptContext, SourceOnlyAttachmentHasNoSnapshotFallback) {
    auto record = base_record();
    record.path.clear();
    record.blob_url.clear();
    record.metadata = {
        {"source_path", "D:/outside/large.pdf"},
        {"storage", "source_reference"},
    };

    const std::string text = acecode::file_attachment_reference_text(record);

    EXPECT_NE(text.find(R"("source_path": "D:/outside/large.pdf")"),
              std::string::npos);
    EXPECT_NE(text.find(R"("read_path": "D:/outside/large.pdf")"),
              std::string::npos);
    EXPECT_EQ(text.find("snapshot_path"), std::string::npos);
    EXPECT_NE(text.find("no session snapshot was created"), std::string::npos);
    EXPECT_NE(text.find("only when the user asks"), std::string::npos);
}

namespace {

acecode::AttachmentRecord pasted_record(bool split) {
    auto record = base_record();
    record.id = "att_paste";
    record.name = "pasted-text.txt";
    record.path = "C:/acecode/sessions/att_paste.txt";
    record.size_bytes = 400000;
    nlohmann::json stats = {{"chars", 300000}, {"lines", 9220}};
    if (split) {
        stats["part"] = 1;
        stats["parts"] = 2;
    }
    record.metadata = {{"origin", "pasted_text"}, {"pasted_text", stats}};
    return record;
}

} // namespace

// 触发场景:Web 输入框里的大段粘贴(>= 128 KiB)被存成 text/plain 附件,
// 记录 metadata 带 origin=pasted_text 与 pasted_text={chars, lines[, part, parts]}。
// 期望行为:引用 JSON 标出 origin / lines / chars,分段时再标 part / parts,
// 并追加「这是用户粘贴的正文、通常需要先读」的说明(分段时写明 part k of n);
// 原先「只在需要时再读」那句不再出现(两句意思相反)。未分段的粘贴不输出 part。
TEST(AttachmentPromptContext, PastedTextReferenceNamesOriginAndLineCount) {
    acecode::ScopedModelToolNameMappings no_rewrite{
        acecode::ToolProtocolNameMappings{}};

    const std::string split =
        acecode::file_attachment_reference_text(pasted_record(true));

    EXPECT_NE(split.find(R"("origin": "pasted_text")"), std::string::npos);
    EXPECT_NE(split.find(R"("lines": 9220)"), std::string::npos);
    EXPECT_NE(split.find(R"("chars": 300000)"), std::string::npos);
    EXPECT_NE(split.find(R"("part": 1)"), std::string::npos);
    EXPECT_NE(split.find(R"("parts": 2)"), std::string::npos);
    EXPECT_NE(split.find(R"("read_path": "C:/acecode/sessions/att_paste.txt")"),
              std::string::npos);
    EXPECT_NE(split.find("This is text the user pasted into the message"
                         " (part 1 of 2); ACECode stored it as a file because"
                         " it is large."),
              std::string::npos);
    EXPECT_NE(split.find("The user's request usually depends on it: read"
                         " `read_path` with `file_read` before answering, in"
                         " line or byte windows if it is long."),
              std::string::npos);
    EXPECT_EQ(split.find("only when the task needs the contents"),
              std::string::npos);
    EXPECT_NE(split.find("never modify it"), std::string::npos);

    const std::string whole =
        acecode::file_attachment_reference_text(pasted_record(false));
    EXPECT_NE(whole.find(R"("lines": 9220)"), std::string::npos);
    EXPECT_EQ(whole.find(R"("part")"), std::string::npos);
    EXPECT_EQ(whole.find("(part "), std::string::npos);
    EXPECT_NE(whole.find("This is text the user pasted into the message;"),
              std::string::npos);
}

// 触发场景:开启「工具重写」,把 file_read 映射成模型侧名 read。
// 期望行为:粘贴文件引用里新增的那句用映射后的 read,整段不出现 file_read。
// 回归:工具名写死成 file_read 时,模型会去调用一个它工具表里并不存在的名字。
TEST(AttachmentPromptContext, PastedTextReferenceUsesMappedReadToolName) {
    acecode::ScopedModelToolNameMappings mapped{acecode::ToolProtocolNameMappings{
        acecode::ToolProtocolNameMapping{"file_read", "read"}}};

    const std::string text =
        acecode::file_attachment_reference_text(pasted_record(true));

    EXPECT_NE(text.find("read `read_path` with `read` before answering"),
              std::string::npos);
    EXPECT_EQ(text.find("file_read"), std::string::npos);
}

// 触发场景:普通附件(会话快照附件、带 source_path 的原文件引用附件)。
// 期望行为:输出与加入粘贴来源说明之前逐字节相同 —— 这段文本进 prompt,
// 任何变化都会改变模型行为,并打穿已有会话的 prompt cache 前缀。
TEST(AttachmentPromptContext, OrdinaryReferenceTextIsByteIdentical) {
    acecode::ScopedModelToolNameMappings no_rewrite{
        acecode::ToolProtocolNameMappings{}};

    const std::string snapshot_expected =
        "[Attached file reference]\n"
        "{\n"
        "  \"attachment_id\": \"att_reference\",\n"
        "  \"mime_type\": \"text/plain\",\n"
        "  \"name\": \"notes.txt\",\n"
        "  \"read_path\": \"C:/acecode/sessions/att_reference.txt\",\n"
        "  \"size_bytes\": 321,\n"
        "  \"snapshot_path\": \"C:/acecode/sessions/att_reference.txt\"\n"
        "}\n"
        "The file content is not included in this message. Read `read_path`"
        " with `file_read` or another suitable read-only inspection tool only"
        " when the task needs the contents. `snapshot_path` is the session"
        " copy; never modify it.";
    EXPECT_EQ(acecode::file_attachment_reference_text(base_record()),
              snapshot_expected);

    auto source_backed = base_record();
    source_backed.metadata = {{"source_path", "D:/outside/source notes.txt"}};
    const std::string source_expected =
        "[Attached file reference]\n"
        "{\n"
        "  \"attachment_id\": \"att_reference\",\n"
        "  \"mime_type\": \"text/plain\",\n"
        "  \"name\": \"notes.txt\",\n"
        "  \"read_path\": \"D:/outside/source notes.txt\",\n"
        "  \"size_bytes\": 321,\n"
        "  \"snapshot_path\": \"C:/acecode/sessions/att_reference.txt\",\n"
        "  \"source_path\": \"D:/outside/source notes.txt\"\n"
        "}\n"
        "The file content is not included in this message. Read `read_path`"
        " with `file_read` or another suitable read-only inspection tool only"
        " when the task needs the contents. If `source_path` is unavailable,"
        " read `snapshot_path` instead. Modify `source_path` only when the"
        " user asks to change the original file; never modify"
        " `snapshot_path`.";
    EXPECT_EQ(acecode::file_attachment_reference_text(source_backed),
              source_expected);
}
