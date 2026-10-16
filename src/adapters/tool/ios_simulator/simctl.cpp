#include "simctl.hpp"

#include "platform/process/process_runner.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace acecode::ios_simulator {
namespace {

std::string trim(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
    return text.substr(start);
}

// "com.apple.CoreSimulator.SimRuntime.iOS-26-5" -> "iOS 26.5"
std::string runtime_name_from_identifier(const std::string& identifier) {
    const auto dot = identifier.rfind('.');
    std::string tail = dot == std::string::npos ? identifier : identifier.substr(dot + 1);
    const auto dash = tail.find('-');
    if (dash == std::string::npos) return tail;
    std::string name = tail.substr(0, dash) + " " + tail.substr(dash + 1);
    std::replace(name.begin() + static_cast<long>(dash) + 1, name.end(), '-', '.');
    return name;
}

} // namespace

std::string SimctlResult::message() const {
    if (aborted) return "Cancelled.";
    if (timed_out) return "simctl timed out.";
    auto text = trim(err.empty() ? out : err);
    if (text.size() > 2000) text = text.substr(0, 2000) + "…";
    if (text.empty()) text = "simctl exited with code " + std::to_string(exit_code) + ".";
    return text;
}

bool is_booted_state(const std::string& state) { return state == "Booted"; }

std::vector<SimctlDevice> parse_simctl_list(const nlohmann::json& list) {
    struct RuntimeInfo { std::string name, version, platform; };
    std::vector<std::pair<std::string, RuntimeInfo>> runtimes;
    if (list.contains("runtimes") && list["runtimes"].is_array()) {
        for (const auto& runtime : list["runtimes"]) {
            if (!runtime.is_object()) continue;
            runtimes.push_back({runtime.value("identifier", std::string()),
                                {runtime.value("name", std::string()), runtime.value("version", std::string()),
                                 runtime.value("platform", std::string())}});
        }
    }
    std::vector<SimctlDevice> devices;
    if (!list.contains("devices") || !list["devices"].is_object()) return devices;
    for (const auto& [runtime_id, entries] : list["devices"].items()) {
        if (!entries.is_array()) continue;
        RuntimeInfo info{runtime_name_from_identifier(runtime_id), {}, {}};
        for (const auto& [id, value] : runtimes) {
            if (id == runtime_id) { info = value; break; }
        }
        if (info.platform.empty()) {
            const auto space = info.name.find(' ');
            info.platform = space == std::string::npos ? info.name : info.name.substr(0, space);
        }
        if (info.version.empty()) {
            const auto space = info.name.find(' ');
            if (space != std::string::npos) info.version = info.name.substr(space + 1);
        }
        for (const auto& entry : entries) {
            if (!entry.is_object()) continue;
            SimctlDevice device;
            device.udid = entry.value("udid", std::string());
            device.name = entry.value("name", std::string());
            device.state = entry.value("state", std::string());
            device.device_type_identifier = entry.value("deviceTypeIdentifier", std::string());
            device.available = entry.value("isAvailable", true);
            device.availability_error = entry.value("availabilityError", std::string());
            device.last_booted_at = entry.value("lastBootedAt", std::string());
            device.runtime_identifier = runtime_id;
            device.runtime_name = info.name;
            device.runtime_version = info.version;
            device.platform = info.platform;
            if (!device.udid.empty()) devices.push_back(std::move(device));
        }
    }
    // Booted first, then by platform/runtime (newest first) and name.
    std::stable_sort(devices.begin(), devices.end(), [](const SimctlDevice& a, const SimctlDevice& b) {
        const bool ab = is_booted_state(a.state);
        const bool bb = is_booted_state(b.state);
        if (ab != bb) return ab;
        if (a.platform != b.platform) return a.platform < b.platform;
        if (a.runtime_version != b.runtime_version) return a.runtime_version > b.runtime_version;
        return a.name < b.name;
    });
    return devices;
}

nlohmann::json to_json(const SimctlDevice& device) {
    return {{"udid", device.udid}, {"name", device.name}, {"state", device.state},
            {"booted", is_booted_state(device.state)}, {"device_type", device.device_type_identifier},
            {"runtime", device.runtime_name}, {"runtime_identifier", device.runtime_identifier},
            {"runtime_version", device.runtime_version}, {"platform", device.platform},
            {"available", device.available}, {"availability_error", device.availability_error}};
}

