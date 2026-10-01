// 覆盖 src/adapters/tool/memory_read_tool.{hpp,cpp}(openspec unify-memory-system 4.1):
// - 无参数列出全局与当前工作区两个作用域,每项标明作用域
// - 按名字读取时先工作区后全局;指定 scope 时只看那个作用域
// - query 做不区分大小写的子串匹配并返回命中片段
// - 读前重扫磁盘:另一个进程(另一个服务实例)刚写的条目立刻可见
// - 不存在的名字返回 {found:false} 且调用成功;非法 type / scope 报错
// - 会话没有工作区时只有全局作用域;记忆整体关闭时拒绝执行

#include <gtest/gtest.h>

#include "memory/memory_service.hpp"
#include "memory/memory_types.hpp"
#include "test_support/memory/memory_test_home.hpp"
#include "tool/memory_read_tool.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <string>

namespace {

using acecode_test::MemoryTestHome;

// 工作区会话的工具上下文:没有 SessionManager 时按 cwd 推导项目目录。
acecode::ToolContext workspace_ctx(const std::string& cwd) {
    acecode::ToolContext ctx;
    ctx.cwd = cwd;
    ctx.session_id = "session-read";
    return ctx;
}

void write_entry(acecode::MemoryRegistry& registry, const std::string& name,
                 acecode::MemoryType type, const std::string& description,
                 const std::string& body) {
    std::string err;
    ASSERT_TRUE(registry.upsert(name, type, description, body,
                                acecode::MemoryWriteMode::Upsert, err).has_value())
        << err;
}

nlohmann::json run(const acecode::ToolImpl& tool, const std::string& args,
                   const acecode::ToolContext& ctx, bool expect_success = true) {
    auto result = tool.execute(args, ctx);
    EXPECT_EQ(result.success, expect_success) << result.output;
    return nlohmann::json::parse(result.output, nullptr, false);
}

} // namespace

// 场景:全局与工作区各有一条记忆,模型调用 memory_read({})。
// 期望:两条都列出,并分别标明 global / workspace 作用域。
TEST(MemoryReadToolTest, NoArgsListsBothScopesWithScopeLabels) {
    MemoryTestHome home("memory-read-list");
    auto memory = home.service();
    const std::string cwd = home.workspace_cwd("ws");
    write_entry(memory->global(), "prefs", acecode::MemoryType::User, "prefers pnpm", "use pnpm\n");
    write_entry(*memory->workspace(MemoryTestHome::project_dir(cwd)), "build",
                acecode::MemoryType::Project, "build with ninja", "ninja -C build\n");

    auto tool = acecode::create_memory_read_tool(memory);
    auto j = run(tool, "{}", workspace_ctx(cwd));
    ASSERT_EQ(j["count"].get<int>(), 2);
    EXPECT_TRUE(j["workspace_available"].get<bool>());
    std::map<std::string, std::string> scope_of;
    for (const auto& item : j["entries"]) {
        scope_of[item["name"].get<std::string>()] = item["scope"].get<std::string>();
    }
    EXPECT_EQ(scope_of["prefs"], "global");
    EXPECT_EQ(scope_of["build"], "workspace");
}

// 场景:全局与工作区都有名为 build 的条目。
// 期望:不指定作用域时返回工作区那条;scope:"global" 时返回全局那条。
TEST(MemoryReadToolTest, NameLookupPrefersWorkspaceUnlessScopeGiven) {
    MemoryTestHome home("memory-read-priority");
    auto memory = home.service();
    const std::string cwd = home.workspace_cwd("ws");
    write_entry(memory->global(), "build", acecode::MemoryType::Feedback, "global build", "G\n");
    write_entry(*memory->workspace(MemoryTestHome::project_dir(cwd)), "build",
                acecode::MemoryType::Project, "workspace build", "W\n");

    auto tool = acecode::create_memory_read_tool(memory);
    auto ws = run(tool, R"({"name":"build"})", workspace_ctx(cwd));
    EXPECT_TRUE(ws["found"].get<bool>());
    EXPECT_EQ(ws["scope"], "workspace");
    EXPECT_EQ(ws["body"], "W\n");

    auto global = run(tool, R"({"name":"build","scope":"global"})", workspace_ctx(cwd));
    EXPECT_EQ(global["scope"], "global");
    EXPECT_EQ(global["body"], "G\n");
}

