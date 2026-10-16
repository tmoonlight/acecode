#pragma once

// Finds usable Xcode installations without relying on xcode-select. Xcode is
// often copied to an arbitrary folder ("green install", e.g. ~/Downloads);
// such a copy works as long as tools are launched from its own
// Contents/Developer directory, which is what the simulator runtime does.

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace acecode::ios_simulator {

struct XcodeInstall {
    std::string app_path;       // .../Xcode.app
    std::string developer_dir;  // .../Xcode.app/Contents/Developer
    std::string version;        // CFBundleShortVersionString, e.g. "26.6"
    std::string build;          // ProductBuildVersion, e.g. "17F113"
    bool simulator_kit = false; // SimulatorKit.framework present (input injection)
    std::vector<std::string> sources;  // configured, DEVELOPER_DIR, xcode-select, applications, spotlight
};

struct XcodeResolution {
    std::optional<XcodeInstall> selected;
    std::string selected_source;
    std::string configured_path;
    std::string configured_error;  // configured path given but unusable
    std::string error;             // nothing usable
    std::string xcode_select_path; // raw xcode-select target (may be CommandLineTools)
    std::string core_simulator_version;
    std::vector<XcodeInstall> installs;
};

// Accepts an Xcode.app path or its Contents/Developer path (a trailing slash
// is fine). Returns nullopt when it is not a usable Xcode.
std::optional<XcodeInstall> inspect_xcode(const std::string& path, std::string* why = nullptr);

// --- Pure helpers (unit tested) --------------------------------------------

// Value of <key>key</key><string>value</string> in an XML property list.
std::string plist_string_value(const std::string& xml, const std::string& key);
// Numeric dotted comparison: -1, 0, 1.
int compare_versions(const std::string& a, const std::string& b);
// Maps an Xcode.app or Developer path to {app_path, developer_dir}; empty
// strings when the shape is not recognized.
std::pair<std::string, std::string> xcode_paths_for(const std::string& path);
// Merges duplicates (same app) keeping every source, then picks:
// configured > DEVELOPER_DIR > xcode-select > newest version.
XcodeResolution choose_xcode(const std::string& configured_path, const std::string& configured_error,
                             std::vector<XcodeInstall> installs);

// --- Discovery -------------------------------------------------------------

struct DiscoveryOptions {
    bool spotlight = true;       // mdfind for copies outside /Applications
    int spotlight_timeout_ms = 3000;
};

// Full resolution with the given configured path (may be empty).
XcodeResolution resolve_xcode(const std::string& configured_path, const DiscoveryOptions& options = {});

// Version of the system CoreSimulator framework (installed by Xcode's first
// launch), empty when missing.
std::string core_simulator_version();

nlohmann::json to_json(const XcodeInstall& install);
nlohmann::json to_json(const XcodeResolution& resolution);

} // namespace acecode::ios_simulator
