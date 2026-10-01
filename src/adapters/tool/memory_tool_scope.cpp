#include "memory_tool_scope.hpp"

#include "session/session_manager.hpp"
#include "session/session_storage.hpp"

namespace acecode {

MemoryToolScope resolve_memory_tool_scope(const ToolContext& ctx) {
    MemoryToolScope scope;
    if (ctx.session_manager) {
        scope.session_id = ctx.session_manager->current_session_id();
        if (!ctx.session_manager->is_no_workspace()) {
            scope.project_dir = ctx.session_manager->current_project_dir();
        }
    } else if (!ctx.cwd.empty()) {
        scope.project_dir = SessionStorage::get_project_dir(ctx.cwd);
    }
    if (scope.session_id.empty()) scope.session_id = ctx.session_id;
    return scope;
}

} // namespace acecode
