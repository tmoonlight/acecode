#pragma once

#include "agent/agent_callbacks.hpp"
#include "agent/turn/turn_types.hpp"
#include "config/config.hpp"
#include <mutex>
#include <string>
#include <vector>

namespace acecode::agent {

// The mutex is a leaf: snapshots and state updates finish before any callback
// or progress emission. Callbacks may query or reconfigure this narrator.
class ActivityNarrator {
public:
    explicit ActivityNarrator(AgentCallbacks& callbacks) : callbacks_(callbacks) {}
    void set_config(const ToolPreambleConfig& cfg);
    ToolPreambleConfig config() const;
    bool enabled() const;
    void set_phase(const ToolPreambleTitle& preamble);
    ToolPreambleTitle phase() const;
    void publish_phase(const ToolPreambleTitle& preamble, const ProgressEmitter& emit);
    void note_planned_tool(int index, const std::string& name);
    void reset_step();
    void reset_turn();
    void announce(const std::string& label);
    ToolPreambleTitle for_phase(const std::string& phase) const;
    ToolPreambleTitle resolve_step(const ChatResponse& response);
    static ToolPreambleTitle reasoning_title(const std::string& reasoning);
    static const char* responding_label();

private:
    AgentCallbacks& callbacks_;
    mutable std::mutex tool_preamble_mu_;
    ToolPreambleConfig tool_preamble_cfg_;
    ToolPreambleTitle phase_preamble_;
    std::vector<std::string> step_planned_tools_;
    ToolPreambleTitle current_batch_activity_;
    std::vector<std::string> last_batch_tools_;
    std::string last_announced_activity_;
};

} // namespace acecode::agent
