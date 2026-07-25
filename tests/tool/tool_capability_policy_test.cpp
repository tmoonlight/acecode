#include <gtest/gtest.h>

#include "tool/tool_executor.hpp"

#include <atomic>
#include <unordered_set>

namespace {

acecode::ToolImpl make_tool(std::string name,
                            acecode::ToolSource source,
                            std::string owner,
                            std::atomic<int>* calls) {
    acecode::ToolImpl tool;
    tool.definition.name = std::move(name);
    tool.definition.description = "test";
    tool.definition.parameters = nlohmann::json::object();
    tool.source = source;
    tool.source_owner = std::move(owner);
    tool.execute = [calls](const std::string&, const acecode::ToolContext&) {
        calls->fetch_add(1);
        return acecode::ToolResult{"ok", true};
    };
    return tool;
}

std::vector<std::string> names(const std::vector<acecode::ToolDef>& defs) {
    std::vector<std::string> result;
    for (const auto& def : defs) result.push_back(def.name);
    return result;
}

} // namespace

TEST(ToolCapabilityPolicy, FiltersBuiltinsAndExactMcpOwners) {
    std::atomic<int> calls{0};
    acecode::ToolExecutor tools;
    tools.register_tool(make_tool("file_read", acecode::ToolSource::Builtin,
                                  {}, &calls));
    tools.register_tool(make_tool("file_write", acecode::ToolSource::Builtin,
                                  {}, &calls));
    tools.register_tool(make_tool("mcp_alpha_search",
                                  acecode::ToolSource::Mcp, "alpha", &calls));
    tools.register_tool(make_tool("mcp_alphabet_search",
                                  acecode::ToolSource::Mcp, "alphabet", &calls));

    acecode::ToolCapabilityPolicy policy;
    policy.builtin_tools =
        std::unordered_set<std::string>{"file_read"};
    policy.mcp_servers =
        std::unordered_set<std::string>{"alpha"};

    EXPECT_EQ(names(tools.get_tool_definitions(&policy)),
              std::vector<std::string>({
                  "file_read",
                  "mcp_alpha_search",
              }));
    EXPECT_TRUE(tools.is_allowed("mcp_alpha_search", &policy));
    EXPECT_FALSE(tools.is_allowed("mcp_alphabet_search", &policy));
    EXPECT_TRUE(tools.is_denied_by_policy(
        "mcp_alphabet_search", &policy));
    EXPECT_FALSE(tools.is_denied_by_policy(
        "not_registered", &policy));

    acecode::ToolContext context;
    context.capability_policy = policy;
    EXPECT_TRUE(tools.execute("file_read", "{}", context).success);
    const auto denied = tools.execute("file_write", "{}", context);
    EXPECT_FALSE(denied.success);
    EXPECT_NE(denied.output.find("expert capability policy"),
              std::string::npos);
    EXPECT_EQ(calls.load(), 1);
}

TEST(ToolCapabilityPolicy, MissingScopesInheritAndEmptyScopesDenyAll) {
    std::atomic<int> calls{0};
    acecode::ToolExecutor tools;
    tools.register_tool(make_tool("file_read", acecode::ToolSource::Builtin,
                                  {}, &calls));
    tools.register_tool(make_tool("mcp_server_read", acecode::ToolSource::Mcp,
                                  "server", &calls));

    acecode::ToolCapabilityPolicy inherited;
    EXPECT_EQ(tools.get_tool_definitions(&inherited).size(), 2u);

    acecode::ToolCapabilityPolicy empty;
    empty.builtin_tools = std::unordered_set<std::string>{};
    empty.mcp_servers = std::unordered_set<std::string>{};
    EXPECT_TRUE(tools.get_tool_definitions(&empty).empty());
}

TEST(ToolCapabilityPolicy, RegisteredCatalogKeepsExactIdAndOwner) {
    std::atomic<int> calls{0};
    acecode::ToolExecutor tools;
    tools.register_tool(make_tool("AskUserQuestion",
                                  acecode::ToolSource::Builtin, {}, &calls));
    tools.register_tool(make_tool("mcp_GitHub_search",
                                  acecode::ToolSource::Mcp, "GitHub", &calls));

    const auto catalog = tools.get_registered_tools();
    ASSERT_EQ(catalog.size(), 2u);
    EXPECT_EQ(catalog[0].definition.name, "AskUserQuestion");
    EXPECT_EQ(catalog[0].source, acecode::ToolSource::Builtin);
    EXPECT_TRUE(catalog[0].source_owner.empty());
    EXPECT_EQ(catalog[1].definition.name, "mcp_GitHub_search");
    EXPECT_EQ(catalog[1].source, acecode::ToolSource::Mcp);
    EXPECT_EQ(catalog[1].source_owner, "GitHub");
}
