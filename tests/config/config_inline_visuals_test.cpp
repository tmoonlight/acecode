// 覆盖 config.inline_visuals(设置 > 常规 >「可视化图表」)的解析与稀疏序列化:
//   1. 默认开启;缺省配置不写 inline_visuals 键(sparse-on-write)
//   2. 关闭后 save / load 往返保持关闭
//   3. 类型不对(字符串、数字)时保持默认开启,不报错
// 运行中从 config.json 读回新值的用例在 default_session_preferences_test.cpp。
// 写临时 config.json 再 load_config_from_path,不碰用户 ~/.acecode。

#include <gtest/gtest.h>

#include "config/config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {

class ConfigInlineVisuals : public testing::Test {
protected:
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("acecode-inline-visuals-cfg-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path path = directory / "config.json";

    void SetUp() override { std::filesystem::create_directories(directory); }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
    }
    acecode::AppConfig load(const nlohmann::json& value) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << value.dump();
        out.close();
        return acecode::load_config_from_path(path.string(), false);
    }
    nlohmann::json saved_json() {
        std::ifstream input(path);
        return nlohmann::json::parse(input);
    }
};

// 场景:全新安装,config.json 里没有 inline_visuals。
// 期望:开关默认开启;原样保存后文件里仍然没有 inline_visuals 键(默认值不写出)。
TEST_F(ConfigInlineVisuals, DefaultsOnAndSparseWriteOmitsIt) {
    EXPECT_TRUE(acecode::InlineVisualsConfig{}.enabled);
    auto cfg = load(nlohmann::json::object());
    EXPECT_TRUE(cfg.inline_visuals.enabled);
    acecode::save_config(cfg, path.string());
    EXPECT_FALSE(saved_json().contains("inline_visuals"));
}

// 场景:用户在设置页关闭「可视化图表」,配置写成 {inline_visuals:{enabled:false}},
// 随后重启。期望:加载、保存、再加载每一步都是关闭,文件里只写 enabled:false。
TEST_F(ConfigInlineVisuals, DisabledRoundTrips) {
    auto cfg = load({{"inline_visuals", {{"enabled", false}}}});
    EXPECT_FALSE(cfg.inline_visuals.enabled);
    acecode::save_config(cfg, path.string());
    EXPECT_EQ(saved_json().at("inline_visuals"), (nlohmann::json{{"enabled", false}}));
    const auto restored = acecode::load_config_from_path(path.string(), false);
    EXPECT_FALSE(restored.inline_visuals.enabled);
}

// 场景:手改 config.json 时把 enabled 写成字符串 "false" 或数字 0,或整段写成数组。
// 期望:类型不对一律保持默认开启,加载不报错 —— 写错的值不应悄悄关掉功能。
TEST_F(ConfigInlineVisuals, InvalidValuesKeepDefault) {
    EXPECT_TRUE(load({{"inline_visuals", {{"enabled", "false"}}}}).inline_visuals.enabled);
    EXPECT_TRUE(load({{"inline_visuals", {{"enabled", 0}}}}).inline_visuals.enabled);
    EXPECT_TRUE(load({{"inline_visuals", nlohmann::json::array()}}).inline_visuals.enabled);
}

} // namespace