SimctlResult Simctl::run(const std::vector<std::string>& args, const std::string& stdin_text, int timeout_ms,
                         const std::atomic<bool>* abort_flag) const {
    SimctlResult result;
    if (developer_dir_.empty()) {
        result.err = "No Xcode is selected.";
        return result;
    }
    HookProcessOptions options;
    options.timeout_ms = timeout_ms;
    options.abort_flag = abort_flag;
    options.max_stdout_bytes = 8 * 1024 * 1024;
    options.max_stderr_bytes = 256 * 1024;
    options.append_output_truncation_notice = false;
    options.terminate_process_tree = true;
    auto process = run_hook_process({developer_dir_ + "/usr/bin/simctl", args}, stdin_text, "", options);
    result.exit_code = process.exit_code;
    result.timed_out = process.timed_out;
    result.aborted = process.aborted;
    result.out = std::move(process.stdout_text);
    result.err = process.started ? std::move(process.stderr_text) : process.error;
    result.ok = process.started && !process.timed_out && !process.aborted && process.exit_code == 0;
    return result;
}

std::vector<SimctlDevice> Simctl::list(std::string* error) const {
    auto result = run({"list", "-j", "devices", "runtimes"}, {}, 30000);
    if (!result.ok) {
        if (error) *error = result.message();
        return {};
    }
    auto json = nlohmann::json::parse(result.out, nullptr, false);
    if (json.is_discarded()) {
        if (error) *error = "simctl returned invalid JSON.";
        return {};
    }
    return parse_simctl_list(json);
}

SimctlResult Simctl::boot(const std::string& udid, const std::atomic<bool>* abort_flag) const {
    auto result = run({"boot", udid}, {}, 120000, abort_flag);
    // "Unable to boot device in current state: Booted" is success for us.
    if (!result.ok && result.err.find("current state: Booted") == std::string::npos) return result;
    return run({"bootstatus", udid, "-b"}, {}, 240000, abort_flag);
}

SimctlResult Simctl::shutdown(const std::string& udid) const {
    auto result = run({"shutdown", udid}, {}, 60000);
    if (!result.ok && result.err.find("current state: Shutdown") != std::string::npos) result.ok = true;
    return result;
}

SimctlResult Simctl::install(const std::string& udid, const std::string& app_path,
                             const std::atomic<bool>* abort_flag) const {
    return run({"install", udid, app_path}, {}, 300000, abort_flag);
}

SimctlResult Simctl::launch(const std::string& udid, const std::string& bundle_id,
                            const std::vector<std::string>& arguments, bool terminate_running, long& pid) const {
    std::vector<std::string> args{"launch"};
    if (terminate_running) args.push_back("--terminate-running-process");
    args.push_back(udid);
    args.push_back(bundle_id);
    args.insert(args.end(), arguments.begin(), arguments.end());
    auto result = run(args, {}, 60000);
    pid = 0;
    if (result.ok) {
        const auto colon = result.out.rfind(':');
        if (colon != std::string::npos) pid = std::strtol(result.out.c_str() + colon + 1, nullptr, 10);
    }
    return result;
}

SimctlResult Simctl::terminate(const std::string& udid, const std::string& bundle_id) const {
    return run({"terminate", udid, bundle_id}, {}, 30000);
}

SimctlResult Simctl::open_url(const std::string& udid, const std::string& url) const {
    return run({"openurl", udid, url}, {}, 30000);
}

SimctlResult Simctl::pbcopy(const std::string& udid, const std::string& text) const {
    return run({"pbcopy", udid}, text, 15000);
}

nlohmann::json Simctl::list_apps(const std::string& udid, std::string* error) const {
    auto result = run({"listapps", udid}, {}, 30000);
    if (!result.ok) {
        if (error) *error = result.message();
        return nlohmann::json::object();
    }
    // listapps prints an old-style property list; plutil converts it.
    HookProcessOptions options;
    options.timeout_ms = 15000;
    options.max_stdout_bytes = 8 * 1024 * 1024;
    auto converted = run_hook_process({"/usr/bin/plutil", {"-convert", "json", "-o", "-", "-"}}, result.out, "",
                                      options);
    auto json = nlohmann::json::parse(converted.stdout_text, nullptr, false);
    if (converted.exit_code != 0 || !json.is_object()) {
        if (error) *error = "Could not parse the installed application list.";
        return nlohmann::json::object();
    }
    nlohmann::json apps = nlohmann::json::object();
    for (const auto& [bundle_id, info] : json.items()) {
        if (!info.is_object()) continue;
        apps[bundle_id] = {{"name", info.value("CFBundleDisplayName", info.value("CFBundleName", bundle_id))},
                           {"type", info.value("ApplicationType", std::string())},
                           {"path", info.value("Path", std::string())}};
    }
    return apps;
}

} // namespace acecode::ios_simulator
