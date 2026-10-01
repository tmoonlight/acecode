#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

// Four memory categories mirroring Claude Code semantics. Saved verbatim as the
// frontmatter `type` field of each entry file.
enum class MemoryType {
    User = 0,      // persistent facts about the user (role, preferences)
    Feedback = 1,  // collaboration style / dos & donts the user has given us
    Project = 2,   // ongoing work context not derivable from the codebase
    Reference = 3, // pointers to external systems (dashboards, Linear projects)
};

// Convert between MemoryType and its wire/frontmatter string form.
// parse_memory_type returns nullopt for unknown values; callers must decide
// whether to skip the entry or log a warning.
std::string memory_type_to_string(MemoryType t);
std::optional<MemoryType> parse_memory_type(const std::string& s);

// 记忆作用域:全局(跨项目的个人偏好,<data_dir>/memory/)与工作区
// (<data_dir>/projects/<hash>/memory/,hash 与该工作区会话存储同源)。
enum class MemoryScope {
    Global = 0,
    Workspace = 1,
};

std::string memory_scope_to_string(MemoryScope s);
std::optional<MemoryScope> parse_memory_scope(const std::string& s);

// 未显式指定作用域时按类型推断:个人偏好与协作反馈跟人走(全局),
// 项目背景与外部指针跟工作区走。没有所属工作区的会话一律落全局。
MemoryScope default_memory_scope_for_type(MemoryType type);

// 条目来源:手写(模型显式 memory_write、用户在设置页编辑)或记忆摘要整合生成。
inline constexpr const char* kMemorySourceManual = "manual";
inline constexpr const char* kMemorySourceSummary = "summary";

// One memory entry loaded from a file under a scope directory (<dir>/<name>.md).
// `body` is the post-frontmatter markdown; `path` is the absolute on-disk path
// so tools/commands can report it to the user.
struct MemoryEntry {
    std::string name;
    std::string description;
    MemoryType type = MemoryType::User;
    std::filesystem::path path;
    std::string body;
    // 系统维护的来源字段(UTC ISO-8601)。旧条目可能为空,下一次被系统写入时补齐。
    std::string created_at;
    std::string updated_at;
    std::string source;                       // manual | summary | 空(旧条目)
    std::vector<std::string> source_sessions; // 来源会话 id
    // frontmatter 里未知字段的原始文本(逐行、带换行);写回时原样输出,
    // 让用户或其它工具加的字段不会被 ACECode 的写入抹掉。
    std::string extra_frontmatter;
};

} // namespace acecode
