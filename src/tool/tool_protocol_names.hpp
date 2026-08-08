#pragma once

#include "../provider/llm_provider.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acecode {

struct ToolProtocolNameMapping {
    std::string_view native_name;
    std::string_view public_name;
};

const std::array<ToolProtocolNameMapping, 4>& model_tool_name_mappings();

std::string model_tool_name_for_native(std::string_view native_name);

std::optional<std::string> native_tool_name_for_public_alias(
    std::string_view public_name);

bool validate_model_tool_name_mappings(std::string* error = nullptr);

bool translate_tool_definitions_for_model(
    const std::vector<ToolDef>& native_definitions,
    std::vector<ToolDef>& model_definitions,
    std::string* error = nullptr);

void rewrite_tool_calls_for_model(ChatMessage& message);
void rewrite_tool_calls_for_model(std::vector<ChatMessage>& messages);

} // namespace acecode
