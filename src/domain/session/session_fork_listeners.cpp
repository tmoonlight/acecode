#include "session_fork_listeners.hpp"

#include "utils/logger.hpp"

#include <map>
#include <mutex>
#include <vector>

namespace acecode {

namespace {

struct ListenerTable {
    std::mutex mu;
    std::uint64_t next_id = 1;
    std::map<std::uint64_t, SessionForkListener> listeners;
};

ListenerTable& table() {
    static ListenerTable instance;
    return instance;
}

} // namespace

std::uint64_t add_session_fork_listener(SessionForkListener listener) {
    auto& t = table();
    std::lock_guard<std::mutex> lock(t.mu);
    const std::uint64_t id = t.next_id++;
    t.listeners.emplace(id, std::move(listener));
    return id;
}

void remove_session_fork_listener(std::uint64_t id) {
    auto& t = table();
    std::lock_guard<std::mutex> lock(t.mu);
    t.listeners.erase(id);
}

void notify_session_forked(const std::string& project_dir,
                           const std::string& new_session_id,
                           const std::string& forked_from_id) {
    std::vector<SessionForkListener> snapshot;
    {
        auto& t = table();
        std::lock_guard<std::mutex> lock(t.mu);
        for (const auto& [id, listener] : t.listeners) snapshot.push_back(listener);
    }
    for (const auto& listener : snapshot) {
        try {
            if (listener) listener(project_dir, new_session_id, forked_from_id);
        } catch (const std::exception& e) {
            LOG_WARN(std::string("[session] fork listener failed: ") + e.what());
        }
    }
}

} // namespace acecode
