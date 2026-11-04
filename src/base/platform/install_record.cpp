#include "install_record.hpp"

#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace acecode::install_record {
namespace {

namespace fs = std::filesystem;

std::string normalized_dir(const std::string& dir) {
    std::string out;
    out.reserve(dir.size());
    for (char c : dir) {
        const char slash = c == '/' ? '\\' : c;
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(slash))));
    }
    while (out.size() > 3 && out.back() == '\\') out.pop_back();
    if (out.size() >= 2 && out.front() == '"' && out.back() == '"') {
        out = out.substr(1, out.size() - 2);
    }
    return out;
}

#ifdef _WIN32
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = ::MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          out.data(), length);
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          out.data(), length, nullptr, nullptr);
    return out;
}

std::wstring uninstall_key_path() {
    return wide(std::string(kUninstallRegistryParent) + "\\" + kUninstallKeyName);
}

std::optional<std::string> read_string_value(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (::RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (::RegQueryValueExW(key, name, nullptr, &type,
                           reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    value.resize(bytes / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return narrow(value);
}

bool refresh_registry_version(const fs::path& install_dir, const std::string& version) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, uninstall_key_path().c_str(), 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    bool changed = false;
    const auto location = read_string_value(key, L"InstallLocation");
    if (location && same_install_dir(*location, path_to_utf8(install_dir))) {
        const std::wstring value = wide(version);
        changed = ::RegSetValueExW(
            key, L"DisplayVersion", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    }
    ::RegCloseKey(key);
    return changed;
}
#endif

}  // namespace

std::string serialize(const InstallRecord& record) {
    nlohmann::ordered_json json;
    json["schema"] = record.schema;
    json["version"] = record.version;
    json["target"] = record.target;
    json["add_to_path"] = record.add_to_path;
    json["desktop_shortcut"] = record.desktop_shortcut;
    json["installed_at"] = record.installed_at;
    return json.dump(2) + "\n";
}

std::optional<InstallRecord> parse(const std::string& text, std::string* error) {
    const auto json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        if (error) *error = "install record is not a JSON object";
        return std::nullopt;
    }
    InstallRecord record;
    record.schema = json.value("schema", 0);
    if (record.schema != 1) {
        if (error) *error = "unsupported install record schema";
        return std::nullopt;
    }
    const auto text_field = [&json](const char* name) {
        const auto it = json.find(name);
        return it != json.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    const auto bool_field = [&json](const char* name) {
        const auto it = json.find(name);
        return it != json.end() && it->is_boolean() && it->get<bool>();
    };
    record.version = text_field("version");
    record.target = text_field("target");
    record.installed_at = text_field("installed_at");
    record.add_to_path = bool_field("add_to_path");
    record.desktop_shortcut = bool_field("desktop_shortcut");
    if (record.version.empty()) {
        if (error) *error = "install record has no version";
        return std::nullopt;
    }
    return record;
}

std::optional<InstallRecord> read_marker(const fs::path& install_dir) {
    std::ifstream in(install_dir / kMarkerFileName, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream text;
    text << in.rdbuf();
    return parse(text.str());
}

bool write_marker(const fs::path& install_dir, const InstallRecord& record, std::string* error) {
    const fs::path target = install_dir / kMarkerFileName;
    const fs::path temp = install_dir / (std::string(kMarkerFileName) + ".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << serialize(record);
        if (!out) {
            if (error) *error = "failed to write " + path_to_utf8(temp);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, target, ec);
    if (ec) {
        fs::remove(temp, ec);
        if (error) *error = "failed to replace " + path_to_utf8(target);
        return false;
    }
    return true;
}

bool same_install_dir(const std::string& a, const std::string& b) {
    return !a.empty() && normalized_dir(a) == normalized_dir(b);
}

bool refresh_installed_version(const fs::path& install_dir, const std::string& version) {
    auto record = read_marker(install_dir);
    if (!record || version.empty()) return false;
    bool changed = false;
    if (record->version != version) {
        record->version = version;
        changed = write_marker(install_dir, *record);
    }
#ifdef _WIN32
    changed = refresh_registry_version(install_dir, version) || changed;
#endif
    return changed;
}

}  // namespace acecode::install_record
