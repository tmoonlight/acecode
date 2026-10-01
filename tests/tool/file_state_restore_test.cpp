#include "tool/file_state_restore.hpp"
#include "tool/mtime_tracker.hpp"
#include "session/compact_checkpoint.hpp"
#include "utils/uuid.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

namespace {
void append_edit(std::vector<acecode::ChatMessage>& messages, const std::string& path, int index) {
    const auto id = "edit-" + std::to_string(index);
    acecode::ChatMessage call;
    call.role = "assistant";
    call.tool_calls = nlohmann::json::array({{
        {"id", id}, {"function", {{"name", "file_edit"},
        {"arguments", nlohmann::json{{"file_path", path}}.dump()}}},
    }});
    messages.push_back(call);
    acecode::ChatMessage result;
    result.role = "tool";
    result.tool_call_id = id;
    result.content = "Edited file";
    messages.push_back(result);
}
}

TEST(FileStateRestore, EditsBeforeLatestCheckpointDoNotReadWorkspaceFiles) {
    std::vector<acecode::ChatMessage> messages;
    for (int i = 0; i < 2000; ++i) append_edit(messages, "synthetic-" + std::to_string(i % 20), i);
    acecode::CompactCheckpoint checkpoint;
    messages.push_back(acecode::encode_compact_checkpoint(checkpoint));
    EXPECT_EQ(acecode::restore_file_tool_state_from_messages(messages), 0u);
}

TEST(FileStateRestore, RepeatedEditsAndPathAliasesReadCurrentFileOnce) {
    const auto root = std::filesystem::temp_directory_path() / ("ace-restore-" + acecode::generate_uuid());
    std::filesystem::create_directories(root);
    const auto path = root / "sample.txt";
    std::ofstream(path) << "current";
    std::vector<acecode::ChatMessage> messages;
    messages.push_back(acecode::encode_compact_checkpoint({}));
    for (int i = 0; i < 50; ++i) append_edit(messages, i % 2 ? path.string() : "./sample.txt", i);
    EXPECT_EQ(acecode::restore_file_tool_state_from_messages(messages, root.string()), 1u);
    const auto check = acecode::MtimeTracker::instance().validate_read_baseline_for_edit(path.string(), "current");
    EXPECT_EQ(check.status, acecode::MtimeTracker::ReadBaselineStatus::Ok);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
