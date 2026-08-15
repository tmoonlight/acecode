#include <gtest/gtest.h>

#include "session/attachment_prompt_context.hpp"

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
