#include "config_recovery.hpp"

#include "../utils/atomic_file.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace acecode {
namespace {

constexpr int kRecoveryNoticeVersion = 1;
std::atomic<std::uint64_t> g_recovery_file_sequence{0};

void assign_error(std::string* out, const std::string& value) {
    if (out) *out = value;
}

std::int64_t unix_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::uint64_t process_id() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::string utc_filename_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    ::gmtime_s(&tm, &time);
#else
    ::gmtime_r(&time, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y%m%dT%H%M%S")
        << '-' << std::setw(3) << std::setfill('0') << millis.count() << 'Z';
    return out.str();
}

std::string unique_recovery_filename(const fs::path& config_path,
                                     const char* label) {
    const std::uint64_t sequence =
        g_recovery_file_sequence.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream out;
    out << path_to_utf8(config_path.stem()) << '-' << label << '-'
        << utc_filename_timestamp() << '-' << process_id() << '-'
        << sequence << ".json";
    return out.str();
}

std::optional<std::string> read_binary_file(const std::string& path,
                                            std::string* error) {
    std::ifstream input(path_from_utf8(path), std::ios::binary);
    if (!input.is_open()) {
        assign_error(error, "failed to open file: " + path);
        return std::nullopt;
    }
    std::ostringstream bytes;
    bytes << input.rdbuf();
    if (input.bad()) {
        assign_error(error, "failed to read file: " + path);
        return std::nullopt;
    }
    return bytes.str();
}

std::optional<std::string> write_unique_recovery_file(
    const fs::path& directory,
    const fs::path& config_path,
    const char* label,
    const std::string& bytes,
    std::string* error) {
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) {
        assign_error(error, "failed to create recovery directory: " +
            path_to_utf8(directory) + ": " + ec.message());
        return std::nullopt;
    }

    // PID + monotonic sequence already makes collision extraordinarily
    // unlikely; retry also protects deterministic clocks in unit tests.
    for (int attempt = 0; attempt < 32; ++attempt) {
        const fs::path candidate = directory /
            unique_recovery_filename(config_path, label);
        const std::string candidate_utf8 = path_to_utf8(candidate);
        if (fs::exists(candidate, ec)) {
            ec.clear();
            continue;
        }
        if (atomic_write_file(candidate_utf8, bytes, true)) {
            return candidate_utf8;
        }
    }

    assign_error(error, "failed to write unique recovery file under: " +
        path_to_utf8(directory));
    return std::nullopt;
}

} // namespace

ConfigRecoveryPaths config_recovery_paths(const std::string& config_path) {
    fs::path active = path_from_utf8(config_path);
    fs::path parent = active.parent_path();
    if (parent.empty()) parent = fs::current_path();
    const fs::path root = parent / "config-backups";
    const fs::path filename = active.filename().empty()
        ? fs::path("config.json")
        : active.filename();
    return {
        path_to_utf8(root),
        path_to_utf8(root / "last-good" / filename),
        path_to_utf8(root / "invalid"),
        path_to_utf8(root / "staging"),
        path_to_utf8(root / "recovery-notice.json"),
    };
}

bool write_last_good_config(const std::string& config_path,
                            const std::string& bytes,
                            std::string* error) {
    if (error) error->clear();
    const auto paths = config_recovery_paths(config_path);
    if (!atomic_write_file(paths.last_good_path, bytes, true)) {
        assign_error(error, "failed to write last-good config: " +
            paths.last_good_path);
        return false;
    }
    return true;
}

std::optional<std::string> read_last_good_config(
    const std::string& config_path,
    std::string* error) {
    if (error) error->clear();
    return read_binary_file(
        config_recovery_paths(config_path).last_good_path, error);
}

std::optional<std::string> archive_invalid_config(
    const std::string& config_path,
    const std::string& bytes,
    std::string* error) {
    if (error) error->clear();
    const auto paths = config_recovery_paths(config_path);
    return write_unique_recovery_file(
        path_from_utf8(paths.invalid_dir),
        path_from_utf8(config_path),
        "invalid",
        bytes,
        error);
}

std::optional<std::string> stage_config_recovery_candidate(
    const std::string& config_path,
    const std::string& bytes,
    std::string* error) {
    if (error) error->clear();
    const auto paths = config_recovery_paths(config_path);
    return write_unique_recovery_file(
        path_from_utf8(paths.staging_dir),
        path_from_utf8(config_path),
        "candidate",
        bytes,
        error);
}

bool write_config_recovery_notice(
    const std::string& config_path,
    const std::string& invalid_backup_path,
    std::string* error) {
    if (error) error->clear();
    const auto paths = config_recovery_paths(config_path);
    nlohmann::json notice = {
        {"version", kRecoveryNoticeVersion},
        {"pending", true},
        {"recovered_at_ms", unix_time_ms()},
        {"config_path", config_path},
        {"invalid_backup_path", invalid_backup_path},
    };
    if (!atomic_write_file(paths.notice_path, notice.dump(2) + "\n", true)) {
        assign_error(error, "failed to write recovery notice: " +
            paths.notice_path);
        return false;
    }
    return true;
}

ConfigRecoveryNotice read_config_recovery_notice(
    const std::string& config_path) {
    const auto paths = config_recovery_paths(config_path);
    std::string read_error;
    auto bytes = read_binary_file(paths.notice_path, &read_error);
    if (!bytes.has_value()) return {};
    try {
        const auto notice = nlohmann::json::parse(*bytes);
        if (!notice.is_object() ||
            notice.value("version", 0) != kRecoveryNoticeVersion ||
            !notice.value("pending", false)) {
            return {};
        }
        ConfigRecoveryNotice result;
        result.pending = true;
        result.recovered_at_ms = notice.value("recovered_at_ms", std::int64_t{0});
        result.config_path = notice.value("config_path", std::string{});
        result.invalid_backup_path =
            notice.value("invalid_backup_path", std::string{});
        result.invalid_backup_dir = result.invalid_backup_path.empty()
            ? paths.invalid_dir
            : path_to_utf8(
                path_from_utf8(result.invalid_backup_path).parent_path());
        return result;
    } catch (const nlohmann::json::exception& e) {
        LOG_WARN("[config_recovery] malformed recovery notice ignored: " +
                 paths.notice_path + "; json_error_id=" +
                 std::to_string(e.id));
        return {};
    }
}

bool acknowledge_config_recovery_notice(
    const std::string& config_path,
    std::string* error) {
    if (error) error->clear();
    const fs::path notice = path_from_utf8(
        config_recovery_paths(config_path).notice_path);
    std::error_code ec;
    const bool exists = fs::exists(notice, ec);
    if (ec) {
        assign_error(error, "failed to inspect recovery notice: " +
            path_to_utf8(notice) + ": " + ec.message());
        return false;
    }
    if (!exists) return true;
    if (!fs::is_regular_file(notice, ec) || ec) {
        assign_error(error, "recovery notice is not a regular file: " +
            path_to_utf8(notice));
        return false;
    }
    if (!fs::remove(notice, ec) || ec) {
        assign_error(error, "failed to remove recovery notice: " +
            path_to_utf8(notice) + (ec ? ": " + ec.message() : ""));
        return false;
    }
    return true;
}

} // namespace acecode
