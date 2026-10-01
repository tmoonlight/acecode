#include "daemon_memory.hpp"

#include "config/config.hpp"
#include "session/session_storage.hpp"
#include "session/system_notice.hpp"
#include "session_host/session_registry.hpp"

#include <algorithm>
#include <mutex>

namespace acecode::daemon {

MemorySchedulerHost make_memory_scheduler_host(SessionRegistry& registry,
                                               const std::string& cwd,
                                               const AppConfig& config,
                                               std::shared_mutex& config_mutex,
                                               const std::string& config_path) {
    MemorySchedulerHost host;
    SessionRegistry* reg = &registry;
    host.project_dirs = [reg, cwd]() {
        std::vector<std::string> dirs = {SessionStorage::get_project_dir(cwd)};
        for (const auto& info : reg->list_active()) {
            if (info.no_workspace || info.cwd.empty()) continue;
            const std::string dir = SessionStorage::get_project_dir(info.cwd);
            if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) dirs.push_back(dir);
        }
        return dirs;
    };
    host.session_busy = [reg](const std::string& session_id) {
        const auto busy = reg->busy_session_ids();
        return std::find(busy.begin(), busy.end(), session_id) != busy.end();
    };
    const AppConfig* cfg = &config;
    std::shared_mutex* mu = &config_mutex;
    host.app_config = [cfg, mu]() {
        std::shared_lock<std::shared_mutex> lock(*mu);
        return *cfg;
    };
    host.config_path = config_path;
    host.notify = [reg](const std::string& session_id, const std::string& text) {
        auto entry = reg->acquire(session_id);
        if (entry && entry->loop) {
            // 异步完成的报告落进会话记录(只给界面看,不进模型上下文),用户切走再回来也看得到。
            entry->loop->emit_transcript_system_message(text, make_system_notice_metadata("memory_flush_done"));
        }
    };
    return host;
}

} // namespace acecode::daemon
