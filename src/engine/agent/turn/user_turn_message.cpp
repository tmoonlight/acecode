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

ChatMessage build_side_question_message(const std::string& question) {
    ChatMessage message;
    message.role = "user";
    message.content =
        "[SYSTEM NOTE] Answer the side question below using the conversation "
        "context above. This is a separate, read-only, one-turn question. "
        "Do not call tools, do not continue the main task, and do not claim "
        "that you changed files or session state. Answer directly and "
        "concisely.\n\nSide question:\n" + question;
    return message;
}

} // namespace acecode::agent::detail
