#pragma once

#include "jsonl_scanner.hpp"
#include "session_storage.hpp"
#include "compact_checkpoint.hpp"

namespace acecode {

constexpr std::size_t kHistoryDefaultLimit = 200;
constexpr std::size_t kHistoryMaxLimit = 1000;

struct HistoryCursor {
    std::uint64_t offset = 0;
    std::string fingerprint;
    std::string file_identity;
    bool empty = false;
};

std::optional<HistoryCursor> decode_history_cursor(const std::string& encoded);
std::string encode_history_cursor(const SessionFileReader& reader,
                                  const std::optional<JsonlRecord>& record);
JsonlRecord validate_history_cursor(const SessionFileReader& reader, const HistoryCursor& cursor);
bool is_visible_paged_history_message(const ChatMessage& message);

struct PositionedHistoryMessage {
    ChatMessage message;
    std::uint64_t offset = 0;
    std::string cursor;
};

struct SessionHistoryRequest {
    std::size_t limit = kHistoryDefaultLimit;
    std::optional<HistoryCursor> before;
    std::optional<HistoryCursor> after;
    // Explicit navigation may load a continuous interval through the current
    // first page. Legacy ordinals are resolved only for such explicit requests.
    std::optional<std::uint64_t> from_position;
    std::optional<std::uint64_t> from_ordinal;
};

struct SessionHistoryPage {
    std::vector<PositionedHistoryMessage> messages;
    bool has_more = false;
    bool turn_truncated = false;
    std::string before;
    std::string after;
};

SessionHistoryPage load_session_history_page(const std::string& path,
                                             const SessionHistoryRequest& request);

struct CompactCheckpointLocation {
    JsonlRecord record;
    ChatMessage message;
    CompactCheckpoint checkpoint;
};
std::optional<CompactCheckpointLocation> latest_compact_checkpoint(const SessionFileReader& reader);
SessionLoadResult load_session_resume_suffix(const std::string& path);
std::vector<ChatMessage> load_session_file_checkpoints(const std::string& path);

} // namespace acecode
