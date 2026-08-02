#include "tool/agent_browser/browser_tools.hpp"
#include "tool/agent_browser/cdp_client.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>

using namespace acecode;
using namespace acecode::agent_browser;

TEST(AgentBrowserTools, ParsesBoundedSemanticElementReferences) {
    ASSERT_TRUE(parse_agent_browser_element_ref("@e17").has_value());
    EXPECT_EQ(parse_agent_browser_element_ref("@e17")->index, 17);
    EXPECT_FALSE(parse_agent_browser_element_ref("e17"));
    EXPECT_FALSE(parse_agent_browser_element_ref("@e0"));
    EXPECT_FALSE(parse_agent_browser_element_ref("@e1x"));
    EXPECT_FALSE(parse_agent_browser_element_ref("@e10001"));
}

TEST(AgentBrowserTools, SnapshotContractUsesRevisionedRefsWithoutHostBindings) {
    const std::string script = agent_browser_snapshot_script();
    EXPECT_NE(script.find("__aceAgentBrowserSnapshot"), std::string::npos);
    EXPECT_NE(script.find("revision"), std::string::npos);
    EXPECT_NE(script.find("options.revision"), std::string::npos);
    EXPECT_NE(script.find("@e${index + 1}"), std::string::npos);
    EXPECT_EQ(script.find("__aceAgentBrowserSnapshotOptions"), std::string::npos);
    EXPECT_EQ(script.find("chrome.webview"), std::string::npos);
    EXPECT_EQ(script.find("aceDesktop"), std::string::npos);
}

TEST(AgentBrowserTools, RejectsAmbiguousInteractionTargetsBeforeConnecting) {
#if defined(_WIN32) || defined(__APPLE__)
    ToolExecutor tools;
    register_agent_browser_tools(tools);

    const ToolResult click = tools.execute("browser_click", "{}");
    EXPECT_FALSE(click.success);
    const auto click_output = nlohmann::json::parse(click.output);
    EXPECT_EQ(click_output["error"]["code"], "invalid_arguments");

    const ToolResult wait = tools.execute(
        "browser_wait", R"({"condition":"element_hidden","timeout_ms":100})");
    EXPECT_FALSE(wait.success);
    const auto wait_output = nlohmann::json::parse(wait.output);
    EXPECT_EQ(wait_output["error"]["code"], "invalid_arguments");
#endif
}

TEST(AgentBrowserTools, RegistersStructuredDesktopToolSet) {
    ToolExecutor tools;
    register_agent_browser_tools(tools);
#if defined(_WIN32) || defined(__APPLE__)
    for (const auto& name : agent_browser_tool_names()) {
        EXPECT_TRUE(tools.has_tool(name)) << name;
    }
    const auto definitions = tools.get_tool_definitions();
    for (const auto& name : agent_browser_tool_names()) {
        const auto definition = std::find_if(
            definitions.begin(), definitions.end(),
            [&name](const ToolDef& candidate) {
                return candidate.name == name;
            });
        ASSERT_NE(definition, definitions.end()) << name;
        ASSERT_TRUE(definition->parameters.contains("properties")) << name;
        EXPECT_TRUE(definition->parameters["properties"].contains("page_id"))
            << name;
    }
    EXPECT_TRUE(tools.is_read_only("browser_read_page"));
    EXPECT_TRUE(tools.is_read_only("browser_screenshot"));
    EXPECT_FALSE(tools.is_read_only("browser_click"));
    EXPECT_FALSE(tools.has_tool("browser_start"));
    for (const char* name : {
             "browser_click", "browser_fill", "browser_type",
             "browser_press", "browser_hover", "browser_drag",
             "browser_scroll"}) {
        const auto definition = std::find_if(
            definitions.begin(), definitions.end(),
            [name](const ToolDef& candidate) {
                return candidate.name == name;
        });
        ASSERT_NE(definition, definitions.end()) << name;
        const auto& properties = definition->parameters["properties"];
#ifdef __APPLE__
        ASSERT_TRUE(properties.contains("input_mode")) << name;
        EXPECT_EQ(properties["input_mode"]["enum"],
                  nlohmann::json::array({"synthetic", "native"}))
            << name;
        EXPECT_EQ(properties["input_mode"]["default"], "synthetic") << name;
#else
        EXPECT_FALSE(properties.contains("input_mode")) << name;
#endif
    }
#else
    for (const auto& name : agent_browser_tool_names()) {
        EXPECT_FALSE(tools.has_tool(name)) << name;
    }
#endif
}

TEST(AgentBrowserTools, DecodesScreenshotPayloadsDefensively) {
    const auto decoded = decode_agent_browser_base64("SGVsbG8=");
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(std::string(decoded->begin(), decoded->end()), "Hello");
    EXPECT_FALSE(decode_agent_browser_base64("not*base64"));
}

TEST(AgentBrowserTools, ReadsPngDimensionsDefensively) {
    std::vector<unsigned char> png(24, 0);
    const unsigned char signature[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    };
    std::copy(std::begin(signature), std::end(signature), png.begin());
    png[12] = 'I';
    png[13] = 'H';
    png[14] = 'D';
    png[15] = 'R';
    png[18] = 0x02;
    png[19] = 0x80;
    png[22] = 0x01;
    png[23] = 0xE0;

    const auto dimensions = agent_browser_png_dimensions(png);
    ASSERT_TRUE(dimensions.has_value());
    EXPECT_EQ(dimensions->first, 640u);
    EXPECT_EQ(dimensions->second, 480u);

    png[0] = 0;
    EXPECT_FALSE(agent_browser_png_dimensions(png));
}

TEST(AgentBrowserTools, InteractionResultMetadataIsMacOnly) {
    const nlohmann::json base{{"clicked", true}};
#ifdef __APPLE__
    EXPECT_EQ(agent_browser_input_result_metadata(base, "synthetic"),
              nlohmann::json({{"clicked", true},
                              {"input_mode", "synthetic"},
                              {"input_trust", "synthetic"}}));
    EXPECT_EQ(agent_browser_input_result_metadata(base, "native"),
              nlohmann::json({{"clicked", true},
                              {"input_mode", "native"},
                              {"input_trust", "native"}}));
#else
    EXPECT_EQ(agent_browser_input_result_metadata(base, "native"), base);
#endif
}
