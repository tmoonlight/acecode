#include "config/config.hpp"
#include "utils/http_url_validation.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>

using namespace acecode;
using nlohmann::json;

TEST(SpeechRecognitionConfig, DefaultsAndHttpPorts) {
    SpeechRecognitionConfig config;
    EXPECT_FALSE(config.enabled);
    EXPECT_FALSE(speech_recognition_configured(config));
    EXPECT_FALSE(config.use_proxy);
    std::string error;
    ASSERT_TRUE(apply_speech_recognition_config(config,
        {{"enabled", true}, {"base_url", "http://192.168.1.50:9876/"}}, error));
    EXPECT_EQ(config.base_url, "http://192.168.1.50:9876");
    EXPECT_TRUE(speech_recognition_configured(config));
    EXPECT_FALSE(utils::is_valid_http_base_url(config.base_url)); // Other consumers retain their HTTPS policy.
    ASSERT_TRUE(apply_speech_recognition_config(config, {{"protocol", "openai"}}, error));
    EXPECT_FALSE(speech_recognition_configured(config));
    ASSERT_TRUE(apply_speech_recognition_config(config, {{"model", "local-whisper"}}, error));
    EXPECT_TRUE(speech_recognition_configured(config));
}

TEST(SpeechRecognitionConfig, InvalidPatchIsAtomicAndSecretsAreNotEchoed) {
    for (const auto& patch : std::vector<json>{
        json::array(), {{"enabled", "yes"}}, {{"protocol", "unknown"}},
        {{"base_url", "http://server:0"}}, {{"base_url", "http://server:65536"}},
        {{"base_url", "http://server:abc"}}, {{"base_url", "file:///tmp"}},
        {{"base_url", "http://secret@server"}}, {{"base_url", "http://server?key=secret"}},
        {{"api_key", "secret\r\nheader"}}, {{"timeout_ms", 1.5}},
        {{"timeout_ms", 9000000000000000000ULL}}, {{"max_recording_seconds", 0}},
        {{"use_proxy", 1}},
    }) {
        SpeechRecognitionConfig config;
        config.api_key = "saved-secret";
        const auto before = speech_recognition_config_json(config);
        std::string error;
        EXPECT_FALSE(apply_speech_recognition_config(config, patch, error));
        EXPECT_EQ(speech_recognition_config_json(config), before);
        EXPECT_EQ(error.find("secret"), std::string::npos);
    }
}

TEST(SpeechRecognitionConfig, OmittedKeyPreservedAndEmptyKeyCleared) {
    SpeechRecognitionConfig config;
    config.api_key = "fixture-key";
    std::string error;
    ASSERT_TRUE(apply_speech_recognition_config(config, {{"enabled", true}}, error));
    EXPECT_EQ(config.api_key, "fixture-key");
    ASSERT_TRUE(apply_speech_recognition_config(config, {{"api_key", ""}}, error));
    EXPECT_TRUE(config.api_key.empty());
}

TEST(SpeechRecognitionConfig, RealConfigFileRoundTrip) {
    const auto root = std::filesystem::temp_directory_path() /
        ("acecode-speech-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    } cleanup{root};
    const auto path = (root / "config.json").string();
    AppConfig config;
    config.speech_recognition = {true, "openai", "http://speech.internal:9988/v1", "whisper-local",
                                 "fixture-key", "zh", 45000, 90, false};
    save_config(config, path);
    const auto loaded = load_config_from_path(path);
    EXPECT_EQ(speech_recognition_config_json(config.speech_recognition),
              speech_recognition_config_json(loaded.speech_recognition));
}
