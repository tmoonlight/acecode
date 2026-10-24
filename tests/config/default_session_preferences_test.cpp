#include <gtest/gtest.h>

#include "config/config.hpp"
#include "config/model_config_refresh.hpp"
#include "config/saved_models_revision.hpp"

#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

namespace {

acecode::ModelProfile test_model(const std::string& name,
                                  const std::string& model) {
    acecode::ModelProfile profile;
    profile.name = name;
    profile.provider = "copilot";
    profile.model = model;
    return profile;
}

acecode::ModelProfile keyed_model(const std::string& name, const std::string& api_key) {
    acecode::ModelProfile profile;
    profile.name = name;
    profile.provider = "openai";
    profile.base_url = "https://gateway.example/v1";
    profile.api_key = api_key;
    profile.model = "gpt-test";
    return profile;
}

fs::path temp_config_path() {
    return fs::temp_directory_path() /
           ("acecode_default_session_preferences_" +
            std::to_string(std::random_device{}()) + ".json");
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

struct RemoveFileOnExit {
    fs::path path;
    ~RemoveFileOnExit() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

} // namespace

TEST(DefaultSessionPreferencesConfig, RefreshReadsExternalModelAndPermissionDefaults) {
    acecode::AppConfig runtime;
    runtime.saved_models = {test_model("fast", "gpt-fast")};
    runtime.default_model_name = "fast";
    runtime.default_permission_mode = "default";

    acecode::AppConfig disk = runtime;
    disk.saved_models.push_back(test_model("slow", "gpt-slow"));
    disk.default_model_name = "slow";
    disk.default_permission_mode = "plan";

    const auto path = temp_config_path();
    acecode::save_config(disk, path.string());

    std::string error;
    const auto revision = acecode::current_saved_models_revision();
    ASSERT_TRUE(acecode::refresh_default_session_preferences_from_config(
        runtime, path.string(), &error)) << error;

    ASSERT_EQ(runtime.saved_models.size(), 2u);
    EXPECT_EQ(runtime.default_model_name, "slow");
    EXPECT_EQ(runtime.default_permission_mode, "plan");
    EXPECT_EQ(runtime.saved_models[1].name, "slow");
    // 模型列表变了必须推进 revision,否则已打开的会话不会重建 Provider。
    EXPECT_EQ(acecode::current_saved_models_revision(), revision + 1);

    std::error_code ec;
    fs::remove(path, ec);
}

// 触发场景:daemon 运行中,用户直接手改 config.json 里某个模型的 api_key。
// 期望:内容变了才重读并推进一次 revision(已打开的会话据此换 key);
// 文件没再变时不重复解析 —— 内存里之后的改动不会被同一份旧文件冲掉。
TEST(DefaultSessionPreferencesConfig, ChangedConfigFileIsAppliedOnceAndPublished) {
    const auto path = temp_config_path();
    RemoveFileOnExit cleanup{path};
    acecode::AppConfig disk;
    disk.saved_models = {keyed_model("main", "sk-old")};
    disk.default_model_name = "main";
    acecode::save_config(disk, path.string());

    acecode::AppConfig runtime = disk;
    ASSERT_TRUE(acecode::refresh_model_config_if_changed(runtime, path.string()));

    disk.saved_models.front().api_key = "sk-new";
    acecode::save_config(disk, path.string());
    const auto revision = acecode::current_saved_models_revision();
    ASSERT_TRUE(acecode::refresh_model_config_if_changed(runtime, path.string()));
    EXPECT_EQ(runtime.saved_models.front().api_key, "sk-new");
    EXPECT_EQ(acecode::current_saved_models_revision(), revision + 1);

    runtime.saved_models.front().api_key = "sk-set-in-memory";
    ASSERT_TRUE(acecode::refresh_model_config_if_changed(runtime, path.string()));
    EXPECT_EQ(runtime.saved_models.front().api_key, "sk-set-in-memory");
    EXPECT_EQ(acecode::current_saved_models_revision(), revision + 1);
}

// 触发场景:编辑器保存到一半(或手误)时 config.json 不是合法 JSON,恰好此时发消息。
// 期望:保留当前模型配置、不推进 revision、返回 false;文件改成合法内容后下一次
// 检查立即生效。半截文件若被当成「没有模型」,正在用的会话会全部失效。
TEST(DefaultSessionPreferencesConfig, InvalidConfigFileKeepsProfilesUntilFixed) {
    const auto path = temp_config_path();
    RemoveFileOnExit cleanup{path};
    acecode::AppConfig runtime;
    runtime.saved_models = {keyed_model("main", "sk-old")};
    runtime.default_model_name = "main";
    const auto revision = acecode::current_saved_models_revision();

    write_text(path, R"({"saved_models":[{"name":"main","provider":"openai")");
    EXPECT_FALSE(acecode::refresh_model_config_if_changed(runtime, path.string()));
    ASSERT_EQ(runtime.saved_models.size(), 1u);
    EXPECT_EQ(runtime.saved_models.front().api_key, "sk-old");
    EXPECT_EQ(acecode::current_saved_models_revision(), revision);

    acecode::AppConfig fixed = runtime;
    fixed.saved_models.front().api_key = "sk-fixed";
    acecode::save_config(fixed, path.string());
    ASSERT_TRUE(acecode::refresh_model_config_if_changed(runtime, path.string()));
    EXPECT_EQ(runtime.saved_models.front().api_key, "sk-fixed");
    EXPECT_EQ(acecode::current_saved_models_revision(), revision + 1);
}

// 触发场景:手改 config.json 删掉了默认模型那一条,却没改 default_model_name。
// 期望:与启动加载一致,默认模型退到第一条,新模型列表照常生效;
// 不能因为默认名悬空就整份拒绝,让新配置一直读不进来。
TEST(DefaultSessionPreferencesConfig, RemovedDefaultModelFallsBackToFirstProfile) {
    const auto path = temp_config_path();
    RemoveFileOnExit cleanup{path};
    acecode::AppConfig runtime;
    runtime.saved_models = {keyed_model("old", "sk-old")};
    runtime.default_model_name = "old";

    write_text(path, R"({
      "default_model_name": "old",
      "saved_models": [
        {"name": "new-a", "provider": "openai", "base_url": "https://gateway.example/v1",
         "api_key": "sk-a", "model": "gpt-a"},
        {"name": "new-b", "provider": "openai", "base_url": "https://gateway.example/v1",
         "api_key": "sk-b", "model": "gpt-b"}
      ]
    })");
    ASSERT_TRUE(acecode::refresh_model_config_if_changed(runtime, path.string()));
    ASSERT_EQ(runtime.saved_models.size(), 2u);
    EXPECT_EQ(runtime.saved_models[0].name, "new-a");
    EXPECT_EQ(runtime.default_model_name, "new-a");
}

