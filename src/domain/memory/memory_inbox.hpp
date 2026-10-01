#pragma once

#include "memory_types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

// 记忆摘要的「观察」:提炼阶段从一段会话里得到的一条候选记忆,先进作用域的
// inbox/,攒批整合后整体移进 archive/<日期>/。
struct MemoryObservation {
    std::string id;          // <文件名>#<序号>,整合计划用它引用依据
    MemoryType type = MemoryType::User;
    std::string title;
    std::string statement;
};

// 一次提炼在一个作用域里产出的观察,存成一个文件:
// inbox/<session_id>-<from>-<to>.json。同一范围重放会覆盖同名文件,天然幂等。
struct MemoryObservationFile {
    std::filesystem::path path;
    std::string session_id;
    std::int64_t from = 0;   // 消息范围 [from, to)
    std::int64_t to = 0;
    std::string created_at;  // UTC ISO-8601
    std::string model;       // 提炼用的模型(只作展示)
    std::vector<MemoryObservation> observations;
};

std::string memory_observation_file_stem(const std::string& session_id,
                                         std::int64_t from, std::int64_t to);

// 原子写入 inbox/<stem>.json;观察 id 按文件名与序号重新编号。
bool write_memory_observation_file(const std::filesystem::path& scope_dir,
                                   MemoryObservationFile& file,
                                   std::string* error = nullptr);

// 删除某会话某范围的观察文件(重放前清掉上次可能残留的另一作用域文件)。
void remove_memory_observation_file(const std::filesystem::path& scope_dir,
                                    const std::string& session_id,
                                    std::int64_t from, std::int64_t to);

// inbox 里的全部观察文件,按 created_at、文件名升序(最早的在前)。
std::vector<MemoryObservationFile> list_memory_inbox(const std::filesystem::path& scope_dir);

std::size_t count_memory_inbox_observations(const std::filesystem::path& scope_dir);

// 把一批观察文件移进 archive/<date>/(date 形如 2026-10-01)。已不在 inbox 的
// 文件视为已归档(崩溃后重放幂等)。
bool archive_memory_observation_files(const std::filesystem::path& scope_dir,
                                      const std::vector<std::filesystem::path>& files,
                                      const std::string& date,
                                      std::string* error = nullptr);

// 删除某会话在 inbox 与 archive 中的全部观察,返回删除的观察条数。
int forget_memory_session_observations(const std::filesystem::path& scope_dir,
                                       const std::string& session_id);

// 删除 archive/ 下早于 keep_days 天的日期目录,返回删除的目录数。
int cleanup_memory_archive(const std::filesystem::path& scope_dir,
                           std::int64_t now_seconds, int keep_days = 30);

} // namespace acecode
