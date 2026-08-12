#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace acecode::rc {

enum class RcSessionCommandKind {
    NotCommand,
    ListRecent,
    ListAll,
    Search,
    Select,
    Invalid,
};

struct RcSessionCommand {
    RcSessionCommandKind kind = RcSessionCommandKind::NotCommand;
    std::string query;
    std::size_t number = 0;
    std::string error;

    bool recognized() const { return kind != RcSessionCommandKind::NotCommand; }
};

RcSessionCommand parse_rc_session_command(const std::string& text);
std::string rc_session_command_usage();

struct RcSessionCatalogEntry {
    std::string id;
    std::string title;
    std::string summary;
    std::string cwd;
    std::string workspace_hash;
    std::string workspace_name;
    std::string updated_at;
    std::string storage_dir;
    bool no_workspace = false;
    bool active = false;
};

std::string rc_session_entry_key(const RcSessionCatalogEntry& entry);
std::vector<RcSessionCatalogEntry> sort_rc_sessions_newest_first(
    std::vector<RcSessionCatalogEntry> entries);

// content_scores is keyed by rc_session_entry_key(). A positive score makes a
// content-only match eligible; metadata and content scores are then combined.
std::vector<RcSessionCatalogEntry> search_rc_sessions(
    const std::vector<RcSessionCatalogEntry>& entries,
    const std::string& query,
    const std::unordered_map<std::string, int>& content_scores,
    std::size_t limit = 5);

// Formats numbered rows into bounded UTF-8 chunks. Every chunk repeats the
// heading (continuations append " (continued)"), while numbering remains
// one-based and continuous across chunks.
std::vector<std::string> format_rc_session_list(
    const std::vector<RcSessionCatalogEntry>& entries,
    const std::string& heading,
    std::size_t max_chunk_bytes = 1800);

} // namespace acecode::rc
