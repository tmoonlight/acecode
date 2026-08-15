#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode {

constexpr int kSessionTrajectorySchemaVersion = 1;
constexpr std::size_t kSessionTrajectoryDefaultPageSize = 250;
constexpr std::size_t kSessionTrajectoryMaxPageSize = 1000;

struct SessionTrajectoryRecord {
    int schema_version = kSessionTrajectorySchemaVersion;
    std::uint64_t sequence = 0;
    std::int64_t timestamp_ms = 0;
    std::string type;
    nlohmann::json payload = nlohmann::json::object();
};

struct SessionTrajectoryLoadDiagnostics {
    std::size_t malformed_complete_records = 0;
    bool ignored_partial_tail = false;
    bool recovered_unterminated_record = false;

    bool recovered() const {
        return malformed_complete_records > 0 ||
               ignored_partial_tail ||
               recovered_unterminated_record;
    }
};

struct SessionTrajectoryPage {
    std::vector<SessionTrajectoryRecord> records;
    std::uint64_t next_after = 0;
    bool has_more = false;
    SessionTrajectoryLoadDiagnostics diagnostics;
};

nlohmann::json session_trajectory_record_to_json(
    const SessionTrajectoryRecord& record);
bool session_trajectory_record_from_json(
    const nlohmann::json& value,
    SessionTrajectoryRecord* record);

class SessionTrajectoryStorage {
public:
    static std::string file_path(const std::string& project_dir,
                                 const std::string& session_id);

    // Appends one compact JSONL record. A missing newline after an existing
    // tail is inserted first so crash residue cannot swallow the new record.
    static bool append(const std::string& path,
                       const SessionTrajectoryRecord& record);

    // Reads valid records with sequence > after in ascending file order.
    // limit is clamped to [1, kSessionTrajectoryMaxPageSize].
    static SessionTrajectoryPage load_page(
        const std::string& path,
        std::uint64_t after = 0,
        std::size_t limit = kSessionTrajectoryDefaultPageSize);

    // Reads every valid record once. Used by legacy projection to build a
    // complete stable-id coverage set without repeatedly rescanning the file.
    static std::vector<SessionTrajectoryRecord> load_all(
        const std::string& path,
        SessionTrajectoryLoadDiagnostics* diagnostics = nullptr);

    // Returns the greatest valid sequence in the file, or zero when absent.
    static std::uint64_t last_sequence(const std::string& path);
};

} // namespace acecode
