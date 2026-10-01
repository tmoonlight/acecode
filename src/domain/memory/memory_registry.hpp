#pragma once

#include "memory_types.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

class MemoryStateStore;

// Write semantics for upsert().
enum class MemoryWriteMode {
    Create, // fail if an entry with `name` already exists
    Update, // fail if no entry with `name` exists
    Upsert, // create-or-replace (default)
};

// 一次条目写入。source / session 用于来源记录;now_iso 为空时取当前时间。
struct MemoryWriteRequest {
    std::string name;
    MemoryType type = MemoryType::User;
    std::string description;
    std::string body;
    MemoryWriteMode mode = MemoryWriteMode::Upsert;
    std::string source = kMemorySourceManual;
    // 追加到 source_sessions 的会话(去重);replace_source_sessions=true 时整体替换。
    std::vector<std::string> source_sessions;
    bool replace_source_sessions = false;
    std::string now_iso;
};

struct MemoryWriteOutcome {
    bool created = false;     // 新建(而不是改写已有条目)
    int redactions = 0;       // 写入前脱敏替换的处数
};

// Thread-safe cache over one memory scope directory (global <data_dir>/memory/
// or a workspace <project_dir>/memory/). Only top-level <name>.md files are
// entries; MEMORY.md is the generated index; inbox/ and archive/ belong to
// memory summarization and every other subdirectory is ignored.
//
// Mutations rescan the directory first (another process may have written in
// the meantime) and, when a MemoryStateStore is attached, run inside its
// cross-process write lock so two processes cannot interleave scan + write.
class MemoryRegistry {
public:
    // Global scope (get_memory_dir()), kept for existing callers and tests.
    MemoryRegistry();
    explicit MemoryRegistry(std::filesystem::path dir,
                            std::string scope_key = "global",
                            MemoryStateStore* state = nullptr);

    const std::filesystem::path& dir() const { return dir_; }
    const std::string& scope_key() const { return scope_key_; }
    MemoryStateStore* state_store() const { return state_; }

    // Scan disk now. Entries with invalid frontmatter / unreadable files are
    // skipped with LOG_WARN so one bad entry doesn't kill the whole cache.
    void scan();

    // Equivalent to scan(); named for /memory reload clarity and command help.
    void reload() { scan(); }

    // Return a copy of the current entry list, optionally filtered by type.
    std::vector<MemoryEntry> list(std::optional<MemoryType> type_filter = std::nullopt) const;

    // Look up by filesystem stem (the part before ".md"). Returns nullopt
    // when no entry by that name exists.
    std::optional<MemoryEntry> find(const std::string& name) const;

    // Read MEMORY.md raw. Returns empty string when the file doesn't exist or
    // is zero-length. `max_bytes` truncates the returned copy with a marker.
    std::string read_index_raw(std::size_t max_bytes) const;

    // Legacy upsert: manual source, no session provenance.
    std::optional<MemoryEntry> upsert(const std::string& name,
                                      MemoryType type,
                                      const std::string& description,
                                      const std::string& body,
                                      MemoryWriteMode mode,
                                      std::string& error_out);

    // Upsert with provenance. Description and body are redacted before they
    // reach disk; `outcome` reports how many secrets were replaced. Legacy
    // entries get created_at from the file's previous modification time; unknown
    // frontmatter fields are preserved.
    std::optional<MemoryEntry> upsert(const MemoryWriteRequest& request,
                                      std::string& error_out,
                                      MemoryWriteOutcome* outcome = nullptr);

    // Remove an entry. Returns true when the file existed and was deleted.
    // Removes the matching MEMORY.md line as a side effect.
    bool remove(const std::string& name, std::string& error_out);

    // Delete every entry, the index, inbox/ and archive/ of this scope. Other
    // files (the global scope's state.sqlite3, foreign subdirectories) stay.
    bool reset(std::string& error_out);

    // Rebuild MEMORY.md from the entries currently on disk.
    void rebuild_index();

    // Entry count, safe for logging / /memory list.
    std::size_t size() const;

private:
    // Non-locking helpers — callers must already hold mu_.
    void scan_locked();
    std::vector<MemoryEntry>::iterator find_locked(const std::string& name);
    void rewrite_index_locked();
    std::optional<MemoryEntry> upsert_locked(const MemoryWriteRequest& request,
                                             std::string& error_out,
                                             MemoryWriteOutcome* outcome);
    bool remove_locked(const std::string& name, std::string& error_out);

    std::filesystem::path dir_;
    std::string scope_key_;
    MemoryStateStore* state_ = nullptr;  // 可空;构造时注入,生命周期长于本对象
    mutable std::mutex mu_;
    std::vector<MemoryEntry> entries_;
};

} // namespace acecode
