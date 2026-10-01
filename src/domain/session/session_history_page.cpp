#include "session_history_page.hpp"
#include "session_load_metrics.hpp"
#include "session_serializer.hpp"
#include "utils/sha1.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <sstream>

namespace acecode {
namespace {

std::optional<ChatMessage> parse_record(const JsonlRecord& record) {
    if (record.text.empty()) return std::nullopt;
    ++session_read_metrics().records;
    try {
        const auto value = nlohmann::json::parse(record.text);
        if (!value.is_object() || !value.contains("role") ||
            !value["role"].is_string() || value["role"].get_ref<const std::string&>().empty()) return std::nullopt;
        return deserialize_message_json(value);
    } catch (...) {
        return std::nullopt;
    }
}

std::string file_identity(const SessionFileReader& reader) {
    return sha1_hex(reader.identity());
}

bool hex_string(const std::string& text, std::size_t size) {
    return text.size() == size && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

bool has_visible_before(const SessionFileReader& reader, std::uint64_t offset) {
    JsonlScanner scanner(reader, true, offset);
    while (const auto record = scanner.next()) {
        const auto message = parse_record(*record);
        if (message && is_visible_paged_history_message(*message)) return true;
    }
    return false;
}

std::uint64_t position_for_ordinal(const SessionFileReader& reader, std::uint64_t ordinal) {
    JsonlScanner scanner(reader, false);
    std::uint64_t current = 0;
    while (const auto record = scanner.next()) {
        if (!parse_record(*record)) continue;
        if (current++ == ordinal) return record->offset;
    }
    throw std::invalid_argument("history ordinal is outside the transcript");
}

void append_loaded_record(SessionLoadResult& result, const JsonlRecord& record) {
    if (record.text.empty() || record.text == "\r") return;
    auto message = parse_record(record);
    if (message) {
        result.messages.push_back(std::move(*message));
        if (!record.terminated) result.diagnostics.recovered_unterminated_record = true;
    } else if (record.terminated) {
        ++result.diagnostics.malformed_complete_records;
    } else {
        result.diagnostics.ignored_partial_tail = true;
    }
}

SessionHistoryPage assemble_page(const SessionFileReader& reader, const SessionHistoryRequest& request) {
    SessionHistoryPage page;
    const auto limit = std::clamp(request.limit, std::size_t{1}, kHistoryMaxLimit);
    std::uint64_t end = reader.size();
    if (request.before) end = validate_history_cursor(reader, *request.before).offset;
    std::optional<JsonlRecord> after_record;
    bool forward = request.after.has_value() || request.from_position.has_value() || request.from_ordinal.has_value();
    std::uint64_t position = end;
    if (request.after) {
        const auto anchor = validate_history_cursor(reader, *request.after);
        if (!request.after->empty) after_record = anchor;
        position = request.after->empty ? 0 : anchor.end;
    } else if (request.from_position || request.from_ordinal) {
        position = request.from_position.value_or(0);
        if (request.from_ordinal) position = position_for_ordinal(reader, *request.from_ordinal);
        if (position > end || (position > 0 && reader.read(position - 1, 1) != "\n")) {
            throw HistorySnapshotChanged();
        }
    }
    JsonlScanner scanner(reader, !forward, position);
    bool aligned = false;
    while (auto record = scanner.next()) {
        if (forward && record->offset >= end) break;
        // Anchor after the latest physical row, even when that row is hidden or
        // a damaged tail. Completing that tail changes its fingerprint -> reload.
        if (forward || !after_record) after_record = *record;
        auto message = parse_record(*record);
        if (!message || !is_visible_paged_history_message(*message)) continue;
        const bool user = message->role == "user" && !message->is_meta;
        page.messages.push_back({std::move(*message), record->offset, encode_history_cursor(reader, record)});
        if (!forward && page.messages.size() >= limit) {
            if (user) { aligned = true; break; }
            if (page.messages.size() >= 4 * limit) break;
        }
    }
    if (!forward) {
        if (!aligned && page.messages.size() >= 4 * limit) {
            page.messages.resize(limit);
            page.turn_truncated = true;
        }
        std::reverse(page.messages.begin(), page.messages.end());
    }
    if (!request.after && !page.messages.empty()) {
        page.has_more = page.turn_truncated || has_visible_before(reader, page.messages.front().offset);
        if (page.has_more) page.before = page.messages.front().cursor;
    }
    page.after = encode_history_cursor(reader, after_record);
    return page;
}

} // namespace

std::optional<HistoryCursor> decode_history_cursor(const std::string& encoded) {
    if (encoded.size() > 128) return std::nullopt;
    std::array<std::string, 5> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto dot = encoded.find('.', start);
        if ((i + 1 == parts.size()) != (dot == std::string::npos)) return std::nullopt;
        parts[i] = encoded.substr(start, dot == std::string::npos ? dot : dot - start);
        start = dot == std::string::npos ? encoded.size() : dot + 1;
    }
    if (parts[0] != "h1" || parts[1].empty() || parts[1].size() > 16 ||
        parts[1].find_first_not_of("0123456789abcdef") != std::string::npos ||
        !hex_string(parts[2], 40) || !hex_string(parts[3], 40) ||
        (parts[4] != "r" && parts[4] != "e")) return std::nullopt;
    HistoryCursor cursor;
    const auto parsed = std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), cursor.offset, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != parts[1].data() + parts[1].size()) return std::nullopt;
    cursor.fingerprint = std::move(parts[2]);
    cursor.file_identity = std::move(parts[3]);
    cursor.empty = parts[4] == "e";
    if (cursor.empty && (cursor.offset != 0 || cursor.fingerprint != sha1_hex(""))) return std::nullopt;
    return cursor;
}

