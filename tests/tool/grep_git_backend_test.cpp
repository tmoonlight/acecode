#include <gtest/gtest.h>

#include "tool/grep_tool.hpp"
#include "utils/encoding.hpp"
#include "utils/utf8_path.hpp"
#include "worktree/worktree_manager.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace {

struct TempTree {
    fs::path path;

    TempTree() {
        path = fs::temp_directory_path() /
               ("acecode_git_grep_" + std::to_string(std::random_device{}()));
        fs::create_directories(path);
    }

    ~TempTree() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary);
    ASSERT_TRUE(ofs.is_open()) << acecode::path_to_utf8(path);
    ofs << content;
}

void initialize_git_repository(const fs::path& root) {
    auto result = acecode::worktree::run_git(
        {"init", "-q"}, acecode::path_to_utf8(root), 10000, true);
    ASSERT_TRUE(result.ok()) << result.err;
}

std::size_t count_result_lines(const std::string& output,
                               std::string_view path_prefix) {
    std::size_t count = 0;
    std::istringstream lines(output);
    for (std::string line; std::getline(lines, line);) {
        if (line.rfind(path_prefix, 0) == 0) ++count;
    }
    return count;
}

std::string find_result_line(const std::string& output,
                             std::string_view path_prefix) {
    std::istringstream lines(output);
    for (std::string line; std::getline(lines, line);) {
        if (line.rfind(path_prefix, 0) == 0) return line;
    }
    return {};
}

std::size_t utf8_char_count(std::string_view value) {
    return static_cast<std::size_t>(std::count_if(
        value.begin(), value.end(), [](unsigned char byte) {
            return (byte & 0xC0) != 0x80;
        }));
}

} // namespace

TEST(GrepGitBackendTest, SchemaDocumentsGitEreAndBoundedHeadLimit) {
    const auto tool = acecode::create_grep_tool();
    EXPECT_NE(tool.definition.description.find("Git extended regular expressions"),
              std::string::npos);
    const auto& head = tool.definition.parameters["properties"]["head_limit"];
    EXPECT_EQ(head["minimum"].get<int>(), 1);
    EXPECT_EQ(head["maximum"].get<int>(), 2000);
    EXPECT_NE(head["description"].get<std::string>().find("default 200"),
              std::string::npos);
}

TEST(GrepGitBackendTest, HonorsIgnoreRulesButSearchesExplicitIgnoredTargets) {
    TempTree tmp;
    initialize_git_repository(tmp.path);
    write_file(tmp.path / ".gitignore", "ignored/\n");
    write_file(tmp.path / "visible" / "visible.txt",
               "ignore-contract-needle visible\n");
    write_file(tmp.path / "ignored" / "hidden.txt",
               "ignore-contract-needle hidden\n");

    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(tmp.path);
    const auto tool = acecode::create_grep_tool();

    auto root_result = tool.execute(
        R"({"pattern":"ignore-contract-needle"})", ctx);
    ASSERT_TRUE(root_result.success) << root_result.output;
    EXPECT_NE(root_result.output.find("visible/visible.txt"), std::string::npos);
    EXPECT_EQ(root_result.output.find("ignored/hidden.txt"), std::string::npos);

    auto directory_result = tool.execute(nlohmann::json({
        {"pattern", "ignore-contract-needle"},
        {"path", acecode::path_to_utf8(tmp.path / "ignored")},
    }).dump(), ctx);
    ASSERT_TRUE(directory_result.success) << directory_result.output;
    EXPECT_NE(directory_result.output.find("hidden.txt:1:"), std::string::npos);

    auto file_result = tool.execute(nlohmann::json({
        {"pattern", "ignore-contract-needle"},
        {"path", acecode::path_to_utf8(tmp.path / "ignored" / "hidden.txt")},
    }).dump(), ctx);
    ASSERT_TRUE(file_result.success) << file_result.output;
    EXPECT_NE(file_result.output.find("hidden.txt:1:"), std::string::npos);
}

TEST(GrepGitBackendTest, ResolvesRelativePathAndMapsIncludePatternToPathspec) {
    TempTree tmp;
    write_file(tmp.path / "src" / "nested" / "keep.cpp",
               "relative-path-needle cpp\n");
    write_file(tmp.path / "src" / "nested" / "drop.txt",
               "relative-path-needle text\n");

    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(tmp.path);
    const auto result = acecode::create_grep_tool().execute(
        R"({"pattern":"relative-path-needle","path":"src","include_pattern":"*.cpp"})",
        ctx);

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_NE(result.output.find("nested/keep.cpp:1:"), std::string::npos);
    EXPECT_EQ(result.output.find("drop.txt"), std::string::npos);
}

