// 覆盖 src/project_instructions/instructions_loader.{hpp,cpp}:
// - AGENTS.md native 文件和 CLAUDE.md fallback 都能被读到
// - 同层目录只选一个,按 filenames 优先级
// - 自定义 filenames 顺序生效
// - read_claude_md 开关正确剔除 fallback 文件名
// - 单文件 / 聚合 / 深度上限触发时有 truncated marker
// - 全局 ~/.acecode/ 文件被前置
// - 符号链接循环(至少跨平台的简化模型)不会无限递归

#include <gtest/gtest.h>

#include "project_instructions/instructions_loader.hpp"
#include "utils/encoding.hpp"
#include "utils/utf8_path.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr const char* kHomeEnvName = "USERPROFILE";
#else
constexpr const char* kHomeEnvName = "HOME";
#endif

void set_env(const char* n, const std::string& v) {
#ifdef _WIN32
    SetEnvironmentVariableW(acecode::utf8_to_wide(n).c_str(),
                            acecode::utf8_to_wide(v).c_str());
#else
    setenv(n, v.c_str(), 1);
#endif
}

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary);
    ofs << content;
}

class InstructionsLoaderTest : public ::testing::Test {
protected:
    fs::path temp_home;
    std::string prev_home;

    void SetUp() override {
        const char* e = std::getenv(kHomeEnvName);
        prev_home = e ? e : "";
        temp_home = fs::temp_directory_path() /
                    fs::path("acecode-instr-loader-" +
                             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(temp_home, ec);
        fs::create_directories(temp_home);
        set_env(kHomeEnvName, temp_home.string());
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_home, ec);
        set_env(kHomeEnvName, prev_home);
    }
};

} // namespace

// 场景:cwd 下有 AGENTS.md,cfg 默认时被加载
TEST_F(InstructionsLoaderTest, LoadsProjectAgentMd) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "AGENTS.md", "# repo rules\nuse goroutines\n");

    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_FALSE(merged.merged_body.empty());
    EXPECT_NE(merged.merged_body.find("goroutines"), std::string::npos);
    EXPECT_NE(merged.merged_body.find("AGENTS.md"), std::string::npos);
    EXPECT_EQ(merged.sources.size(), 1u);
}

// 场景:旧 ACECODE.md 不再是默认项目指令文件
TEST_F(InstructionsLoaderTest, AcecodeMdIgnoredByDefault) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "ACECODE.md", "# old ace rules\n");
    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_TRUE(merged.merged_body.empty());
    EXPECT_TRUE(merged.sources.empty());
}

// 场景:cwd 没有 AGENTS.md 只有 CLAUDE.md → 读到 CLAUDE.md
TEST_F(InstructionsLoaderTest, FallbacksToClaudeMd) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "CLAUDE.md", "# claude rules\n");
    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_NE(merged.merged_body.find("claude rules"), std::string::npos);
    EXPECT_NE(merged.merged_body.find("CLAUDE.md"), std::string::npos);
}

// 场景:同层目录同时有 AGENTS / CLAUDE / 旧 ACECODE 时,默认 filenames 优先级选 AGENTS
TEST_F(InstructionsLoaderTest, AgentBeatsLegacyFilesByDefault) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "ACECODE.md", "ace content\n");
    write_file(repo / "AGENTS.md", "agent content\n");
    write_file(repo / "CLAUDE.md", "claude content\n");

    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_NE(merged.merged_body.find("agent content"), std::string::npos);
    EXPECT_EQ(merged.merged_body.find("ace content"), std::string::npos);
    EXPECT_EQ(merged.merged_body.find("claude content"), std::string::npos);
    EXPECT_EQ(merged.sources.size(), 1u);
}

// 场景:cwd 下有 AGENTS.md(复数形式,外部工具惯例)时也能被加载
TEST_F(InstructionsLoaderTest, LoadsAgentsMd) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "AGENTS.md", "# agents rules\nuse agents\n");

    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_NE(merged.merged_body.find("agents rules"), std::string::npos);
    EXPECT_NE(merged.merged_body.find("AGENTS.md"), std::string::npos);
    EXPECT_EQ(merged.sources.size(), 1u);
}

