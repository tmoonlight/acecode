#pragma once

#include "llm/llm_provider.hpp"
#include "prompt/system_prompt.hpp"
#include "session/todo_state.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace acecode { class SessionManager; struct CompactResult; }

namespace acecode::agent::detail {

std::string build_plan_mode_context_prompt(SessionManager* session_manager,
                                           bool ask_user_allowed,
                                           bool exit_plan_mode_allowed);

void append_plan_mode_context_for_api(std::vector<ChatMessage>& messages,
                                      const std::string& context);

void append_todo_context_for_api(std::vector<ChatMessage>& messages,
                                 const std::vector<TodoItem>& todos);

void append_request_context_for_api(std::vector<ChatMessage>& messages,
                                    const std::string& context);

std::string cached_context_for_api(const PromptContextBlock& block,
                                   std::string& cached_key,
                                   std::string& cached_content);

} // namespace acecode::agent::detail
