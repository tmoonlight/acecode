#include "user_turn_message.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <utility>

namespace acecode::agent::detail {

bool has_meaningful_user_input(const UserInput& input) {
    if (input.has_content_parts()) return true;
    return std::any_of(input.text.begin(), input.text.end(), [](unsigned char ch) {
        return std::isspace(ch) == 0;
    });
}

} // namespace acecode::agent::detail