// 场景:同一目录同时存在 AGENTS.md 与 AGENTS.md 时,默认优先级选 AGENTS.md(原生优先)
TEST_F(InstructionsLoaderTest, AgentMdBeatsAgentsMdByDefault) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "AGENTS.md", "agents content\n");
    write_file(repo / "AGENTS.md", "agent content\n");

    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_NE(merged.merged_body.find("agent content"), std::string::npos);
    EXPECT_EQ(merged.merged_body.find("agents content"), std::string::npos);
    EXPECT_EQ(merged.sources.size(), 1u);
}

// 场景:自定义 filenames 顺序把 CLAUDE.md 抬到首位
TEST_F(InstructionsLoaderTest, CustomFilenamesOrderOverridesDefault) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "AGENTS.md", "agent content\n");
    write_file(repo / "CLAUDE.md", "claude content\n");

    acecode::ProjectInstructionsConfig cfg;
    cfg.filenames = {"CLAUDE.md", "AGENTS.md"};
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_NE(merged.merged_body.find("claude content"), std::string::npos);
    EXPECT_EQ(merged.merged_body.find("agent content"), std::string::npos);
}

// 场景:read_claude_md=false 时 CLAUDE.md 被剔除
TEST_F(InstructionsLoaderTest, ReadClaudeMdGateDisabled) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "CLAUDE.md", "claude\n");
    acecode::ProjectInstructionsConfig cfg;
    cfg.read_claude_md = false;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_TRUE(merged.merged_body.empty());
    EXPECT_EQ(merged.sources.size(), 0u);
}

// 场景:cfg.enabled=false 时完全不读文件
TEST_F(InstructionsLoaderTest, DisabledProducesEmpty) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "AGENTS.md", "should not be read\n");
    acecode::ProjectInstructionsConfig cfg;
    cfg.enabled = false;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_TRUE(merged.merged_body.empty());
}

// 场景:~/.acecode/AGENTS.md 作为全局层被前置到项目级之前
TEST_F(InstructionsLoaderTest, GlobalAgentMdPrepended) {
    write_file(temp_home / ".acecode" / "AGENTS.md", "GLOBAL RULES\n");
    fs::path repo = temp_home / "repo";
    write_file(repo / "AGENTS.md", "PROJECT RULES\n");

    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);

    std::size_t gpos = merged.merged_body.find("GLOBAL RULES");
    std::size_t ppos = merged.merged_body.find("PROJECT RULES");
    ASSERT_NE(gpos, std::string::npos);
    ASSERT_NE(ppos, std::string::npos);
    EXPECT_LT(gpos, ppos) << "全局 AGENTS.md 应位于项目 AGENTS.md 之前";
    EXPECT_EQ(merged.sources.size(), 2u);
}

// 场景:外层目录和内层目录同时有 AGENTS.md,合并顺序是外层在前
TEST_F(InstructionsLoaderTest, NestedOuterBeforeInner) {
    fs::path outer = temp_home / "repo";
    fs::path inner = outer / "src";
    write_file(outer / "AGENTS.md", "OUTER\n");
    write_file(inner / "AGENTS.md", "INNER\n");

    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(inner.string(), cfg);
    std::size_t op = merged.merged_body.find("OUTER");
    std::size_t ip = merged.merged_body.find("INNER");
    ASSERT_NE(op, std::string::npos);
    ASSERT_NE(ip, std::string::npos);
    EXPECT_LT(op, ip);
}

// 场景:搜索停在 HOME 边界,HOME 以上的 /etc 级别文件不被读取
TEST_F(InstructionsLoaderTest, StopsAtHomeBoundary) {
    // 我们用 temp_home 作为 HOME,在 HOME 同级或更上不应出现"读到"的效果
    fs::path repo = temp_home / "sub" / "repo";
    write_file(repo / "AGENTS.md", "repo\n");
    // 故意在 temp_home 同级父目录也放一份(不应被读取,因为会越过 HOME)
    fs::path parent_of_home = temp_home.parent_path();
    if (!parent_of_home.empty()) {
        write_file(parent_of_home / "AGENTS.md", "SHOULD_NOT_APPEAR\n");
    }

    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_NE(merged.merged_body.find("repo"), std::string::npos);
    EXPECT_EQ(merged.merged_body.find("SHOULD_NOT_APPEAR"), std::string::npos);
}

