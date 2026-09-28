#pragma once

#include "llm/llm_provider.hpp"

#include <optional>
#include <string>

namespace acecode {

std::optional<std::string> generate_session_title(
    LlmProvider& provider,
    const std::string& first_user_text,
    int max_input_bytes,
    const std::string& locale);

} // namespace acecode
