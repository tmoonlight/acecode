#pragma once

#include "llm/llm_provider.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace acecode::agent::detail {

bool has_meaningful_user_input(const UserInput& input);

ChatMessage build_side_question_message(const std::string& question);

} // namespace acecode::agent::detail
