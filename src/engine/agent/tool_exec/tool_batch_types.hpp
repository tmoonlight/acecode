#pragma once

#include "agent/turn/turn_types.hpp"
#include "llm/tool_result.hpp"
#include "session/tool_result_storage.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
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

// Borrowed only by joined tool calls in one execute_tool_calls invocation.
// The worker owns every referenced object until all futures have been consumed.
struct ToolBatchState {
    SynchronizedDoomGuard& doom_guard;
    const ProgressEmitter& emit_progress;
    const ToolPreambleTitle& step_preamble;
    std::vector<ToolResultReplacementRecord>& delivery_replacements;
    std::vector<DeferredTaskCompleteEnd>& deferred_task_complete_ends;
};

} // namespace acecode::agent
