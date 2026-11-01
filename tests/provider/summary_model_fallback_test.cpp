#include "provider/summary_model_fallback.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace acecode;

class SummaryModelFallbackTest : public testing::Test {
protected:
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("summary-fallback-" + generate_uuid());
    std::string path = path_to_utf8(root / "state.sqlite3");
    AppConfig config;
    void SetUp() override {
        for (const auto* name : {"summary", "chat", "other"}) {
            ModelProfile profile;
            profile.name = name;
            profile.provider = "openai";
            profile.model = name;
            profile.base_url = "http://127.0.0.1:12345/v1";
            profile.api_key = "private-test-credential";
            config.saved_models.push_back(profile);
        }
        config.default_model_name = "other";
    }
    void TearDown() override { std::error_code ec; std::filesystem::remove_all(root, ec); }
    SummaryModelFallback chain(std::string preferred = "summary") {
        return {config, preferred, config.saved_models.back(), path};
    }
};

TEST_F(SummaryModelFallbackTest, FailedPreferredUsesLastSuccessAndPersistsReplacement) {
    remember_successful_chat_model(summary_model_identity(config.saved_models[1]), path);
    std::vector<std::string> calls;
    const auto recovered = chain().run([&](const ModelProfile& profile) {
        calls.push_back(profile.name);
        return profile.name == "chat";
    });
    ASSERT_TRUE(recovered);
    EXPECT_EQ(calls, (std::vector<std::string>{"summary", "chat"}));
    // A new executor reads durable state, with no process cache to rely on.
    EXPECT_EQ(chain().candidates().front().name, "chat");
    std::ifstream file(root / "state.sqlite3", std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(file)), {});
    EXPECT_EQ(bytes.find("private-test-credential"), std::string::npos);
    EXPECT_EQ(bytes.find("127.0.0.1"), std::string::npos);
}

TEST_F(SummaryModelFallbackTest, NoSelectionOrMissingSelectionUsesMostRecentSuccess) {
    remember_successful_chat_model(summary_model_identity(config.saved_models[2]), path);
    remember_successful_chat_model(summary_model_identity(config.saved_models[1]), path);
    EXPECT_EQ(chain("").candidates().front().name, "chat");
    EXPECT_EQ(chain("removed").candidates().front().name, "chat");
}

TEST_F(SummaryModelFallbackTest, ChangedPreferredConfigurationResetsRecovery) {
    ASSERT_TRUE(chain().run([](const ModelProfile& profile) { return profile.name == "chat"; }));
    config.saved_models[0].api_key = "corrected-key";
    EXPECT_EQ(chain().candidates().front().name, "summary");
}

TEST_F(SummaryModelFallbackTest, BackupRetainsNamedProfileAndInheritedHeaderIdentity) {
    auto alias = config.saved_models[1];
    alias.name = "chat-alias";
    config.saved_models.push_back(alias);
    config.openai.request_headers["X-Connection"] = "working";
    remember_successful_chat_model(summary_model_identity(alias, &config), path);
    EXPECT_EQ(chain("").candidates().front().name, "chat-alias");
    config.openai.request_headers["X-Connection"] = "changed";
    const SummaryModelFallback reset(config, "", config.saved_models[2], path);
    EXPECT_EQ(reset.candidates().front().name, "other");
}

TEST_F(SummaryModelFallbackTest, DifferentRequestSettingsRemainSeparateCandidates) {
    auto alias = config.saved_models[0];
    alias.name = "corrected-summary";
    alias.max_output_tokens = 100;
    config.saved_models.push_back(alias);
    std::vector<std::string> calls;
    EXPECT_TRUE(chain().run([&](const ModelProfile& profile) {
        calls.push_back(profile.name);
        return profile.name == "corrected-summary";
    }));
    EXPECT_EQ(calls.front(), "summary");
    EXPECT_EQ(calls.back(), "corrected-summary");
}

TEST_F(SummaryModelFallbackTest, ChangedOrRemovedBackupIsNotTreatedAsSuccessful) {
    remember_successful_chat_model(summary_model_identity(config.saved_models[1]), path);
    config.saved_models[1].base_url = "http://different.invalid";
    EXPECT_EQ(chain("").candidates().front().name, "other");
    config.saved_models.erase(config.saved_models.begin() + 1);
    EXPECT_EQ(chain("").candidates().front().name, "other");
}

TEST_F(SummaryModelFallbackTest, ExceptionsAndExhaustionTryEachConnectionOnce) {
    auto duplicate = config.saved_models[0];
    duplicate.name = "alias";
    config.saved_models.push_back(duplicate);
    int calls = 0;
    EXPECT_FALSE(chain().run([&](const ModelProfile&) -> bool {
        ++calls;
        throw std::runtime_error("failed");
    }));
    EXPECT_EQ(calls, 3);
}

TEST_F(SummaryModelFallbackTest, FailedReplacementContinuesToAnotherCandidate) {
    ASSERT_TRUE(chain().run([](const ModelProfile& profile) { return profile.name == "chat"; }));
    ASSERT_TRUE(chain().run([](const ModelProfile& profile) { return profile.name == "other"; }));
    EXPECT_EQ(chain().candidates().front().name, "other");
}

TEST_F(SummaryModelFallbackTest, CancellationNeverPromotesCandidateOrContinues) {
    std::atomic<bool> cancelled{false};
    int calls = 0;
    EXPECT_FALSE(chain().run([&](const ModelProfile&) {
        ++calls;
        cancelled = true;
        return true;
    }, &cancelled));
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(chain().run([&](const ModelProfile&) { ++calls; return true; }, &cancelled));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(chain().candidates().front().name, "summary");
}

TEST_F(SummaryModelFallbackTest, ConcurrentSuccessWritesRemainReadable) {
    std::vector<std::thread> writers;
    for (const auto& profile : config.saved_models) {
        writers.emplace_back([identity = summary_model_identity(profile), path = path] {
            remember_successful_chat_model(identity, path);
        });
    }
    for (auto& writer : writers) writer.join();
    remember_successful_chat_model(summary_model_identity(config.saved_models[1]), path);
    EXPECT_EQ(chain("").candidates().front().name, "chat");
}
