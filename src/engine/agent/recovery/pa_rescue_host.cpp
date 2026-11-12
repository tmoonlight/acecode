#include "pa_rescue_host.hpp"
#include "llm/token_estimate.hpp"
#include "agent/callbacks_slot.hpp"
#include "agent/compaction/compaction_controller.hpp"
#include "agent/progress/retry_progress.hpp"
#include "agent/request/provider_history.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "session/event_dispatcher.hpp"
#include "session/system_notice.hpp"
#include "session/token_tracker.hpp"
#include "utils/abort_signal.hpp"
#include "utils/logger.hpp"
#include <chrono>
#include <utility>

namespace acecode::agent {
int withhold_inline_images_for_retry(ConversationHistory& history,
                                     TranscriptWriter& transcript,
                                     CompactionController& compaction,
                                     SessionManager* session) {
    ThreadRepairOptions options;
    options.trigger = "repair-inline-images";
    options.withhold_inline_images = true;
    const auto repair = history.repair(session, options);
    LOG_WARN("[thread-repair] withhold inline images status=" +
             std::string(to_string(repair.status)) +
             " withheld_images=" + std::to_string(repair.withheld_images) +
             " reason=" + repair.reason);
    if (!repair.repaired()) return 0;
    compaction.mark_history_repaired();
    if (repair.withheld_images <= 0) return 0;
    transcript.emit_transcript_system_message(session,
        "[智能压缩] 服务端拒收了带图片的请求：已把 " +
            std::to_string(repair.withheld_images) +
            " 张图片改为文件引用后重试（图片文件仍保留）。当前模型可能并不支持看图，"
            "请在「设置 > 模型」中检查它的「视觉」标签。",
        make_system_notice_metadata("context_images_withheld",
            {{"images", repair.withheld_images}}));
    return repair.withheld_images;
}

pa::CompactOutcome compact_history_for_overflow(CompactionController& compaction,
                                                const CompactionInputs* inputs,
                                                AbortSignal& abort,
                                                bool known_oversized) {
    if (!inputs) return pa::CompactOutcome::Failed;
    const int generation = compaction.generation();
    CompactionInputs overflow_inputs = *inputs;
    overflow_inputs.known_oversized = known_oversized;
    const bool compacted = compaction.run_auto(overflow_inputs);
    if (abort.raw()) return pa::CompactOutcome::Cancelled;
    // PostCompact 钩子要求停止时 run_auto 返回 false,但历史已经换成了压缩结果:
    // 按「压过了」重发,不能拿压缩前的规模去收缩已经变了的历史。
    if (compacted || compaction.generation() != generation) {
        return pa::CompactOutcome::Compacted;
    }
    return pa::CompactOutcome::Failed;
}

pa::CompactOutcome PaRescueAdapter::compact() {
    // 兜底走到这一步时主请求已经原样重发两次都被拒,规模确实过不去。
    return compact_history_for_overflow(compaction_, compaction_inputs_, abort_, true);
}

int PaRescueAdapter::history_tokens() const {
    return estimate_message_tokens(detail::recovered_provider_messages(history_.view(), "pa-rescue-estimate"));
}
int PaRescueAdapter::withhold_inline_images() {
    return withhold_inline_images_for_retry(history_, transcript_, compaction_, session_);
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
    const auto callbacks = callbacks_.snapshot();
    if (callbacks.on_stream_retry_reset) callbacks.on_stream_retry_reset();
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
