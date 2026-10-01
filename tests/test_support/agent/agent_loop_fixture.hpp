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
    static acecode::AgentLoopServices dependencies(
        acecode::AgentProviderAccessor provider, acecode::ToolExecutor& tools,
        acecode::AgentCallbacks callbacks, acecode::PermissionManager& permissions,
        acecode::SessionManager* session = nullptr, acecode::HookManager* hooks = nullptr,
        std::shared_ptr<acecode::MemoryService> memory = nullptr,
        std::shared_ptr<const acecode::SkillRegistry> skills = {}) {
        acecode::AgentLoopServices result{tools, permissions};
        result.provider = std::move(provider);
        result.callbacks = std::move(callbacks);
        result.session = session;
        result.hooks = hooks;
        result.memory = std::move(memory);
        result.skills = std::move(skills);
        return result;
    }
    static acecode::AgentLoopOptions configuration(std::string cwd) {
        acecode::AgentLoopOptions result;
        result.cwd = std::move(cwd);
        return result;
    }
    acecode::AgentLoopServices services;
    acecode::AgentLoopOptions options;
};
} // namespace acecode_test
