#include "session_title_generator.hpp"

#include "session/session_title_text.hpp"
#include "platform/locale.hpp"
#include "utils/encoding.hpp"

#include <algorithm>

namespace acecode {

std::optional<std::string> generate_session_title(
    LlmProvider& provider,
    const std::string& first_user_text,
    int max_input_bytes,
    const std::string& locale) {
    const int bounded_input = std::max(1, max_input_bytes);
    const std::string input = truncate_utf8_prefix(
        first_user_text,
        static_cast<std::size_t>(bounded_input),
        "");
    if (!has_session_title_input(input)) return std::nullopt;

    ChatMessage system;
    system.role = "system";
    system.content =
        "Generate a concise title for this coding-agent session. "
        "Return only the title text, without JSON, Markdown, code fences, "
        "quotes, prefixes, or explanation. "
        "Do not include punctuation unless needed for a file or symbol name.";
    system.content += locale == desktop::kLocaleEnUs
        ? " Write the title in English (en-US), using at most 8 words."
        : " Write the title in Simplified Chinese (zh-CN), using at most 24 Chinese characters.";
    system.content +=
        " Follow this selected language even when the user's message is in another language. "
        "Keep file paths, code identifiers, and product names in their original form.";

    ChatMessage user;
    user.role = "user";
    user.content = input;

    ChatResponse response = provider.chat({system, user}, {});
    if (response.finish_reason == "error" ||
        response.has_tool_calls() ||
        is_generated_session_error_title(response.content)) {
        return std::nullopt;
    }
    std::string title = sanitize_generated_session_title(response.content);
    if (title.empty() || is_generated_session_error_title(title)) {
        return std::nullopt;
    }
    return title;
}

} // namespace acecode
