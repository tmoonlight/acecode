#pragma once

#include "tool_executor.hpp"

#include <memory>

namespace acecode {

class ThreadService;

struct ThreadToolDeps {
    std::shared_ptr<ThreadService> service;
};

// Register the one-action-per-tool Codex-compatible thread surface. The deps
// object may be populated after registration, matching daemon/headless startup
// where ToolExecutor is created before SessionRegistry.
void register_codex_thread_tools(
    ToolExecutor& tools,
    std::shared_ptr<ThreadToolDeps> deps);

} // namespace acecode
