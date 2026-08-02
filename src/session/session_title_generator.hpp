#pragma once

#include "../provider/llm_provider.hpp"

#include <optional>
#include <string>

namespace acecode {

// Provider implementations use a bracketed error marker for non-streaming
// failures. Keep this deliberately narrower than a generic "Error" prefix so
// legitimate titles such as "Error handling cleanup" remain valid.
bool is_generated_session_error_title(const std::string& title);

// Normalize model-generated title output. JSON and a single Markdown-wrapped
// JSON object are accepted for compatibility; explanations, code blocks,
// malformed structured output, and overlong text are rejected.
std::string sanitize_generated_session_title(std::string raw);

std::optional<std::string> generate_session_title(
    LlmProvider& provider,
    const std::string& first_user_text,
    int max_input_bytes);

} // namespace acecode
