#include "session_trajectory.hpp"

#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <string_view>

namespace fs = std::filesystem;

namespace acecode {
namespace {

std::mutex& trajectory_append_mutex() {
    static std::mutex mu;
    return mu;
}

bool parse_record_line(std::string_view line,
                       SessionTrajectoryRecord* record) {
    if (!record || line.empty()) return false;
    try {
        return session_trajectory_record_from_json(
            nlohmann::json::parse(line.begin(), line.end()), record);
    } catch (...) {
        return false;
    }
}

template <typename Visitor>
SessionTrajectoryLoadDiagnostics visit_records(const std::string& path,
                                                Visitor&& visitor) {
    SessionTrajectoryLoadDiagnostics diagnostics;
    std::ifstream input(path_from_utf8(path), std::ios::binary);
    if (!input.is_open()) return diagnostics;

    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    std::size_t start = 0;
    while (start < content.size()) {
        const std::size_t newline = content.find('\n', start);
        const bool is_tail = newline == std::string::npos;
        const std::size_t end = is_tail ? content.size() : newline;
        std::string_view line(content.data() + start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        if (!line.empty()) {
            SessionTrajectoryRecord record;
            if (parse_record_line(line, &record)) {
                if (is_tail) diagnostics.recovered_unterminated_record = true;
                if (!visitor(record)) return diagnostics;
            } else if (is_tail) {
                diagnostics.ignored_partial_tail = true;
            } else {
                ++diagnostics.malformed_complete_records;
            }
        }
        if (is_tail) break;
        start = newline + 1;
    }
    return diagnostics;
}

} // namespace

nlohmann::json session_trajectory_record_to_json(
    const SessionTrajectoryRecord& record) {
    return nlohmann::json{
        {"schema_version", record.schema_version},
        {"sequence", record.sequence},
        {"timestamp_ms", record.timestamp_ms},
        {"type", record.type},
        {"payload", record.payload.is_null()
            ? nlohmann::json::object()
            : record.payload},
    };
}

bool session_trajectory_record_from_json(
    const nlohmann::json& value,
    SessionTrajectoryRecord* record) {
    if (!record || !value.is_object()) return false;
    if (!value.contains("schema_version") ||
        !value["schema_version"].is_number_integer() ||
        !value.contains("sequence") ||
        (!value["sequence"].is_number_unsigned() &&
         !value["sequence"].is_number_integer()) ||
        !value.contains("timestamp_ms") ||
        (!value["timestamp_ms"].is_number_integer() &&
         !value["timestamp_ms"].is_number_unsigned()) ||
        !value.contains("type") || !value["type"].is_string() ||
        value["type"].get_ref<const std::string&>().empty()) {
        return false;
    }

    const auto schema_version = value["schema_version"].get<int>();
    if (schema_version <= 0) return false;

    std::uint64_t sequence = 0;
    if (value["sequence"].is_number_unsigned()) {
        sequence = value["sequence"].get<std::uint64_t>();
    } else {
        const auto signed_sequence = value["sequence"].get<std::int64_t>();
        if (signed_sequence <= 0) return false;
        sequence = static_cast<std::uint64_t>(signed_sequence);
    }
    if (sequence == 0) return false;

    std::int64_t timestamp_ms = 0;
    if (value["timestamp_ms"].is_number_unsigned()) {
        const auto raw = value["timestamp_ms"].get<std::uint64_t>();
        if (raw > static_cast<std::uint64_t>(
                      std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        timestamp_ms = static_cast<std::int64_t>(raw);
    } else {
        timestamp_ms = value["timestamp_ms"].get<std::int64_t>();
    }
    if (timestamp_ms < 0) return false;

    record->schema_version = schema_version;
    record->sequence = sequence;
    record->timestamp_ms = timestamp_ms;
    record->type = value["type"].get<std::string>();
    record->payload = value.value("payload", nlohmann::json::object());
    return true;
}

std::string SessionTrajectoryStorage::file_path(
    const std::string& project_dir,
    const std::string& session_id) {
    if (project_dir.empty() || session_id.empty()) return {};
    return path_to_utf8(path_from_utf8(project_dir) /
                        path_from_utf8(session_id) /
                        "trajectory.jsonl");
}

bool SessionTrajectoryStorage::append(
    const std::string& path,
    const SessionTrajectoryRecord& record) {
    if (path.empty() || record.sequence == 0 || record.type.empty() ||
        record.schema_version <= 0 || record.timestamp_ms < 0) {
        return false;
    }

    std::string encoded;
    try {
        encoded = session_trajectory_record_to_json(record).dump(-1);
    } catch (...) {
        return false;
    }

    std::lock_guard<std::mutex> lock(trajectory_append_mutex());
    const fs::path native_path = path_from_utf8(path);
    std::error_code ec;
    const fs::path parent = native_path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) return false;
    }

    bool isolate_existing_tail = false;
    if (fs::exists(native_path, ec)) {
        if (ec || !fs::is_regular_file(native_path, ec) || ec) return false;
        const auto size = fs::file_size(native_path, ec);
        if (ec) return false;
        if (size > 0) {
            std::ifstream tail(native_path, std::ios::binary);
            if (!tail.is_open()) return false;
            tail.seekg(-1, std::ios::end);
            char last = '\0';
            tail.read(&last, 1);
            if (!tail.good()) return false;
            isolate_existing_tail = last != '\n';
        }
    } else if (ec) {
        return false;
    }

    std::ofstream output(native_path, std::ios::binary | std::ios::app);
    if (!output.is_open()) return false;
    if (isolate_existing_tail) output.put('\n');
    output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    output.put('\n');
    output.flush();
    return output.good();
}

SessionTrajectoryPage SessionTrajectoryStorage::load_page(
    const std::string& path,
    std::uint64_t after,
    std::size_t limit) {
    SessionTrajectoryPage page;
    limit = std::clamp<std::size_t>(
        limit == 0 ? kSessionTrajectoryDefaultPageSize : limit,
        1,
        kSessionTrajectoryMaxPageSize);

    bool saw_extra = false;
    page.diagnostics = visit_records(path, [&](const SessionTrajectoryRecord& record) {
        if (record.sequence <= after) return true;
        if (page.records.size() >= limit) {
            saw_extra = true;
            return false;
        }
        page.records.push_back(record);
        return true;
    });
    page.has_more = saw_extra;
    page.next_after = page.records.empty()
        ? after
        : page.records.back().sequence;
    return page;
}

std::vector<SessionTrajectoryRecord> SessionTrajectoryStorage::load_all(
    const std::string& path,
    SessionTrajectoryLoadDiagnostics* diagnostics) {
    std::vector<SessionTrajectoryRecord> records;
    auto loaded_diagnostics = visit_records(
        path, [&records](const SessionTrajectoryRecord& record) {
            records.push_back(record);
            return true;
        });
    if (diagnostics) *diagnostics = loaded_diagnostics;
    return records;
}

std::uint64_t SessionTrajectoryStorage::last_sequence(
    const std::string& path) {
    std::uint64_t last = 0;
    visit_records(path, [&](const SessionTrajectoryRecord& record) {
        last = std::max(last, record.sequence);
        return true;
    });
    return last;
}

} // namespace acecode
