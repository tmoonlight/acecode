#include <gtest/gtest.h>
#include "web/handlers/image_generation_handler.hpp"
#include "tool/image_generate/image_generation_policy.hpp"

using namespace acecode;
using namespace acecode::web;
using nlohmann::json;

TEST(ImageGenerationSettings, DefaultUrlOnlyRequiresAnApiKey) {
    AppConfig config;
    EXPECT_EQ(image_generation_settings(config)["base_url"], constants::ACEMODEL_API_BASE_URL);
    EXPECT_FALSE(image_generation_settings(config)["configured"].get<bool>());
    std::string error;
    ASSERT_TRUE(apply_image_generation_settings(config, {{"api_key", "fixture-image-key"}}, error));
    EXPECT_TRUE(image_generation_settings(config)["configured"].get<bool>());
}

TEST(ImageGenerationSettings, SavedKeyIsReturnedForEditingAndOmissionPreservesIt) {
    AppConfig config;
    std::string error;
    ASSERT_TRUE(apply_image_generation_settings(config,
        {{"base_url", "https://example.invalid/v1"}, {"api_key", "private-test-key"}}, error));
    auto snapshot = image_generation_settings(config);
    EXPECT_TRUE(snapshot["configured"].get<bool>());
    EXPECT_TRUE(snapshot["has_api_key"].get<bool>());
    EXPECT_EQ(snapshot["api_key"], "private-test-key");
    ASSERT_TRUE(apply_image_generation_settings(config, {{"enabled", false}}, error));
    EXPECT_EQ(config.image_generation.api_key, "private-test-key");
    EXPECT_TRUE(image_generation_settings(config)["configured"].get<bool>());
    ASSERT_TRUE(apply_image_generation_settings(config, {{"api_key", ""}}, error));
    EXPECT_FALSE(image_generation_settings(config)["configured"].get<bool>());
    EXPECT_EQ(image_generation_settings(config)["api_key"], "");
}

TEST(ImageGenerationSettings, InvalidPatchIsAtomicAndDoesNotEchoCredentials) {
    const std::vector<json> invalid = {
        json::array(), {{"enabled", "yes"}}, {{"source", "other"}},
        {{"base_url", "http://public.example/v1"}},
        {{"base_url", "https://secret@example.invalid/v1"}},
        {{"api_key", "secret\r\nheader"}}, {{"models", "wrong"}},
        {{"models", {{"standard", ""}}}}, {{"default_quality", "4k"}},
        {{"timeout_ms", 1.5}}, {{"api_key", false}},
        {{"source", "saved_model"}, {"saved_model_name", "missing"}},
    };
    for (auto patch : invalid) {
        AppConfig config;
        config.image_generation.api_key = "unchanged-secret";
        if (patch.is_object()) patch["enabled"] = patch.value("enabled", json(false));
        std::string error;
        EXPECT_FALSE(apply_image_generation_settings(config, patch, error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(error.find("secret"), std::string::npos);
        EXPECT_TRUE(config.image_generation.enabled);
        EXPECT_EQ(config.image_generation.api_key, "unchanged-secret");
    }
}

TEST(ImageGenerationSettings, ReusableConnectionsExcludeNonOpenAiAndFullEndpoints) {
    AppConfig config;
    ModelProfile profile;
    profile.name = "gateway";
    profile.provider = "openai";
    profile.base_url = "https://example.invalid/v1";
    profile.api_key = "connection-secret";
    config.saved_models.push_back(profile);
    profile.name = "anthropic";
    profile.provider = "anthropic";
    config.saved_models.push_back(profile);
    profile.name = "full-endpoint";
    profile.provider = "openai";
    profile.endpoint_mode = "full_url";
    config.saved_models.push_back(profile);
    std::string error;
    ASSERT_TRUE(apply_image_generation_settings(config,
        {{"source", "saved_model"}, {"saved_model_name", "gateway"}}, error));
    const auto snapshot = image_generation_settings(config);
    EXPECT_TRUE(snapshot["configured"].get<bool>());
    ASSERT_EQ(snapshot["connections"].size(), 1u);
    EXPECT_EQ(snapshot["connections"][0]["name"], "gateway");
    EXPECT_EQ(snapshot.dump().find("connection-secret"), std::string::npos);
    EXPECT_EQ(config.saved_models.size(), 3u);
    EXPECT_FALSE(apply_image_generation_settings(config, {{"saved_model_name", "full-endpoint"}}, error));
    config.saved_models.clear();
    EXPECT_FALSE(image_generation_settings(config)["configured"].get<bool>());
    EXPECT_TRUE(apply_image_generation_settings(config, {{"enabled", false}}, error));
    EXPECT_FALSE(config.image_generation.enabled);
}

TEST(ImageGenerationSettings, TimeoutClampsBeforeNarrowingAndMappingsAreIndependent) {
    AppConfig config;
    std::string error;
    ASSERT_TRUE(apply_image_generation_settings(config, {{"timeout_ms", -1}}, error));
    EXPECT_EQ(config.image_generation.timeout_ms, 30000);
    ASSERT_TRUE(apply_image_generation_settings(config, {{"timeout_ms", UINT64_MAX}}, error));
    EXPECT_EQ(config.image_generation.timeout_ms, 600000);
    ASSERT_TRUE(apply_image_generation_settings(config,
        {{"models", {{"high", "custom-image-hd"}}}, {"default_quality", "high"}}, error));
    EXPECT_EQ(config.image_generation.model_standard, "acemodel-image");
    EXPECT_EQ(config.image_generation.model_high, "custom-image-hd");
    EXPECT_EQ(config.image_generation.default_quality, "high");
}
