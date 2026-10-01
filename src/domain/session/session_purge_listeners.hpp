#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace acecode {

// 会话被永久删除(SessionStorage::purge_session_files 成功)后的进程内通知。
// 所有「清除」入口(REST ?purge=1、TUI /tasks clear、设置页清理、线程服务)
// 最终都走 purge_session_files,所以记忆摘要的「遗忘」只需挂在这一处。
// 回调在 purge 的调用线程上同步执行,不得抛异常,也不应长时间阻塞。
using SessionPurgeListener =
    std::function<void(const std::string& project_dir, const std::string& session_id)>;

std::uint64_t add_session_purge_listener(SessionPurgeListener listener);
void remove_session_purge_listener(std::uint64_t id);
void notify_session_purged(const std::string& project_dir, const std::string& session_id);

} // namespace acecode
