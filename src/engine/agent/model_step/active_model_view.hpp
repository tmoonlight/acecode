#pragma once

#include "llm/llm_provider.hpp"
#include "prompt/system_prompt.hpp"
#include <memory>
#include <optional>
#include <string>

namespace acecode::agent {
struct ContextRejectionNotice {
    std::string text;
    nlohmann::json metadata;
};

// Uses the caller's provider lease. It never re-queries a provider accessor,
// and never snapshots ToolExecutor's process-wide tool name mapping.
class ActiveModelView {
public:
    ActiveModelView(std::shared_ptr<LlmProvider> provider, int declared_window);
    int effective_window() const;
    bool can_read_images() const;
    SystemPromptModelState prompt_state() const;
    std::optional<ContextRejectionNotice> note_rejected(int request_tokens) const;
    void note_accepted(const std::vector<ChatMessage>& messages) const;
private:
    // Shared with the request/active-provider scope for exactly this model step.
    std::shared_ptr<LlmProvider> provider_;
    int declared_window_;
    std::string provider_name_;
    std::string model_name_;
};
} // namespace acecode::agent
