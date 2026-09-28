#pragma once

#include "session/thread_goal_store.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace acecode::agent::detail {

std::string escape_xml_text(const std::string& input);

std::string format_goal_status_chip(const ThreadGoal& goal);

struct GoalPromptTools {
    bool update_goal = false;
    bool ask_user_question = false;
};

std::string build_goal_context_prompt(const ThreadGoal& goal, GoalPromptTools tools);
std::string build_goal_budget_limit_prompt(const ThreadGoal& goal, GoalPromptTools tools);
std::string build_goal_objective_updated_prompt(const ThreadGoal& goal, GoalPromptTools tools);

} // namespace acecode::agent::detail
