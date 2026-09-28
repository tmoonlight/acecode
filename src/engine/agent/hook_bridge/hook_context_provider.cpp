#include "hook_context_provider.hpp"

#include "agent/boundary/workspace_boundary.hpp"
#include "hooks/hook_runtime.hpp"
#include "llm/llm_provider.hpp"
#include "permissions/permissions.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"

namespace acecode::agent {

std::string HookContextProvider::cwd() const { return boundary_.cwd(); }

HookCommonPayloadFields HookContextProvider::fields(
    const std::string& event_name, SessionManager* session) const {
    HookCommonPayloadFields fields;
    fields.cwd = boundary_.cwd();
    fields.hook_event_name = event_name;
    fields.permission_mode = PermissionManager::mode_name(permissions_.mode());
    if (session) {
        fields.session_id = session->current_session_id();
        if (!fields.session_id.empty()) {
            fields.transcript_path = SessionStorage::session_path(
                SessionStorage::get_project_dir(boundary_.cwd()), fields.session_id);
        }
    }
    if (provider_) {
        auto provider = provider_();
        if (provider) fields.model = provider->model();
    }
    return fields;
}

} // namespace acecode::agent
