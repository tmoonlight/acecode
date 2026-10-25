// 覆盖 config.image_compression(设置 > 常规 >「压缩图片」)的解析、稀疏序列化与
// 运行中刷新:
//   1. 默认开启;缺省配置不写 image_compression 键(sparse-on-write)
//   2. 关闭后 save / load 往返保持关闭;类型不对时保持默认开启
//   3. 手改 config.json(或另一个 daemon 在设置页改了开关)后,
//      refresh_model_config_if_changed 把新值写进内存配置并同步进程级开关
// 与 config_tool_preamble_test 同款:写临时 config.json 再 load_config_from_path,
// 不碰用户 ~/.acecode。

#include <gtest/gtest.h>

#include "config/config.hpp"
#include "config/model_config_refresh.hpp"
#include "image/image_processor.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {

class ConfigImageCompression : public testing::Test {
protected:
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("acecode-image-compression-cfg-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path path = directory / "config.json";
    bool previous_switch = acecode::image::model_image_compression_enabled();

    void SetUp() override { std::filesystem::create_directories(directory); }
    void TearDown() override {
        acecode::image::set_model_image_compression_enabled(previous_switch);
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
    }
    void write(const nlohmann::json& value) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << value.dump();
    }
    acecode::AppConfig load(const nlohmann::json& value) {
        write(value);
        return acecode::load_config_from_path(path.string(), false);
    }
    nlohmann::json saved_json() {
        std::ifstream input(path);
        return nlohmann::json::parse(input);
    }
};

// 场景:缺省配置加载后原样保存。期望:开关开启;保存出的文件里没有 image_compression 键。
TEST_F(ConfigImageCompression, DefaultsOnAndSparseWriteOmitsIt) {
    EXPECT_TRUE(acecode::ImageCompressionConfig{}.enabled);
    auto cfg = load(nlohmann::json::object());
    EXPECT_TRUE(cfg.image_compression.enabled);
    acecode::save_config(cfg, path.string());
    EXPECT_FALSE(saved_json().contains("image_compression"));
}

// 场景:{image_compression:{enabled:false}} 加载、保存、再加载。
// 期望:每一步都是关闭,保存出的文件只写 enabled:false。
TEST_F(ConfigImageCompression, DisabledRoundTrips) {
    auto cfg = load({{"image_compression", {{"enabled", false}}}});
    EXPECT_FALSE(cfg.image_compression.enabled);
    acecode::save_config(cfg, path.string());
    EXPECT_EQ(saved_json().at("image_compression"), (nlohmann::json{{"enabled", false}}));
    const auto restored = acecode::load_config_from_path(path.string(), false);
    EXPECT_FALSE(restored.image_compression.enabled);
}

// 场景:enabled 写成字符串 "false"。期望:类型不对保持默认开启,不报错。
TEST_F(ConfigImageCompression, InvalidValueKeepsDefault) {
    const auto cfg = load({{"image_compression", {{"enabled", "false"}}}});
    EXPECT_TRUE(cfg.image_compression.enabled);
}

// 场景:daemon 运行中,config.json 被改成 enabled:false(手改,或 Desktop 另一个工作区的
// daemon 在设置页改了开关),随后发消息触发 refresh_model_config_if_changed。
// 期望:内存配置变为关闭,进程级开关同步关闭;再改回开启同样跟上 —— 修复前其它 daemon
// 要重启才会读到新值。
TEST_F(ConfigImageCompression, RefreshAppliesHandEditedSwitch) {
    acecode::AppConfig cfg;
    acecode::image::set_model_image_compression_enabled(true);

    write({{"image_compression", {{"enabled", false}}}});
    acecode::refresh_model_config_if_changed(cfg, path.string());
    EXPECT_FALSE(cfg.image_compression.enabled);
    EXPECT_FALSE(acecode::image::model_image_compression_enabled());

    write(nlohmann::json::object());
    acecode::refresh_model_config_if_changed(cfg, path.string());
    EXPECT_TRUE(cfg.image_compression.enabled);
    EXPECT_TRUE(acecode::image::model_image_compression_enabled());
}

} // namespace
