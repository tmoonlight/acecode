#include "session/session_load_metrics.hpp"
#include "session/session_manager.hpp"
#include "session_host/session_registry.hpp"
#include "tool/tool_executor.hpp"
#include "utils/uuid.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <future>

using namespace std::chrono_literals;

TEST(SessionHistoryConcurrency, HistoryReadDoesNotBlockListsOtherSessionsOrAppend) {
    const auto cwd = std::filesystem::temp_directory_path() / ("ace-history-" + acecode::generate_uuid());
    std::filesystem::create_directories(cwd);
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::SessionRegistryDeps deps;
    deps.tools = &tools;
    deps.template_permissions = &permissions;
    deps.cwd = cwd.string();
    deps.provider_accessor = [] { return std::shared_ptr<acecode::LlmProvider>{}; };
    // The test and its bounded asynchronous operations share the registry.
    auto registry = std::make_shared<acecode::SessionRegistry>(std::move(deps));
    const auto a = registry->acquire(registry->create({}));
    const auto b = registry->acquire(registry->create({}));
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    acecode::ChatMessage first;
    first.role = "user";
    first.content = "first";
    a->sm->on_message(first);

    struct Gate {
        std::promise<void> entered;
        std::promise<void> release;
        std::shared_future<void> released = release.get_future().share();
    };
    // Reader and test share the gate until every future has joined.
    auto gate = std::make_shared<Gate>();
    auto entered = gate->entered.get_future();
    auto reader = std::async(std::launch::async, [a, gate] {
        acecode::ScopedSessionReadObserver observer([gate] {
            gate->entered.set_value();
            gate->released.wait();
        });
        return a->sm->load_active_messages();
    });
    const auto ready = entered.wait_for(5s);
    EXPECT_EQ(ready, std::future_status::ready);
    auto listing = std::async(std::launch::async, [registry] { return registry->list_active(); });
    EXPECT_EQ(listing.wait_for(1s), std::future_status::ready);
    auto draft = std::async(std::launch::async, [registry, id = b->id] {
        auto entry = registry->acquire(id);
        entry->sm->set_input_draft("independent draft");
        entry->sm->set_permission_mode("plan");
        return entry->sm->current_input_draft();
    });
    EXPECT_EQ(draft.wait_for(1s), std::future_status::ready);
    auto append = std::async(std::launch::async, [a] {
        acecode::ChatMessage message;
        message.role = "assistant";
        message.content = "newly appended";
        a->sm->on_message(message);
    });
    EXPECT_EQ(append.wait_for(1s), std::future_status::ready);
    gate->release.set_value();
    EXPECT_EQ(draft.get(), "independent draft");
    EXPECT_EQ(listing.get().size(), 2u);
    append.get();
    EXPECT_EQ(reader.get().size(), 1u); // The snapshot excludes the concurrent append.
    EXPECT_EQ(a->sm->load_active_messages().size(), 2u);
    registry->shutdown_all();
    std::error_code ec;
    std::filesystem::remove_all(cwd, ec);
}
