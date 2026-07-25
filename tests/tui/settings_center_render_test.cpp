#include "tui/settings/management_center.hpp"
#include "tui/settings/settings_center.hpp"
#include "tui/theme_palette.hpp"
#include "tool/mcp_manager.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ats = acecode::tui::settings;
namespace fs = std::filesystem;

namespace {

acecode::AppConfig render_config() {
    acecode::AppConfig config;
    config.default_permission_mode = "default";
    config.tui.theme = "dark";
    config.upgrade.base_url = "https://updates.example.test";
    config.custom_instructions.set_text(
        "Prefer focused changes and verify the result.");

    acecode::ModelProfile alpha;
    alpha.name = "alpha";
    alpha.provider = "openai";
    alpha.model = "gpt-alpha";
    alpha.base_url = "https://api.example.test/v1";
    alpha.api_key = "render-test-secret";
    alpha.context_window = 128000;

    acecode::ModelProfile beta;
    beta.name = "beta";
    beta.provider = "anthropic";
    beta.model = "claude-beta";
    beta.base_url = "https://anthropic.example.test";

    config.saved_models = {alpha, beta};
    config.default_model_name = "alpha";
    return config;
}

std::string render_component(
    const ftxui::Component& component,
    int width = 128,
    int height = 40) {
    ftxui::Screen screen(width, height);
    ftxui::Render(screen, component->Render());
    return screen.ToString();
}

ats::SettingsCenter make_settings_center(acecode::AppConfig& config) {
    ats::SettingsCenterDependencies deps;
    deps.config = &config;
    deps.acecode_version = "render-test";
    deps.acecode_dir_override = fs::temp_directory_path()
        .append("acecode_settings_render_empty")
        .string();
    deps.post_to_ui = [](std::function<void()>) {};
    return ats::SettingsCenter(std::move(deps));
}

ats::ManagementCenter make_management_center() {
    ats::ManagementCenterDependencies deps;
    return ats::ManagementCenter(std::move(deps));
}

class SettingsCenterRenderTest
    : public testing::TestWithParam<
          std::pair<ats::SettingsTab, const char*>> {
protected:
    static void SetUpTestSuite() {
        acecode::tui::init_theme_palette("dark");
    }
};

TEST_P(SettingsCenterRenderTest, RendersEnglishPageAndTopRail) {
    for (const char* palette : {"dark", "light"}) {
        acecode::tui::swap_theme_palette(palette);
        auto config = render_config();
        auto center = make_settings_center(config);
        center.open(GetParam().first);
        center.component()->TakeFocus();

        const std::string output =
            render_component(center.component());

        EXPECT_NE(output.find("ACECode Settings"), std::string::npos)
            << palette;
        EXPECT_NE(output.find(GetParam().second), std::string::npos)
            << palette;
        EXPECT_NE(output.find("General"), std::string::npos)
            << palette;
        EXPECT_NE(output.find("Appearance"), std::string::npos)
            << palette;
        EXPECT_EQ(output.find("设置"), std::string::npos)
            << palette;
    }
    acecode::tui::swap_theme_palette("dark");
}

INSTANTIATE_TEST_SUITE_P(
    EveryTab,
    SettingsCenterRenderTest,
    testing::Values(
        std::make_pair(ats::SettingsTab::General, "Default permission mode"),
        std::make_pair(
            ats::SettingsTab::Appearance,
            "Choose the TUI color theme"),
        std::make_pair(
            ats::SettingsTab::Configuration,
            "Upgrade service URL"),
        std::make_pair(
            ats::SettingsTab::Personalization,
            "Custom instructions"),
        std::make_pair(
            ats::SettingsTab::Models,
            "Manage saved profiles"),
        std::make_pair(ats::SettingsTab::Usage, "Usage"),
        std::make_pair(ats::SettingsTab::Archived, "Archived"),
        std::make_pair(ats::SettingsTab::About, "Build and installation")));

class ManagementCenterRenderTest
    : public testing::TestWithParam<
          std::pair<ats::ManagementTab, const char*>> {
protected:
    static void SetUpTestSuite() {
        acecode::tui::init_theme_palette("dark");
    }
};

TEST_P(ManagementCenterRenderTest, RendersEnglishPageAndTopRail) {
    for (const char* palette : {"dark", "light"}) {
        acecode::tui::swap_theme_palette(palette);
        auto center = make_management_center();
        center.open(GetParam().first);
        center.component()->TakeFocus();

        const std::string output =
            render_component(center.component());

        EXPECT_NE(output.find("ACECode Capabilities"), std::string::npos)
            << palette;
        EXPECT_NE(output.find(GetParam().second), std::string::npos)
            << palette;
        EXPECT_NE(output.find("Skills"), std::string::npos)
            << palette;
        EXPECT_NE(output.find("MCP Servers"), std::string::npos)
            << palette;
    }
    acecode::tui::swap_theme_palette("dark");
}

INSTANTIATE_TEST_SUITE_P(
    EveryTab,
    ManagementCenterRenderTest,
    testing::Values(
        std::make_pair(
            ats::ManagementTab::Skills,
            "Installed instruction packages"),
        std::make_pair(
            ats::ManagementTab::McpServers,
            "Inspect configured servers"),
        std::make_pair(
            ats::ManagementTab::Connectors,
            "External integrations"),
        std::make_pair(
            ats::ManagementTab::Tools,
            "Effective tool registry"),
        std::make_pair(
            ats::ManagementTab::Hooks,
            "Normalized hook definitions")));

TEST(ManagementCenterRender, NarrowTopRailFollowsDeepLinkedTab) {
    acecode::tui::init_theme_palette("dark");
    auto center = make_management_center();
    center.open(ats::ManagementTab::Hooks);
    center.component()->TakeFocus();

    const std::string output =
        render_component(center.component(), 54, 24);

    EXPECT_NE(output.find("Hooks"), std::string::npos) << output;
    EXPECT_NE(output.find("No matching hooks"), std::string::npos)
        << output;
}

TEST(ManagementCenterRender, SearchInputOwnsShortcutLettersWhileFocused) {
    acecode::tui::init_theme_palette("dark");
    auto center = make_management_center();
    center.open(ats::ManagementTab::Skills);
    const auto component = center.component();
    component->TakeFocus();

    ASSERT_TRUE(component->OnEvent(ftxui::Event::Character('/')));
    ASSERT_TRUE(component->OnEvent(ftxui::Event::Character('r')));
    ASSERT_TRUE(component->OnEvent(ftxui::Event::Character('e')));
    ASSERT_TRUE(component->OnEvent(ftxui::Event::Character('l')));

    const std::string output = render_component(component);
    EXPECT_EQ(output.find("/ to search skills"), std::string::npos)
        << output;
    EXPECT_NE(output.find("rel"), std::string::npos) << output;
}

TEST(ManagementCenterRender, McpDetailsAndEditorDoNotExposeSecrets) {
    acecode::tui::init_theme_palette("dark");
    acecode::AppConfig config;
    acecode::McpServerConfig server;
    server.command = "node";
    server.args = {
        "server.js",
        "--token",
        "super-secret-token",
        "--api-key=another-secret",
    };
    server.env["MCP_SECRET"] = "environment-secret";
    server.disabled = true;
    config.mcp_servers["private-server"] = server;

    acecode::McpManager manager;
    ASSERT_TRUE(manager.connect_all(config));

    ats::ManagementCenterDependencies deps;
    deps.config = &config;
    deps.mcp = &manager;
    ats::ManagementCenter center(std::move(deps));
    center.open(ats::ManagementTab::McpServers);
    const auto component = center.component();
    component->TakeFocus();

    std::string output = render_component(component);
    EXPECT_NE(output.find("node (4 arguments hidden)"), std::string::npos)
        << output;
    EXPECT_EQ(output.find("super-secret-token"), std::string::npos)
        << output;
    EXPECT_EQ(output.find("another-secret"), std::string::npos)
        << output;
    EXPECT_EQ(output.find("environment-secret"), std::string::npos)
        << output;

    ASSERT_TRUE(component->OnEvent(ftxui::Event::Character('e')));
    output = render_component(component);
    EXPECT_NE(output.find("<redacted>"), std::string::npos) << output;
    EXPECT_EQ(output.find("super-secret-token"), std::string::npos)
        << output;
    EXPECT_EQ(output.find("another-secret"), std::string::npos)
        << output;
    EXPECT_EQ(output.find("environment-secret"), std::string::npos)
        << output;
}

TEST(SettingsCenterRender, NarrowTopRailFollowsDeepLinkedTab) {
    acecode::tui::init_theme_palette("dark");
    auto config = render_config();
    auto center = make_settings_center(config);
    center.open(ats::SettingsTab::About);
    center.component()->TakeFocus();

    const std::string output =
        render_component(center.component(), 58, 24);

    EXPECT_NE(output.find("About"), std::string::npos) << output;
    EXPECT_NE(output.find("Config path"), std::string::npos) << output;
}

TEST(SettingsCenterRender, LightPaletteRendersTheSameControlContract) {
    acecode::tui::init_theme_palette("light");
    auto config = render_config();
    config.tui.theme = "light";
    auto center = make_settings_center(config);
    center.open(ats::SettingsTab::General);

    const std::string output = render_component(center.component());

    EXPECT_NE(output.find("Default permission mode"), std::string::npos);
    EXPECT_NE(output.find("Native notifications"), std::string::npos);
    acecode::tui::swap_theme_palette("dark");
}

TEST(SettingsCenterRender, ReopenWithoutDeepLinkKeepsProcessTab) {
    acecode::tui::init_theme_palette("dark");
    auto config = render_config();
    auto center = make_settings_center(config);

    center.open(ats::SettingsTab::Personalization);
    center.open();

    EXPECT_EQ(
        center.active_tab(),
        ats::SettingsTab::Personalization);
    const std::string output = render_component(center.component());
    EXPECT_NE(output.find("Custom instructions"), std::string::npos);
}

TEST(SettingsCenterRender, StaleUsageCompletionCannotOverwriteNewTab) {
    acecode::tui::init_theme_palette("dark");
    auto config = render_config();
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::function<void()>> posted;

    ats::SettingsCenterDependencies deps;
    deps.config = &config;
    deps.acecode_version = "render-test";
    deps.acecode_dir_override = fs::temp_directory_path()
        .append("acecode_settings_render_empty")
        .string();
    deps.post_to_ui =
        [&](std::function<void()> task) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                posted.push_back(std::move(task));
            }
            ready.notify_one();
        };
    ats::SettingsCenter center(std::move(deps));
    center.open(ats::SettingsTab::Usage);

    std::function<void()> completion;
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(ready.wait_for(
            lock,
            std::chrono::seconds(2),
            [&]() { return !posted.empty(); }));
        completion = std::move(posted.front());
        posted.erase(posted.begin());
    }

    center.open(ats::SettingsTab::About);
    completion();

    EXPECT_EQ(center.active_tab(), ats::SettingsTab::About);
    const std::string output = render_component(center.component());
    EXPECT_EQ(
        output.find("Status: Usage refreshed"),
        std::string::npos)
        << output;
    EXPECT_NE(output.find("Config path"), std::string::npos) << output;
}

