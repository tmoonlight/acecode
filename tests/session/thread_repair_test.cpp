#include <gtest/gtest.h>

#include "session/compact_checkpoint.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/thread_repair.hpp"

#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace {

acecode::ChatMessage message(std::string role, std::string content) {
    acecode::ChatMessage item;
    item.role = std::move(role);
    item.content = std::move(content);
    return item;
}

nlohmann::json tool_call(const std::string& id) {
    return nlohmann::json{
        {"id", id},
        {"type", "function"},
        {"function", {
            {"name", "file_write"},
            {"arguments", R"({"path":"a.txt"})"},
        }},
    };
}

std::filesystem::path unique_cwd(const std::string& label) {
    auto cwd = std::filesystem::temp_directory_path() /
        ("acecode_thread_repair_" + label + "_" +
         std::to_string(std::random_device{}()));
    std::filesystem::create_directories(cwd);
    return cwd;
}

bool contains(const std::vector<acecode::ChatMessage>& messages,
              const std::string& text) {
    for (const auto& item : messages) {
        if (item.content.find(text) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST(ThreadRepair, PrunesWholeOldTurnAndPreservesCurrentInput) {
    const std::vector<acecode::ChatMessage> history{
        message("user", "old request"),
        message("assistant", "old answer"),
        message("user", "middle request"),
        message("assistant", "middle answer"),
        message("user", "current request"),
    };
    acecode::ThreadRepairOptions options;
    options.force_prune_one_group = true;
    options.target_tokens = 100000;

    const auto result = acecode::plan_thread_repair(history, options);

    ASSERT_EQ(result.status, acecode::ThreadRepairStatus::Repaired);
    EXPECT_EQ(result.pruned_groups, 1);
    EXPECT_EQ(result.pruned_messages, 2);
    EXPECT_FALSE(contains(result.replacement_history, "old request"));
    EXPECT_TRUE(contains(result.replacement_history, "middle request"));
    EXPECT_TRUE(contains(result.replacement_history, "current request"));
}

TEST(ThreadRepair, RecoversMissingToolResultWithoutReplayingTool) {
    auto assistant = message("assistant", "working");
    assistant.tool_calls = nlohmann::json::array({tool_call("call-1")});
    const std::vector<acecode::ChatMessage> history{
        message("user", "write it"),
        assistant,
        message("user", "continue"),
    };

    const auto result = acecode::plan_thread_repair(history, {});

    ASSERT_EQ(result.status, acecode::ThreadRepairStatus::Repaired);
    EXPECT_EQ(result.history_issues.synthesized_tool_results, 1u);
    ASSERT_EQ(result.replacement_history.size(), 4u);
    EXPECT_EQ(result.replacement_history[2].role, "tool");
    EXPECT_EQ(result.replacement_history[2].tool_call_id, "call-1");
    EXPECT_NE(result.replacement_history[2].content.find("outcome is unknown"),
              std::string::npos);
}

TEST(ThreadRepair, ReportsExhaustedInsteadOfTruncatingOnlyCurrentTurn) {
    acecode::ThreadRepairOptions options;
    options.force_prune_one_group = true;
    options.target_tokens = 1;

    const auto result = acecode::plan_thread_repair(
        {message("user", "current input must remain")}, options);

    EXPECT_EQ(result.status, acecode::ThreadRepairStatus::HistoryExhausted);
    EXPECT_TRUE(result.checkpoint.id.empty());
    ASSERT_EQ(result.replacement_history.size(), 1u);
    EXPECT_EQ(result.replacement_history[0].content,
              "current input must remain");
}

TEST(ThreadRepair, HealthyHistoryReportsNoChangeWithoutWritingCheckpoint) {
    const auto result = acecode::plan_thread_repair(
        {message("user", "request"), message("assistant", "answer")}, {});

    EXPECT_EQ(result.status, acecode::ThreadRepairStatus::NoChange);
    EXPECT_TRUE(result.checkpoint.id.empty());
    EXPECT_EQ(result.reason, "provider history is already consistent");
}

TEST(ThreadRepair, ApplyAppendsCheckpointWithoutRewritingTranscript) {
    const auto cwd = unique_cwd("append");
    const std::string cwd_string = cwd.string();
    const std::string project_dir =
        acecode::SessionStorage::get_project_dir(cwd_string);
    std::filesystem::remove_all(project_dir);

    {
        acecode::SessionManager manager;
        manager.start_session(cwd_string, "stub", "model");
        std::vector<acecode::ChatMessage> history{
            message("user", "old request"),
            message("assistant", "old answer"),
            message("user", "current request"),
        };
        for (const auto& item : history) manager.on_message(item);
        const std::string id = manager.current_session_id();

        acecode::ThreadRepairOptions options;
        options.trigger = "repair-test";
        options.force_prune_one_group = true;
        const auto result = acecode::apply_thread_repair(
            &manager, history, options);
        ASSERT_TRUE(result.repaired());

        const auto raw = acecode::SessionStorage::load_messages(
            acecode::SessionStorage::session_path(project_dir, id));
        ASSERT_EQ(raw.size(), 4u);
        EXPECT_EQ(raw[0].content, "old request");
        EXPECT_EQ(raw[1].content, "old answer");
        EXPECT_EQ(raw[2].content, "current request");
        EXPECT_TRUE(acecode::is_compact_checkpoint_message(raw[3]));
        const auto effective =
            acecode::reconstruct_effective_model_history(raw);
        ASSERT_EQ(effective.size(), 1u);
        EXPECT_EQ(effective[0].content, "current request");
        manager.finalize();
    }

    std::filesystem::remove_all(project_dir);
    std::filesystem::remove_all(cwd);
}
