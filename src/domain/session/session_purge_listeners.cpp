#include "session_purge_listeners.hpp"

#include "utils/logger.hpp"

#include <map>
#include <mutex>
#include <vector>

namespace acecode {

namespace {

struct ListenerTable {
    std::mutex mu;
    std::uint64_t next_id = 1;
    std::map<std::uint64_t, SessionPurgeListener> listeners;
};

ListenerTable& table() {
    static ListenerTable instance;
    return instance;
}

} // namespace

std::uint64_t add_session_purge_listener(SessionPurgeListener listener) {
    auto& t = table();
    std::lock_guard<std::mutex> lock(t.mu);
    const std::uint64_t id = t.next_id++;
    t.listeners.emplace(id, std::move(listener));
    return id;
}

void remove_session_purge_listener(std::uint64_t id) {
    auto& t = table();
    std::lock_guard<std::mutex> lock(t.mu);
    t.listeners.erase(id);
}

void notify_session_purged(const std::string& project_dir, const std::string& session_id) {
    std::vector<SessionPurgeListener> snapshot;
    {
        auto& t = table();
        std::lock_guard<std::mutex> lock(t.mu);
        for (const auto& [id, listener] : t.listeners) snapshot.push_back(listener);
    }
    // 锁外回调:监听者内部可能再去读写会话或记忆存储。
    for (const auto& listener : snapshot) {
        try {
            if (listener) listener(project_dir, session_id);
        } catch (const std::exception& e) {
            LOG_WARN(std::string("[session] purge listener failed: ") + e.what());
        }
    }
}

} // namespace acecode
