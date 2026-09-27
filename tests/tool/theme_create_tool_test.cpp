#include <gtest/gtest.h>
#include "tool/theme_create_tool.hpp"
#include "session/session_manager.hpp"
#include "permissions/interaction_mode.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"
#include "test_support/themes/theme_test_resources.hpp"

namespace {
using nlohmann::json;

TEST(ThemeCreateTool, MissingSessionFailsWithoutCreatingResources) {
    const auto result = acecode::create_theme_create_tool().execute(R"({"action":"status"})", {});
    EXPECT_FALSE(result.success);
    EXPECT_EQ(json::parse(result.output)["error"], "THEME_SESSION_REQUIRED");
}

TEST(ThemeCreateTool, MissingChannelDenyUnattendedAndHeadlessCannotCreateApprovals) {
    const auto root = std::filesystem::temp_directory_path() / ("ace-theme-tool-" + acecode::generate_uuid());
    acecode::SessionManager session;
    session.start_session(acecode::path_to_utf8(root), "test", "model", "theme-tool-session");
    acecode::ToolContext ctx;
    ctx.session_manager = &session;
    auto tool = acecode::create_theme_create_tool(root / "themes");
    const auto rejected = [&] {
        const auto result = tool.execute(R"({"action":"palette"})", ctx);
        EXPECT_FALSE(result.success);
        EXPECT_EQ(json::parse(result.output)["error"], "THEME_INTERACTION_REQUIRED");
        EXPECT_FALSE(std::filesystem::exists(root / "themes"));
    };
    rejected();
    int asked = 0;
    ctx.ask_user_questions = [&](const json&) { ++asked; return json::object(); };
    ctx.question_policy = [] {
        acecode::ResolvedQuestionPolicy policy;
        policy.policy = acecode::QuestionPolicy::Deny;
        return policy;
    };
    rejected();
    ctx.question_policy = {};
    ctx.goal_unattended_active = [] { return true; };
    rejected();
    ctx.goal_unattended_active = {};
    const bool previous_headless = acecode::headless::active();
    acecode::headless::set_active(true);
    rejected();
    acecode::headless::set_active(previous_headless);
    EXPECT_EQ(asked, 0);
    const auto status = tool.execute(R"({"action":"status"})", ctx);
    EXPECT_TRUE(status.success);
    EXPECT_TRUE(json::parse(status.output)["drafts"].empty());
    EXPECT_FALSE(status.metadata.contains("theme_created"));
}
TEST(ThemeCreateTool, AppearanceIsAdvertisedPersistedAndRejectedBeforeInvalidConfirmation) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("ace-theme-tool-" + acecode::generate_uuid());
    acecode::SessionManager session;
    session.start_session(acecode::path_to_utf8(root), "test", "model", "theme-appearance-session");
    acecode::ToolContext ctx;
    ctx.session_manager = &session;
    int asked = 0;
    ctx.ask_user_questions = [&](const json& payload) {
        ++asked;
        return json{{"answers", json::array({{{"question_id", payload[0]["id"]},
            {"selected", json::array({payload[0]["options"][0]["value"]})}, {"custom_text", ""}}})}};
    };
    auto tool = acecode::create_theme_create_tool(root / "themes");
    EXPECT_TRUE(tool.definition.parameters.at("properties").contains("appearance"));
    json args = {{"action", "palette"}, {"name", "Example"}, {"mode", "dark"},
        {"colors", theme_test::definition("").at("colors")}};
    for (const auto& invalid : std::vector<json>{nullptr, json::array(), {{"unknown", true}},
             {{"logo_color", "#FFF"}}, {{"home_title_color", false}}, {{"extend_to_titlebar", 1}}}) {
        args["appearance"] = invalid;
        const auto result = tool.execute(args.dump(), ctx);
        EXPECT_FALSE(result.success) << invalid;
        EXPECT_EQ(json::parse(result.output)["error"], "THEME_INVALID_APPEARANCE") << invalid;
    }
    EXPECT_EQ(asked, 0);
    EXPECT_FALSE(fs::exists(root / "themes"));
    const json appearance = {{"logo_color", "#9B6DFF"}, {"home_title_color", "#FFFFFF"},
        {"extend_to_titlebar", true}};
    args["appearance"] = appearance;
    const auto result = tool.execute(args.dump(), ctx);
    ASSERT_TRUE(result.success) << result.output;
    const auto state = json::parse(result.output);
    EXPECT_TRUE(state["confirmed"]);
    EXPECT_EQ(state["appearance"], appearance);
    EXPECT_EQ(asked, 1);
    const auto status = tool.execute(json{{"action", "status"}, {"draft_id", state.at("draft_id")}}.dump(), ctx);
    ASSERT_TRUE(status.success) << status.output;
    EXPECT_EQ(json::parse(status.output)["appearance"], appearance);
    std::error_code ec;
    fs::remove_all(root, ec);
}
} // namespace