// 场景:模型按关键字查找,关键字只出现在正文里;英文关键字大小写与正文不同。
// 期望:返回命中条目与包含关键字的片段;ASCII 不区分大小写。
TEST(MemoryReadToolTest, QueryMatchesBodyCaseInsensitivelyWithSnippet) {
    MemoryTestHome home("memory-read-query");
    auto memory = home.service();
    write_entry(memory->global(), "window", acecode::MemoryType::Feedback, "ui notes",
                "Keep the 撰写窗口 open while drafting.\nAlways run PNPM install first.\n");
    write_entry(memory->global(), "other", acecode::MemoryType::User, "unrelated", "nothing here\n");

    auto tool = acecode::create_memory_read_tool(memory);
    auto j = run(tool, R"({"query":"撰写窗口"})", acecode::ToolContext{});
    ASSERT_EQ(j["count"].get<int>(), 1);
    EXPECT_EQ(j["entries"][0]["name"], "window");
    EXPECT_NE(j["entries"][0]["snippet"].get<std::string>().find("撰写窗口"), std::string::npos);

    auto ascii = run(tool, R"({"query":"pnpm"})", acecode::ToolContext{});
    ASSERT_EQ(ascii["count"].get<int>(), 1);
    EXPECT_NE(ascii["entries"][0]["snippet"].get<std::string>().find("PNPM"), std::string::npos);
}

// 场景:另一个 ACECode 进程(这里用第二个服务实例模拟)刚写入条目 x。
// 期望:本进程的 memory_read({name:"x"}) 读前重扫,返回 found:true。
// 回归:曾经只读启动时的缓存,别的进程写的条目要重启才看得见。
TEST(MemoryReadToolTest, SeesEntryWrittenByAnotherProcess) {
    MemoryTestHome home("memory-read-other-process");
    auto mine = home.service();
    auto tool = acecode::create_memory_read_tool(mine);
    ASSERT_FALSE(run(tool, R"({"name":"x"})", acecode::ToolContext{})["found"].get<bool>());

    auto other = home.service();
    write_entry(other->global(), "x", acecode::MemoryType::User, "from other process", "hi\n");

    auto j = run(tool, R"({"name":"x"})", acecode::ToolContext{});
    EXPECT_TRUE(j["found"].get<bool>());
    EXPECT_EQ(j["description"], "from other process");
}

// 场景:读取不存在的名字。期望:{found:false},工具调用成功。
TEST(MemoryReadToolTest, MissingNameReturnsNotFoundNotError) {
    MemoryTestHome home("memory-read-missing");
    auto tool = acecode::create_memory_read_tool(home.service());
    auto j = run(tool, R"({"name":"ghost"})", acecode::ToolContext{});
    EXPECT_FALSE(j["found"].get<bool>());
}

// 场景:按类型过滤。期望:只返回该类型的条目。
TEST(MemoryReadToolTest, TypeFilter) {
    MemoryTestHome home("memory-read-type");
    auto memory = home.service();
    write_entry(memory->global(), "u1", acecode::MemoryType::User, "user1", "b\n");
    write_entry(memory->global(), "f1", acecode::MemoryType::Feedback, "feedback1", "b\n");
    auto tool = acecode::create_memory_read_tool(memory);
    auto j = run(tool, R"({"type":"feedback"})", acecode::ToolContext{});
    ASSERT_EQ(j["count"].get<int>(), 1);
    EXPECT_EQ(j["entries"][0]["name"], "f1");
}

// 场景:非法 type / scope 枚举值。期望:success=false。
TEST(MemoryReadToolTest, InvalidTypeOrScopeIsError) {
    MemoryTestHome home("memory-read-invalid");
    auto tool = acecode::create_memory_read_tool(home.service());
    run(tool, R"({"type":"notes"})", acecode::ToolContext{}, false);
    run(tool, R"({"scope":"team"})", acecode::ToolContext{}, false);
}

// 场景:会话没有工作区(工具上下文里既无会话也无 cwd)。
// 期望:只列全局条目,workspace_available=false;不会凭空创建工作区目录。
TEST(MemoryReadToolTest, SessionWithoutWorkspaceSeesOnlyGlobal) {
    MemoryTestHome home("memory-read-no-workspace");
    auto memory = home.service();
    write_entry(memory->global(), "g", acecode::MemoryType::User, "global only", "b\n");
    auto tool = acecode::create_memory_read_tool(memory);
    auto j = run(tool, "{}", acecode::ToolContext{});
    EXPECT_FALSE(j["workspace_available"].get<bool>());
    ASSERT_EQ(j["count"].get<int>(), 1);
    EXPECT_EQ(j["entries"][0]["scope"], "global");
}

// 场景:用户在设置里关闭了「使用记忆」,模型仍按历史调用 memory_read。
// 期望:工具拒绝执行并说明记忆已关闭。
TEST(MemoryReadToolTest, DisabledMemoryRejectsCall) {
    MemoryTestHome home("memory-read-disabled");
    acecode::MemoryConfig config;
    config.enabled = false;
    auto tool = acecode::create_memory_read_tool(home.service(config));
    auto j = run(tool, "{}", acecode::ToolContext{}, false);
    EXPECT_NE(j["error"].get<std::string>().find("disabled"), std::string::npos);
}
