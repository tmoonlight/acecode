#pragma once

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace acecode::computer_use {

struct InstalledApplicationIdentity {
    std::string id;
    std::string display_name;
    std::string executable_path;
};

inline std::string application_path_key(std::string path) {
    std::replace(path.begin(), path.end(), '/', '\\');
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (path.compare(0, 8, "\\\\?\\unc\\") == 0) path = "\\\\" + path.substr(8);
    else if (path.compare(0, 4, "\\\\?\\") == 0) path.erase(0, 4);
    return path;
}

// AppsFolder identities and process executable paths are different identifiers.
// Prefer an exact AUMID, and use a path only when it identifies exactly one app:
// two browser profiles or packaged applications may share the same executable.
inline nlohmann::json merge_discovered_applications(
    const std::vector<InstalledApplicationIdentity>& installed,
    const nlohmann::json& windows) {
    using json = nlohmann::json;
    std::map<std::string, json> apps;
    std::map<std::string, std::vector<std::string>> path_ids;
    for (const auto& app : installed) {
        if (app.id.empty()) continue;
        apps[app.id] = {{"id", app.id}, {"displayName", app.display_name.empty() ? app.id : app.display_name},
                       {"isRunning", false}, {"windows", json::array()}};
        if (!app.executable_path.empty()) {
            auto& candidates = path_ids[application_path_key(app.executable_path)];
            if (std::find(candidates.begin(), candidates.end(), app.id) == candidates.end()) candidates.push_back(app.id);
        }
    }
    for (auto window : windows) {
        auto id = window.value("app", std::string());
        auto executable = window.value("executable_path", std::string());
        if (executable.empty() && (id.find('\\') != std::string::npos || id.find('/') != std::string::npos)) executable = id;
        const bool process_path_identity = id.empty() || id.find('\\') != std::string::npos || id.find('/') != std::string::npos;
        if (!apps.count(id) && process_path_identity) {
            auto candidates = path_ids.find(application_path_key(executable));
            if (candidates != path_ids.end() && candidates->second.size() == 1) id = candidates->second.front();
        }
        if (id.empty()) id = executable;
        if (id.empty()) continue;
        if (!apps.count(id)) apps[id] = {{"id", id}, {"displayName", id.substr(id.find_last_of("\\/") + 1)},
                                        {"isRunning", false}, {"windows", json::array()}};
        window["app"] = id;
        apps[id]["isRunning"] = true;
        apps[id]["windows"].push_back(std::move(window));
    }
    json output = json::array();
    for (auto& app : apps) output.push_back(std::move(app.second));
    return output;
}

} // namespace acecode::computer_use
