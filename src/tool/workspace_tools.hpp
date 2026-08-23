#pragma once

#include "tool_executor.hpp"

#include <memory>
#include <string>

namespace acecode::desktop {
class WorkspaceRegistry;
}

namespace acecode {

struct WorkspaceToolDeps {
    desktop::WorkspaceRegistry* registry = nullptr;
    std::string projects_dir;
};

// Register model-visible workspace operations. The dependency object may be
// populated after registration so capability listing can expose the same tool
// surface without constructing a runtime workspace registry.
void register_workspace_tools(
    ToolExecutor& tools,
    std::shared_ptr<WorkspaceToolDeps> deps);

} // namespace acecode
