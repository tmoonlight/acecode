#include "memory_paths.hpp"

#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>

namespace fs = std::filesystem;

namespace acecode {

fs::path get_memory_dir() {
    return path_from_utf8(get_acecode_dir()) / "memory";
}

fs::path get_memory_index_path() {
    return get_memory_dir() / "MEMORY.md";
}

fs::path get_memory_state_db_path() {
    return get_memory_dir() / "state.sqlite3";
}

fs::path workspace_memory_dir(const std::string& project_dir_utf8) {
    if (project_dir_utf8.empty()) return {};
    return path_from_utf8(project_dir_utf8) / "memory";
}

fs::path memory_inbox_dir(const fs::path& scope_dir) {
    return scope_dir / "inbox";
}

fs::path memory_archive_dir(const fs::path& scope_dir) {
    return scope_dir / "archive";
}

std::string validate_memory_name(const std::string& name) {
    if (name.empty()) return "memory name is empty";
    if (name.size() > 64) return "memory name exceeds 64 bytes: " + name;
    for (unsigned char c : name) {
        const bool ok =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-';
        if (!ok) {
            return std::string("memory name has invalid character '") +
                   static_cast<char>(c) + "'; allowed: [A-Za-z0-9_-]";
        }
    }
    // Reserve MEMORY as the index filename. Case-insensitive to avoid a
    // Windows user creating a memory called "memory" that collides with
    // MEMORY.md on a case-insensitive filesystem.
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lower == "memory") return "memory name 'MEMORY' is reserved for the index file";
    return "";
}

fs::path resolve_memory_entry_path(const std::string& name) {
    return resolve_memory_entry_path_in(get_memory_dir(), name);
}

fs::path resolve_memory_entry_path_in(const fs::path& dir, const std::string& name) {
    if (dir.empty() || !validate_memory_name(name).empty()) return {};
    return dir / (name + ".md");
}

bool is_within_directory(const fs::path& path, const fs::path& dir) {
    if (path.empty() || dir.empty()) return false;
    std::error_code ec;
    fs::path canonical_target = fs::weakly_canonical(path, ec);
    if (ec) canonical_target = path;

    fs::path canonical_dir = fs::weakly_canonical(dir, ec);
    if (ec) canonical_dir = dir;

    // Compare as generic (forward-slash) strings case-insensitively on Windows,
    // case-sensitively elsewhere. A simple lexical prefix check is enough here
    // because weakly_canonical has already resolved .. and symlinks.
    std::string target_s = path_to_utf8_generic(canonical_target);
    std::string dir_s = path_to_utf8_generic(canonical_dir);
    while (!dir_s.empty() && dir_s.back() == '/') dir_s.pop_back();

#ifdef _WIN32
    auto to_lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    target_s = to_lower(target_s);
    dir_s = to_lower(dir_s);
#endif

    if (target_s.size() <= dir_s.size()) return false;
    if (target_s.compare(0, dir_s.size(), dir_s) != 0) return false;
    // Guard against prefix matches like `memory_foo` vs `memory`.
    return target_s[dir_s.size()] == '/';
}

bool is_within_memory_dir(const fs::path& path) {
    return is_within_directory(path, get_memory_dir());
}

} // namespace acecode
