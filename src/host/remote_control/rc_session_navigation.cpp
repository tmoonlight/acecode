#include "rc_session_navigation.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace acecode::rc {
namespace {

std::string trim_copy(const std::string& value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return value.substr(first, last - first);
}

std::string ascii_lower_copy(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

bool contains_folded(const std::string& value, const std::string& folded_query) {
    return ascii_lower_copy(value).find(folded_query) != std::string::npos;
}

bool is_ascii_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\f' || c == '\v';
}

bool same_session_identity(const RcSessionTarget& left,
                           const RcSessionTarget& right) {
    if (left.session_id != right.session_id ||
        left.no_workspace != right.no_workspace) {
        return false;
    }
    if (left.no_workspace) return left.cwd == right.cwd;
    if (!left.workspace_hash.empty() && !right.workspace_hash.empty()) {
        return left.workspace_hash == right.workspace_hash;
    }
    return left.cwd == right.cwd;
}

bool is_utf8_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

std::size_t utf8_codepoint_end(const std::string& text, std::size_t pos) {
    if (pos >= text.size()) return text.size();
    const unsigned char lead = static_cast<unsigned char>(text[pos]);
    std::size_t width = 1;
    if ((lead & 0xE0) == 0xC0) width = 2;
    else if ((lead & 0xF0) == 0xE0) width = 3;
    else if ((lead & 0xF8) == 0xF0) width = 4;
    return (std::min)(text.size(), pos + width);
}

bool is_positive_decimal(const std::string& text, std::size_t* value) {
    if (text.empty()) return false;
    std::size_t out = 0;
    for (unsigned char c : text) {
        if (!std::isdigit(c)) return false;
        const std::size_t digit = static_cast<std::size_t>(c - '0');
        if (out > (static_cast<std::size_t>(-1) - digit) / 10) return false;
        out = out * 10 + digit;
    }
    if (out == 0) return false;
    *value = out;
    return true;
}

std::string display_title(const RcSessionTarget& target) {
    return target.title.empty() ? target.session_id : target.title;
}

} // namespace

RcSessionCommand parse_rc_session_command(const std::string& text) {
    const std::string trimmed = trim_copy(text);
    if (trimmed.empty() || trimmed.front() != '/') return {};
    const std::size_t split = trimmed.find_first_of(" \t\r\n");
    const std::string name = ascii_lower_copy(trimmed.substr(1, split - 1));
    if (name != "session" && name != "sessions" && name != "resume") return {};

    const std::string args = split == std::string::npos ? std::string{} :
        trim_copy(trimmed.substr(split + 1));
    if (args.empty()) return {RcSessionCommandKind::Recent};
    const std::string lowered = ascii_lower_copy(args);
    if (lowered == "all" || lowered == "more") return {RcSessionCommandKind::All};
    if (lowered == "search") {
        return {RcSessionCommandKind::UsageError, {}, 0,
                "Usage: /sessions [more|all|search <query>|<number>]"};
    }
    constexpr std::size_t kSearchLength = 6;
    if (lowered.size() > kSearchLength &&
        lowered.compare(0, kSearchLength, "search") == 0 &&
        is_ascii_space(static_cast<unsigned char>(lowered[kSearchLength]))) {
        const std::string query = trim_copy(args.substr(kSearchLength));
        if (query.empty()) {
            return {RcSessionCommandKind::UsageError, {}, 0,
                    "Usage: /sessions search <query>"};
        }
        return {RcSessionCommandKind::Search, query};
    }
    std::size_t selection = 0;
    if (is_positive_decimal(args, &selection)) {
        return {RcSessionCommandKind::Select, {}, selection};
    }
    return {RcSessionCommandKind::UsageError, {}, 0,
            "Usage: /sessions [more|all|search <query>|<number>]"};
}

