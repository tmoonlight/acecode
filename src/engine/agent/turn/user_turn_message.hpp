#pragma once

#include "llm/llm_provider.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace acecode::agent::detail {

bool has_meaningful_user_input(const UserInput& input);

} // namespace acecode::agent::detail
