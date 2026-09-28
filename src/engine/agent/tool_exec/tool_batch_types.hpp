#pragma once

#include "agent/turn/turn_types.hpp"
#include "llm/tool_result.hpp"
#include "session/tool_result_storage.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace acecode {
class AgentLoopDoomGuard;
struct ToolContext;
}

namespace acecode::agent {

class SynchronizedDoomGuard;

struct DeferredTaskCompleteEnd {
    std::int64_t started_at_ms = 0;
    std::int64_t completed_at_ms = 0;
    std::int64_t duration_ms = 0;
    double elapsed_seconds = 0.0;
};

using ToolRunner = std::function<ToolResult(const ToolCall&,
                                             const ToolContext&,
                                             const std::string&,
                                             const std::string&)>;

// A call returns all of its data; only the worker publishes it into a slot.
struct ToolCallOutcome {
    ToolResult result;
    ToolResultReplacementRecord delivery_replacement;
    DeferredTaskCompleteEnd deferred_end;
};

struct ToolCallSlot {
    std::size_t original_index;
    ToolCall call;
    std::optional<ToolCallOutcome> outcome;
};

struct ToolBatchOutcome {
    bool terminator_fired = false;
    bool terminate_session_after_turn = false;
    std::vector<std::function<void()>> post_turn_actions;
};

// Dependencies remain valid until every joined call has returned. Tool workers
// read the batch inputs but never write slots; the calling worker owns slots.
struct ToolBatchState {
    SynchronizedDoomGuard& doom_guard;
    const ProgressEmitter& emit_progress;
    const ToolPreambleTitle& step_preamble;
    std::vector<ToolCallSlot> slots;
};

} // namespace acecode::agent
