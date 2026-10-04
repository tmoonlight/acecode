#pragma once

#include "agent/hook_bridge/hook_context_provider.hpp"
#include "hooks/hook_runtime.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace acecode { class HookManager; class AbortSignal; struct ChatMessage; }

namespace acecode::agent {
class TranscriptWriter;
class ConversationHistory;

class AgentHookBridge {
public:
    AgentHookBridge(WorkspaceBoundary& boundary, PermissionManager& permissions,
                    HookContextProvider::ProviderAccessor provider,
                    TranscriptWriter& transcript, ConversationHistory& history, AbortSignal& abort)
        : context_(boundary, permissions, std::move(provider)), transcript_(transcript), history_(history), abort_(abort) {}
    HookCommonPayloadFields common_fields(const std::string& event, SessionManager* session) const;
    HookAggregateOutcome dispatch(HookManager* manager, const std::string& event,
                                  const std::string& matcher, const nlohmann::json& payload);
    void apply(const HookAggregateOutcome& outcome, bool include_context = true);
    std::string drain_context();
    void clear_context();
    void assistant_completed(HookManager* manager, SessionManager* session,
                             const ChatMessage& message, const std::shared_ptr<LlmProvider>& provider);
    void session_start(HookManager* manager, SessionManager* session, const std::string& source);
    void session_title_changed(HookManager* manager, SessionManager* session,
                               const std::string& title, const std::string& source,
                               const std::string& title_source);
    bool continue_from_stop(HookManager* manager, SessionManager* session,
                            const std::string& last_assistant_message);
private:
    HookContextProvider context_;
    TranscriptWriter& transcript_;
    ConversationHistory& history_;
    AbortSignal& abort_;
    std::mutex context_mu_; // Leaf: append or swap only, no observers/store calls.
    std::vector<std::string> request_context_;
    bool stop_active_ = false; // Worker-only, intentionally survives turn boundaries.
};

} // namespace acecode::agent
