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
        SessionManager* session, AskUserQuestionPrompter* prompter, AskQuestionChannel channel);
    ~AskQuestionBinding();
    AskQuestionBinding(const AskQuestionBinding&) = delete;
    AskQuestionBinding& operator=(const AskQuestionBinding&) = delete;
    void bind(ToolContext& context, const ToolCall& call, int index,
        const ProgressEmitter& progress);
private:
    struct State;
    // Shared only with callbacks already admitted; retained callbacks are weak.
    std::shared_ptr<State> state_;
};
} // namespace acecode::agent
