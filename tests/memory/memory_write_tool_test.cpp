// 覆盖 src/adapters/tool/memory_write_tool.{hpp,cpp}(openspec unify-memory-system 4.2):
// - 未指定作用域时按类型推断:user/feedback → 全局,project/reference → 工作区
// - 显式 scope 覆盖推断;会话没有工作区时一律写全局并在结果中说明
// - 写入前脱敏,结果里给出替换处数与提示
// - 记录来源:source: manual、当前会话 id、created_at / updated_at
// - 参数校验:缺 description、路径穿越、create 撞名、update 缺失、非法 type / scope

#include <gtest/gtest.h>

#include "memory/memory_frontmatter.hpp"
#include "memory/memory_paths.hpp"
#include "memory/memory_service.hpp"
#include "test_support/memory/memory_test_home.hpp"
#include "tool/memory_write_tool.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

using acecode_test::MemoryTestHome;

acecode::ToolContext ctx_for(const std::string& cwd) {
    acecode::ToolContext ctx;
    ctx.cwd = cwd;
    ctx.session_id = "20261001-101010-abcd";
    return ctx;
}

std::string read_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

} // namespace

// 场景:合法参数、type user、未指定作用域。
// 期望:写入全局目录,带 source: manual、会话 id 与时间字段;结果标明 scope=global。
TEST(MemoryWriteToolTest, UserTypeDefaultsToGlobalWithProvenance) {
    MemoryTestHome home("memory-write-global");
    auto memory = home.service();
    const std::string cwd = home.workspace_cwd("ws");
    auto tool = acecode::create_memory_write_tool(memory);

    auto r = tool.execute(R"({"name":"foo","type":"user","description":"foo desc","body":"body"})",
                          ctx_for(cwd));
    ASSERT_TRUE(r.success) << r.output;
    auto j = nlohmann::json::parse(r.output);
    EXPECT_EQ(j["scope"], "global");
    EXPECT_TRUE(j["created"].get<bool>());

    auto entry = acecode::parse_memory_entry_file(acecode::get_memory_dir() / "foo.md");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->source, "manual");
    ASSERT_EQ(entry->source_sessions.size(), 1u);
    EXPECT_EQ(entry->source_sessions[0], "20261001-101010-abcd");
    EXPECT_FALSE(entry->created_at.empty());
    EXPECT_FALSE(entry->updated_at.empty());
}

// 场景:type project 未指定作用域;另写一条 feedback 显式指定 workspace。
// 期望:两条都写进当前工作区作用域(<projects>/<hash>/memory/),全局目录里没有。
TEST(MemoryWriteToolTest, ProjectTypeAndExplicitScopeWriteWorkspace) {
    MemoryTestHome home("memory-write-workspace");
    auto memory = home.service();
    const std::string cwd = home.workspace_cwd("ws");
    const fs::path ws_dir = acecode::workspace_memory_dir(MemoryTestHome::project_dir(cwd));
    auto tool = acecode::create_memory_write_tool(memory);

    auto a = tool.execute(R"({"name":"layout","type":"project","description":"d","body":"b"})", ctx_for(cwd));
    ASSERT_TRUE(a.success) << a.output;
    EXPECT_EQ(nlohmann::json::parse(a.output)["scope"], "workspace");
    auto b = tool.execute(
        R"({"name":"style","type":"feedback","description":"d","body":"b","scope":"workspace"})", ctx_for(cwd));
    ASSERT_TRUE(b.success) << b.output;

    EXPECT_TRUE(fs::exists(ws_dir / "layout.md"));
    EXPECT_TRUE(fs::exists(ws_dir / "style.md"));
    EXPECT_FALSE(fs::exists(acecode::get_memory_dir() / "layout.md"));
    EXPECT_FALSE(fs::exists(acecode::get_memory_dir() / "style.md"));
}

// 场景:会话没有工作区(上下文无 cwd、无会话),模型写 type project。
// 期望:落到全局作用域,结果的 notice 说明原因。
TEST(MemoryWriteToolTest, NoWorkspaceFallsBackToGlobal) {
    MemoryTestHome home("memory-write-no-workspace");
    auto tool = acecode::create_memory_write_tool(home.service());
    auto r = tool.execute(R"({"name":"p","type":"project","description":"d","body":"b"})",
                          acecode::ToolContext{});
    ASSERT_TRUE(r.success) << r.output;
    auto j = nlohmann::json::parse(r.output);
    EXPECT_EQ(j["scope"], "global");
    EXPECT_NE(j["notice"].get<std::string>().find("no workspace"), std::string::npos);
    EXPECT_TRUE(fs::exists(acecode::get_memory_dir() / "p.md"));
}

