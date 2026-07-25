#include "config/config_mutation.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

class ConfigMutationTempDir {
public:
    ConfigMutationTempDir() {
        path_ = std::filesystem::temp_directory_path() /
            ("acecode-config-mutation-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }

    ~ConfigMutationTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    std::string config_path() const {
        return (path_ / "config.json").string();
    }

private:
    std::filesystem::path path_;
};

class ScopedEnvironmentValue {
public:
    ScopedEnvironmentValue(const char* name, const char* value)
        : name_(name) {
        if (const char* previous = std::getenv(name)) {
            previous_ = previous;
            had_previous_ = true;
        }
#ifdef _WIN32
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }

    ~ScopedEnvironmentValue() {
#ifdef _WIN32
        _putenv_s(name_.c_str(), had_previous_ ? previous_.c_str() : "");
#else
        if (had_previous_) {
            setenv(name_.c_str(), previous_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

private:
    std::string name_;
    std::string previous_;
    bool had_previous_ = false;
};

} // namespace

TEST(ConfigMutation, ConcurrentFocusedUpdatesDoNotLoseWrites) {
    ConfigMutationTempDir temp;
    acecode::AppConfig initial;
    initial.max_sessions = 50;
    initial.context_window = 128000;
    acecode::save_config(initial, temp.config_path());

    constexpr int kThreadCount = 8;
    constexpr int kUpdatesPerThread = 12;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreadCount);

    for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
        workers.emplace_back([&, thread_index]() {
            for (int update = 0; update < kUpdatesPerThread; ++update) {
                auto result = acecode::mutate_config(
                    [thread_index](acecode::AppConfig& cfg, std::string&) {
                        if ((thread_index % 2) == 0) {
                            ++cfg.max_sessions;
                        } else {
                            ++cfg.context_window;
                        }
                        return true;
                    },
                    temp.config_path());
                if (!result.ok || !result.changed) ++failures;
            }
        });
    }
    for (auto& worker : workers) worker.join();

    ASSERT_EQ(failures.load(), 0);
    const acecode::AppConfig saved =
        acecode::load_config_from_path(temp.config_path());
    EXPECT_EQ(
        saved.max_sessions,
        50 + (kThreadCount / 2) * kUpdatesPerThread);
    EXPECT_EQ(
        saved.context_window,
        128000 + (kThreadCount / 2) * kUpdatesPerThread);
}

TEST(ConfigMutation, ExplicitLoadDoesNotPersistEnvironmentSecrets) {
    ConfigMutationTempDir temp;
    acecode::AppConfig initial;
    initial.openai.api_key = "disk-secret";
    acecode::save_config(initial, temp.config_path());

    ScopedEnvironmentValue override("ACECODE_OPENAI_API_KEY", "env-secret");
    EXPECT_EQ(
        acecode::load_config_from_path(temp.config_path()).openai.api_key,
        "disk-secret");
    EXPECT_EQ(
        acecode::load_config_from_path(temp.config_path(), true).openai.api_key,
        "env-secret");

    const auto result = acecode::mutate_config(
        [](acecode::AppConfig& cfg, std::string&) {
            cfg.max_sessions = 77;
            return true;
        },
        temp.config_path());
    ASSERT_TRUE(result.ok) << result.error;

    const auto saved = acecode::load_config_from_path(temp.config_path());
    EXPECT_EQ(saved.openai.api_key, "disk-secret");
    EXPECT_EQ(saved.max_sessions, 77);
}

TEST(ConfigMutation, ValidationFailureLeavesFileUnchanged) {
    ConfigMutationTempDir temp;
    acecode::AppConfig initial;
    initial.default_permission_mode = "default";
    acecode::save_config(initial, temp.config_path());

    const auto result = acecode::mutate_config(
        [](acecode::AppConfig& cfg, std::string& error) {
            cfg.default_permission_mode = "invalid";
            error = "unsupported permission mode";
            return false;
        },
        temp.config_path());

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(result.error, "unsupported permission mode");
    EXPECT_EQ(
        acecode::load_config_from_path(temp.config_path())
            .default_permission_mode,
        "default");
}
