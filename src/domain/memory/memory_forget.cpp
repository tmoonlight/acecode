#include "memory_forget.hpp"

#include "memory_inbox.hpp"
#include "memory_service.hpp"

#include "utils/logger.hpp"

#include <algorithm>

namespace acecode {

MemoryForgetResult forget_memory_session(MemoryService& memory, const std::string& session_id,
                                         const std::string& project_dir) {
    MemoryForgetResult result;
    if (session_id.empty()) return result;
    const std::shared_ptr<MemoryRegistry> scopes[] = {
        memory.scope(MemoryScope::Global, project_dir),
        memory.scope(MemoryScope::Workspace, project_dir),
    };
    for (const auto& registry : scopes) {
        if (!registry) continue;
        result.observations_removed += forget_memory_session_observations(registry->dir(), session_id);
        registry->reload();
        for (const auto& entry : registry->list()) {
            if (entry.source != kMemorySourceSummary) continue;  // 手写条目不受影响
            auto sessions = entry.source_sessions;
            const auto it = std::find(sessions.begin(), sessions.end(), session_id);
            if (it == sessions.end()) continue;
            sessions.erase(it);
            std::string error;
            if (sessions.empty()) {
                if (registry->remove(entry.name, error)) {
                    memory.state().add_tombstone(registry->scope_key(), entry.name,
                                                 entry.description, memory_now_ms());
                    ++result.entries_deleted;
                }
            } else {
                MemoryWriteRequest request;
                request.name = entry.name;
                request.type = entry.type;
                request.description = entry.description;
                request.body = entry.body;
                request.mode = MemoryWriteMode::Update;
                request.source = kMemorySourceSummary;
                request.source_sessions = sessions;
                request.replace_source_sessions = true;
                request.now_iso = entry.updated_at;  // 只改来源,不算内容更新
                if (registry->upsert(request, error)) ++result.entries_updated;
            }
            if (!error.empty()) {
                LOG_WARN("[memory] forget " + session_id + " failed on " + entry.name + ": " + error);
            }
        }
    }
    memory.state().forget_extraction(session_id);
    return result;
}

} // namespace acecode