std::string encode_history_cursor(const SessionFileReader& reader, const std::optional<JsonlRecord>& record) {
    std::ostringstream result;
    result << "h1." << std::hex << (record ? record->offset : 0) << '.'
           << sha1_hex(record ? record->text : "") << '.' << file_identity(reader)
           << (record ? ".r" : ".e");
    return result.str();
}

JsonlRecord validate_history_cursor(const SessionFileReader& reader, const HistoryCursor& cursor) {
    if (cursor.file_identity != file_identity(reader)) throw HistorySnapshotChanged();
    if (cursor.empty) return {};
    if (cursor.offset >= reader.size() ||
        (cursor.offset > 0 && reader.read(cursor.offset - 1, 1) != "\n")) throw HistorySnapshotChanged();
    JsonlScanner scanner(reader, false, cursor.offset);
    auto record = scanner.next();
    if (!record || sha1_hex(record->text) != cursor.fingerprint) throw HistorySnapshotChanged();
    return *record;
}

bool is_visible_paged_history_message(const ChatMessage& message) {
    if (message.is_meta && (message.subtype == "file_checkpoint" || message.subtype == "compact_checkpoint")) return false;
    const auto hidden = message.metadata.find("hidden_goal_context");
    return !message.metadata.is_object() || hidden == message.metadata.end() ||
           !hidden->is_boolean() || !hidden->get<bool>();
}

SessionHistoryPage load_session_history_page(const std::string& path, const SessionHistoryRequest& request) {
    SessionLoadTimer timer("history_page");
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            SessionFileReader reader(path);
            if (!reader.valid()) {
                if (request.before || request.after) throw HistorySnapshotChanged();
                SessionHistoryPage empty;
                empty.after = encode_history_cursor(reader, {});
                return empty;
            }
            const auto prefix = reader.prefix();
            auto page = assemble_page(reader, request);
            if (reader.unchanged(prefix)) return page;
        } catch (const HistorySnapshotChanged&) {
            if (attempt == 1) throw;
        }
    }
    throw HistorySnapshotChanged();
}

std::optional<CompactCheckpointLocation> latest_compact_checkpoint(const SessionFileReader& reader) {
    JsonlScanner scanner(reader, true);
    while (auto record = scanner.next()) {
        if (record->text.find("compact_checkpoint") == std::string::npos) continue;
        auto message = parse_record(*record);
        if (!message) continue;
        try {
            if (auto checkpoint = decode_compact_checkpoint(*message)) {
                return CompactCheckpointLocation{std::move(*record), std::move(*message), std::move(*checkpoint)};
            }
        } catch (...) {}
    }
    return std::nullopt;
}

SessionLoadResult load_session_resume_suffix(const std::string& path) {
    SessionLoadTimer timer("resume_suffix");
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            SessionFileReader reader(path);
            if (!reader.valid()) return {};
            const auto prefix = reader.prefix();
            SessionLoadResult result;
            auto checkpoint = latest_compact_checkpoint(reader);
            std::uint64_t start = 0;
            if (checkpoint) {
                result.start_offset = checkpoint->record.offset;
                result.messages.push_back(std::move(checkpoint->message));
                start = checkpoint->record.end;
            }
            JsonlScanner scanner(reader, false, start);
            while (const auto record = scanner.next()) append_loaded_record(result, *record);
            if (reader.unchanged(prefix)) return result;
        } catch (const HistorySnapshotChanged&) {
            if (attempt == 1) throw;
        }
    }
    throw HistorySnapshotChanged();
}

std::vector<ChatMessage> load_session_file_checkpoints(const std::string& path) {
    SessionLoadTimer timer("file_checkpoints");
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            SessionFileReader reader(path);
            if (!reader.valid()) return {};
            const auto prefix = reader.prefix();
            std::vector<ChatMessage> result;
            JsonlScanner scanner(reader, false);
            while (const auto record = scanner.next()) {
                if (record->text.find("file_checkpoint") == std::string::npos) continue;
                auto message = parse_record(*record);
                if (message && message->is_meta && message->subtype == "file_checkpoint") result.push_back(std::move(*message));
            }
            if (reader.unchanged(prefix)) return result;
        } catch (const HistorySnapshotChanged&) {
            if (attempt == 1) throw;
        }
    }
    throw HistorySnapshotChanged();
}

} // namespace acecode
