#pragma once

#include "session_host/memory_scheduler.hpp"

#include <shared_mutex>
#include <string>

namespace acecode {
class SessionRegistry;
struct AppConfig;
}

namespace acecode::daemon {

// daemon 的记忆摘要宿主回调:扫描进程 cwd 与活跃会话所在的工作区;忙碌判定走
// 注册表;配置快照在共享锁下复制;/memory flush 完成后经会话 system message 通知。
// 回调只按值持有指针,调用方保证 registry / config 活得比调度器久(停机时先停调度器)。
MemorySchedulerHost make_memory_scheduler_host(SessionRegistry& registry,
                                               const std::string& cwd,
                                               const AppConfig& config,
                                               std::shared_mutex& config_mutex,
                                               const std::string& config_path);

} // namespace acecode::daemon
