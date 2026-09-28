#include "agent/agent_loop.hpp"
#include "activity_narrator.hpp"

namespace acecode {
void AgentLoop::set_tool_preamble_config(const ToolPreambleConfig& config) {
    activity_->set_config(config);
}
ToolPreambleConfig AgentLoop::tool_preamble_config() const {
    return activity_->config();
}
} // namespace acecode
