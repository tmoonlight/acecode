#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace acecode {

// 会话分叉成功(SessionManager::fork_session_to_new_id 写好新会话)后的进程内通知。
// Web 分叉路由与线程服务分叉都走 fork_session_to_new_id,所以只挂在这一处,与
// session_purge_listeners 对称。用途:Agent Browser 按会话存储的 cookie 复制
// (openspec add-agent-browser-cookie-modes D7)。
//
// 回调在分叉的调用线程上同步执行,且仍持有源会话 SessionManager 的锁:监听者不得
// 回调 SessionManager,不得抛异常,也不应长时间阻塞(写一下状态文件即可)。
using SessionForkListener = std::function<void(const std::string& project_dir,
                                               const std::string& new_session_id,
                                               const std::string& forked_from_id)>;

std::uint64_t add_session_fork_listener(SessionForkListener listener);
void remove_session_fork_listener(std::uint64_t id);
void notify_session_forked(const std::string& project_dir,
                           const std::string& new_session_id,
                           const std::string& forked_from_id);

} // namespace acecode
