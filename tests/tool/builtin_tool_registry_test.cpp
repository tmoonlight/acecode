#include <gtest/gtest.h>

#include "config/config.hpp"
#include "tool/agent_browser/browser_tools.hpp"
#include "tool/builtin_tool_registry.hpp"
#include "tool/tool_executor.hpp"

using namespace acecode;

TEST(BuiltinToolRegistry, RegistersSharedCoreAndPlatformBrowserTools) {
    AppConfig config;
    config.web_search.enabled = false;
    ToolExecutor tools;

    register_session_builtin_tools(tools, config);

    EXPECT_TRUE(tools.has_tool("bash"));
    EXPECT_TRUE(tools.has_tool("file_read"));
    EXPECT_TRUE(tools.has_tool("show_image"));
    EXPECT_TRUE(tools.has_tool("task_complete"));
    EXPECT_TRUE(tools.has_tool("TodoWrite"));
    EXPECT_TRUE(tools.has_tool("EnterPlanMode"));
    EXPECT_TRUE(tools.has_tool("ExitPlanMode"));
    EXPECT_FALSE(tools.is_read_only("TodoWrite"));
    EXPECT_FALSE(tools.is_read_only("EnterPlanMode"));
    EXPECT_FALSE(tools.is_read_only("ExitPlanMode"));
    EXPECT_TRUE(tools.is_read_only("show_image"));
    EXPECT_FALSE(tools.has_tool("browser_start"));
#ifdef _WIN32
    EXPECT_TRUE(tools.has_tool("browser_open"));
    EXPECT_TRUE(tools.has_tool("browser_read_page"));
    EXPECT_TRUE(tools.has_tool("browser_click"));
    EXPECT_TRUE(tools.has_tool("browser_screenshot"));
#else
    EXPECT_FALSE(tools.has_tool("browser_open"));
#endif
}

TEST(BuiltinToolRegistry, NativeBrowserToolsCanBeUnregisteredAsOneGroup) {
    AppConfig config;
    config.web_search.enabled = false;
    ToolExecutor tools;

    register_session_builtin_tools(tools, config);
    const std::size_t removed = agent_browser::unregister_agent_browser_tools(tools);

#ifdef _WIN32
    EXPECT_GT(removed, 0u);
#else
    EXPECT_EQ(removed, 0u);
#endif
    EXPECT_FALSE(tools.has_tool("browser_open"));
    EXPECT_FALSE(tools.has_tool("browser_click"));
    EXPECT_TRUE(tools.has_tool("bash"));
}
