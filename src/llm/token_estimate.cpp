#include "token_estimate.hpp"

#include <algorithm>
#include <limits>

namespace acecode {

namespace {

bool is_utf8_continuation(unsigned char value) {
    return (value & 0xC0u) == 0x80u;
}

std::string truncation_marker(std::size_t removed_tokens) {
    const char* ellipsis = "\xE2\x80\xA6";
    return std::string(ellipsis) + std::to_string(removed_tokens) +
           " tokens truncated" + ellipsis;
}

} // namespace

std::size_t estimate_message_payload_bytes(const ChatMessage& msg) {
    std::size_t bytes = msg.content.size() + msg.reasoning_content.size();
    if (msg.content_parts.is_array() && !msg.content_parts.empty()) {
        bytes += msg.content_parts.dump().size();
    }
    if (!msg.tool_calls.is_null() && !msg.tool_calls.empty()) {
        bytes += msg.tool_calls.dump().size();
    }
    bytes += msg.tool_call_id.size();
    // Account for the role and request envelope without pretending to have a
    // provider-specific tokenizer.
    bytes += msg.role.size() + 16;
    return bytes;
}

std::size_t approx_token_count(const std::string& text) {
    constexpr std::size_t kApproxBytesPerToken = 4;
    const std::size_t padded =
        text.size() > std::numeric_limits<std::size_t>::max() -
                          (kApproxBytesPerToken - 1)
        ? std::numeric_limits<std::size_t>::max()
        : text.size() + kApproxBytesPerToken - 1;
    return padded / kApproxBytesPerToken;
}

std::string truncate_text_to_token_budget(const std::string& text,
                                          std::size_t max_tokens) {
    if (text.empty()) return {};

    constexpr std::size_t kApproxBytesPerToken = 4;
    const std::size_t max_bytes = max_tokens >
            std::numeric_limits<std::size_t>::max() / kApproxBytesPerToken
        ? std::numeric_limits<std::size_t>::max()
        : max_tokens * kApproxBytesPerToken;

    if (max_tokens > 0 && text.size() <= max_bytes) return text;
    if (max_bytes == 0) return truncation_marker(approx_token_count(text));

    const std::size_t left_budget = max_bytes / 2;
    const std::size_t right_budget = max_bytes - left_budget;

    std::size_t prefix_end = std::min(left_budget, text.size());
    while (prefix_end > 0 && prefix_end < text.size() &&
           is_utf8_continuation(static_cast<unsigned char>(text[prefix_end]))) {
        --prefix_end;
    }

    std::size_t suffix_start = text.size() > right_budget
        ? text.size() - right_budget
        : 0;
    while (suffix_start < text.size() &&
           is_utf8_continuation(static_cast<unsigned char>(text[suffix_start]))) {
        ++suffix_start;
    }
    if (suffix_start < prefix_end) suffix_start = prefix_end;

    const std::size_t removed_bytes = text.size() > max_bytes
        ? text.size() - max_bytes
        : 0;
    const std::size_t removed_tokens =
        (removed_bytes + kApproxBytesPerToken - 1) / kApproxBytesPerToken;
    return text.substr(0, prefix_end) + truncation_marker(removed_tokens) +
           text.substr(suffix_start);
}

int estimate_message_tokens(const std::vector<ChatMessage>& messages) {
    std::size_t total_bytes = 0;
    for (const auto& msg : messages) {
        const std::size_t bytes = estimate_message_payload_bytes(msg);
        if (total_bytes > std::numeric_limits<std::size_t>::max() - bytes) {
            return std::numeric_limits<int>::max();
        }
        total_bytes += bytes;
    }
    const std::size_t tokens = (total_bytes + 3) / 4;
    return tokens > static_cast<std::size_t>(std::numeric_limits<int>::max())
        ? std::numeric_limits<int>::max()
        : static_cast<int>(tokens);
}

} // namespace acecode
