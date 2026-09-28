#pragma once
#include "agent/agent_callbacks.hpp"
#include "agent/guards/doom_guard.hpp"
#include "agent/model_step/turn_usage_accountant.hpp"
#include "agent/recovery/context_overflow_recovery.hpp"
#include "agent/tool_exec/tool_batch_types.hpp"
#include "computer_use/session_lease.hpp"
#include "response_recovery.hpp"
#include "turn_types.hpp"
#include <memory>
#include <optional>

namespace acecode::agent {
class AgentProgressEmitter;

// The worker creates this before a chat task and resets it after normal or
// recovered completion. Provider accounting survives stack unwinding. Shell,
// compact and control tasks have no chat context; recovery accepts null.
struct TurnContext {
    explicit TurnContext(AgentCallbacks snapshot) : callbacks(std::move(snapshot)) {}
    AgentCallbacks callbacks;
    UserTurnInfo info;
    bool swarm_mode = false;
    int total_iterations = 0;
    int model_step_index = 0;
    bool preturn_compaction_failed = false;
    bool terminator_fired = false;
    int observed_compact_generation = 0;
    RequestRecoveryState recovery;
    SynchronizedDoomGuard doom_guard;
    ResponseRecoveryState response_recovery;
    std::string timing_status = "completed";
    TurnUsageRecord usage;
    ToolBatchOutcome tools;
    ToolPreambleTitle preamble;
    std::vector<std::string> model_tool_names;
    std::function<std::chrono::steady_clock::time_point()> progress_clock;
    // Shared only with tool progress closures admitted by the joined batch.
    std::shared_ptr<AgentProgressEmitter> progress;
    // Reset before recovery reporting on unwind, and before finishing the task
    // on normal return. Normal release-before-terminal + destructor is retained.
    std::optional<computer_use::SessionLease> desktop_lease;
};

} // namespace acecode::agent
