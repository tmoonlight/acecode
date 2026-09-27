// 场景:迁移原状态持久化用例,保持文件格式、容错与失败返回语义。
#include <gtest/gtest.h>
#include "provider/model_probe_cache.hpp"
#include "test_support/utils/state_file_fixture.hpp"

#include <cstdint>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using acecode::test_support::StateFileTest;
using acecode::test_support::write_raw;

TEST_F(StateFileTest, ModelProbeCacheRoundTripsAndPreservesOtherState) {
    const std::string fingerprint(64, 'a');
    acecode::write_state_flag("some_flag", true);

    acecode::ModelProbeCacheEntry entry;
    entry.models = {"starrylight", "moonlight", "starrylight"};
    entry.context_windows = {
        {"starrylight", 200000},
        {"moonlight", 128000},
        {"not-in-models", 999},
    };
    entry.probed_at_ms = 123456789;

    ASSERT_TRUE(acecode::write_model_probe_cache(fingerprint, entry));
    auto cached = acecode::read_model_probe_cache(fingerprint);
    ASSERT_TRUE(cached.has_value());
    EXPECT_EQ(cached->models,
              (std::vector<std::string>{"starrylight", "moonlight"}));
    EXPECT_EQ(cached->context_windows,
              (std::map<std::string, int>{{"moonlight", 128000},
                                          {"starrylight", 200000}}));
    EXPECT_EQ(cached->probed_at_ms, 123456789);
    EXPECT_TRUE(acecode::read_state_flag("some_flag"));

    entry.models = {"aurora"};
    entry.context_windows = {{"aurora", 200000}};
    entry.probed_at_ms = 123456790;
    ASSERT_TRUE(acecode::write_model_probe_cache(fingerprint, entry));
    cached = acecode::read_model_probe_cache(fingerprint);
    ASSERT_TRUE(cached.has_value());
    EXPECT_EQ(cached->models, (std::vector<std::string>{"aurora"}));
    EXPECT_EQ(cached->context_windows.at("aurora"), 200000);
    EXPECT_EQ(cached->probed_at_ms, 123456790);
}

TEST_F(StateFileTest, ModelProbeCacheRejectsInvalidFingerprintAndMalformedEntry) {
    acecode::ModelProbeCacheEntry entry;
    entry.models = {"model-a"};
    EXPECT_FALSE(acecode::write_model_probe_cache("not-a-sha256", entry));
    EXPECT_FALSE(acecode::read_model_probe_cache("not-a-sha256").has_value());
    EXPECT_FALSE(fs::exists(path_));

    const std::string fingerprint(64, 'b');
    nlohmann::json malformed = {
        {"model_probe_cache",
         {{fingerprint,
           {{"version", 1}, {"models", "not-an-array"}}}}},
    };
    write_raw(path_, malformed.dump());
    EXPECT_FALSE(acecode::read_model_probe_cache(fingerprint).has_value());
}

TEST_F(StateFileTest, ModelProbeCachePreservesReasoningAndExplicitRemoval) {
    const std::string fingerprint(64, 'd');
    acecode::ModelReasoningOptions reasoning;
    reasoning.supported = true;
    reasoning.default_enabled = true;
    reasoning.supported_efforts = {"low", "high"};
    reasoning.default_effort = "high";
    acecode::ModelProbeCacheEntry entry;
    entry.models = {"supported", "unknown"};
    entry.reasoning["supported"] = reasoning;
    entry.reasoning["not-in-models"] = reasoning;
    ASSERT_TRUE(acecode::write_model_probe_cache(fingerprint, entry));
    auto restored = acecode::read_model_probe_cache(fingerprint);
    ASSERT_TRUE(restored.has_value());
    ASSERT_TRUE(restored->reasoning.at("supported").has_value());
    EXPECT_EQ(restored->reasoning.at("supported")->supported_efforts,
              reasoning.supported_efforts);
    EXPECT_EQ(restored->reasoning.at("supported")->default_effort, "high");
    EXPECT_FALSE(restored->reasoning.at("unknown").has_value());
    EXPECT_EQ(restored->reasoning.count("not-in-models"), 0u);

    entry.reasoning.clear();
    ASSERT_TRUE(acecode::write_model_probe_cache(fingerprint, entry));
    restored = acecode::read_model_probe_cache(fingerprint);
    ASSERT_TRUE(restored.has_value());
    ASSERT_EQ(restored->reasoning.size(), 2u);
    EXPECT_FALSE(restored->reasoning.at("supported").has_value());
    EXPECT_FALSE(restored->reasoning.at("unknown").has_value());
}

TEST_F(StateFileTest, LegacyOrInvalidProbeReasoningRestoresAsExplicitNull) {
    const std::string fingerprint(64, 'e');
    nlohmann::json cache{
        {"version", 1}, {"models", {"model"}}, {"probed_at_ms", 123}};
    auto write_cache = [&] {
        write_raw(path_, nlohmann::json{
            {"model_probe_cache", {{fingerprint, cache}}}}.dump());
    };
    write_cache();
    auto restored = acecode::read_model_probe_cache(fingerprint);
    ASSERT_TRUE(restored.has_value());
    EXPECT_FALSE(restored->reasoning.at("model").has_value());

    cache["model_reasoning"] = {{"model", {
        {"supported", true}, {"mandatory", false}, {"default_enabled", true},
        {"supports_max_tokens", false}, {"supported_efforts", {"unknown"}},
    }}};
    write_cache();
    restored = acecode::read_model_probe_cache(fingerprint);
    ASSERT_TRUE(restored.has_value());
    EXPECT_FALSE(restored->reasoning.at("model").has_value());
    EXPECT_EQ(restored->probed_at_ms, 123);
}
