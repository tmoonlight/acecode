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

// 场景:产品对「侧栏是否显示任务时间」有分歧,做成可配置项。期望:默认开,
// 只有显式 false 才关,非布尔值保留默认,默认值不写进 config.json(稀疏落盘
// 约定),非默认值能完整往返。
TEST(ConfigWebUiPreferencesSidebarSessionTime, DefaultsToShown) {
    WebUiPreferencesConfig prefs;
    EXPECT_TRUE(prefs.sidebar_session_time);

    AppConfig cfg;
    EXPECT_TRUE(cfg.web_ui.sidebar_session_time);
}

TEST(ConfigWebUiPreferencesSidebarSessionTime, ExplicitFalseLoads) {
    const auto path = temp_config_path("session-time-false");
    write_json(path, nlohmann::json{
        {"web_ui", {{"sidebar_session_time", false}}},
    });

    AppConfig cfg = load_config_from_path(path.string());
    EXPECT_FALSE(cfg.web_ui.sidebar_session_time);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ConfigWebUiPreferencesSidebarSessionTime, NonBooleanKeepsDefault) {
    const auto path = temp_config_path("session-time-invalid");
    write_json(path, nlohmann::json{
        {"web_ui", {{"sidebar_session_time", "no"}}},
    });

    AppConfig cfg = load_config_from_path(path.string());
    EXPECT_TRUE(cfg.web_ui.sidebar_session_time);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ConfigWebUiPreferencesSidebarSessionTime, OnlyNonDefaultIsPersisted) {
    const auto path = temp_config_path("session-time-roundtrip");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    AppConfig cfg;
    // 默认值不落盘:整个 web_ui 块应当仍然缺席。
    save_config(cfg, path.string());
    {
        std::ifstream ifs(path);
        ASSERT_TRUE(ifs.is_open());
        const auto saved = nlohmann::json::parse(ifs);
        EXPECT_FALSE(saved.contains("web_ui"));
    }

    cfg.web_ui.sidebar_session_time = false;
    save_config(cfg, path.string());
    {
        std::ifstream ifs(path);
        ASSERT_TRUE(ifs.is_open());
        const auto saved = nlohmann::json::parse(ifs);
        ASSERT_TRUE(saved.contains("web_ui"));
        EXPECT_EQ(saved["web_ui"]["sidebar_session_time"], false);
    }

    AppConfig loaded = load_config_from_path(path.string());
    EXPECT_FALSE(loaded.web_ui.sidebar_session_time);

    std::filesystem::remove(path, ec);
}

TEST(ConfigWebUiPreferencesValidation, AcceptsOnlyCanonicalValues) {
    EXPECT_TRUE(is_valid_web_ui_theme("system"));
    EXPECT_TRUE(is_valid_web_ui_theme("light"));
    EXPECT_TRUE(is_valid_web_ui_theme("dark"));
    EXPECT_FALSE(is_valid_web_ui_theme("auto"));

    EXPECT_TRUE(is_valid_web_ui_color_theme("blue"));
    EXPECT_TRUE(is_valid_web_ui_color_theme("orange"));
    EXPECT_TRUE(is_valid_web_ui_color_theme("eva-01"));
    EXPECT_TRUE(is_valid_web_ui_color_theme("national-day-2026"));
    EXPECT_TRUE(is_valid_web_ui_color_theme("ai-eva-01"));
    EXPECT_FALSE(is_valid_web_ui_color_theme("ai-../outside"));
    EXPECT_FALSE(is_valid_web_ui_color_theme("green"));

    EXPECT_TRUE(is_valid_web_ui_font_size("small"));
    EXPECT_TRUE(is_valid_web_ui_font_size("medium"));
    EXPECT_TRUE(is_valid_web_ui_font_size("large"));
    EXPECT_FALSE(is_valid_web_ui_font_size("huge"));
}

