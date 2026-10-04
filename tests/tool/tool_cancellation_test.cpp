#include "tool/tool_executor.hpp"
#include "tool/vision_subagent_tool.hpp"
#include "tool/web_search/backend_router.hpp"
#include "provider/openai_provider.hpp"
#include "provider/anthropic_provider.hpp"
#include "utils/abandonable_call.hpp"
#include "utils/joining_thread.hpp"
#include "utils/scope_exit.hpp"
#include "test_support/utils/concurrency_gate.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <stdexcept>

namespace acecode {
namespace {
using namespace std::chrono_literals;

struct BlockedObservation {
    test::ConcurrencyGate entered;
    test::ConcurrencyGate release;
    test::ConcurrencyGate returned;
    std::atomic<bool> cancelled{false};
    std::atomic<int> destroyed{0};
};

TEST(CancellableCallTest, LateWorkerOwnsLatchedCancellationAfterCallerFlagIsDestroyed) {
    auto state = std::make_shared<BlockedObservation>();
    ScopeExit unblock([state] { state->release.open(); });
    auto abort = std::make_unique<std::atomic<bool>>(false);
    JoiningThread caller([state, flag = abort.get()] {
        const auto result = run_cancellable<int>([state](const std::atomic<bool>* cancellation) {
            state->entered.open();
            state->release.wait();
            state->cancelled = cancellation && cancellation->load();
            return 42;
        }, flag);
        EXPECT_FALSE(result);
        state->returned.open();
    });
    EXPECT_TRUE(state->entered.wait());
    *abort = true;
    EXPECT_TRUE(state->returned.wait(1s));
    caller.join();
    *abort = false;
    abort.reset();
    state->release.open();
    EXPECT_TRUE(wait_for_abandoned_work(2s));
    EXPECT_TRUE(state->cancelled);
}

TEST(CancellableCallTest, HandlesInlinePreCancelledSuccessAndExceptions) {
    EXPECT_EQ(*run_cancellable<int>([](auto flag) { EXPECT_EQ(flag, nullptr); return 7; }, nullptr), 7);
    std::atomic<bool> abort{true};
    EXPECT_FALSE(run_cancellable<void>([](auto) { ADD_FAILURE() << "cancelled work started"; }, &abort));
    abort = false;
    auto result = run_cancellable<std::unique_ptr<int>>(
        [](auto) { return std::make_unique<int>(8); }, &abort);
    ASSERT_TRUE(result);
    EXPECT_EQ(**result, 8);
    EXPECT_THROW(run_cancellable<int>([](auto) -> int { throw std::runtime_error("failed"); }, &abort), std::runtime_error);
    EXPECT_TRUE(run_cancellable<void>([](auto) {}, &abort));
    EXPECT_TRUE(wait_for_abandoned_work(2s));
}

TEST(ToolCancellationTest, AdmissionStopsAnyToolButRetainsCompletedMutation) {
    ToolExecutor executor;
    std::atomic<bool> abort{true};
    int writes = 0;
    ToolImpl tool;
    tool.definition.name = "custom_mutation";
    tool.execute = [&writes, &abort](const std::string&, const ToolContext&) {
        ++writes;
        abort = true;
        return ToolResult{"written", true};
    };
    ASSERT_TRUE(executor.register_tool(std::move(tool)));
    ToolContext context;
    context.abort_flag = &abort;
    const auto skipped = executor.execute("custom_mutation", "{}", context);
    EXPECT_FALSE(skipped.success);
    EXPECT_TRUE(skipped.metadata.value("cancelled", false));
    EXPECT_EQ(writes, 0);
    abort = false;
    const auto completed = executor.execute("custom_mutation", "{}", context);
    EXPECT_TRUE(completed.success);
    EXPECT_EQ(completed.output, "written");
    EXPECT_EQ(writes, 1);
    EXPECT_FALSE(executor.execute("custom_mutation", "{}", context).success);
    EXPECT_EQ(writes, 1);
}

class BlockingSearch final : public WebSearchBackend {
public:
    BlockingSearch(std::string name, std::shared_ptr<BlockedObservation> state)
        : name_(std::move(name)), state_(std::move(state)) {}
    ~BlockingSearch() override { ++state_->destroyed; }
    std::string name() const override { return name_; }
    bool requires_api_key() const override { return false; }
    std::variant<SearchResponse, SearchError> search(
        std::string_view query, int, const std::atomic<bool>* cancellation) override {
        state_->entered.open();
        state_->release.wait();
        state_->cancelled = cancellation && cancellation->load();
        EXPECT_EQ(query, "owned query");
        return SearchResponse{};
    }
private:
    std::string name_;
    // Shared by test synchronization and the independently owned backend.
    std::shared_ptr<BlockedObservation> state_;
};

void verify_search_cancellation(bool parallel) {
    auto first = std::make_shared<BlockedObservation>();
    auto second = std::make_shared<BlockedObservation>();
    ScopeExit unblock([first, second] { first->release.open(); second->release.open(); });
    std::atomic<bool> abort{false};
    JoiningThread caller([first, second, &abort, parallel] {
        WebSearchConfig cfg;
        cfg.backend = parallel ? "parallel" : "rss";
        {
            web_search::BackendRouter router(cfg);
            router.register_backend(std::make_unique<BlockingSearch>("rss", first));
            router.register_backend(std::make_unique<BlockingSearch>("duckduckgo", second));
            router.resolve_active(web_search::Region::Unknown);
            auto result = router.search_with_fallback("owned query", 3, &abort, {});
            EXPECT_TRUE(std::holds_alternative<SearchError>(result));
        }
        first->returned.open();
    });
    EXPECT_TRUE(first->entered.wait());
    if (parallel) EXPECT_TRUE(second->entered.wait());
    abort = true;
    EXPECT_TRUE(first->returned.wait(1s));
    caller.join();
    EXPECT_EQ(first->destroyed, 0);
    // The router and caller are gone while blocked backend work remains alive.
    abort = false;
    first->release.open();
    second->release.open();
    EXPECT_TRUE(wait_for_abandoned_work(2s));
    EXPECT_TRUE(first->cancelled);
    EXPECT_EQ(first->destroyed, 1);
    EXPECT_EQ(second->destroyed, 1);
    if (parallel) EXPECT_TRUE(second->cancelled);
}

TEST(ToolCancellationTest, SerialSearchCancelsWithoutStartingFallback) { verify_search_cancellation(false); }
TEST(ToolCancellationTest, ParallelSearchCancelsAndRetainsBackendLifetimes) { verify_search_cancellation(true); }

class BlockingVision final : public LlmProvider {
public:
    explicit BlockingVision(std::shared_ptr<BlockedObservation> state) : state_(std::move(state)) {}
    ~BlockingVision() override { ++state_->destroyed; }
    ChatResponse chat(const std::vector<ChatMessage>&, const std::vector<ToolDef>&) override {
        state_->entered.open();
        state_->release.wait();
        ChatResponse response;
        response.content = "late result";
        return response;
    }
    void chat_stream(const std::vector<ChatMessage>&, const std::vector<ToolDef>&,
                     const StreamCallback&, std::atomic<bool>*) override {}
    std::string name() const override { return "blocking"; }
    std::string model() const override { return "vision"; }
    bool is_authenticated() override { return true; }
    void set_model(const std::string&) override {}
private:
    std::shared_ptr<BlockedObservation> state_; // Shared by provider and test synchronization.
};

TEST(ToolCancellationTest, VisionReturnsBeforeLegacyProviderAndDiscardsLateResult) {
    auto state = std::make_shared<BlockedObservation>();
    ScopeExit unblock([state] { state->release.open(); });
    std::atomic<bool> abort{false};
    JoiningThread caller([state, &abort] {
        AppConfig config;
        ModelProfile profile;
        profile.name = "vision";
        profile.provider = "openai";
        profile.capabilities = {"vision"};
        config.saved_models.push_back(profile);
        VisionSubagentToolOptions options;
        options.provider_factory = [state](const ModelProfile&) { return std::make_shared<BlockingVision>(state); };
        auto tool = create_vision_analyze_tool(config, options);
        ToolContext context;
        context.abort_flag = &abort;
        context.active_model_can_read_images = false;
        auto result = tool.execute(R"({"prompt":"inspect","attachment":{"id":"a","session_id":"s","name":"x.png","kind":"image","mime_type":"image/png","path":"x.png","blob_url":"/b","size_bytes":1}})", context);
        EXPECT_FALSE(result.success);
        EXPECT_EQ(result.output, "[Interrupted]");
        state->returned.open();
    });
    EXPECT_TRUE(state->entered.wait());
    abort = true;
    EXPECT_TRUE(state->returned.wait(1s));
    caller.join();
    EXPECT_EQ(state->destroyed, 0);
    state->release.open();
    EXPECT_TRUE(wait_for_abandoned_work(2s));
    EXPECT_EQ(state->destroyed, 1);
}

TEST(ToolCancellationTest, NonStreamingProvidersRejectPreCancelledRequestsWithoutTransport) {
    std::atomic<bool> abort{true};
    OpenAiCompatProvider openai("http://127.0.0.1:1/v1", "test", "model");
    EXPECT_EQ(openai.chat_cancellable({}, {}, &abort).provider_error.kind, ProviderErrorKind::UserCancelled);
    AnthropicProvider anthropic("http://127.0.0.1:1/v1", "test", "model");
    EXPECT_EQ(anthropic.chat_cancellable({}, {}, &abort).provider_error.kind, ProviderErrorKind::UserCancelled);
}
} // namespace
} // namespace acecode
