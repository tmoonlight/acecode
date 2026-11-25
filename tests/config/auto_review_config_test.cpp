// 自动模式（试验）的配置段 auto_review(openspec add-auto-review-mode,规格
// 「裁决模型的配置与接口」)。只有一个字段 model_name:saved_models 里的名字,
// 留空 = 跟随会话当前模型。

#include <gtest/gtest.h>

#include "config/config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace {

std::filesystem::path temp_config_path(const std::string& label) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ("acecode-auto-review-config-" + label + "-" + std::to_string(suffix) + ".json");
}

void write_json(const std::filesystem::path& path, const nlohmann::json& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    output << value.dump(2);
    ASSERT_TRUE(output.good());
}

nlohmann::json read_json(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input.is_open());
    return nlohmann::json::parse(input);
}

void remove_file(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace

// 场景:全新配置,用户从没碰过这个设置。
// 期望:model_name 为空,含义是「用会话当前的模型裁决」。
TEST(AutoReviewConfig, DefaultsToFollowingTheSessionModel) {
    acecode::AppConfig cfg;
    EXPECT_TRUE(cfg.auto_review.model_name.empty());

    const auto path = temp_config_path("missing");
    write_json(path, nlohmann::json::object());
    EXPECT_TRUE(acecode::load_config_from_path(path.string()).auto_review.model_name.empty());
    remove_file(path);
}

// 场景:config.json 里写了 auto_review.model_name。
// 期望:原样读出。这里不校验名字是否还在 saved_models 里 —— 模型被删掉后运行时
// 回退到会话当前模型,不能让整份配置因此加载失败、触发配置回滚。
TEST(AutoReviewConfig, LoadsTheConfiguredModelName) {
    const auto path = temp_config_path("load");
    write_json(path, {{"auto_review", {{"model_name", "reviewer"}}}});
    EXPECT_EQ(acecode::load_config_from_path(path.string()).auto_review.model_name, "reviewer");
    remove_file(path);

    const auto dangling = temp_config_path("dangling");
    write_json(dangling, {{"auto_review", {{"model_name", "deleted-model"}}}});
    EXPECT_EQ(acecode::load_config_from_path(dangling.string()).auto_review.model_name, "deleted-model");
    remove_file(dangling);
}

// 场景:保存配置 —— 一次选了裁决模型,一次改回「跟随当前模型」。
// 期望:选了就写出 {"auto_review":{"model_name":…}};留空时整段省略(写入时省略
// 默认值的约定),不留下一个空对象。保存后再读,值不变。
TEST(AutoReviewConfig, SavesSparselyAndRoundTrips) {
    const auto path = temp_config_path("save");
    acecode::AppConfig cfg;
    cfg.auto_review.model_name = "reviewer";
    acecode::save_config(cfg, path.string());

    const auto saved = read_json(path);
    ASSERT_TRUE(saved.contains("auto_review"));
    EXPECT_EQ(saved["auto_review"], (nlohmann::json{{"model_name", "reviewer"}}));
    EXPECT_EQ(acecode::load_config_from_path(path.string()).auto_review.model_name, "reviewer");

    cfg.auto_review.model_name.clear();
    acecode::save_config(cfg, path.string());
    EXPECT_FALSE(read_json(path).contains("auto_review"));
    remove_file(path);
}

// 场景:手改 config.json 写错了类型 —— model_name 写成数字,或者整段写成字符串 / 数组。
// 期望:解析抛出异常并指出是 auto_review 这一段(配置加载据此判定这份文件无效),
// 已有的值不被改动。这里直接测这一段的解析函数:走完整的加载入口会触发配置回滚
// 流程,不适合单测。
TEST(AutoReviewConfig, RejectsWrongTypes) {
    for (const nlohmann::json& bad : {nlohmann::json{{"auto_review", {{"model_name", 7}}}},
                                      nlohmann::json{{"auto_review", "reviewer"}},
                                      nlohmann::json{{"auto_review", nlohmann::json::array()}}}) {
        acecode::AutoReviewConfig config;
        config.model_name = "kept";
        try {
            acecode::read_auto_review_config(bad, config);
            ADD_FAILURE() << "accepted " << bad.dump();
        } catch (const std::runtime_error& error) {
            EXPECT_NE(std::string(error.what()).find("auto_review"), std::string::npos) << error.what();
        }
        EXPECT_EQ(config.model_name, "kept");
    }

    // 没有这一段、或者段里没有 model_name:保持原值,不报错。
    acecode::AutoReviewConfig config;
    config.model_name = "kept";
    acecode::read_auto_review_config(nlohmann::json::object(), config);
    EXPECT_EQ(config.model_name, "kept");
    acecode::read_auto_review_config({{"auto_review", nlohmann::json::object()}}, config);
    EXPECT_TRUE(config.model_name.empty());
}