TEST(GrepGitBackendTest, EnforcesDefaultExplicitAndHardHeadLimits) {
    TempTree tmp;
    std::ostringstream content;
    for (int line = 0; line < 2105; ++line) content << "needle\n";
    const fs::path file = tmp.path / "m";
    write_file(file, content.str());
    const auto tool = acecode::create_grep_tool();

    auto default_result = tool.execute(nlohmann::json({
        {"pattern", "needle"}, {"path", acecode::path_to_utf8(file)},
    }).dump(), acecode::ToolContext{});
    ASSERT_TRUE(default_result.success) << default_result.output;
    EXPECT_EQ(count_result_lines(default_result.output, "m:"), 200u);
    EXPECT_NE(default_result.output.find("Found at least 201 matching lines"),
              std::string::npos);

    auto explicit_result = tool.execute(nlohmann::json({
        {"pattern", "needle"}, {"path", acecode::path_to_utf8(file)},
        {"head_limit", 3},
    }).dump(), acecode::ToolContext{});
    ASSERT_TRUE(explicit_result.success) << explicit_result.output;
    EXPECT_EQ(count_result_lines(explicit_result.output, "m:"), 3u);

    auto hard_cap_result = tool.execute(nlohmann::json({
        {"pattern", "needle"}, {"path", acecode::path_to_utf8(file)},
        {"head_limit", 9999},
    }).dump(), acecode::ToolContext{});
    ASSERT_TRUE(hard_cap_result.success) << hard_cap_result.output;
    EXPECT_EQ(count_result_lines(hard_cap_result.output, "m:"), 2000u);
    EXPECT_LE(hard_cap_result.output.size(), 40000u);
}

TEST(GrepGitBackendTest, LimitsEachUtf8ResultLineToOneThousandCharacters) {
    TempTree tmp;
    const fs::path file = tmp.path / "unicode.log";
    std::string content = "needle-";
    for (int i = 0; i < 1200; ++i) content += u8"界";
    content.push_back('\n');
    write_file(file, content);

    auto result = acecode::create_grep_tool().execute(nlohmann::json({
        {"pattern", "needle"}, {"path", acecode::path_to_utf8(file)},
    }).dump(), acecode::ToolContext{});

    ASSERT_TRUE(result.success) << result.output;
    const std::string result_line = find_result_line(result.output, "unicode.log:");
    ASSERT_FALSE(result_line.empty()) << result.output;
    EXPECT_EQ(utf8_char_count(result_line), 1000u);
    EXPECT_NE(result_line.find("[truncated;"), std::string::npos);
    EXPECT_TRUE(acecode::is_valid_utf8(result.output));
}

TEST(GrepGitBackendTest, StopsAtRawStdoutBudgetForHugeMatchingLine) {
    TempTree tmp;
    const fs::path file = tmp.path / "huge.log";
    std::string content = "needle-";
    content.reserve(6 * 1024 * 1024 + 3);
    while (content.size() < 6 * 1024 * 1024) content += u8"界";
    write_file(file, content);
    ASSERT_GT(fs::file_size(file), 5000000u);

    auto result = acecode::create_grep_tool().execute(nlohmann::json({
        {"pattern", "needle"}, {"path", acecode::path_to_utf8(file)},
    }).dump(), acecode::ToolContext{});

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_NE(result.output.find(u8"needle-界界"), std::string::npos);
    EXPECT_NE(result.output.find("truncated at subprocess output limit"),
              std::string::npos);
    EXPECT_NE(result.output.find("[Results truncated by"), std::string::npos);
    EXPECT_LE(result.output.size(), 40000u);
    EXPECT_TRUE(acecode::is_valid_utf8(result.output));
}

TEST(GrepGitBackendTest, SupportsGitEreAndReportsNoMatchAndInvalidPattern) {
    TempTree tmp;
    const fs::path file = tmp.path / "plain.txt";
    write_file(file, "alpha42 beta\nordinary text\n");
    const auto tool = acecode::create_grep_tool();

    auto ere_match = tool.execute(nlohmann::json({
        {"pattern", "(alpha|omega)[0-9]+[[:space:]]beta"},
        {"path", acecode::path_to_utf8(file)},
    }).dump(), acecode::ToolContext{});
    ASSERT_TRUE(ere_match.success) << ere_match.output;
    EXPECT_NE(ere_match.output.find("plain.txt:1:alpha42 beta"),
              std::string::npos);

    auto no_match = tool.execute(nlohmann::json({
        {"pattern", "missing-needle"}, {"path", acecode::path_to_utf8(file)},
    }).dump(), acecode::ToolContext{});
    EXPECT_TRUE(no_match.success) << no_match.output;
    EXPECT_NE(no_match.output.find("No matches found"), std::string::npos);

    auto invalid = tool.execute(nlohmann::json({
        {"pattern", "("}, {"path", acecode::path_to_utf8(file)},
    }).dump(), acecode::ToolContext{});
    EXPECT_FALSE(invalid.success);
    EXPECT_NE(invalid.output.find("Invalid Git extended regex"),
              std::string::npos) << invalid.output;
}
