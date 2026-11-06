#pragma once

#include "loop_types.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace acecode {
class SessionRegistry;
class SessionClient;
}

namespace acecode::loop {

enum class SessionCleanupMode { Off, Archive, Delete };

SessionCleanupMode session_cleanup_mode(const LoopDefinition& loop);

// 一个执行会话在清理时的状态。visible = 侧栏里看得到:存在、未归档、未置顶。
struct LoopSessionState {
    std::string session_id;
    bool visible = false;
    bool busy = false;
};

// newest_first 按运行先后从新到旧。前 keep 个可见会话保留(在跑的也占一个
// 名额),之后可见且不在跑的会话返回给调用方归档或删除;不可见的不计数也不动。
std::vector<std::string> select_old_loop_sessions(
    const std::vector<LoopSessionState>& newest_first, int keep);

// 执行会话 meta 的存放位置。工作区任务的会话都在工作区项目目录(进 worktree
// 不改存储位置);无工作区任务每个会话有自己的缓存目录,置顶清单在缓存根。
struct LoopSessionStorage {
    std::function<std::string(const std::string& session_id)> project_dir;
    std::filesystem::path pinned_sessions_path;
};

LoopSessionStorage loop_session_storage(const LoopDefinition& loop,
                                        const std::string& no_workspace_cache_root);

struct LoopSessionCleanupResult {
    std::vector<std::string> archived;
    std::vector<std::string> deleted;
};

// 新一轮运行开始后调用:只处理这个任务运行记录里的会话,按 loop 的开关把超出
// keep_recent_sessions 的旧会话归档或连同子会话永久删除。开关都关时什么都不做。
LoopSessionCleanupResult cleanup_old_loop_sessions(
    const LoopDefinition& loop,
    const std::vector<std::string>& session_ids_newest_first,
    const LoopSessionStorage& storage,
    SessionRegistry& registry,
    SessionClient& client);

} // namespace acecode::loop
