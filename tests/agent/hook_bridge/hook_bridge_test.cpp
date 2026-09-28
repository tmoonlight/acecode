#include <gtest/gtest.h>

#include "agent/agent_callbacks.hpp"
#include "agent/boundary/workspace_boundary.hpp"
#include "agent/hook_bridge/agent_hook_bridge.hpp"
#include "agent/hook_bridge/tool_hook_bridge.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/turn/turn_outcome.hpp"
#include "hooks/hook_manager.hpp"
#include "permissions/permissions.hpp"
#include "session/event_dispatcher.hpp"
#include "utils/joining_thread.hpp"
#include "utils/lifetime_token.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
struct HookBridgeHarness {
    std::atomic<bool> busy{false};
    acecode::AgentCallbacks callbacks;
    acecode::PermissionManager permissions;
    acecode::EventDispatcher events;
    acecode::agent::ConversationHistory history{busy};
    acecode::agent::TurnOutcomeRecord outcome;
    acecode::agent::TranscriptWriter transcript{history, events, callbacks, outcome};
    acecode::agent::WorkspaceBoundary boundary{".", permissions};
    acecode::agent::AgentHookBridge hooks{boundary, permissions, {}, transcript, history};
    acecode::agent::ToolHookBridge tools{hooks};
};

acecode::HookRegistrySnapshot permission_registry() {
    acecode::HookRegistrySnapshot snapshot;
    snapshot.feature_enabled = true;
    for (const auto* event : {acecode::kCodexHookEventPermissionRequest,
                              acecode::kCodexHookEventPermissionResolved}) {
        acecode::NormalizedHook hook;
        hook.id = event;
        hook.source_id = "test-source";
        hook.event_name = event;
        hook.matcher = "*";
        hook.kind = acecode::HookHandlerKind::Command;
        hook.command.command = "fake-hook";
        hook.command.timeout_seconds = 1;
        hook.trust_status = acecode::HookTrustStatus::Trusted;
        snapshot.hooks.push_back(std::move(hook));
    }
    return snapshot;
}
} // namespace

TEST(AgentHookBridge, ParallelContextAppendAndDrainDoNotLoseOrDuplicateItems) {
    HookBridgeHarness harness;
    acecode::LifetimeToken lifetime;
    const auto owner = lifetime.ref(harness.hooks);
    auto done = std::make_shared<std::atomic<int>>(0);
    auto output = std::make_shared<std::string>();
    acecode::JoiningThread drain([owner, done, output] {
        while (done->load() != 4) {
            owner.with([&output](auto& hooks) { *output += hooks.drain_context(); });
            std::this_thread::yield();
        }
    });
    std::vector<acecode::JoiningThread> producers;
    for (int i = 0; i < 4; ++i) {
        producers.emplace_back([owner, done, i] {
            for (int n = 0; n < 32; ++n) {
                acecode::HookAggregateOutcome outcome;
                outcome.additional_context.push_back("context-" + std::to_string(i) + "-" + std::to_string(n));
                owner.with([&outcome](auto& hooks) { hooks.apply(outcome); });
            }
            done->fetch_add(1);
        });
    }
    for (auto& producer : producers) producer.join();
    drain.join();
    *output += harness.hooks.drain_context();
    for (int i = 0; i < 4; ++i) {
        for (int n = 0; n < 32; ++n) {
            const auto needle = "\ncontext-" + std::to_string(i) + "-" + std::to_string(n) + "\n";
            const auto first = output->find(needle);
            ASSERT_NE(first, std::string::npos);
            EXPECT_EQ(output->find(needle, first + 1), std::string::npos);
        }
    }
    EXPECT_TRUE(harness.hooks.drain_context().empty());
}

TEST(PermissionHookSession, ResolvesOnceAndPreservesExceptionWithoutResolution) {
    HookBridgeHarness harness;
    struct Trace { std::mutex mu; std::vector<nlohmann::json> events; };
    auto trace = std::make_shared<Trace>();
    acecode::HookManager manager(permission_registry(), acecode::HookProcessRunner{},
        [trace](const std::string&, const std::string& payload, int, const std::string&) {
            {
                std::lock_guard<std::mutex> lock(trace->mu);
                trace->events.push_back(nlohmann::json::parse(payload));
            }
            acecode::HookProcessResult result;
            result.started = true;
            result.exit_code = 0;
            result.stdout_text = "{}";
            result.output = "{}";
            return result;
        });
    {
        acecode::agent::PermissionHookSession permission(harness.tools, &manager, nullptr, "probe");
        permission.request({{"value", "request snapshot"}});
        permission.resolve("deny", "interactive");
        permission.resolve("allow", "implicit");
    }
    {
        acecode::agent::PermissionHookSession permission(harness.tools, &manager, nullptr, "probe");
        permission.request({{"value", "implicit snapshot"}});
    }
    EXPECT_THROW({
        acecode::agent::PermissionHookSession permission(harness.tools, &manager, nullptr, "probe");
        permission.request({{"value", "exception snapshot"}});
        throw std::runtime_error("confirmation failed");
    }, std::runtime_error);
    std::lock_guard<std::mutex> lock(trace->mu);
    ASSERT_EQ(trace->events.size(), 5u);
    EXPECT_EQ(trace->events[1]["permission_decision"], "deny");
    EXPECT_EQ(trace->events[1]["tool_input"]["value"], "request snapshot");
    EXPECT_EQ(trace->events[3]["permission_source"], "implicit");
    EXPECT_EQ(trace->events[4]["hook_event_name"], acecode::kCodexHookEventPermissionRequest);
}
