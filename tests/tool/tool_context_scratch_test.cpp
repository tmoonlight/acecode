// 验证每轮文件变更摘要使用 ToolContext::scratch_dir 识别工作区临时目录，
// 不在展示层重复硬编码 `.acecode/tmp`。

#include <gtest/gtest.h>

#include "tool/tool_executor.hpp"
#include "utils/utf8_path.hpp"

#include <atomic>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

struct ScratchFixture {
    fs::path root;

    ScratchFixture() {
        static std::atomic<int> seq{0};
        root = fs::temp_directory_path() /
            ("acecode_tool_context_scratch_" + std::to_string(++seq));
        fs::create_directories(root);
    }

    ~ScratchFixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

acecode::ToolContext make_context(const ScratchFixture& fixture) {
    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(fixture.root);
    ctx.scratch_dir = acecode::path_to_utf8(
        fixture.root / ".acecode" / "tmp" / "session-current");
    return ctx;
}

acecode::ToolResult changed_file_result(const fs::path& path) {
    acecode::ToolResult result{"changed", true};
    acecode::ToolSummary summary;
    summary.object = acecode::path_to_utf8(path);
    result.summary = std::move(summary);
    result.hunks = std::vector<acecode::DiffHunk>{acecode::DiffHunk{}};
    return result;
}

} // namespace

TEST(ToolContextScratchPath, RecognizesWholeManagedTemporaryRoot) {
    ScratchFixture fixture;
    const auto scratch_root =
        fixture.root / ".acecode" / "tmp";
    fs::create_directories(scratch_root / "session-current");
    fs::create_directories(scratch_root / "session-other");

    const auto ctx = make_context(fixture);
    EXPECT_TRUE(ctx.is_workspace_scratch_path(acecode::path_to_utf8(
        scratch_root / "session-current" / "helper.py")));
    EXPECT_TRUE(ctx.is_workspace_scratch_path(acecode::path_to_utf8(
        scratch_root / "session-other" / "helper.ps1")));
    EXPECT_TRUE(ctx.is_workspace_scratch_path(
        ".acecode/tmp/session-current/relative.js"));
    EXPECT_FALSE(ctx.is_workspace_scratch_path(acecode::path_to_utf8(
        fixture.root / ".acecode" / "tmp-backup" / "keep.txt")));
    EXPECT_FALSE(ctx.is_workspace_scratch_path(acecode::path_to_utf8(
        fixture.root / "src" / "main.cpp")));
}

TEST(ToolContextScratchPath, MarksOnlySuccessfulStructuredScratchChanges) {
    ScratchFixture fixture;
    const auto ctx = make_context(fixture);
    const auto scratch_file =
        fixture.root / ".acecode" / "tmp" / "session-current" / "helper.py";

    auto scratch_result = changed_file_result(scratch_file);
    acecode::mark_workspace_scratch_change(scratch_result, ctx);
    EXPECT_TRUE(scratch_result.metadata.value(
        acecode::kExcludeFromTurnChangeSummaryMetadata, false));

    auto source_result = changed_file_result(fixture.root / "src" / "main.cpp");
    acecode::mark_workspace_scratch_change(source_result, ctx);
    EXPECT_FALSE(source_result.metadata.contains(
        acecode::kExcludeFromTurnChangeSummaryMetadata));

    auto failed_result = changed_file_result(scratch_file);
    failed_result.success = false;
    acecode::mark_workspace_scratch_change(failed_result, ctx);
    EXPECT_FALSE(failed_result.metadata.contains(
        acecode::kExcludeFromTurnChangeSummaryMetadata));
}

TEST(ToolContextScratchPath, ResolvesSupportedAliasesToActiveSessionScratch) {
    ScratchFixture fixture;
    const auto ctx = make_context(fixture);
    const auto expected = acecode::path_to_utf8(
        fixture.root / ".acecode" / "tmp" / "session-current" /
        "nested" / "helper.ps1");

    for (const std::string alias_path : {
             "%ACECODE_TMPDIR%\\nested\\helper.ps1",
             "$ACECODE_TMPDIR/nested/helper.ps1",
             "${ACECODE_TMPDIR}/nested/helper.ps1",
             "%acecode_tmpdir%/nested/helper.ps1",
         }) {
        const auto resolved = ctx.resolve_scratch_path_alias(alias_path);
        ASSERT_TRUE(resolved.success) << resolved.error;
        EXPECT_TRUE(resolved.used_alias);
        EXPECT_EQ(resolved.path, expected);
    }

    const std::string ordinary = acecode::path_to_utf8(
        fixture.root / "src" / "main.cpp");
    const auto unchanged = ctx.resolve_scratch_path_alias(ordinary);
    ASSERT_TRUE(unchanged.success);
    EXPECT_FALSE(unchanged.used_alias);
    EXPECT_EQ(unchanged.path, ordinary);
}

TEST(ToolContextScratchPath, RejectsUnavailableMisplacedAndEscapingAliases) {
    ScratchFixture fixture;
    const auto ctx = make_context(fixture);

    acecode::ToolContext unavailable = ctx;
    unavailable.scratch_dir.clear();
    const auto missing = unavailable.resolve_scratch_path_alias(
        "%ACECODE_TMPDIR%\\helper.ps1");
    EXPECT_FALSE(missing.success);
    EXPECT_NE(missing.error.find("unavailable"), std::string::npos);

    const auto embedded = ctx.resolve_scratch_path_alias(
        acecode::path_to_utf8(fixture.root) +
        "/%ACECODE_TMPDIR%/helper.ps1");
    EXPECT_FALSE(embedded.success);
    EXPECT_NE(embedded.error.find("first path component"), std::string::npos);

    const auto traversal = ctx.resolve_scratch_path_alias(
        "%ACECODE_TMPDIR%/nested/../../outside.txt");
    EXPECT_FALSE(traversal.success);
    EXPECT_NE(traversal.error.find("parent traversal"), std::string::npos);

    const auto missing_file = ctx.resolve_scratch_path_alias(
        "%ACECODE_TMPDIR%");
    EXPECT_FALSE(missing_file.success);
    EXPECT_NE(missing_file.error.find("file name"), std::string::npos);

    const auto dot_file = ctx.resolve_scratch_path_alias(
        "%ACECODE_TMPDIR%/.");
    EXPECT_FALSE(dot_file.success);
    EXPECT_NE(dot_file.error.find("identify a file"), std::string::npos);

    acecode::ToolContext relative_context;
    relative_context.cwd = "relative-workspace";
    relative_context.scratch_dir = ".acecode/tmp/session-relative";
    const auto relative_root = relative_context.resolve_scratch_path_alias(
        "%ACECODE_TMPDIR%/helper.txt");
    EXPECT_FALSE(relative_root.success);
    EXPECT_NE(relative_root.error.find("absolute scratch directory"),
              std::string::npos);

    EXPECT_TRUE(acecode::ToolContext::references_scratch_path_alias(
        "mkdir \"%ACECODE_TMPDIR%\""));
    EXPECT_TRUE(acecode::ToolContext::references_scratch_path_alias(
        "printf > \"$ACECODE_TMPDIR/out.txt\""));
    EXPECT_FALSE(acecode::ToolContext::references_scratch_path_alias(
        "echo ordinary"));
}
