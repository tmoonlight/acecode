#include "rc_session_navigation.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <tuple>

namespace acecode::rc {
namespace {

std::string trim_copy(const std::string& value) {
    std::size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }
    return value.substr(first, last - first);
}

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool is_alias(const std::string& value) {
    const std::string lower = ascii_lower(value);
    return lower == "/session" || lower == "/sessions" || lower == "/resume";
}

bool all_ascii_digits(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
}

std::size_t utf8_prefix_bytes(const std::string& value, std::size_t max_bytes) {
    if (value.size() <= max_bytes) return value.size();
    std::size_t end = max_bytes;
    while (end > 0 &&
           (static_cast<unsigned char>(value[end]) & 0xC0u) == 0x80u) {
        --end;
    }
    return end;
}

std::string bounded_text(const std::string& value, std::size_t max_bytes) {
    if (value.size() <= max_bytes) return value;
    const std::size_t suffix_bytes = 3; // UTF-8 ellipsis
    if (max_bytes <= suffix_bytes) return value.substr(0, utf8_prefix_bytes(value, max_bytes));
    return value.substr(0, utf8_prefix_bytes(value, max_bytes - suffix_bytes)) + "…";
}

std::string display_title(const RcSessionCatalogEntry& entry) {
    const std::string title = trim_copy(entry.title);
    return title.empty() ? entry.id : title;
}

std::string display_workspace(const RcSessionCatalogEntry& entry) {
    if (entry.no_workspace) return "无工作区";
    if (!trim_copy(entry.workspace_name).empty()) return trim_copy(entry.workspace_name);
    if (!trim_copy(entry.cwd).empty()) return trim_copy(entry.cwd);
    return entry.workspace_hash.empty() ? std::string("workspace") : entry.workspace_hash;
}

std::string display_time(std::string value) {
    value = trim_copy(value);
    if (value.size() >= 16) {
        value.resize(16);
        if (value.size() > 10 && value[10] == 'T') value[10] = ' ';
    }
    return value.empty() ? std::string("unknown time") : value;
}

int metadata_score(const RcSessionCatalogEntry& entry, const std::string& query) {
    const std::string q = ascii_lower(trim_copy(query));
    if (q.empty()) return 0;
    const auto score_field = [&](const std::string& raw, int exact, int prefix, int contains) {
        const std::string value = ascii_lower(raw);
        if (value.empty()) return 0;
        if (value == q) return exact;
        if (value.rfind(q, 0) == 0) return prefix;
        return value.find(q) != std::string::npos ? contains : 0;
    };

    int score = 0;
    score = (std::max)(score, score_field(entry.id, 1500, 1400, 1300));
    score = (std::max)(score, score_field(display_title(entry), 1300, 1200, 1000));
    score = (std::max)(score, score_field(entry.summary, 800, 750, 700));
    score = (std::max)(score, score_field(entry.workspace_name, 650, 600, 550));
    score = (std::max)(score, score_field(entry.cwd, 500, 450, 400));
    score = (std::max)(score, score_field(entry.workspace_hash, 350, 325, 300));
    return score;
}

} // namespace

std::string rc_session_command_usage() {
    return "用法：/sessions [more|all|search <关键词>|<编号>]（/session 与 /resume 等价）";
}

