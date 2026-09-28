#pragma once
#include "agent/agent_loop.hpp"
#include <memory>
#include <utility>

namespace acecode_test {
// Test assembly is explicit and follows production: fixed services, value
// options, prompters/subscriptions, then start(). No mutable service setters.
struct AgentLoopFixture {
    AgentLoopFixture(acecode::AgentProviderAccessor provider, acecode::ToolExecutor& tools,
        acecode::AgentCallbacks callbacks, std::string cwd, acecode::PermissionManager& permissions)
        : services{tools, permissions} {
        services.provider = std::move(provider);
        services.callbacks = std::move(callbacks);
        options.cwd = std::move(cwd);
    }
    std::unique_ptr<acecode::AgentLoop> make() const {
        return std::make_unique<acecode::AgentLoop>(services, options);
    }
    acecode::AgentLoopServices services;
    acecode::AgentLoopOptions options;
};
} // namespace acecode_test