// 场景:正文里有 password=hunter2 与 sk- 开头的密钥。
// 期望:落盘内容里二者都变成 [REDACTED],结果 redactions=2 并带提示。
TEST(MemoryWriteToolTest, RedactsSecretsBeforeSaving) {
    MemoryTestHome home("memory-write-redact");
    auto tool = acecode::create_memory_write_tool(home.service());
    auto r = tool.execute(
        R"({"name":"creds","type":"user","description":"login notes","body":"password=hunter2 key sk-abcdefghijklmnopqrstuvwx"})",
        acecode::ToolContext{});
    ASSERT_TRUE(r.success) << r.output;
    auto j = nlohmann::json::parse(r.output);
    EXPECT_EQ(j["redactions"].get<int>(), 2);
    EXPECT_NE(j["notice"].get<std::string>().find("[REDACTED]"), std::string::npos);
    const std::string content = read_file(acecode::get_memory_dir() / "creds.md");
    EXPECT_EQ(content.find("hunter2"), std::string::npos);
    EXPECT_EQ(content.find("sk-abcdefghijklmnopqrstuvwx"), std::string::npos);
    EXPECT_NE(content.find("[REDACTED]"), std::string::npos);
}

// 场景:缺少必填字段 description。期望:返回错误。
TEST(MemoryWriteToolTest, MissingDescriptionFails) {
    MemoryTestHome home("memory-write-missing-desc");
    auto tool = acecode::create_memory_write_tool(home.service());
    auto r = tool.execute(R"({"name":"foo","type":"user","body":"b"})", acecode::ToolContext{});
    EXPECT_FALSE(r.success);
}

// 场景:name 为 ../etc/passwd。期望:名字校验拒绝,记忆目录里不出现任何条目文件。
TEST(MemoryWriteToolTest, PathTraversalRejected) {
    MemoryTestHome home("memory-write-traversal");
    auto tool = acecode::create_memory_write_tool(home.service());
    auto r = tool.execute(R"({"name":"../etc/passwd","type":"user","description":"x","body":"y"})",
                          acecode::ToolContext{});
    EXPECT_FALSE(r.success);
    int entry_count = 0;
    for (auto& de : fs::directory_iterator(acecode::get_memory_dir())) {
        if (de.path().extension() == ".md") ++entry_count;
    }
    EXPECT_EQ(entry_count, 0);
}

// 场景:mode=create 撞名;mode=update 写不存在的条目。期望:都返回错误,磁盘不变。
TEST(MemoryWriteToolTest, CreateConflictAndUpdateMissingFail) {
    MemoryTestHome home("memory-write-modes");
    auto tool = acecode::create_memory_write_tool(home.service());
    ASSERT_TRUE(tool.execute(
        R"({"name":"dup","type":"user","description":"a","body":"a","mode":"create"})",
        acecode::ToolContext{}).success);
    EXPECT_FALSE(tool.execute(
        R"({"name":"dup","type":"user","description":"b","body":"b","mode":"create"})",
        acecode::ToolContext{}).success);
    EXPECT_NE(read_file(acecode::get_memory_dir() / "dup.md").find("\"a\""), std::string::npos);
    EXPECT_FALSE(tool.execute(
        R"({"name":"ghost","type":"user","description":"x","body":"y","mode":"update"})",
        acecode::ToolContext{}).success);
    EXPECT_FALSE(fs::exists(acecode::get_memory_dir() / "ghost.md"));
}

// 场景:非法 type / scope 枚举值。期望:返回错误。
TEST(MemoryWriteToolTest, InvalidTypeOrScopeFails) {
    MemoryTestHome home("memory-write-invalid");
    auto tool = acecode::create_memory_write_tool(home.service());
    EXPECT_FALSE(tool.execute(R"({"name":"foo","type":"notes","description":"x","body":"y"})",
                              acecode::ToolContext{}).success);
    EXPECT_FALSE(tool.execute(R"({"name":"foo","type":"user","description":"x","body":"y","scope":"team"})",
                              acecode::ToolContext{}).success);
}
