#include "pa_rescue_host.hpp"
#include "agent/agent_callbacks.hpp"
#include "agent/compaction/compaction_controller.hpp"
#include "agent/progress/retry_progress.hpp"
#include "agent/request/provider_history.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "session/event_dispatcher.hpp"
#include "session/token_tracker.hpp"
#include "utils/abort_signal.hpp"
#include <chrono>
#include <utility>

namespace acecode::agent {
int PaRescueAdapter::history_tokens() const {
    return estimate_message_tokens(detail::recovered_provider_messages(history_.view(), "pa-rescue-estimate"));
}
void PaRescueAdapter::note_rejection(int tokens) {
    const auto notice = model_.note_rejected(tokens);
    if (notice) transcript_.emit_transcript_system_message(session_, notice->text, notice->metadata);
}
ThreadRepairResult PaRescueAdapter::repair(const ThreadRepairOptions& options) {
    return history_.repair(session_, options);
}
bool PaRescueAdapter::wait(int milliseconds) {
    return !abort_.wait_for(std::chrono::milliseconds(milliseconds));
}
void PaRescueAdapter::reset_stream() {
    if (callbacks_.on_stream_retry_reset) callbacks_.on_stream_retry_reset();
}
void PaRescueAdapter::history_repaired() { compaction_.mark_history_repaired(); }
void PaRescueAdapter::notice(const std::string& text, nlohmann::json metadata) {
    transcript_.emit_transcript_system_message(session_, text, std::move(metadata));
}
void PaRescueAdapter::progress(nlohmann::json payload) {
    events_.emit(SessionEventKind::AgentProgress, std::move(payload));
}
void PaRescueAdapter::retry(const ProviderErrorInfo& info, bool waiting,
                             std::string label, std::string detail) {
    retry_.emit(info, waiting, {waiting ? "model_retry" : "model_waiting",
                               std::move(label), std::move(detail)});
}
} // namespace acecode::agent