TEST(ConfigWebUiPreferencesSave, LocalThemeRoundTripsWithOrdinaryModePreference) {
    const auto path = temp_config_path("ai-theme");
    AppConfig cfg;
    cfg.web_ui.theme = "system";
    cfg.web_ui.color_theme = "ai-eva-night";
    save_config(cfg, path.string());
    const auto loaded = load_config_from_path(path.string());
    EXPECT_EQ(loaded.web_ui.theme, "system");
    EXPECT_EQ(loaded.web_ui.color_theme, "ai-eva-night");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ConfigWebUiPreferencesSave, NationalDayThemeRoundTripsForUpgradedProfiles) {
    const auto path = temp_config_path("national-day-theme");
    AppConfig cfg;
    cfg.web_ui.theme = "system";
    cfg.web_ui.color_theme = "national-day-2026";
    save_config(cfg, path.string());
    const auto loaded = load_config_from_path(path.string());
    EXPECT_EQ(loaded.web_ui.theme, "system");
    EXPECT_EQ(loaded.web_ui.color_theme, "national-day-2026");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// 消息自动折叠默认开启，旧配置和无效值兼容默认；关闭后稀疏落盘并可恢复。
TEST(ConfigWebUiPreferencesMessageAutoCollapse, DefaultsAndLegacyConfigStayEnabled) {
    EXPECT_TRUE(WebUiPreferencesConfig{}.message_auto_collapse);
    const auto path = temp_config_path("message-collapse-default");
    for (const auto& value : {nlohmann::json::object(),
                              nlohmann::json{{"message_auto_collapse", "false"}}}) {
        write_json(path, {{"web_ui", value}});
        EXPECT_TRUE(load_config_from_path(path.string()).web_ui.message_auto_collapse);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ConfigWebUiPreferencesMessageAutoCollapse, DisabledRoundTripsAndDefaultIsOmitted) {
    const auto path = temp_config_path("message-collapse-roundtrip");
    AppConfig cfg;
    cfg.web_ui.message_auto_collapse = false;
    save_config(cfg, path.string());
    {
        std::ifstream input(path);
        const auto saved = nlohmann::json::parse(input);
        EXPECT_EQ(saved["web_ui"]["message_auto_collapse"], false);
    }
    EXPECT_FALSE(load_config_from_path(path.string()).web_ui.message_auto_collapse);
    cfg.web_ui.message_auto_collapse = true;
    save_config(cfg, path.string());
    {
        std::ifstream input(path);
        EXPECT_FALSE(nlohmann::json::parse(input).contains("web_ui"));
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// 场景:主题风格 ID 的格式校验(配置文件、REST 与前端共用同一规则)。
// 期望:空串表示主题默认风格;只接受 1-24 位小写字母数字、单个连字符分隔的词。
// 大写、首尾或连续连字符、超长、非 ASCII 都拒绝——它会直接写进 HTML 属性。
TEST(ConfigWebUiPreferencesThemeVariant, ValidatorAcceptsOnlyLowercaseSlugs) {
    EXPECT_TRUE(is_valid_web_ui_theme_variant(""));
    EXPECT_TRUE(is_valid_web_ui_theme_variant("red"));
    EXPECT_TRUE(is_valid_web_ui_theme_variant("cyan-2"));
    EXPECT_TRUE(is_valid_web_ui_theme_variant(std::string(24, 'a')));
    EXPECT_FALSE(is_valid_web_ui_theme_variant(std::string(25, 'a')));
    // "\xE7\xBA\xA2" 是 UTF-8 的「红」:非 ASCII 风格名同样拒绝。
    for (const char* bad : {"Red", "-red", "red-", "red--cyan", "red cyan", "red_cyan", "\xE7\xBA\xA2", "red\n"}) {
        EXPECT_FALSE(is_valid_web_ui_theme_variant(bad)) << bad;
    }
}

// 场景:旧配置没有新字段 / 新字段合法 / 新字段被手改坏。
// 期望:缺省为默认风格 + 动效开启;合法值读入;坏值逐项回退默认并只记警告,
// 不能连带其它外观字段一起失效(外观整体判失败会把主题回退成蓝色)。
TEST(ConfigWebUiPreferencesThemeVariant, LoaderReadsValidValuesAndIgnoresInvalidOnes) {
    {
        const AppConfig defaults;
        EXPECT_EQ(defaults.web_ui.theme_variant, "");
        EXPECT_TRUE(defaults.web_ui.theme_effects);
    }
    const auto path = temp_config_path("theme-variant");
    write_json(path, {{"web_ui", {{"color_theme", "orange"}, {"theme_variant", "cyan"}, {"theme_effects", false}}}});
    {
        const AppConfig cfg = load_config_from_path(path.string());
        EXPECT_EQ(cfg.web_ui.theme_variant, "cyan");
        EXPECT_FALSE(cfg.web_ui.theme_effects);
        EXPECT_EQ(cfg.web_ui.color_theme, "orange");
    }
    write_json(path, {{"web_ui", {{"color_theme", "orange"}, {"theme_variant", "Cyan"}, {"theme_effects", "off"}}}});
    {
        const AppConfig cfg = load_config_from_path(path.string());
        EXPECT_EQ(cfg.web_ui.theme_variant, "");
        EXPECT_TRUE(cfg.web_ui.theme_effects);
        EXPECT_EQ(cfg.web_ui.color_theme, "orange");
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// 场景:保存配置时风格与动效保持稀疏写盘。
// 期望:默认值(空风格、动效开)不出现在 config.json;非默认值写盘后能原样读回,
// 切回默认后整个 web_ui 段再次消失。
TEST(ConfigWebUiPreferencesThemeVariant, SaveIsSparseAndRoundTrips) {
    const auto path = temp_config_path("theme-variant-save");
    AppConfig cfg;
    cfg.web_ui.theme_variant = "yellow";
    cfg.web_ui.theme_effects = false;
    save_config(cfg, path.string());
    {
        std::ifstream input(path);
        const auto saved = nlohmann::json::parse(input);
        EXPECT_EQ(saved["web_ui"]["theme_variant"], "yellow");
        EXPECT_EQ(saved["web_ui"]["theme_effects"], false);
    }
    const auto loaded = load_config_from_path(path.string());
    EXPECT_EQ(loaded.web_ui.theme_variant, "yellow");
    EXPECT_FALSE(loaded.web_ui.theme_effects);
    cfg.web_ui.theme_variant.clear();
    cfg.web_ui.theme_effects = true;
    save_config(cfg, path.string());
    {
        std::ifstream input(path);
        EXPECT_FALSE(nlohmann::json::parse(input).contains("web_ui"));
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ConfigWebUiPreferencesSave, DownloadedThemePreservesOrdinaryDarkPreference) {
    const auto path = temp_config_path("eva-theme");
    AppConfig cfg;
    cfg.web_ui.theme = "dark";
    cfg.web_ui.color_theme = "eva-01";
    save_config(cfg, path.string());
    const auto loaded = load_config_from_path(path.string());
    EXPECT_EQ(loaded.web_ui.theme, "dark");
    EXPECT_EQ(loaded.web_ui.color_theme, "eva-01");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
