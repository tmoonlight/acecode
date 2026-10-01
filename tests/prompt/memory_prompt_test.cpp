// 覆盖 src/engine/prompt/memory_prompt.{hpp,cpp}(openspec unify-memory-system 5.2):
// 记忆上下文快照的渲染 —— 按作用域分段、条目附距今天数、每作用域预算与省略说明、
// 固定使用说明、两个作用域都为空时不注入、工作区之间隔离;以及静态提示里
// # Memory 指引随本会话记忆开关出现 / 消失。

#include <gtest/gtest.h>

#include "memory/memory_frontmatter.hpp"
#include "memory/memory_paths.hpp"
#include "memory/memory_service.hpp"
#include "prompt/memory_prompt.hpp"
#include "prompt/system_prompt.hpp"
#include "test_support/memory/memory_test_home.hpp"
#include "tool/tool_executor.hpp"

#include <fstream>
#include <sstream>

namespace {

using acecode_test::MemoryTestHome;

std::int64_t at(const char* iso) {
    return acecode::parse_memory_iso8601(iso).value_or(0);
}

void write_at(acecode::MemoryRegistry& registry, const std::string& name, acecode::MemoryType type,
              const std::string& description, const std::string& iso) {
    acecode::MemoryWriteRequest request;
    request.name = name;
    request.type = type;
    request.description = description;
    request.body = "body of " + name + "\n";
    request.now_iso = iso;
    std::string err;
    ASSERT_TRUE(registry.upsert(request, err).has_value()) << err;
}

acecode::PromptContextBlock snapshot(const std::shared_ptr<acecode::MemoryService>& memory,
                                     const std::string& project_dir, std::size_t budget,
                                     const char* now_iso) {
    acecode::MemorySnapshotSource source;
    source.memory = memory.get();
    source.project_dir = project_dir;
    source.max_index_bytes = budget;
    source.now_seconds = at(now_iso);
    return acecode::build_memory_snapshot_prompt(source);
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

} // namespace

// 场景:全局一条 3 天前更新的偏好,工作区一条今天更新的项目约定。
// 期望:分「Global memory」「Workspace memory」两段,行尾标注 3 days ago / today,
// 并带上「记忆是历史、用前核实、当前指令优先」的使用说明。
TEST(MemoryPromptTest, RendersScopesWithAgeLabelsAndUsageNote) {
    MemoryTestHome home("memory-prompt-render");
    auto memory = home.service();
    const std::string project_dir = MemoryTestHome::project_dir(home.workspace_cwd("ws"));
    write_at(memory->global(), "prefer_pnpm", acecode::MemoryType::Feedback,
             "Use pnpm, never npm", "2026-09-28T09:00:00Z");
    write_at(*memory->workspace(project_dir), "build_dir", acecode::MemoryType::Project,
             "Build into L:/build", "2026-10-01T08:00:00Z");

    const auto block = snapshot(memory, project_dir, 8192, "2026-10-01T12:00:00Z");
    ASSERT_FALSE(block.content.empty());
    const auto& text = block.content;
    EXPECT_NE(text.find("## Global memory"), std::string::npos);
    EXPECT_NE(text.find("## Workspace memory"), std::string::npos);
    EXPECT_NE(text.find("- [feedback] prefer_pnpm"), std::string::npos);
    EXPECT_NE(text.find("Use pnpm, never npm (3 days ago)"), std::string::npos) << text;
    EXPECT_NE(text.find("Build into L:/build (today)"), std::string::npos) << text;
    EXPECT_NE(text.find("verify it with tools"), std::string::npos);
    EXPECT_NE(text.find("follow the current instructions"), std::string::npos);
    EXPECT_LT(text.find("## Global memory"), text.find("## Workspace memory"));
    EXPECT_FALSE(block.cache_key.empty());
    // 同样输入渲染两次逐字节相同(冻结前提)。
    EXPECT_EQ(snapshot(memory, project_dir, 8192, "2026-10-01T12:00:00Z").content, text);
}

// 场景:工作区有 30 条记忆,预算只够放几行。
// 期望:按更新时间倒序列出最新的条目,其余改为一行省略说明(可用 memory_read 查);
// 磁盘上的 MEMORY.md 不被修改。
TEST(MemoryPromptTest, BudgetKeepsNewestEntriesAndAddsOmissionNote) {
    MemoryTestHome home("memory-prompt-budget");
    auto memory = home.service();
    const std::string project_dir = MemoryTestHome::project_dir(home.workspace_cwd("ws"));
    auto workspace = memory->workspace(project_dir);
    for (int i = 0; i < 30; ++i) {
        char iso[32];
        std::snprintf(iso, sizeof(iso), "2026-09-%02dT10:00:00Z", i + 1);
        write_at(*workspace, "entry_" + std::to_string(i), acecode::MemoryType::Project,
                 "rule number " + std::to_string(i), iso);
    }
    const std::string index_before = read_file(workspace->dir() / "MEMORY.md");

    const auto block = snapshot(memory, project_dir, 300, "2026-10-01T12:00:00Z");
    const auto& text = block.content;
    EXPECT_NE(text.find("entry_29"), std::string::npos) << text;
    EXPECT_EQ(text.find("entry_0 "), std::string::npos) << text;
    EXPECT_NE(text.find("older entries were omitted"), std::string::npos) << text;
    EXPECT_NE(text.find("memory_read"), std::string::npos);
    EXPECT_EQ(read_file(workspace->dir() / "MEMORY.md"), index_before);
}

// 场景:两个作用域都没有条目。期望:返回空块,请求中不出现记忆上下文。
TEST(MemoryPromptTest, EmptyScopesProduceNoBlock) {
    MemoryTestHome home("memory-prompt-empty");
    auto memory = home.service();
    const std::string project_dir = MemoryTestHome::project_dir(home.workspace_cwd("ws"));
    EXPECT_TRUE(snapshot(memory, project_dir, 8192, "2026-10-01T12:00:00Z").content.empty());
    acecode::MemorySnapshotSource none;
    EXPECT_TRUE(acecode::build_memory_snapshot_prompt(none).content.empty());
}

// 场景:工作区 A 与 B 各有一条记忆,渲染 A 的会话快照。
// 期望:只含全局与 A 的条目,不含 B 的。
TEST(MemoryPromptTest, WorkspaceSnapshotsAreIsolated) {
    MemoryTestHome home("memory-prompt-isolation");
    auto memory = home.service();
    const std::string a = MemoryTestHome::project_dir(home.workspace_cwd("a"));
    const std::string b = MemoryTestHome::project_dir(home.workspace_cwd("b"));
    write_at(*memory->workspace(a), "a_rule", acecode::MemoryType::Project, "rule a", "2026-09-30T00:00:00Z");
    write_at(*memory->workspace(b), "b_rule", acecode::MemoryType::Project, "rule b", "2026-09-30T00:00:00Z");
    const auto text = snapshot(memory, a, 8192, "2026-10-01T00:00:00Z").content;
    EXPECT_NE(text.find("a_rule"), std::string::npos);
    EXPECT_EQ(text.find("b_rule"), std::string::npos);
}

// 场景:旧条目没有 updated_at / created_at。期望:按文件修改时间标注天数。
TEST(MemoryPromptTest, LegacyEntriesUseFileModificationTime) {
    MemoryTestHome home("memory-prompt-legacy");
    std::ofstream(acecode::get_memory_dir() / "legacy.md", std::ios::binary)
        << "---\nname: \"legacy\"\ndescription: \"old rule\"\ntype: user\n---\n\nbody\n";
    auto memory = home.service();
    acecode::MemorySnapshotSource source;
    source.memory = memory.get();
    source.now_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto text = acecode::build_memory_snapshot_prompt(source).content;
    EXPECT_NE(text.find("old rule (today)"), std::string::npos) << text;
}

// 场景:天数标签的边界。期望:0 天 today、1 天单数、无法解析的时间返回空。
TEST(MemoryPromptTest, AgeLabels) {
    const auto now = at("2026-10-01T12:00:00Z");
    EXPECT_EQ(acecode::memory_age_label("2026-10-01T00:00:00Z", now), "today");
    EXPECT_EQ(acecode::memory_age_label("2026-09-30T11:00:00Z", now), "1 day ago");
    EXPECT_EQ(acecode::memory_age_label("2026-09-01T12:00:00Z", now), "30 days ago");
    EXPECT_EQ(acecode::memory_age_label("not a time", now), "");
}

// 场景:工具表里有 memory_write,但本会话执行了 /memory off(请求配置 enabled=false)。
// 期望:静态提示里不出现 # Memory 指引 —— 模型不该被引导去调一个不在工具表里的工具。
TEST(MemoryPromptTest, StaticGuidanceFollowsSessionMemorySwitch) {
    acecode::ToolExecutor tools;
    for (const char* name : {"memory_write", "memory_read"}) {
        acecode::ToolImpl tool;
        tool.definition.name = name;
        tool.definition.description = "probe";
        tool.definition.parameters = {{"type", "object"}};
        tool.execute = [](const std::string&, const acecode::ToolContext&) {
            return acecode::ToolResult{"{}", true};
        };
        ASSERT_TRUE(tools.register_tool(tool));
    }
    acecode::MemoryConfig on;
    acecode::MemoryConfig off;
    off.enabled = false;
    EXPECT_NE(acecode::build_system_prompt(tools, "", nullptr, nullptr, &on).find("# Memory\n"),
              std::string::npos);
    EXPECT_EQ(acecode::build_system_prompt(tools, "", nullptr, nullptr, &off).find("# Memory\n"),
              std::string::npos);
}
