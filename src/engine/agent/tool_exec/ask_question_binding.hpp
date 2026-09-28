#pragma once
#include "tool/tool_executor.hpp"
#include "agent/turn/turn_types.hpp"
#include "utils/lifetime_token.hpp"

namespace acecode { class AskUserQuestionPrompter; class AbortSignal; struct AgentLoopConfig; }
namespace acecode::agent {
class GoalRuntime;
using AskQuestionChannel = std::function<nlohmann::json(
    const nlohmann::json&, std::atomic<bool>*, int, const std::string&)>;

class AskQuestionBinding {
public:
    AskQuestionBinding(GoalRuntime& goal, AbortSignal& abort, const AgentLoopConfig& config,
        SessionManager* session, AskUserQuestionPrompter* prompter, AskQuestionChannel channel)
        : goal_(goal), abort_(abort), config_(config), session_manager_(session),
          prompter_(prompter), channel_(std::move(channel)) {}
    void bind(ToolContext& context, const ToolCall& call, int index,
        const ProgressEmitter& progress);
private:
    nlohmann::json ask_daemon(const nlohmann::json& payload, const std::string& tool,
        const std::string& id, int index, const ProgressEmitter& progress);
    GoalRuntime& goal_;
    AbortSignal& abort_;
    const AgentLoopConfig& config_;
    SessionManager* session_manager_; // Nullable borrowed constructor dependency.
    AskUserQuestionPrompter* prompter_; // Nullable borrowed; owner outlives this binding.
    AskQuestionChannel channel_;
    LifetimeToken lifetime_;
};
} // namespace acecode::agent
