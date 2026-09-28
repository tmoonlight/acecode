#pragma once

#include <chrono>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

namespace acecode::upgrade {

// Separate from Logger: upgrade commands also run before normal logging starts.
// A context belongs to one check/upgrade; callers can share it with a GUI worker.
class DiagnosticLog {
public:
    explicit DiagnosticLog(const std::string& operation,
                           const std::filesystem::path& directory = {}) noexcept;
    void record(const std::string& event,
                nlohmann::json details = nlohmann::json::object()) noexcept;
    void phase(const std::string& phase,
               nlohmann::json details = nlohmann::json::object()) noexcept;
    std::string path() const;
    std::string error() const;
    std::string with_location(const std::string& message) const;

private:
    void write_locked(const std::string& event, nlohmann::json details);
    mutable std::mutex mutex_;
    std::filesystem::path path_;
    std::string operation_id_;
    std::string phase_ = "checking";
    std::string error_;
    bool created_ = false;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};

std::string redact_upgrade_diagnostic(std::string text);

} // namespace acecode::upgrade
