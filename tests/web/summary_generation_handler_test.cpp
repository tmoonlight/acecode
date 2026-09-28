#include "web/handlers/summary_generation_handler.hpp"
#include "session_host/session_auto_title.hpp"
#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace acecode;
using namespace acecode::web;
using nlohmann::json;

namespace {
AppConfig summary_config() {
    AppConfig config;
    config.provider.clear();
    for (const auto* name : {"chat", "summary", "legacy"}) {
        ModelProfile profile;
        profile.name = name;
        profile.provider = "openai";
        profile.model = std::string(name) + "-model";
        profile.base_url = "http://127.0.0.1:11434/v1";
        profile.api_key = "fixture-secret";
        config.saved_models.push_back(profile);
    }
    config.default_model_name = "chat";
    return config;
}
} // namespace

TEST(SummaryGeneration, DisabledPreservesCurrentAndLegacyTitleModels) {
    auto config = summary_config();
    EXPECT_FALSE(config.summary_generation.enabled);
    config.summary_generation.model_name = "summary";
    auto selected = resolve_auto_title_profile(config, "chat", "");
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected->name, "chat");
    selected = resolve_auto_title_profile(config, "", "");
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected->name, "chat");
    config.session_title.model_name = "legacy";
    selected = resolve_auto_title_profile(config, "chat", "");
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected->name, "legacy");
}

TEST(SummaryGeneration, OverrideWinsAndPreservesFullProviderConfiguration) {
    auto config = summary_config();
    config.summary_generation = {true, "summary"};
    config.session_title.model_name = "legacy";
    auto& profile = config.saved_models[1];
    profile.endpoint_mode = "full_url";
    profile.base_url = "http://127.0.0.1:11434/v1/responses";
    profile.request_headers["X-Test"] = "fixture-header";
    profile.stream_timeout_ms = 9000;
    for (const auto* chat : {"chat", "legacy", "missing", ""}) {
        const auto selected = resolve_auto_title_profile(config, chat, "");
        ASSERT_TRUE(selected);
        EXPECT_EQ(selected->name, "summary");
        EXPECT_EQ(selected->model, "summary-model");
        EXPECT_EQ(selected->base_url, profile.base_url);
        EXPECT_EQ(selected->endpoint_mode, "full_url");
        EXPECT_EQ(selected->request_headers, profile.request_headers);
        EXPECT_EQ(selected->api_key, "fixture-secret");
        EXPECT_EQ(selected->stream_timeout_ms, 9000);
    }
    profile.provider = "copilot";
    EXPECT_EQ(resolve_auto_title_profile(config, "chat", "")->provider, "copilot");
    config.summary_generation.model_name = "chat";
    EXPECT_EQ(resolve_auto_title_profile(config, "legacy", "")->name, "chat");
}

TEST(SummaryGeneration, MissingEnabledModelNeverFallsBackToChat) {
    auto config = summary_config();
    config.session_title.model_name = "legacy";
    for (const auto* name : {"deleted", ""}) {
        config.summary_generation = {true, name};
        EXPECT_FALSE(resolve_auto_title_profile(config, "chat", ""));
    }
}

TEST(SummaryGeneration, PublicSettingsAndAtomicValidation) {
    auto config = summary_config();
    std::string error;
    EXPECT_FALSE(summary_generation_settings(config)["enabled"].get<bool>());
    ASSERT_TRUE(apply_summary_generation_settings(config,
        {{"enabled", true}, {"model_name", "summary"}}, error));
    const auto before = summary_generation_settings(config);
    EXPECT_TRUE(before["configured"].get<bool>());
    EXPECT_EQ(before["models"].size(), 3u);
    EXPECT_EQ(before.dump().find("fixture-secret"), std::string::npos);
    for (const auto& patch : std::vector<json>{
        json::array(), {{"enabled", "true"}}, {{"model_name", 2}},
        {{"model_name", "missing"}}, {{"model_name", ""}},
        {{"enabled", false}, {"api_key", "never-echo-this"}},
    }) {
        EXPECT_FALSE(apply_summary_generation_settings(config, patch, error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(error.find("never-echo-this"), std::string::npos);
        EXPECT_EQ(summary_generation_settings(config), before);
    }
    EXPECT_EQ(config.default_model_name, "chat");
    EXPECT_TRUE(config.session_title.enabled);
}

TEST(SummaryGeneration, DeletedReferenceCanBeDisabledAndSelectionRetained) {
    auto config = summary_config();
    config.summary_generation = {true, "summary"};
    config.saved_models.erase(config.saved_models.begin() + 1);
    EXPECT_FALSE(summary_generation_settings(config)["configured"].get<bool>());
    std::string error;
    ASSERT_TRUE(apply_summary_generation_settings(config, {{"enabled", false}}, error));
    EXPECT_EQ(config.summary_generation.model_name, "summary");
    EXPECT_FALSE(apply_summary_generation_settings(config, {{"enabled", true}}, error));
    EXPECT_FALSE(config.summary_generation.enabled);
    EXPECT_TRUE(apply_summary_generation_settings(config, {{"model_name", "chat"}}, error));
    EXPECT_TRUE(apply_summary_generation_settings(config, {{"enabled", true}}, error));
}

TEST(SummaryGeneration, ConfigRoundTripAndSparseDisabledDefaults) {
    const auto root = std::filesystem::temp_directory_path() /
        ("acecode-summary-config-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    } cleanup{root};
    const auto path = path_to_utf8(root / "config.json");
    auto config = summary_config();
    save_config(config, path);
    {
        std::ifstream input(root / "config.json");
        EXPECT_FALSE(json::parse(input).contains("summary_generation"));
    }
    EXPECT_FALSE(load_config_from_path(path, false).summary_generation.enabled);
    config.summary_generation = {true, "summary"};
    save_config(config, path);
    auto loaded = load_config_from_path(path, false);
    EXPECT_TRUE(loaded.summary_generation.enabled);
    EXPECT_EQ(loaded.summary_generation.model_name, "summary");
    loaded.summary_generation.enabled = false;
    save_config(loaded, path);
    loaded = load_config_from_path(path, false);
    EXPECT_FALSE(loaded.summary_generation.enabled);
    EXPECT_EQ(loaded.summary_generation.model_name, "summary");
    EXPECT_TRUE(loaded.session_title.enabled);
    EXPECT_EQ(loaded.default_model_name, "chat");
}
