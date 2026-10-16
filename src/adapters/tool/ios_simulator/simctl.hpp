#pragma once

// Thin wrapper over the selected Xcode's simctl. The binary is launched from
// <developer_dir>/usr/bin/simctl, which resolves its own Xcode, so
// xcode-select and DEVELOPER_DIR do not need to point at it.

#include <nlohmann/json.hpp>

#include <atomic>
#include <string>
#include <vector>

namespace acecode::ios_simulator {

struct SimctlDevice {
    std::string udid;
    std::string name;
    std::string state;               // Booted, Shutdown, Booting, ...
    std::string device_type_identifier;
    std::string runtime_identifier;
    std::string runtime_name;        // "iOS 26.5"
    std::string runtime_version;     // "26.5"
    std::string platform;            // "iOS", "watchOS", ...
    bool available = true;
    std::string availability_error;
    std::string last_booted_at;
};

struct SimctlResult {
    bool ok = false;
    int exit_code = -1;
    bool timed_out = false;
    bool aborted = false;
    std::string out;
    std::string err;
    // Human-readable failure (stderr, or a timeout / abort description).
    std::string message() const;
};

// Pure: parses `simctl list -j` (devices + runtimes). Devices of unknown
// runtimes keep the identifier-derived name.
std::vector<SimctlDevice> parse_simctl_list(const nlohmann::json& list);
nlohmann::json to_json(const SimctlDevice& device);
bool is_booted_state(const std::string& state);

class Simctl {
public:
    explicit Simctl(std::string developer_dir) : developer_dir_(std::move(developer_dir)) {}

    SimctlResult run(const std::vector<std::string>& args, const std::string& stdin_text = {},
                     int timeout_ms = 60000, const std::atomic<bool>* abort_flag = nullptr) const;

    std::vector<SimctlDevice> list(std::string* error = nullptr) const;
    SimctlResult boot(const std::string& udid, const std::atomic<bool>* abort_flag = nullptr) const;
    SimctlResult shutdown(const std::string& udid) const;
    SimctlResult install(const std::string& udid, const std::string& app_path,
                         const std::atomic<bool>* abort_flag = nullptr) const;
    // Returns the pid on success (0 when simctl did not report one).
    SimctlResult launch(const std::string& udid, const std::string& bundle_id,
                        const std::vector<std::string>& arguments, bool terminate_running, long& pid) const;
    SimctlResult terminate(const std::string& udid, const std::string& bundle_id) const;
    SimctlResult open_url(const std::string& udid, const std::string& url) const;
    SimctlResult pbcopy(const std::string& udid, const std::string& text) const;
    // Installed applications as {bundle_id: {name, type, path}}; empty on failure.
    nlohmann::json list_apps(const std::string& udid, std::string* error = nullptr) const;

    const std::string& developer_dir() const { return developer_dir_; }

private:
    std::string developer_dir_;
};

} // namespace acecode::ios_simulator
