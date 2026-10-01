#include <gtest/gtest.h>

#include "agent/boundary/workspace_boundary.hpp"
#include "agent/side_question/side_chat_tools.hpp"
#include "permissions/permissions.hpp"
#include "tool/bash_tool.hpp"
#include "tool/file_read_tool.hpp"
#include "tool/glob_tool.hpp"
#include "tool/grep_tool.hpp"
#include "tool/mtime_tracker.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

namespace {

namespace fs = std::filesystem;

// 临时工作区:ws/ 是会话 cwd,outside/ 是 cwd 之外的兄弟目录。注册真实的
// bash / file_read / grep / glob,外加两个带 is_read_only 但不该放出的工具。
struct SideChatToolsFixture {
    fs::path root;
    fs::path workspace;
    fs::path outside;
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    std::unique_ptr<acecode::agent::WorkspaceBoundary> boundary;
    std::shared_ptr<std::atomic<int>> hidden_calls = std::make_shared<std::atomic<int>>(0);

    SideChatToolsFixture() {
        const auto unique = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        root = fs::path(testing::TempDir()) / ("acecode_side_chat_tools_" + unique);
        workspace = root / "ws";
        outside = root / "outside";
        fs::create_directories(workspace);
        fs::create_directories(outside);
        write(workspace / "a.txt", "alpha\n");
        write(outside / "b.txt", "beta\n");
        tools.register_tool(acecode::create_bash_tool());
        tools.register_tool(acecode::create_file_read_tool());
        tools.register_tool(acecode::create_grep_tool());
        tools.register_tool(acecode::create_glob_tool());
        tools.register_tool(hidden_tool("spawn_subagent", acecode::ToolSource::Builtin, ""));
        tools.register_tool(hidden_tool("mcp__srv__read", acecode::ToolSource::Mcp, "srv"));
        boundary = std::make_unique<acecode::agent::WorkspaceBoundary>(
            workspace.string(), permissions);
    }

    ~SideChatToolsFixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    static void write(const fs::path& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }

    acecode::ToolImpl hidden_tool(const std::string& name, acecode::ToolSource source,
                                  const std::string& owner) {
        acecode::ToolImpl impl;
        impl.definition = {name, "read-only but not for side chat",
                           nlohmann::json{{"type", "object"}}};
        impl.execute = [calls = hidden_calls](const std::string&, const acecode::ToolContext&) {
            ++*calls;
            return acecode::ToolResult{"ran", true};
        };
        impl.is_read_only = true;
        impl.source = source;
        impl.source_owner = owner;
        return impl;
    }

    acecode::SideChatToolset build(const acecode::ToolCapabilityPolicy& policy = {}) {
        return acecode::agent::build_side_chat_toolset(
            tools, permissions, *boundary, nullptr, policy);
    }

    static acecode::ToolCall call(const std::string& name, const nlohmann::json& args) {
        return {"call_" + name, name, args.dump()};
    }
};

std::vector<std::string> names(const acecode::SideChatToolset& toolset) {
    std::vector<std::string> out;
    for (const auto& definition : toolset.definitions) out.push_back(definition.name);
    return out;
}

// 场景:会话注册了 bash、MCP 工具,以及同样带 is_read_only(免确认)标记的
// spawn_subagent。期望:侧边对话只放出白名单里的只读内置工具;模型硬要调用其它
// 工具时返回「不可用」并列出可用工具,被拒的工具一次都不执行。
// 回归:侧边对话里出现 bash 调用记录。
TEST(SideChatTools, OnlyAllowlistedReadToolsAreExposedAndOthersRefused) {
    SideChatToolsFixture fx;
    auto toolset = fx.build();
    EXPECT_EQ(names(toolset), (std::vector<std::string>{"file_read", "glob", "grep"}));
    ASSERT_TRUE(toolset.enabled());
    for (const char* name : {"bash", "spawn_subagent", "mcp__srv__read"}) {
        const auto result = toolset.execute(fx.call(name, {{"command", "ls"}}), nullptr);
        EXPECT_FALSE(result.success) << name;
        EXPECT_NE(result.output.find("not available in this read-only side chat"),
                  std::string::npos) << name;
        EXPECT_NE(result.output.find("file_read, glob, grep"), std::string::npos) << name;
    }
    EXPECT_EQ(fx.hidden_calls->load(), 0);
}

// 场景:会话启用了专家能力策略,只允许 grep。期望:侧边对话不会绕过策略放出 file_read。
TEST(SideChatTools, ExpertCapabilityPolicyStillNarrowsSideChatTools) {
    SideChatToolsFixture fx;
    acecode::ToolCapabilityPolicy policy;
    policy.builtin_tools = std::unordered_set<std::string>{"grep"};
    auto toolset = fx.build(policy);
    EXPECT_EQ(names(toolset), (std::vector<std::string>{"grep"}));
    const auto result = toolset.execute(
        fx.call("file_read", {{"file_path", (fx.workspace / "a.txt").string()}}), nullptr);
    EXPECT_FALSE(result.success);
}

// 场景:侧边对话用 file_read 读了一个主代理从没读过的文件。期望:拿到真实内容,
// 但主代理的编辑基线仍是「未读」—— 否则主代理随后读这个文件会拿到「未变化」短桩,
// 或者不读就能直接编辑。
TEST(SideChatTools, FileReadReturnsContentWithoutTouchingAgentReadState) {
    SideChatToolsFixture fx;
    auto toolset = fx.build();
    const auto path = (fx.workspace / "a.txt").string();
    const auto result = toolset.execute(fx.call("file_read", {{"file_path", path}}), nullptr);
    ASSERT_TRUE(result.success) << result.output;
    EXPECT_NE(result.output.find("alpha"), std::string::npos);
    EXPECT_EQ(acecode::MtimeTracker::instance()
                  .validate_read_baseline_for_edit(path, "alpha\n").status,
              acecode::MtimeTracker::ReadBaselineStatus::NotRead);
    EXPECT_FALSE(acecode::MtimeTracker::DetachedReadScope::active());
}

// 场景:配置了针对读取的 Deny 规则,以及 glob 越出会话 cwd。期望:侧边对话没有确认
// 通道,命中 Deny 直接拒绝;路径校验与主会话同口径(cwd 外的搜索被拒,cwd 内照常)。
TEST(SideChatTools, DenyRulesAndWorkspaceBoundaryRefuseWithoutPrompting) {
    SideChatToolsFixture fx;
    fx.permissions.add_rule({"file_read", "*.secret", "", acecode::RuleAction::Deny, 10});
    auto toolset = fx.build();
    const auto denied = toolset.execute(fx.call("file_read", {{"file_path", "notes.secret"}}), nullptr);
    EXPECT_FALSE(denied.success);
    EXPECT_EQ(denied.output.rfind("[Permission denied]", 0), 0u) << denied.output;

    const auto outside = toolset.execute(
        fx.call("glob", {{"pattern", "*.txt"}, {"path", fx.outside.string()}}), nullptr);
    EXPECT_FALSE(outside.success);
    EXPECT_EQ(outside.output.rfind("[Error]", 0), 0u) << outside.output;

    const auto inside = toolset.execute(
        fx.call("glob", {{"pattern", "*.txt"}, {"path", fx.workspace.string()}}), nullptr);
    EXPECT_TRUE(inside.success) << inside.output;
    EXPECT_NE(inside.output.find("a.txt"), std::string::npos) << inside.output;
}

} // namespace
