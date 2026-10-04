#pragma once

#include "loop_store.hpp"
#include "tool/tool_executor.hpp"

#include <memory>
#include <shared_mutex>

namespace acecode {
struct AppConfig;
namespace desktop { class WorkspaceRegistry; }
}

namespace acecode::loop {

class LoopScheduler;

// Owned by the daemon; borrowed dependencies outlive registry shutdown. Tool
// closures only hold a weak_ptr, and lock it for each synchronous operation.
class ScheduledTaskService {
public:
    using Clock = std::function<std::int64_t()>;
    ScheduledTaskService(LoopStore& store, const AppConfig& config,
                         std::shared_mutex& config_mutex,
                         desktop::WorkspaceRegistry& workspaces,
                         std::string projects_dir,
                         LoopScheduler* scheduler = nullptr, // nullable, borrowed
                         Clock clock = {});

    std::optional<LoopDefinition> prepare(nlohmann::json body, ValidationError& error) const;
    ToolResult create(const LoopDefinition& definition);

private:
    LoopStore& store_;
    const AppConfig& config_;
    std::shared_mutex& config_mutex_;
    desktop::WorkspaceRegistry& workspaces_;
    std::string projects_dir_;
    LoopScheduler* scheduler_; // nullable, borrowed
    Clock clock_;
};

ToolImpl create_scheduled_task_tool(std::weak_ptr<ScheduledTaskService> service);

} // namespace acecode::loop
