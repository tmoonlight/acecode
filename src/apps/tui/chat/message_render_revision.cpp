#include "message_render_revision.hpp"
#include <algorithm>
#include <functional>

namespace acecode::tui {

static std::size_t combine_render_hash(std::size_t seed,
                                       std::size_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) +
                   (seed >> 2));
}

std::size_t message_render_revision(const TuiState::Message& msg,
                                           bool transcript_expanded) {
    std::size_t seed = 0;
    auto add_string = [&seed](const std::string& value) {
        seed = combine_render_hash(seed, std::hash<std::string>{}(value));
    };
    auto add_size = [&seed](std::size_t value) {
        seed = combine_render_hash(seed, value);
    };

    add_string(msg.role);
    add_size(msg.is_tool ? 1u : 0u);
    add_size(msg.ask_result ? 1u : 0u);
    add_string(msg.display_override);
    add_size(msg.expanded ? 1u : 0u);
    add_string(msg.compact_notice_id);
    add_size(msg.compact_notice_complete ? 1u : 0u);
    add_size(transcript_expanded ? 1u : 0u);
    add_size(msg.summary.has_value() ? 1u : 0u);
    if (msg.summary.has_value()) {
        add_string(msg.summary->verb);
        add_string(msg.summary->object);
        add_string(msg.summary->icon);
        add_size(msg.summary->metrics.size());
        for (const auto& metric : msg.summary->metrics) {
            add_string(metric.first);
            add_string(metric.second);
        }
    }
    add_size(msg.hunks.has_value() ? 1u : 0u);
    if (msg.hunks.has_value()) {
        add_size(msg.hunks->size());
        for (const auto& hunk : *msg.hunks) {
            add_size(static_cast<std::size_t>(std::max(0, hunk.old_start)));
            add_size(static_cast<std::size_t>(std::max(0, hunk.old_count)));
            add_size(static_cast<std::size_t>(std::max(0, hunk.new_start)));
            add_size(static_cast<std::size_t>(std::max(0, hunk.new_count)));
            add_size(hunk.lines.size());
            for (const auto& line : hunk.lines) {
                add_size(static_cast<std::size_t>(line.kind));
                add_size(line.text.size());
            }
        }
    }
    return seed;
}


std::size_t message_render_cache_revision(const TuiState::Message& message,
    bool transcript_expanded, const std::string& content) {
    return combine_render_hash(message_render_revision(message, transcript_expanded),
        std::hash<std::string>{}(content));
}

} // namespace acecode::tui
