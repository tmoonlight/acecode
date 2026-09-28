#include "turn_model_step_sink.hpp"
#include "turn_context.hpp"
#include "agent/model_step/model_step_recorder.hpp"
#include "agent/model_step/turn_usage_accountant.hpp"

namespace acecode::agent {
void TurnModelStepSink::first_output(int step, int attempt, const std::string& channel) {
    recorder_.first_output(session_, step, attempt, channel);
}
void TurnModelStepSink::accept_usage(const TokenUsage& usage) {
    usage_.accept(turn_.usage, usage, session_);
}
} // namespace acecode::agent
