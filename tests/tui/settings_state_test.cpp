#include "tui/settings/settings_state.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace ats = acecode::tui::settings;

TEST(SettingsState, ParsesSettingsAndManagementDeepLinks) {
    EXPECT_EQ(
        ats::parse_settings_tab("Personalization"),
        ats::SettingsTab::Personalization);
    EXPECT_EQ(
        ats::parse_settings_tab("/archive"),
        ats::SettingsTab::Archived);
    EXPECT_EQ(
        ats::parse_management_tab("MCP Servers"),
        ats::ManagementTab::McpServers);
    EXPECT_EQ(
        ats::parse_management_tab("/tools"),
        ats::ManagementTab::Tools);
    EXPECT_FALSE(ats::parse_settings_tab("feedback").has_value());
    EXPECT_FALSE(ats::parse_management_tab("models").has_value());
}

TEST(SettingsState, SearchMatchesCaseInsensitiveMetadata) {
    const std::vector<std::string_view> fields = {
        "code-review",
        "User skill",
        "N:/skills/code-review",
    };
    EXPECT_TRUE(ats::search_matches("REVIEW", fields));
    EXPECT_TRUE(ats::search_matches("user", fields));
    EXPECT_FALSE(ats::search_matches("bundled", fields));
    EXPECT_TRUE(ats::search_matches("", fields));
}

TEST(SettingsState, DirtyNavigationRequiresExplicitDiscard) {
    ats::SettingsNavigationModel model;
    model.page(ats::SettingsTab::Configuration).filter = "remembered";
    model.set_active_tab_immediately(ats::SettingsTab::Configuration);
    model.set_dirty(true);

    EXPECT_EQ(
        model.request_tab(ats::SettingsTab::Models),
        ats::NavigationResult::NeedsDiscardConfirmation);
    EXPECT_EQ(model.active_tab(), ats::SettingsTab::Configuration);

    model.cancel_pending_navigation();
    EXPECT_EQ(model.active_tab(), ats::SettingsTab::Configuration);
    EXPECT_EQ(
        model.request_tab(ats::SettingsTab::Models),
        ats::NavigationResult::NeedsDiscardConfirmation);
    EXPECT_TRUE(model.discard_and_continue());
    EXPECT_EQ(model.active_tab(), ats::SettingsTab::Models);
    EXPECT_FALSE(model.dirty());
    EXPECT_EQ(
        model.page(ats::SettingsTab::Configuration).filter,
        "remembered");
}

TEST(SettingsState, SavedDraftContinuesPendingNavigation) {
    ats::SettingsNavigationModel model;
    model.set_active_tab_immediately(ats::SettingsTab::Personalization);
    model.set_dirty(true);

    EXPECT_EQ(
        model.request_tab(ats::SettingsTab::About),
        ats::NavigationResult::NeedsDiscardConfirmation);
    EXPECT_TRUE(model.save_and_continue());
    EXPECT_EQ(model.active_tab(), ats::SettingsTab::About);
    EXPECT_FALSE(model.dirty());
}

TEST(SettingsState, ManagementNavigationKeepsPerTabState) {
    ats::ManagementNavigationModel model;
    model.page(ats::ManagementTab::Skills).filter = "review";
    model.page(ats::ManagementTab::Skills).selected = 7;
    model.set_active_tab(ats::ManagementTab::Hooks);
    model.page(ats::ManagementTab::Hooks).filter = "shell";
    model.set_active_tab(ats::ManagementTab::Skills);

    EXPECT_EQ(model.page(ats::ManagementTab::Skills).filter, "review");
    EXPECT_EQ(model.page(ats::ManagementTab::Skills).selected, 7);
    EXPECT_EQ(model.page(ats::ManagementTab::Hooks).filter, "shell");
}

TEST(SettingsState, FootersExposeOnlyContextualActions) {
    auto config_clean = ats::settings_footer_actions(
        ats::SettingsTab::Configuration,
        false);
    EXPECT_EQ(
        std::find(
            config_clean.begin(),
            config_clean.end(),
            ats::FooterAction::Save),
        config_clean.end());

    auto config_dirty = ats::settings_footer_actions(
        ats::SettingsTab::Configuration,
        true);
    EXPECT_NE(
        std::find(
            config_dirty.begin(),
            config_dirty.end(),
            ats::FooterAction::Save),
        config_dirty.end());

    auto model_actions = ats::settings_footer_actions(
        ats::SettingsTab::Models,
        false);
    EXPECT_NE(
        std::find(
            model_actions.begin(),
            model_actions.end(),
            ats::FooterAction::Edit),
        model_actions.end());
    EXPECT_EQ(
        std::find(
            model_actions.begin(),
            model_actions.end(),
            ats::FooterAction::Add),
        model_actions.end());

    ats::ManagementRowCapabilities immutable_tool;
    auto tool_actions = ats::management_footer_actions(
        ats::ManagementTab::Tools,
        immutable_tool);
    EXPECT_EQ(
        std::find(
            tool_actions.begin(),
            tool_actions.end(),
            ats::FooterAction::Toggle),
        tool_actions.end());

    ats::ManagementRowCapabilities browser_group;
    browser_group.toggle = true;
    auto browser_actions = ats::management_footer_actions(
        ats::ManagementTab::Tools,
        browser_group);
    EXPECT_NE(
        std::find(
            browser_actions.begin(),
            browser_actions.end(),
            ats::FooterAction::Toggle),
        browser_actions.end());
}
