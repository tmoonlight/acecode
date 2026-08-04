#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace acecode::rc {

// A complete, secret-free target description. Numbered selections retain this
// value instead of resolving an id again against a freshly sorted catalog.
struct RcSessionTarget {
    std::string session_id;
    std::string workspace_hash;
    std::string cwd;
    std::string title;
    std::string summary;
    std::string workspace_label;
    std::string updated_at;
    bool no_workspace = false;
    bool active = false;
    int content_match_score = 0;
};

enum class RcSessionCommandKind {
    NotCommand,
    Recent,
    All,
    Search,
    Select,
    UsageError,
};

struct RcSessionCommand {
    RcSessionCommandKind kind = RcSessionCommandKind::NotCommand;
    std::string query;
    std::size_t selection = 0;  // one-based; valid only for Select
    std::string error;
};

// RC-only parser. TUI /resume keeps its existing, unrelated semantics.
RcSessionCommand parse_rc_session_command(const std::string& text);

// Sorts deterministicly: content score (search only), newest update, then
// stable identity fields. The input may contain both disk and active entries.
void sort_rc_session_targets(std::vector<RcSessionTarget>& targets,
                             bool prefer_content_matches = false);

std::vector<RcSessionTarget> filter_rc_session_targets(
    const std::vector<RcSessionTarget>& targets,
    const std::string& query,
    std::size_t limit);

std::string format_rc_session_listing(const std::vector<RcSessionTarget>& targets,
                                      const std::string& heading);

std::vector<std::string> chunk_rc_session_output(const std::string& text,
                                                  std::size_t max_bytes = 3000);

std::optional<RcSessionTarget> select_rc_session_snapshot(
    const std::vector<RcSessionTarget>& snapshot,
    std::size_t one_based_index);

constexpr std::size_t kRcSessionRecentLimit = 10;
constexpr std::size_t kRcSessionSearchLimit = 5;

} // namespace acecode::rc
