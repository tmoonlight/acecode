// GET / PUT /api/config/auto-review 的纯函数部分(openspec add-auto-review-mode,规格
// 「裁决模型的配置与接口」)。自动模式（试验）的裁决模型:saved_models 里的一个名字,
// 留空 = 跟随会话当前模型。

#include <gtest/gtest.h>

#include "web/handlers/auto_review_handler.hpp"

#include <string>

using namespace acecode;
using namespace acecode::web;
using nlohmann::json;

namespace {

AppConfig review_config() {
    AppConfig config;
    for (const auto* name : {"main", "reviewer"}) {
        ModelProfile profile;
        profile.name = name;
        profile.provider = "openai";
        profile.model = std::string(name) + "-model";
        profile.base_url = "http://127.0.0.1:11434/v1";
        profile.api_key = "fixture-secret";
        config.saved_models.push_back(profile);
    }
    return config;
}

} // namespace

// 场景:设置页打开「自动模式（试验）」卡片时读取当前设置。
// 期望:没配置时 model_name 为空、configured 为 false;配置了存在的模型时 configured
// 为 true;返回可选模型的名字、provider 与模型标识,但绝不带凭据或端点。
TEST(AutoReviewHandler, ReportsTheCurrentChoiceWithoutSecrets) {
    auto config = review_config();
    auto initial = auto_review_settings(config);
    EXPECT_EQ(initial["model_name"], "");
    EXPECT_EQ(initial["configured"], false);
    ASSERT_EQ(initial["models"].size(), 2u);
    EXPECT_EQ(initial["models"][1], (json{{"name", "reviewer"}, {"provider", "openai"},
                                          {"model", "reviewer-model"}}));
    EXPECT_EQ(initial.dump().find("fixture-secret"), std::string::npos);
    EXPECT_EQ(initial.dump().find("11434"), std::string::npos);

    config.auto_review.model_name = "reviewer";
    EXPECT_EQ(auto_review_settings(config)["configured"], true);
}

// 场景:用户在卡片里选了一个已保存模型,之后又改回「跟随当前模型」。
// 期望:两次都成功;改回时 model_name 变为空串。
TEST(AutoReviewHandler, AcceptsAnExistingModelAndTheEmptyChoice) {
    auto config = review_config();
    std::string error;
    ASSERT_TRUE(apply_auto_review_settings(config, {{"model_name", "reviewer"}}, error)) << error;
    EXPECT_EQ(config.auto_review.model_name, "reviewer");
    ASSERT_TRUE(apply_auto_review_settings(config, {{"model_name", ""}}, error)) << error;
    EXPECT_TRUE(config.auto_review.model_name.empty());
    // 空对象 = 什么都不改。
    config.auto_review.model_name = "reviewer";
    ASSERT_TRUE(apply_auto_review_settings(config, json::object(), error)) << error;
    EXPECT_EQ(config.auto_review.model_name, "reviewer");
}

// 场景:提交一个不在已保存模型里的名字(规格「拒绝未知名字」),或者类型 / 字段不对。
// 期望:失败并给出原因,配置保持原值。路由据此返回 400。
TEST(AutoReviewHandler, RejectsUnknownNamesAndMalformedPatches) {
    auto config = review_config();
    config.auto_review.model_name = "reviewer";
    std::string error;
    for (const json& bad : {json{{"model_name", "no-such-model"}}, json{{"model_name", 7}},
                            json{{"enabled", true}}, json::array(), json("reviewer")}) {
        EXPECT_FALSE(apply_auto_review_settings(config, bad, error)) << bad.dump();
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(config.auto_review.model_name, "reviewer");
    }
}

// 场景:配置的裁决模型后来被用户从已保存模型里删掉了,设置页原样回写这个旧名字
// (比如只是重新保存一次)。
// 期望:不报错 —— 运行时本来就会回退到会话当前模型;读取时 configured 为 false,
// 前端据此把它显示成失效的选项。换成另一个不存在的名字仍然拒绝。
TEST(AutoReviewHandler, KeepsAPreviouslySavedNameThatNoLongerExists) {
    auto config = review_config();
    config.auto_review.model_name = "deleted";
    EXPECT_EQ(auto_review_settings(config)["configured"], false);

    std::string error;
    EXPECT_TRUE(apply_auto_review_settings(config, {{"model_name", "deleted"}}, error)) << error;
    EXPECT_EQ(config.auto_review.model_name, "deleted");
    EXPECT_FALSE(apply_auto_review_settings(config, {{"model_name", "also-missing"}}, error));
    EXPECT_EQ(config.auto_review.model_name, "deleted");
}
