#pragma once
#include "turn_types.hpp"

namespace acecode { class SessionManager; class HookManager; }
namespace acecode::agent {
class ConversationHistory;
class TranscriptWriter;
class SideQuestionService;
class AgentHookBridge;
class ModelStepRecorder;

class AssistantOutput {
public:
    AssistantOutput(ConversationHistory& history, TranscriptWriter& transcript,
        SideQuestionService& side_questions, AgentHookBridge& hooks, ModelStepRecorder& recorder)
        : history_(history), transcript_(transcript), side_questions_(side_questions),
          hooks_(hooks), model_steps_(recorder) {}
    void interrupted(const ChatResponse& response, SessionManager* session);
    void completed(const ChatResponse& response, const ApiRequestBundle& bundle,
        const std::shared_ptr<LlmProvider>& provider, int step, const TokenUsage& usage,
        SessionManager* session, HookManager* hooks);
    void ignored_text_call(const TextToolCallDiagnostic& diagnostic, SessionManager* session);
private:
    ConversationHistory& history_;
    TranscriptWriter& transcript_;
    SideQuestionService& side_questions_;
    AgentHookBridge& hooks_;
    ModelStepRecorder& model_steps_;
};
} // namespace acecode::agent
