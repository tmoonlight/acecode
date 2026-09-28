#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace acecode {
class LlmProvider;
class PermissionManager;
class SessionManager;
struct HookCommonPayloadFields;
}

namespace acecode::agent {
class WorkspaceBoundary;

// Only context queries; it cannot mutate the loop, queue or turn.
class HookContextProvider {
public:
    using ProviderAccessor = std::function<std::shared_ptr<LlmProvider>()>;
    HookContextProvider(WorkspaceBoundary& boundary, PermissionManager& permissions,
                        ProviderAccessor provider)
        : boundary_(boundary), permissions_(permissions), provider_(std::move(provider)) {}
    HookCommonPayloadFields fields(const std::string& event, SessionManager* session) const;
    std::string cwd() const;
private:
    WorkspaceBoundary& boundary_;
    PermissionManager& permissions_;
    ProviderAccessor provider_;
};

} // namespace acecode::agent
