#pragma once

#include "llm/llm_provider.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace acecode::agent::detail {

std::vector<ChatMessage> recovered_provider_messages(
    const std::vector<ChatMessage>& messages,
    const char* boundary);

std::vector<ChatMessage> model_facing_provider_messages(
    const std::vector<ChatMessage>& messages,
    const char* boundary);

} // namespace acecode::agent::detail