TEST(SettingsCenterRender, ModelSearchFiltersRowsAndDirtyEscapeUsesModal) {
    acecode::tui::init_theme_palette("dark");
    auto config = render_config();
    {
        auto center = make_settings_center(config);
        center.open(ats::SettingsTab::Models);
        const auto component = center.component();
        component->TakeFocus();

        ASSERT_TRUE(component->OnEvent(ftxui::Event::Character('/')));
        ASSERT_TRUE(component->OnEvent(ftxui::Event::Character('a')));
        ASSERT_TRUE(component->OnEvent(ftxui::Event::Character('l')));
        ASSERT_TRUE(component->OnEvent(ftxui::Event::Character('p')));
        ASSERT_TRUE(component->OnEvent(ftxui::Event::Character('h')));
        ASSERT_TRUE(component->OnEvent(ftxui::Event::Character('a')));
        const std::string output = render_component(component);
        EXPECT_NE(output.find("alpha"), std::string::npos) << output;
        EXPECT_EQ(output.find("beta  anthropic"), std::string::npos)
            << output;
    }

    auto modal_center = make_settings_center(config);
    modal_center.open(ats::SettingsTab::Models);
    const auto modal_component = modal_center.component();
    modal_component->TakeFocus();
    ASSERT_TRUE(
        modal_component->OnEvent(ftxui::Event::Character('a')));
    ASSERT_TRUE(
        modal_component->OnEvent(ftxui::Event::Character('x')));
    ASSERT_TRUE(modal_component->OnEvent(ftxui::Event::Escape));
    const std::string output = render_component(modal_component);
    EXPECT_NE(output.find("Unsaved model profile"), std::string::npos)
        << output;
    EXPECT_NE(output.find("Save changes"), std::string::npos) << output;
    EXPECT_NE(output.find("Discard changes"), std::string::npos) << output;
}

} // namespace
