#pragma once

#include "memory_types.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace acecode {

// Parse a memory entry file (<name>.md). Returns nullopt when frontmatter is
// unusable, a required field is missing, or `type` is unknown. The returned
// MemoryEntry.path is set to `path`, name is derived from filename (not
// frontmatter) so on-disk and in-memory names stay in sync. Provenance fields
// (created_at / updated_at / source / source_sessions) are optional; unknown
// top-level fields are captured verbatim in `extra_frontmatter`.
std::optional<MemoryEntry> parse_memory_entry_file(const std::filesystem::path& path);

// Same as parse_memory_entry_file but over in-memory text (path only names the
// entry). Used by tests and by the consolidation rollback snapshot.
std::optional<MemoryEntry> parse_memory_entry_text(const std::string& content,
                                                   const std::filesystem::path& path);

// Render a memory entry back to disk format: frontmatter + blank line + body.
// Provenance fields are written only when non-empty; `extra_frontmatter` is
// appended unchanged so fields added by users or other tools survive.
std::string render_memory_entry(const MemoryEntry& entry);

// 当前 UTC 时间的 ISO-8601 文本(秒精度,`Z` 结尾)。
std::string memory_now_iso8601();

// 把 ISO-8601(memory_now_iso8601 的格式)解析为距 epoch 的秒;失败返回 nullopt。
std::optional<long long> parse_memory_iso8601(const std::string& text);

// 文件最后修改时间的 ISO-8601 文本;取不到时返回空串。旧条目补 created_at 用。
std::string memory_file_mtime_iso8601(const std::filesystem::path& path);

} // namespace acecode
