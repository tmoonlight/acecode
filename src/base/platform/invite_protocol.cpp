#include "invite_protocol.hpp"
#include "platform/unique_resource.hpp"
#include "utils/utf8_path.hpp"
#include "utils/atomic_file.hpp"
#include "platform/process/process_runner.hpp"
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#endif

namespace acecode::platform {
namespace {
std::string normalize(std::string path) {
    std::replace(path.begin(), path.end(), '/', '\\');
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
    });
    return path;
}
#ifdef _WIN32
struct RegKeyTraits {
    using handle_type = HKEY;
    static HKEY invalid() noexcept { return nullptr; }
    static bool valid(HKEY key) noexcept { return key != nullptr; }
    static void close(HKEY key) noexcept { ::RegCloseKey(key); }
};
using UniqueRegKey = UniqueResource<RegKeyTraits>;
std::wstring root_key(const std::string& override_path) {
    return override_path.empty() ? L"Software\\Classes\\acecode" : path_from_utf8(override_path).wstring();
}
std::wstring command(const std::wstring& root) {
    UniqueRegKey key;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, (root + L"\\shell\\open\\command").c_str(), 0, KEY_QUERY_VALUE, key.put()) != ERROR_SUCCESS) return {};
    DWORD bytes = 0, type = 0;
    if (::RegQueryValueExW(key.get(), nullptr, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || type != REG_SZ || bytes > 65536) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (::RegQueryValueExW(key.get(), nullptr, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS) return {};
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}
std::string executable_of(const std::wstring& value) {
    if (value.empty() || value.front() != L'"') return {};
    const auto end = value.find(L'"', 1);
    if (end == std::wstring::npos) return {};
    return path_to_utf8(std::filesystem::path(value.substr(1, end - 1)));
}
bool write_value(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    UniqueRegKey key;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, key.put(), nullptr) != ERROR_SUCCESS) return false;
    return ::RegSetValueExW(key.get(), name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}
#endif
} // namespace

bool should_register_invite_protocol(const std::string& registered, bool exists, const std::string& current) {
    return !current.empty() && (registered.empty() || !exists || normalize(registered) == normalize(current));
}
bool register_invite_protocol(const std::string& executable, const std::string& registry_root) {
#ifdef _WIN32
    if (executable.empty()) return false;
    const auto root = root_key(registry_root);
    const auto existing_command = command(root);
    const auto existing = executable_of(existing_command);
    // Unknown command shapes belong to another handler; preserve them.
    if (!existing_command.empty() && existing.empty()) return true;
    std::error_code ec;
    const bool exists = !existing.empty() && std::filesystem::exists(path_from_utf8(existing), ec);
    if (!should_register_invite_protocol(existing, exists, executable)) return true;
    const auto wide = path_from_utf8(executable).wstring();
    return write_value(root, nullptr, L"URL:ACECode") && write_value(root, L"URL Protocol", L"") &&
        write_value(root + L"\\DefaultIcon", nullptr, L"\"" + wide + L"\",0") &&
        write_value(root + L"\\shell\\open\\command", nullptr, L"\"" + wide + L"\" --open-invite \"%1\"");
#elif !defined(__APPLE__)
    (void)registry_root;
    if (executable.empty()) return false;
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const char* user_home = std::getenv("HOME");
    if ((!xdg || !*xdg) && (!user_home || !*user_home)) return false;
    const auto directory = (xdg && *xdg ? path_from_utf8(xdg)
        : path_from_utf8(user_home) / ".local/share") / "applications";
    std::error_code ec; std::filesystem::create_directories(directory, ec);
    if (ec) return false;
    const auto existing = run_hook_process({"xdg-mime", {"query", "default", "x-scheme-handler/acecode"}}, "", 3000, "");
    if (existing.exit_code == 0 && !existing.stdout_text.empty() &&
        existing.stdout_text.find("acecode.desktop") == std::string::npos) return true;
    std::string escaped;
    for (const char c : executable) {
        if (c == '\n' || c == '\r' || c == '\0') return false;
        if (c == '"' || c == '\\' || c == '$' || c == '`') escaped += '\\';
        if (c == '%') escaped += '%';
        escaped += c;
    }
    const std::string entry = "[Desktop Entry]\nType=Application\nName=ACECode\nExec=\"" + escaped +
        "\" --open-invite %u\nTerminal=false\nCategories=Development;\nMimeType=x-scheme-handler/acecode;\n";
    if (!atomic_write_file(path_to_utf8(directory / "acecode.desktop"), entry)) return false;
    const auto registered = run_hook_process({"xdg-mime", {"default", "acecode.desktop", "x-scheme-handler/acecode"}}, "", 3000, "");
    return registered.started && registered.exit_code == 0;
#else
    (void)executable; (void)registry_root; return true;
#endif
}
bool unregister_invite_protocol(const std::string& executable, const std::string& registry_root) {
#ifdef _WIN32
    const auto root = root_key(registry_root);
    if (normalize(executable_of(command(root))) != normalize(executable) || executable.empty()) return true;
    const auto result = ::RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
#else
    (void)executable; (void)registry_root; return true;
#endif
}
} // namespace acecode::platform
