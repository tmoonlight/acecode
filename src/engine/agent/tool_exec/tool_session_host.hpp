#pragma once
#include <string>
#include <vector>

namespace acecode::agent {

// Workspace operations needed by tools and approval, independent of AgentLoop.
class ToolSessionHost {
public:
    virtual ~ToolSessionHost() = default;
    virtual std::string cwd() const = 0;
    virtual std::string write_root() const = 0;
    virtual std::vector<std::string> writable_workspace_folders() const = 0;
    virtual bool path_in_workspace_folders(const std::string& path) const = 0;
    virtual void switch_cwd(const std::string& cwd) = 0;
};

} // namespace acecode::agent
