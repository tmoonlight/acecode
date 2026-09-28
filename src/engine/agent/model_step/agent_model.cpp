#include "agent/agent_loop.hpp"
#include "active_model_view.hpp"

namespace acecode {
int AgentLoop::compaction_context_window() const {
    return agent::ActiveModelView(provider_accessor_ ? provider_accessor_() : nullptr,
                                  context_window_.load(std::memory_order_relaxed)).effective_window();
}
void AgentLoop::note_pa_context_rejection(int request_tokens) {
    const agent::ActiveModelView model(provider_accessor_ ? provider_accessor_() : nullptr,
                                       context_window_.load(std::memory_order_relaxed));
    const auto notice = model.note_rejected(request_tokens);
    if (notice) emit_transcript_system_message(notice->text, notice->metadata);
}
void AgentLoop::note_pa_context_accepted(const std::vector<ChatMessage>& messages) {
    const agent::ActiveModelView model(provider_accessor_ ? provider_accessor_() : nullptr,
                                       context_window_.load(std::memory_order_relaxed));
    model.note_accepted(messages);
}
} // namespace acecode
