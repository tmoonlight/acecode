#pragma once
#include "llm/llm_provider.hpp"
#include <string>
#include <vector>
namespace acecode {
std::string attachment_name_from_json(const nlohmann::json& attachment);
std::string display_prompt_with_attachments(const std::string& prompt,
    const std::vector<nlohmann::json>& attachments);
UserInput build_user_input_with_attachments(const std::string& prompt,
    const std::string& display_text, const std::vector<nlohmann::json>& attachments);
}
