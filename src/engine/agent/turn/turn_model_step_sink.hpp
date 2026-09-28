#pragma once
#include "agent/model_step/model_step_sink.hpp"

namespace acecode { class SessionManager; }
namespace acecode::agent {
struct TurnContext;
class TurnUsageAccountant;
class ModelStepRecorder;

// Fixed constructor dependencies for the duration of one joined provider call.
class TurnModelStepSink final : public ModelStepSink {
public:
    TurnModelStepSink(TurnContext& turn, TurnUsageAccountant& usage,
        ModelStepRecorder& recorder, SessionManager* session)
        : turn_(turn), usage_(usage), recorder_(recorder), session_(session) {}
    void first_output(int step, int attempt, const std::string& channel) override;
    void accept_usage(const TokenUsage& usage) override;
private:
    TurnContext& turn_;
    TurnUsageAccountant& usage_;
    ModelStepRecorder& recorder_;
    SessionManager* session_; // Nullable borrowed constructor dependency.
};
} // namespace acecode::agent