// 场景:单文件超过 max_bytes 时 merged_body 被截断并出现 truncated marker
TEST_F(InstructionsLoaderTest, PerFileTruncation) {
    fs::path repo = temp_home / "repo";
    std::string big(4096, 'x');
    write_file(repo / "AGENTS.md", big);

    acecode::ProjectInstructionsConfig cfg;
    cfg.max_bytes = 1024;
    cfg.max_total_bytes = 10 * 1024;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_TRUE(merged.truncated);
    EXPECT_NE(merged.merged_body.find("per-file cap"), std::string::npos);
}

// 场景:全部缺失时 merged_body 为空且 sources 也为空
TEST_F(InstructionsLoaderTest, NoFilesEmptyResult) {
    fs::path repo = temp_home / "repo";
    fs::create_directories(repo);
    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_TRUE(merged.merged_body.empty());
    EXPECT_EQ(merged.sources.size(), 0u);
}

TEST_F(InstructionsLoaderTest, PreservesUtf8PathAndContentInternally) {
    fs::path repo = temp_home / acecode::path_from_utf8(u8"中文项目");
    write_file(repo / "AGENTS.md", u8"# 规则\n默认使用 UTF-8\n");

    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(acecode::path_to_utf8(repo), cfg);

    EXPECT_NE(merged.merged_body.find(u8"中文项目"), std::string::npos);
    EXPECT_NE(merged.merged_body.find(u8"默认使用 UTF-8"), std::string::npos);
    EXPECT_TRUE(acecode::is_valid_utf8(merged.merged_body));
    ASSERT_EQ(merged.sources.size(), 1u);
    EXPECT_EQ(acecode::path_to_utf8(merged.sources[0].parent_path().filename()), u8"中文项目");
}

// 场景:cwd 是 linked worktree(<repo>/.acecode/worktrees/wt,.git 是指向
// <repo>/.git/worktrees/wt 的指针文件),worktree 与主 checkout 各有一份 AGENTS.md。
// 期望:只加载 worktree 里那份,不再向上把主 checkout 的再加载一遍 —— 那份内容
// 重复,还带着主 checkout 绝对路径的 Source 头;worktree 会话和它派生的子代理
// 就是拿着这条路径把改动写进主仓的。
TEST_F(InstructionsLoaderTest, LinkedWorktreeRootStopsAscent) {
    fs::path repo = temp_home / "repo";
    fs::path wt = repo / ".acecode" / "worktrees" / "wt";
    write_file(repo / "AGENTS.md", "# main checkout rules\n");
    write_file(wt / "AGENTS.md", "# worktree rules\n");
    write_file(wt / ".git",
               "gitdir: " + (repo / ".git" / "worktrees" / "wt").string() + "\n");

    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(wt.string(), cfg);
    EXPECT_NE(merged.merged_body.find("worktree rules"), std::string::npos);
    EXPECT_EQ(merged.merged_body.find("main checkout rules"), std::string::npos);
    ASSERT_EQ(merged.sources.size(), 1u);
}

// 场景:cwd 是子模块(.git 指针指向 /.git/modules/...),外层仓库有 AGENTS.md。
// 期望:子模块不是 worktree 边界,外层指令照旧加载(outer-first,两份都在)。
TEST_F(InstructionsLoaderTest, SubmoduleMarkerKeepsAscending) {
    fs::path repo = temp_home / "repo";
    fs::path sub = repo / "libs" / "sub";
    write_file(repo / "AGENTS.md", "# outer rules\n");
    write_file(sub / "AGENTS.md", "# sub rules\n");
    write_file(sub / ".git", "gitdir: ../../.git/modules/libs/sub\n");

    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(sub.string(), cfg);
    EXPECT_NE(merged.merged_body.find("outer rules"), std::string::npos);
    EXPECT_NE(merged.merged_body.find("sub rules"), std::string::npos);
    EXPECT_EQ(merged.sources.size(), 2u);
}