void merge_active_rc_session_targets(
    std::vector<RcSessionTarget>& persisted,
    const std::vector<RcSessionTarget>& active,
    const std::vector<RcSessionTarget>& archived) {
    for (const auto& live : active) {
        const bool archived_in_same_scope = std::any_of(
            archived.begin(), archived.end(),
            [&](const RcSessionTarget& candidate) {
                return same_session_identity(candidate, live);
            });
        if (live.session_id.empty() || archived_in_same_scope) {
            continue;
        }
        auto found = std::find_if(
            persisted.begin(), persisted.end(),
            [&](const RcSessionTarget& candidate) {
                return same_session_identity(candidate, live);
            });
        if (found == persisted.end()) {
            persisted.push_back(live);
            persisted.back().active = true;
            continue;
        }
        found->active = true;
        if (!live.title.empty()) found->title = live.title;
        if (!live.summary.empty()) found->summary = live.summary;
        if (!live.cwd.empty()) found->cwd = live.cwd;
        if (!live.workspace_hash.empty() && !live.no_workspace) {
            found->workspace_hash = live.workspace_hash;
        }
        if (!live.workspace_label.empty()) {
            found->workspace_label = live.workspace_label;
        }
        if (!live.updated_at.empty()) found->updated_at = live.updated_at;
    }
}

void sort_rc_session_targets(std::vector<RcSessionTarget>& targets,
                             bool prefer_content_matches) {
    std::sort(targets.begin(), targets.end(), [prefer_content_matches](
        const RcSessionTarget& left, const RcSessionTarget& right) {
        if (prefer_content_matches &&
            left.content_match_score != right.content_match_score) {
            return left.content_match_score > right.content_match_score;
        }
        if (left.updated_at != right.updated_at) return left.updated_at > right.updated_at;
        if (left.session_id != right.session_id) return left.session_id < right.session_id;
        if (left.workspace_hash != right.workspace_hash) {
            return left.workspace_hash < right.workspace_hash;
        }
        return left.cwd < right.cwd;
    });
}

std::vector<RcSessionTarget> filter_rc_session_targets(
    const std::vector<RcSessionTarget>& targets,
    const std::string& query,
    std::size_t limit) {
    const std::string folded = ascii_lower_copy(trim_copy(query));
    std::vector<RcSessionTarget> out;
    if (folded.empty()) return out;
    for (const auto& target : targets) {
        if (target.content_match_score > 0 || contains_folded(target.session_id, folded) ||
            contains_folded(target.title, folded) || contains_folded(target.summary, folded) ||
            contains_folded(target.workspace_label, folded) || contains_folded(target.cwd, folded) ||
            contains_folded(target.workspace_hash, folded)) {
            out.push_back(target);
        }
    }
    sort_rc_session_targets(out, true);
    if (out.size() > limit) out.resize(limit);
    return out;
}

std::string format_rc_session_listing(const std::vector<RcSessionTarget>& targets,
                                      const std::string& heading) {
    std::ostringstream out;
    out << heading;
    if (targets.empty()) {
        out << "\n(no resumable sessions found)";
        return out.str();
    }
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const auto& target = targets[i];
        out << "\n" << (i + 1) << ". " << display_title(target)
            << " | "
            << (target.no_workspace ? "无工作区" :
                (target.workspace_label.empty() ? target.workspace_hash : target.workspace_label))
            << " | " << target.updated_at;
    }
    out << "\nUse /sessions <number> to switch.";
    return out.str();
}

std::vector<std::string> chunk_rc_session_output(const std::string& text,
                                                  std::size_t max_bytes) {
    if (text.empty() || max_bytes == 0 || text.size() <= max_bytes) return {text};
    std::vector<std::string> chunks;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = (std::min)(text.size(), pos + max_bytes);
        if (end < text.size()) {
            const std::size_t newline = text.rfind('\n', end);
            if (newline != std::string::npos && newline > pos) {
                end = newline;
            } else {
                while (end > pos &&
                       is_utf8_continuation(static_cast<unsigned char>(text[end]))) {
                    --end;
                }
            }
        }
        if (end == pos) end = utf8_codepoint_end(text, pos);
        chunks.push_back(text.substr(pos, end - pos));
        pos = end;
        if (pos < text.size() && text[pos] == '\n') ++pos;
    }
    return chunks;
}

std::optional<RcSessionTarget> select_rc_session_snapshot(
    const std::vector<RcSessionTarget>& snapshot,
    std::size_t one_based_index) {
    if (one_based_index == 0 || one_based_index > snapshot.size()) return std::nullopt;
    return snapshot[one_based_index - 1];
}

} // namespace acecode::rc
