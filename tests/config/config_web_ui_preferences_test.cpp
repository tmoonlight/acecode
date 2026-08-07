// 覆盖 Web UI 偏好配置段的默认值、解析兼容和落盘语义。

#include <gtest/gtest.h>

#include "config/config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

using namespace acecode;

namespace {

std::filesystem::path temp_config_path(const std::string& label) {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ("acecode-web-ui-prefs-" + label + "-" +
         std::to_string(suffix) + ".json");
}

void write_json(const std::filesystem::path& path,
                const nlohmann::json& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    output << value.dump(2);
    ASSERT_TRUE(output.good());
}

} // namespace

TEST(ConfigWebUiPreferencesDefaults, StructUsesStableAppearanceDefaults) {
    WebUiPreferencesConfig prefs;
    EXPECT_FALSE(prefs.show_acecode_avatar);
    EXPECT_EQ(prefs.theme, "system");
    EXPECT_EQ(prefs.color_theme, "blue");
    EXPECT_EQ(prefs.font_size, "medium");
}

TEST(ConfigWebUiPreferencesDefaults, NestedInAppConfigUsesStableDefaults) {
    AppConfig cfg;
    EXPECT_FALSE(cfg.web_ui.show_acecode_avatar);
    EXPECT_EQ(cfg.web_ui.theme, "system");
    EXPECT_EQ(cfg.web_ui.color_theme, "blue");
    EXPECT_EQ(cfg.web_ui.font_size, "medium");
}

TEST(ConfigWebUiPreferencesLoader, MissingBlockKeepsDefault) {
    const auto path = temp_config_path("missing");
    write_json(path, nlohmann::json::object());
    const AppConfig cfg = load_config_from_path(path.string());
    EXPECT_EQ(cfg.web_ui.theme, "system");
    EXPECT_EQ(cfg.web_ui.color_theme, "blue");
    EXPECT_EQ(cfg.web_ui.font_size, "medium");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ConfigWebUiPreferencesLoader, LegacyExplicitTrueIsIgnored) {
    const auto path = temp_config_path("legacy-avatar");
    write_json(path, {{"web_ui", {{"show_acecode_avatar", true}}}});
    const AppConfig cfg = load_config_from_path(path.string());
    EXPECT_FALSE(cfg.web_ui.show_acecode_avatar);
    EXPECT_EQ(cfg.web_ui.theme, "system");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ConfigWebUiPreferencesLoader, ValidAppearanceValuesLoad) {
    const auto path = temp_config_path("valid");
    write_json(path, {{"web_ui", {
        {"theme", "dark"},
        {"color_theme", "orange"},
        {"font_size", "large"},
    }}});
    const AppConfig cfg = load_config_from_path(path.string());
    EXPECT_EQ(cfg.web_ui.theme, "dark");
    EXPECT_EQ(cfg.web_ui.color_theme, "orange");
    EXPECT_EQ(cfg.web_ui.font_size, "large");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ConfigWebUiPreferencesLoader, InvalidAppearanceValuesKeepDefaults) {
    const auto path = temp_config_path("invalid");
    write_json(path, {{"web_ui", {
        {"theme", "sepia"},
        {"color_theme", 7},
        {"font_size", "huge"},
    }}});
    const AppConfig cfg = load_config_from_path(path.string());
    EXPECT_EQ(cfg.web_ui.theme, "system");
    EXPECT_EQ(cfg.web_ui.color_theme, "blue");
    EXPECT_EQ(cfg.web_ui.font_size, "medium");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ConfigWebUiPreferencesSave, DoesNotPersistDisabledDefault) {
    const auto path = temp_config_path("default-save");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    AppConfig cfg;
    cfg.web_ui.show_acecode_avatar = false;
    save_config(cfg, path.string());

    std::ifstream ifs(path);
    ASSERT_TRUE(ifs.is_open());
    const auto j = nlohmann::json::parse(ifs);
    EXPECT_FALSE(j.contains("web_ui"));

    std::filesystem::remove(path, ec);
}

TEST(ConfigWebUiPreferencesSave, NonDefaultAppearanceRoundTrips) {
    const auto path = temp_config_path("roundtrip");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    AppConfig cfg;
    cfg.web_ui.theme = "light";
    cfg.web_ui.color_theme = "orange";
    cfg.web_ui.font_size = "small";
    save_config(cfg, path.string());

    std::ifstream input(path, std::ios::binary);
    ASSERT_TRUE(input.is_open());
    const auto json = nlohmann::json::parse(input);
    ASSERT_TRUE(json.contains("web_ui"));
    EXPECT_EQ(json["web_ui"]["theme"], "light");
    EXPECT_EQ(json["web_ui"]["color_theme"], "orange");
    EXPECT_EQ(json["web_ui"]["font_size"], "small");
    EXPECT_FALSE(json["web_ui"].contains("show_acecode_avatar"));

    const AppConfig loaded = load_config_from_path(path.string());
    EXPECT_EQ(loaded.web_ui.theme, "light");
    EXPECT_EQ(loaded.web_ui.color_theme, "orange");
    EXPECT_EQ(loaded.web_ui.font_size, "small");

    std::filesystem::remove(path, ec);
}

TEST(ConfigWebUiPreferencesValidation, AcceptsOnlyCanonicalValues) {
    EXPECT_TRUE(is_valid_web_ui_theme("system"));
    EXPECT_TRUE(is_valid_web_ui_theme("light"));
    EXPECT_TRUE(is_valid_web_ui_theme("dark"));
    EXPECT_FALSE(is_valid_web_ui_theme("auto"));

    EXPECT_TRUE(is_valid_web_ui_color_theme("blue"));
    EXPECT_TRUE(is_valid_web_ui_color_theme("orange"));
    EXPECT_FALSE(is_valid_web_ui_color_theme("green"));

    EXPECT_TRUE(is_valid_web_ui_font_size("small"));
    EXPECT_TRUE(is_valid_web_ui_font_size("medium"));
    EXPECT_TRUE(is_valid_web_ui_font_size("large"));
    EXPECT_FALSE(is_valid_web_ui_font_size("huge"));
}
