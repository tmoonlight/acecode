#include "session/session_data_diagnostics.hpp"
#include "utils/uuid.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

namespace {
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
                                 ("ace-diagnostics-" + acecode::generate_uuid());
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    void write(const std::string& hash, const std::string& id, const std::string& content) {
        std::filesystem::create_directories(root / hash);
        std::ofstream(root / hash / (id + ".jsonl"), std::ios::binary) << content;
    }
};
}

TEST(SessionDataDiagnostics, SamplesOnlyFiveLargestInRegisteredWorkspaceWithoutContent) {
    Fixture fixture;
    const std::string hash = "0123456789abcdef";
    const std::string secret = "private-message-body-and-path";
    for (int i = 0; i < 7; ++i) {
        fixture.write(hash, "20260101-000000-000" + std::to_string(i),
                      "{\"role\":\"user\",\"content\":\"" + secret + std::string(i, 'x') + "\"}\n");
    }
    fixture.write("fedcba9876543210", "20260101-000000-0000", "unregistered");
    acecode::desktop::WorkspaceMeta workspace;
    workspace.hash = hash;
    workspace.name = "Visible name";
    workspace.cwd = "private-workspace-path";
    const auto report = acecode::diagnose_session_data(fixture.root.string(), {workspace});
    ASSERT_EQ(report["workspaces"].size(), 1u);
    const auto& row = report["workspaces"][0];
    EXPECT_EQ(row["session_count"], 7);
    ASSERT_EQ(row["samples"].size(), 5u);
    EXPECT_EQ(row["samples"][0]["session_id"], "20260101-000000-0006");
    EXPECT_EQ(row["samples"][0]["composition"]["user"]["fraction"], 1.0);
    EXPECT_EQ(row["samples"][0]["composition"]["user"]["records"], 1);
    EXPECT_EQ(report.dump().find(secret), std::string::npos);
    EXPECT_EQ(report.dump().find(workspace.cwd), std::string::npos);
    EXPECT_FALSE(report["cancelled"].get<bool>());
}

TEST(SessionDataDiagnostics, BodyKeywordsDoNotChangeCategoryAndPartialTailCountsBytes) {
    Fixture fixture;
    const std::string hash = "0123456789abcdef";
    const std::string raw = "{\"role\":\"user\",\"content\":\"compact_checkpoint file_checkpoint\"}";
    fixture.write(hash, "20260101-000000-0000", raw);
    acecode::desktop::WorkspaceMeta workspace;
    workspace.hash = hash;
    const auto result = acecode::diagnose_session_data(fixture.root.string(), {workspace});
    const auto& sample = result["workspaces"][0]["samples"][0];
    EXPECT_EQ(sample["sampled_bytes"], raw.size());
    EXPECT_EQ(sample["composition"]["user"]["bytes"], raw.size());
    EXPECT_EQ(sample["composition"]["compact_checkpoint"]["bytes"], 0);
}

TEST(SessionDataDiagnostics, CancellationStopsBeforeAnyTranscriptRead) {
    Fixture fixture;
    acecode::desktop::WorkspaceMeta workspace;
    workspace.hash = "0123456789abcdef";
    const auto result = acecode::diagnose_session_data(fixture.root.string(), {workspace}, [] { return true; });
    EXPECT_TRUE(result["cancelled"].get<bool>());
    EXPECT_TRUE(result["workspaces"].empty());
}
