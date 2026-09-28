#include "request_context.hpp"
#include "session/session_manager.hpp"
#include "tool/mtime_tracker.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <utility>

namespace acecode::agent::detail {

std::string build_plan_mode_context_prompt(SessionManager* session_manager,
                                           bool ask_user_allowed,
                                           bool exit_plan_mode_allowed, MtimeTracker& mtime) {
    if (!session_manager) return {};
    const std::string plan_file = session_manager->ensure_plan_file_path();
    if (plan_file.empty()) return {};
    const std::string existing_plan = session_manager->read_plan_file();
    mtime.record_read(plan_file, existing_plan, false);

    std::ostringstream oss;
    oss << "<plan_mode>\n"
        << "Plan mode is active. You MUST NOT make any edits except to the plan file.\n\n"
        << "Plan file path: " << plan_file << "\n"
        << "Plan exists: " << (existing_plan.empty() ? "false" : "true") << "\n\n"
        << "Workflow:\n"
        << "1. Explore the codebase with read-only tools until the approach is clear.\n"
        << "2. Keep the implementation plan in the plan file. Update that file as your plan changes.\n";
    int workflow_step = 3;
    if (ask_user_allowed) {
        oss << workflow_step++
            << ". Use AskUserQuestion only for unresolved requirements or approach choices.\n";
    }
    if (exit_plan_mode_allowed) {
        oss << workflow_step++
            << ". When the plan is complete and unambiguous, call ExitPlanMode for user approval.\n\n";
        if (ask_user_allowed) {
            oss << "Do not ask the user whether the plan is OK with AskUserQuestion; ExitPlanMode is the approval request.\n";
        }
    } else {
        oss << workflow_step
            << ". When the plan is complete, present the result in your final reply.\n";
    }
    oss << "</plan_mode>";
    return oss.str();
}

void append_plan_mode_context_for_api(std::vector<ChatMessage>& messages,
                                      const std::string& context) {
    if (context.empty()) return;
    ChatMessage msg;
    msg.role = "user";
    msg.content = context;
    msg.metadata = nlohmann::json{{"hidden_plan_mode_context", true}};
    messages.push_back(std::move(msg));
}

void append_todo_context_for_api(std::vector<ChatMessage>& messages,
                                 const std::vector<TodoItem>& todos) {
    std::string context = format_todo_injection(todos);
    if (context.empty()) return;
    ChatMessage msg;
    msg.role = "user";
    msg.content = std::move(context);
    msg.metadata = nlohmann::json{{"hidden_todo_context", true}};
    messages.push_back(std::move(msg));
}

void append_request_context_for_api(std::vector<ChatMessage>& messages,
                                    const std::string& context) {
    if (context.empty()) return;

    ChatMessage msg;
    msg.role = "user";
    msg.content = context;
    messages.push_back(std::move(msg));
}

std::string cached_context_for_api(const PromptContextBlock& block,
                                   std::string& cached_key,
                                   std::string& cached_content) {
    if (block.cache_key != cached_key) {
        cached_key = block.cache_key;
        cached_content = block.content;
    }
    return cached_content;
}

} // namespace acecode::agent::detail
