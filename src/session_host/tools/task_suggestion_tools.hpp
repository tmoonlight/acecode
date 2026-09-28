#pragma once

#include "tool_executor.hpp"

#include <memory>

namespace acecode {

class TaskSuggestionService;

// Host-only offers. There is intentionally no model-facing acceptance tool.
void register_task_suggestion_tools(
    ToolExecutor& tools, std::shared_ptr<TaskSuggestionService> service);

} // namespace acecode
