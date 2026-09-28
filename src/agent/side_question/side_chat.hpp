#pragma once

#include "session_client.hpp"

#include <atomic>
#include <memory>
#include <mutex>

namespace acecode {

constexpr std::size_t kMaxSideChatHistoryMessages = 200;
constexpr std::size_t kMaxSideChatHistoryBytes = 256 * 1024;

struct SideChatMessage {
    std::string role;
    std::string content;
};

struct SideChatResult {
    SideQuestionResult response;
    bool cancelled = false;
    std::string code;
};

// This cancellation belongs only to one detached request. Waking the provider
// also interrupts its retry backoff; it never sets the main AgentLoop abort.
class SideChatCancellation {
public:
    void cancel();
    void bind_provider(const std::shared_ptr<LlmProvider>& provider);
    std::atomic<bool> aborted{false};

private:
    std::mutex mutex_;
    std::weak_ptr<LlmProvider> provider_;
};

// reset=true discards provisional output from an upstream retry attempt.
using SideChatStreamCallback = std::function<void(const std::string& delta, bool reset)>;

std::string validate_side_chat_history(const std::vector<SideChatMessage>& history);

SideChatResult run_side_chat(
    std::shared_ptr<LlmProvider> provider,
    std::vector<ChatMessage> context,
    const std::string& question,
    const std::vector<SideChatMessage>& history,
    SideChatCancellation& cancellation,
    const SideChatStreamCallback& callback);

} // namespace acecode
