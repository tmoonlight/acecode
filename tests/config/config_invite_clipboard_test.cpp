#include <gtest/gtest.h>
#include "config/config.hpp"
#include "utils/uuid.hpp"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

TEST(ConfigInviteClipboard, DefaultIsSparseAndExplicitDisabledSurvivesReload) {
    // 场景：默认开启不写配置；关闭后重启保留，重新开启时删除默认字段。
    const auto root = std::filesystem::temp_directory_path() / ("acecode-clipboard-" + acecode::generate_uuid());
    std::filesystem::create_directories(root);
    const auto path = root / "config.json";
    acecode::AppConfig config;
    EXPECT_TRUE(config.desktop.detect_invite_links_in_clipboard);
    config.desktop.detect_invite_links_in_clipboard = false;
    acecode::save_config(config, path.string());
    auto restored = acecode::load_config_from_path(path.string(), false);
    EXPECT_FALSE(restored.desktop.detect_invite_links_in_clipboard);
    restored.desktop.detect_invite_links_in_clipboard = true;
    acecode::save_config(restored, path.string());
    std::ifstream stream(path);
    const auto json = nlohmann::json::parse(stream);
    EXPECT_FALSE(json.contains("desktop") && json.at("desktop").contains("detect_invite_links_in_clipboard"));
    EXPECT_TRUE(acecode::load_config_from_path(path.string(), false).desktop.detect_invite_links_in_clipboard);
    stream.close();
    std::error_code ec; std::filesystem::remove_all(root, ec);
}
