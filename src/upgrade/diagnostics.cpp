#include "diagnostics.hpp"

#include "apply.hpp"
#include "../config/config.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <atomic>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace acecode::upgrade {
namespace {

std::mutex file_mutex;
std::atomic<unsigned long long> next_operation{0};

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
        << std::setfill('0') << std::setw(3) << ms << 'Z';
    return out.str();
}

void sanitize(nlohmann::json& value) {
    if (value.is_string()) {
        value = redact_upgrade_diagnostic(value.get<std::string>());
    } else if (value.is_structured()) {
        for (auto& child : value) sanitize(child);
    }
}

} // namespace

std::string redact_upgrade_diagnostic(std::string text) {
    // Sanitize complete URL tokens, including URLs embedded in transport errors.
    // Scheme detection is case-insensitive and covers proxy URL schemes as well.
    size_t cursor = 0;
    while ((cursor = text.find("://", cursor)) != std::string::npos) {
        const size_t authority = cursor + 3;
        size_t end = text.find_first_of(" \t\r\n\"'<>", authority);
        if (end == std::string::npos) end = text.size();
        std::string url = text.substr(authority, end - authority);
        const auto secret = url.find_first_of("?#");
        if (secret != std::string::npos) url.replace(secret, std::string::npos, "[redacted]");
        const auto slash = url.find('/');
        const auto at = url.rfind('@', slash == std::string::npos ? url.size() : slash);
        if (at != std::string::npos) url.replace(0, at, "[redacted]");
        text.replace(authority, end - authority, url);
        cursor = authority + url.size();
    }
    // Sanitize before truncating, so a long credential cannot survive a cutoff.
    constexpr size_t kMaxDiagnosticString = 8192;
    if (text.size() > kMaxDiagnosticString) {
        text.resize(kMaxDiagnosticString);
        text += "...[truncated]";
    }
    return text;
}

DiagnosticLog::DiagnosticLog(const std::string& operation,
                             const std::filesystem::path& directory) noexcept {
    try {
        const auto now = timestamp();
        const auto pid = std::to_string(current_process_id());
        operation_id_ = pid + "-" + now + "-" + std::to_string(++next_operation);
        const auto root = directory.empty()
            ? path_from_utf8(get_acecode_dir()) / "logs" : directory;
        path_ = root / ("upgrade-" + now.substr(0, 10) + "-" + pid + ".log");
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        if (ec) throw std::runtime_error("cannot create log directory: " + ec.message());
        record("operation_started", {{"operation", operation}});
    } catch (const std::exception& e) {
        error_ = redact_upgrade_diagnostic(e.what());
    } catch (...) {
        error_ = "cannot initialize upgrade diagnostics";
    }
}

void DiagnosticLog::write_locked(const std::string& event, nlohmann::json details) {
    sanitize(details);
    const nlohmann::json record{
        {"time", timestamp()}, {"operation_id", operation_id_},
        {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_).count()},
        {"phase", phase_}, {"event", event}, {"details", std::move(details)},
    };
    const auto line = record.dump(-1, ' ', false,
        nlohmann::json::error_handler_t::replace) + "\n";
    std::lock_guard<std::mutex> file_lock(file_mutex);
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
    out.flush();
    if (!out) throw std::runtime_error("cannot write upgrade log: " + path_to_utf8(path_));
    created_ = true;
}

void DiagnosticLog::record(const std::string& event, nlohmann::json details) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!error_.empty()) return;
    try {
        write_locked(event, std::move(details));
    } catch (const std::exception& e) {
        error_ = redact_upgrade_diagnostic(e.what());
        LOG_WARN("[upgrade] diagnostics unavailable: " + error_);
    } catch (...) {
        error_ = "cannot write upgrade diagnostics";
    }
}

void DiagnosticLog::phase(const std::string& phase, nlohmann::json details) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        phase_ = phase;
        if (error_.empty()) write_locked("phase_started", std::move(details));
    } catch (const std::exception& e) {
        error_ = redact_upgrade_diagnostic(e.what());
    } catch (...) {
        error_ = "cannot write upgrade diagnostics";
    }
}

std::string DiagnosticLog::path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return created_ ? path_to_utf8(path_) : std::string{};
}

std::string DiagnosticLog::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

std::string DiagnosticLog::with_location(const std::string& message) const {
    auto result = message;
    const auto log_path = path();
    if (!log_path.empty() && result.find(log_path) == std::string::npos) {
        if (!result.empty() && result.back() != '\n') result += '\n';
        result += "Upgrade log: " + log_path;
    }
    const auto log_error = error();
    if (!log_error.empty()) result += "\nUpgrade diagnostics unavailable or incomplete: " + log_error;
    return result;
}

} // namespace acecode::upgrade
