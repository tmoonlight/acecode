#pragma once

#include "config/config.hpp"
#include "llm/llm_provider.hpp"

#include <optional>

namespace acecode {

struct FollowUpContext {
    std::string completion_key;
    nlohmann::json messages = nlohmann::json::array();
};

// Reads at most 2 MiB of the persisted tail. No reasoning, tool arguments or
// system instructions enter the bounded model context.
std::optional<FollowUpContext> read_follow_up_context(const std::string& path);
std::vector<ChatMessage> follow_up_messages(const FollowUpContext& context,
                                          const std::string& locale);
std::optional<std::string> parse_follow_up_prompt(const std::string& content);
std::optional<std::string> generate_follow_up_prompt(
    LlmProvider& provider, const FollowUpContext& context, const std::string& locale,
    const std::atomic<bool>* cancelled = nullptr);
std::optional<std::string> generate_session_follow_up_prompt(
    const AppConfig& config, const std::string& session_model_name,
    const FollowUpContext& context);

} // namespace acecode