RcSessionCommand parse_rc_session_command(const std::string& text) {
    const std::string trimmed = trim_copy(text);
    const std::size_t split = trimmed.find_first_of(" \t\r\n");
    const std::string name = split == std::string::npos
                                 ? trimmed
                                 : trimmed.substr(0, split);
    if (!is_alias(name)) return {};

    RcSessionCommand command;
    const std::string args = split == std::string::npos
                                 ? std::string{}
                                 : trim_copy(trimmed.substr(split + 1));
    if (args.empty()) {
        command.kind = RcSessionCommandKind::ListRecent;
        return command;
    }

    const std::string lower = ascii_lower(args);
    if (lower == "more" || lower == "all") {
        command.kind = RcSessionCommandKind::ListAll;
        return command;
    }

    const std::size_t arg_split = args.find_first_of(" \t\r\n");
    const std::string first = arg_split == std::string::npos
                                  ? args
                                  : args.substr(0, arg_split);
    if (ascii_lower(first) == "search") {
        command.query = arg_split == std::string::npos
                            ? std::string{}
                            : trim_copy(args.substr(arg_split + 1));
        if (command.query.empty()) {
            command.kind = RcSessionCommandKind::Invalid;
            command.error = rc_session_command_usage();
        } else {
            command.kind = RcSessionCommandKind::Search;
        }
        return command;
    }

    if (all_ascii_digits(args)) {
        try {
            const unsigned long long parsed = std::stoull(args);
            if (parsed == 0 || parsed > (std::numeric_limits<std::size_t>::max)()) {
                command.kind = RcSessionCommandKind::Invalid;
                command.error = "会话编号必须是正整数。";
            } else {
                command.kind = RcSessionCommandKind::Select;
                command.number = static_cast<std::size_t>(parsed);
            }
        } catch (...) {
            command.kind = RcSessionCommandKind::Invalid;
            command.error = "会话编号无效。";
        }
        return command;
    }

    command.kind = RcSessionCommandKind::Invalid;
    command.error = rc_session_command_usage();
    return command;
}

std::string rc_session_entry_key(const RcSessionCatalogEntry& entry) {
    return (entry.no_workspace ? std::string("no-workspace") : entry.workspace_hash) +
           "\n" + entry.id;
}

std::vector<RcSessionCatalogEntry> sort_rc_sessions_newest_first(
    std::vector<RcSessionCatalogEntry> entries) {
    std::stable_sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.updated_at != rhs.updated_at) return lhs.updated_at > rhs.updated_at;
        if (lhs.id != rhs.id) return lhs.id < rhs.id;
        return rc_session_entry_key(lhs) < rc_session_entry_key(rhs);
    });
    return entries;
}

std::vector<RcSessionCatalogEntry> search_rc_sessions(
    const std::vector<RcSessionCatalogEntry>& entries,
    const std::string& query,
    const std::unordered_map<std::string, int>& content_scores,
    std::size_t limit) {
    struct Ranked {
        RcSessionCatalogEntry entry;
        int score = 0;
    };
    std::vector<Ranked> ranked;
    for (const auto& entry : entries) {
        const int meta = metadata_score(entry, query);
        const auto it = content_scores.find(rc_session_entry_key(entry));
        const int content = it == content_scores.end() ? 0 : it->second;
        if (meta <= 0 && content <= 0) continue;
        ranked.push_back({entry, meta + content});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& lhs, const Ranked& rhs) {
        if (lhs.score != rhs.score) return lhs.score > rhs.score;
        if (lhs.entry.updated_at != rhs.entry.updated_at) {
            return lhs.entry.updated_at > rhs.entry.updated_at;
        }
        return rc_session_entry_key(lhs.entry) < rc_session_entry_key(rhs.entry);
    });
    if (ranked.size() > limit) ranked.resize(limit);
    std::vector<RcSessionCatalogEntry> out;
    out.reserve(ranked.size());
    for (auto& item : ranked) out.push_back(std::move(item.entry));
    return out;
}

std::vector<std::string> format_rc_session_list(
    const std::vector<RcSessionCatalogEntry>& entries,
    const std::string& heading,
    std::size_t max_chunk_bytes) {
    const std::size_t safe_limit = (std::max)(max_chunk_bytes, std::size_t{256});
    if (entries.empty()) return {heading + "\n没有找到可恢复的会话。"};

    std::vector<std::string> chunks;
    std::string current = heading;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        std::ostringstream row;
        row << (i + 1) << ". " << bounded_text(display_title(entry), 180)
            << " | " << bounded_text(display_workspace(entry), 120)
            << " | " << display_time(entry.updated_at);
        const std::string line = row.str();
        if (current.size() + 1 + line.size() > safe_limit && current != heading) {
            chunks.push_back(std::move(current));
            current = heading + " (continued)";
        }
        current += "\n" + line;
    }
    chunks.push_back(std::move(current));
    return chunks;
}

} // namespace acecode::rc
