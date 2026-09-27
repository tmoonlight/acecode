// ToolExecutor::resolve_model_tool_name_to_native 的大小写容错(fix-feedback-0924 第 3 条子项 d)。
//
// 背景:yubo2 现场的 dots 模型把 bash 写成 `Bash`(无论原生调用还是文本调用)。
// 旧实现只做「精确名 → public alias」两步,`Bash` 原样透传,AgentLoop 报
// `Unknown tool: Bash`,错误文本也不列可用工具名,回合只能靠模型自己猜。
// 现在两步都没命中时做 ASCII 大小写不敏感匹配,候选唯一才采用。

#include <gtest/gtest.h>

#include "tool/tool_executor.hpp"
#include "llm/tool_protocol_names.hpp"
#include "utils/tool_errors.hpp"

#include <string>
#include <utility>
#include <vector>

namespace {

acecode::ToolImpl make_named_tool(std::string name) {
    acecode::ToolImpl tool;
    tool.definition.name = std::move(name);
    tool.definition.description = "name resolution test tool";
    tool.definition.parameters = {
        {"type", "object"},
        {"properties", nlohmann::json::object()},
    };
    tool.execute = [](const std::string&, const acecode::ToolContext&) {
        return acecode::ToolResult{"ok", true};
    };
    tool.is_read_only = true;
    return tool;
}

} // namespace

// 触发场景:只注册了 bash,模型调用名写成 `Bash` / `BASH`。
// 期望行为:解析为唯一候选 `bash`。
// 回归:旧实现 `Bash` 原样透传 → AgentLoop 报 Unknown tool: Bash,工具不执行。
TEST(ToolExecutorNameResolution, CaseInsensitiveUniqueMatchResolvesToNative) {
    acecode::ScopedModelToolNameMappings none(acecode::ToolProtocolNameMappings{});
    acecode::ToolExecutor tools;
    ASSERT_TRUE(tools.register_tool(make_named_tool("bash")));
    ASSERT_TRUE(tools.register_tool(make_named_tool("file_read")));

    EXPECT_EQ(tools.resolve_model_tool_name_to_native("Bash"), "bash");
    EXPECT_EQ(tools.resolve_model_tool_name_to_native("BASH"), "bash");
    EXPECT_EQ(tools.resolve_model_tool_name_to_native("File_Read"), "file_read");
    // 大小写之外的差异不做任何猜测。
    EXPECT_EQ(tools.resolve_model_tool_name_to_native("Bash2"), "Bash2");
}

// 触发场景:同时注册了 `Bash` 与 `bash`(例如 MCP 工具与内置工具)。
// 期望行为:精确名永远优先,不走大小写容错。
TEST(ToolExecutorNameResolution, ExactNameWinsOverCaseInsensitive) {
    acecode::ScopedModelToolNameMappings none(acecode::ToolProtocolNameMappings{});
    acecode::ToolExecutor tools;
    ASSERT_TRUE(tools.register_tool(make_named_tool("bash")));
    ASSERT_TRUE(tools.register_tool(make_named_tool("Bash")));

    EXPECT_EQ(tools.resolve_model_tool_name_to_native("bash"), "bash");
    EXPECT_EQ(tools.resolve_model_tool_name_to_native("Bash"), "Bash");
}

// 触发场景:注册了 `foo` 与 `Foo` 两个工具,模型写 `FOO`。
// 期望行为:候选不唯一,不猜,原样返回 `FOO`(由调用方报 Unknown tool 并列出可用名)。
TEST(ToolExecutorNameResolution, AmbiguousCaseInsensitiveNameIsNotResolved) {
    acecode::ScopedModelToolNameMappings none(acecode::ToolProtocolNameMappings{});
    acecode::ToolExecutor tools;
    ASSERT_TRUE(tools.register_tool(make_named_tool("foo")));
    ASSERT_TRUE(tools.register_tool(make_named_tool("Foo")));

    EXPECT_EQ(tools.resolve_model_tool_name_to_native("FOO"), "FOO");
}

// 触发场景:「工具重写」把 file_read 映射成 read,模型写 `Read`。
// 期望行为:public 名大小写不敏感命中,且对应 handler 已注册 → file_read;
// handler 未注册的 public 名不因大小写容错而凭空解析。
TEST(ToolExecutorNameResolution, PublicAliasMatchesCaseInsensitively) {
    acecode::ScopedModelToolNameMappings scoped{
        {"file_read", "read"},
        {"TodoWrite", "todowrite"},
    };
    acecode::ToolExecutor tools;
    ASSERT_TRUE(tools.register_tool(make_named_tool("file_read")));

    EXPECT_EQ(tools.resolve_model_tool_name_to_native("Read"), "file_read");
    EXPECT_EQ(tools.resolve_model_tool_name_to_native("READ"), "file_read");
    // TodoWrite 没注册:大小写容错也不能把 TodoWRITE 解析过去。
    EXPECT_EQ(tools.resolve_model_tool_name_to_native("TodoWRITE"), "TodoWRITE");
    EXPECT_EQ(acecode::native_tool_name_for_public_alias_ci("READ").value_or(""),
              "file_read");
    EXPECT_FALSE(acecode::native_tool_name_for_public_alias_ci("").has_value());
}

// 触发场景:provider 回了一个名字为空的调用(流式续传帧异常)。
// 期望行为:空名原样返回空串,不会被大小写容错匹配到任何工具。
TEST(ToolExecutorNameResolution, EmptyNameStaysEmpty) {
    acecode::ScopedModelToolNameMappings none(acecode::ToolProtocolNameMappings{});
    acecode::ToolExecutor tools;
    ASSERT_TRUE(tools.register_tool(make_named_tool("bash")));

    EXPECT_EQ(tools.resolve_model_tool_name_to_native(""), "");
}

// 触发场景:构造 Unknown tool 错误文本。
// 期望行为:列出可用名并要求照抄;为空时回退旧文案;超过 100 个时截断并写 (+N more)。
TEST(ToolExecutorNameResolution, UnknownToolErrorListsAvailableNames) {
    EXPECT_EQ(acecode::ToolErrors::unknown_tool("exec", {}),
              "[Error] Unknown tool: exec");
    EXPECT_EQ(acecode::ToolErrors::unknown_tool("exec", {"bash", "read"}),
              "[Error] Unknown tool: exec. Available tools: bash, read. "
              "Call one of these exact names.");

    std::vector<std::string> many;
    for (int i = 0; i < 103; ++i) many.push_back("t" + std::to_string(i));
    const std::string text = acecode::ToolErrors::unknown_tool("x", many);
    EXPECT_NE(text.find("t99"), std::string::npos);
    EXPECT_EQ(text.find("t100,"), std::string::npos);
    EXPECT_NE(text.find("(+3 more)"), std::string::npos);
}
