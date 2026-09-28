#pragma once
#include "session_host/session_registry.hpp"
#include "session_host/tools/spawn_subagent_tool.hpp"
#include "session_host/tools/thread_tools.hpp"
#include "utils/scope_exit.hpp"

namespace acecode::headless {
// Declare immediately after the client. The cleanup runs before client and
// registry destruction, so tools can still use the client while loops stop.
// Tool closures may outlive this scope; remove their borrowed backfill only
// after every session worker has joined.
inline auto make_session_cleanup(SessionRegistry& registry,
    std::shared_ptr<SubagentToolDeps> subagents, std::shared_ptr<ThreadToolDeps> threads) {
    return ScopeExit([&registry, subagents = std::move(subagents), threads = std::move(threads)] {
        registry.shutdown_all();
        subagents->registry = nullptr;
        subagents->client = nullptr;
        subagents->config = nullptr;
        threads->service.reset();
    });
}
} // namespace acecode::headless
