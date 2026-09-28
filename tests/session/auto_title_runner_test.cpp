#include <gtest/gtest.h>
#include "session_host/auto_title_runner.hpp"
#include "test_support/agent_loop/characterization_fixture.hpp"
#include "utils/scope_exit.hpp"

namespace {
acecode::AppConfig title_config() {
    acecode::AppConfig config;
    config.session_title.enabled = true;
    config.session_title.model_name = "title-profile";
    acecode::ModelProfile profile;
    profile.name = "title-profile";
    profile.provider = "openai";
    profile.model = "stub-title-model";
    config.saved_models.push_back(profile);
    return config;
}
struct BlockedGeneration {
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool released = false;
    void release() {
        { std::lock_guard<std::mutex> lock(mutex); released = true; }
        changed.notify_all();
    }
};
}
// 中文回归说明：stop 会等待已接受的标题任务结束，显示文本在提交前按原规则选取。
TEST(AutoTitleRunner, AcceptedGenerationFinishesBeforeStopReturns) {
    acecode_test::characterization::Isolation isolation;
    acecode_test::characterization::Harness harness(isolation);
    auto config = title_config();
    struct Result { std::string input, session, title; };
    auto result = std::make_shared<Result>();
    acecode::AutoTitleRunner runner(config, *harness.session, *harness.loop,
        [result](const std::string& session, const std::string& title) {
            result->session = session; result->title = title;
        },
        [result](acecode::ModelProfile profile, const std::string& text, const acecode::AppConfig&)
            -> std::optional<std::string> {
            EXPECT_EQ(profile.name, "title-profile");
            result->input = text;
            return "Improve retry handling";
        });
    acecode::UserInput input;
    input.text = "raw";
    input.display_text = "visible intent";
    runner.maybe_start(input);
    runner.stop();
    EXPECT_EQ(result->input, "visible intent");
    EXPECT_EQ(result->session, harness.session->current_session_id());
    EXPECT_EQ(result->title, "Improve retry handling");
    EXPECT_EQ(harness.session->current_title(), "Improve retry handling");
}

// 中文竞态说明：异步生成期间切换会话，旧任务不得重命名新会话或触发标题显示回调。
TEST(AutoTitleRunner, LateResultCannotRenameReplacementSession) {
    acecode_test::characterization::Isolation isolation;
    acecode_test::characterization::Harness harness(isolation);
    auto config = title_config();
    auto gate = std::make_shared<BlockedGeneration>();
    auto applied = std::make_shared<std::atomic<int>>(0);
    acecode::AutoTitleRunner runner(config, *harness.session, *harness.loop,
        [applied](const std::string&, const std::string&) { ++*applied; },
        [gate](acecode::ModelProfile, const std::string&, const acecode::AppConfig&)
            -> std::optional<std::string> {
            std::unique_lock<std::mutex> lock(gate->mutex);
            gate->started = true;
            gate->changed.notify_all();
            if (!gate->changed.wait_for(lock, std::chrono::seconds(3), [gate] { return gate->released; }))
                return std::nullopt;
            return "Old session title";
        });
    acecode::ScopeExit release([gate] { gate->release(); });
    acecode::UserInput input; input.text = "old session intent";
    runner.maybe_start(input);
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        ASSERT_TRUE(gate->changed.wait_for(lock, std::chrono::seconds(2), [gate] { return gate->started; }));
    }
    harness.session->start_session(acecode::path_to_utf8(harness.cwd), "stub", "stub-1", "replacement");
    gate->release();
    runner.stop();
    EXPECT_EQ(harness.session->current_session_id(), "replacement");
    EXPECT_TRUE(harness.session->current_title().empty());
    EXPECT_EQ(applied->load(), 0);
}

// 中文回归说明：失败与 turn 完成顺序可交错，但最多触发一次基于原输入的重试。
TEST(AutoTitleRunner, CompletedTurnRetriesOneFailedGeneration) {
    acecode_test::characterization::Isolation isolation;
    acecode_test::characterization::Harness harness(isolation);
    auto config = title_config();
    struct Probe {
        std::atomic<int> attempts{0};
        std::promise<void> first_returning, applied;
    };
    auto probe = std::make_shared<Probe>();
    auto first = probe->first_returning.get_future();
    auto applied = probe->applied.get_future();
    acecode::AutoTitleRunner runner(config, *harness.session, *harness.loop,
        [probe](const std::string&, const std::string&) { probe->applied.set_value(); },
        [probe](acecode::ModelProfile, const std::string& text, const acecode::AppConfig&)
            -> std::optional<std::string> {
            EXPECT_EQ(text, "original intent");
            if (++probe->attempts == 1) {
                probe->first_returning.set_value();
                return std::nullopt;
            }
            return "Improve request retries";
        });
    acecode::UserInput input; input.text = "original intent";
    runner.maybe_start(input);
    ASSERT_EQ(first.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    runner.turn_finished("completed");
    EXPECT_EQ(applied.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    runner.stop();
    EXPECT_EQ(probe->attempts.load(), 2);
    EXPECT_EQ(harness.session->current_title(), "Improve request retries");
}
