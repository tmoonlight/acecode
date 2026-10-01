#pragma once

#include <filesystem>
#include <string>

namespace acecode {

// Absolute path to the global scope directory <data_dir>/memory/
// (computed from get_acecode_dir()). Does NOT create the directory.
std::filesystem::path get_memory_dir();

// Absolute path to the global scope index <data_dir>/memory/MEMORY.md.
std::filesystem::path get_memory_index_path();

// 所有进程共用的记忆状态库 <data_dir>/memory/state.sqlite3。
std::filesystem::path get_memory_state_db_path();

// 工作区作用域目录 = 会话项目目录(<data_dir>/projects/<hash>)下的 memory/。
// project_dir 是 UTF-8 字符串(与 SessionManager::current_project_dir 同源)。
std::filesystem::path workspace_memory_dir(const std::string& project_dir_utf8);

// 作用域目录下的保留子目录:待整合观察与已整合观察。
std::filesystem::path memory_inbox_dir(const std::filesystem::path& scope_dir);
std::filesystem::path memory_archive_dir(const std::filesystem::path& scope_dir);

// Reject names that would escape the memory directory or include separators.
// Returns empty string on success; otherwise a rejection reason.
std::string validate_memory_name(const std::string& name);

// Resolve <name>.md inside the global memory directory. Returns the absolute
// path on success; empty path for an invalid name.
std::filesystem::path resolve_memory_entry_path(const std::string& name);

// Resolve <name>.md inside an arbitrary scope directory.
std::filesystem::path resolve_memory_entry_path_in(const std::filesystem::path& dir,
                                                   const std::string& name);

// True iff `path` resolves (after weakly_canonical, so symlinks and `..` are
// followed) to an entry strictly inside `dir`.
bool is_within_directory(const std::filesystem::path& path,
                         const std::filesystem::path& dir);

// True iff `path` resolves to a file strictly inside get_memory_dir().
bool is_within_memory_dir(const std::filesystem::path& path);

} // namespace acecode
