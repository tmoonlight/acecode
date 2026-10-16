#include <gtest/gtest.h>

#include "config/config.hpp"
#include "permissions/permissions.hpp"
#include "tool/ios_simulator/runtime.hpp"
#include "tool/ios_simulator/tool_actions.hpp"
#include "tool/ios_simulator_tool.hpp"

using namespace acecode;

TEST(IosSimulatorTool, RegistersOnlyWhereSupportedAndEnabled) {
    ToolExecutor tools;
    AppConfig config;
    EXPECT_TRUE(config.ios_simulator.enabled);
    refresh_ios_simulator_tools(tools, config);
    EXPECT_EQ(tools.has_tool("ios_simulator"), ios_simulator::Runtime::supported());
    EXPECT_EQ(tools.has_tool("ios_simulator_build"), ios_simulator::Runtime::supported());
    config.ios_simulator.enabled = false;
    refresh_ios_simulator_tools(tools, config);
    EXPECT_FALSE(tools.has_tool("ios_simulator"));
    EXPECT_FALSE(tools.has_tool("ios_simulator_build"));
}

TEST(IosSimulatorTool, SchemaListsEveryAction) {
    const auto tools = create_ios_simulator_tools();
    ASSERT_EQ(tools.size(), 2u);
    const auto& actions = tools[0].definition.parameters["properties"]["action"]["enum"];
    for (const char* action : {"attach", "screenshot", "tap", "swipe", "touch_path", "touch2_path", "text", "key",
                               "button", "open_url", "launch", "inspect", "logs", "boot", "list_devices"}) {
        EXPECT_NE(std::find(actions.begin(), actions.end(), action), actions.end()) << action;
    }
    EXPECT_TRUE(tools[0].requires_serial_execution);
}

TEST(IosSimulatorTool, ControlToolIsAutoAllowedButBuildPrompts) {
    PermissionManager permissions;
    EXPECT_TRUE(permissions.should_auto_allow("ios_simulator", false));
    EXPECT_FALSE(permissions.should_auto_allow("ios_simulator_build", false));
}

TEST(IosSimulatorTool, DisabledRuntimeRefusesActions) {
    ToolExecutor tools;
    AppConfig config;
    config.ios_simulator.enabled = false;
    refresh_ios_simulator_tools(tools, config);
    ToolContext ctx;
    auto result = ios_simulator::run_simulator_action("list_devices", nlohmann::json::object(), ctx);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("disabled"), std::string::npos);
    config.ios_simulator.enabled = true;
    refresh_ios_simulator_tools(tools, config);
}

TEST(IosSimulatorTool, FormatsAccessibilityTreesAndSearches) {
    const auto result = nlohmann::json::parse(R"({
      "root": {"role": "Application", "label": "设置", "frame": {"x": 0, "y": 0, "width": 390, "height": 844},
               "children": [
                 {"role": "Button", "label": "通用", "identifier": "general",
                  "frame": {"x": 16, "y": 278, "width": 358, "height": 52}},
                 {"role": "Button", "label": "辅助功能", "enabled": false,
                  "frame": {"x": 16, "y": 330.5, "width": 358, "height": 52}}]},
      "truncated": false})");
    const auto tree = ios_simulator::format_accessibility(result, "", 50);
    EXPECT_NE(tree.find("- Application \"设置\""), std::string::npos);
    EXPECT_NE(tree.find("  - Button \"通用\" id=general (16,278 358x52) center=(195,304)"), std::string::npos);
    EXPECT_NE(tree.find("disabled"), std::string::npos);
    const auto found = ios_simulator::format_accessibility(result, "GENERAL", 50);
    EXPECT_NE(found.find("通用"), std::string::npos);
    EXPECT_EQ(found.find("辅助功能"), std::string::npos);
    EXPECT_NE(ios_simulator::format_accessibility(result, "nothing", 50).find("no element"), std::string::npos);
    EXPECT_NE(ios_simulator::format_accessibility(result, "", 1).find("truncated"), std::string::npos);
}

TEST(IosSimulatorConfig, SparseRoundTripAndClamping) {
    IosSimulatorConfig defaults;
    EXPECT_TRUE(ios_simulator_config_to_json(defaults).empty());
    IosSimulatorConfig custom;
    custom.enabled = false;
    custom.xcode_path = "/Users/me/Downloads/Xcode.app";
    custom.stream_fps = 60;
    auto json = ios_simulator_config_to_json(custom);
    EXPECT_EQ(json["xcode_path"], custom.xcode_path);
    IosSimulatorConfig loaded;
    load_ios_simulator_config_json(json, loaded);
    EXPECT_FALSE(loaded.enabled);
    EXPECT_EQ(loaded.xcode_path, custom.xcode_path);
    EXPECT_EQ(loaded.stream_fps, 60);
    IosSimulatorConfig clamped;
    load_ios_simulator_config_json(nlohmann::json{{"stream_fps", 500}, {"stream_scale", 0.01}}, clamped);
    EXPECT_EQ(clamped.stream_fps, 60);
    EXPECT_DOUBLE_EQ(clamped.stream_scale, 0.25);
    EXPECT_THROW(load_ios_simulator_config_json(nlohmann::json{{"enabled", "yes"}}, clamped), std::runtime_error);
}
