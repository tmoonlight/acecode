#include "tool_protocol_names.hpp"

#include <unordered_set>

namespace acecode {

namespace {

constexpr std::array<ToolProtocolNameMapping, 4> kModelToolNameMappings{{
    {"file_write", "write"},
    {"file_edit", "edit"},
    {"file_read", "read"},
    {"TodoWrite", "todowrite"},
}};

void set_error(std::string* error, const std::string& value) {
    if (error) *error = value;
}

void rewrite_tool_call_for_model(nlohmann::json& tool_call) {
    if (!tool_call.is_object()) return;
    auto function_it = tool_call.find("function");
    if (function_it == tool_call.end() || !function_it->is_object()) return;
    auto name_it = function_it->find("name");
    if (name_it == function_it->end() || !name_it->is_string()) return;
    *name_it = model_tool_name_for_native(name_it->get<std::string>());
}

} // namespace

const std::array<ToolProtocolNameMapping, 4>& model_tool_name_mappings() {
    return kModelToolNameMappings;
}

std::string model_tool_name_for_native(std::string_view native_name) {
    for (const auto& mapping : kModelToolNameMappings) {
        if (mapping.native_name == native_name) {
            return std::string(mapping.public_name);
        }
    }
    return std::string(native_name);
}

std::optional<std::string> native_tool_name_for_public_alias(
    std::string_view public_name) {
    for (const auto& mapping : kModelToolNameMappings) {
        if (mapping.public_name == public_name) {
            return std::string(mapping.native_name);
        }
    }
    return std::nullopt;
}

bool validate_model_tool_name_mappings(std::string* error) {
    std::unordered_set<std::string> native_names;
    std::unordered_set<std::string> public_names;
    for (const auto& mapping : kModelToolNameMappings) {
        if (mapping.native_name.empty() || mapping.public_name.empty()) {
            set_error(error, "tool protocol names must not be empty");
            return false;
        }
        if (mapping.native_name == mapping.public_name) {
            set_error(error, "tool protocol mapping must change the native name '" +
                                 std::string(mapping.native_name) + "'");
            return false;
        }
        if (!native_names.emplace(mapping.native_name).second) {
            set_error(error, "duplicate native tool protocol name '" +
                                 std::string(mapping.native_name) + "'");
            return false;
        }
        if (!public_names.emplace(mapping.public_name).second) {
            set_error(error, "duplicate public tool protocol name '" +
                                 std::string(mapping.public_name) + "'");
            return false;
        }
    }

    for (const auto& mapping : kModelToolNameMappings) {
        if (native_names.count(std::string(mapping.public_name)) != 0) {
            set_error(error, "public tool protocol name '" +
                                 std::string(mapping.public_name) +
                                 "' collides with a mapped native name");
            return false;
        }
    }

    if (error) error->clear();
    return true;
}

bool translate_tool_definitions_for_model(
    const std::vector<ToolDef>& native_definitions,
    std::vector<ToolDef>& model_definitions,
    std::string* error) {
    if (!validate_model_tool_name_mappings(error)) return false;

    std::vector<ToolDef> translated;
    translated.reserve(native_definitions.size());
    std::unordered_set<std::string> emitted_names;
    for (const auto& native_definition : native_definitions) {
        ToolDef definition = native_definition;
        definition.name = model_tool_name_for_native(native_definition.name);
        if (!emitted_names.emplace(definition.name).second) {
            set_error(error, "duplicate model-facing tool name '" +
                                 definition.name + "'");
            return false;
        }
        translated.push_back(std::move(definition));
    }

    model_definitions = std::move(translated);
    if (error) error->clear();
    return true;
}

void rewrite_tool_calls_for_model(ChatMessage& message) {
    if (message.tool_calls.is_array()) {
        for (auto& tool_call : message.tool_calls) {
            rewrite_tool_call_for_model(tool_call);
        }
    } else if (message.tool_calls.is_object()) {
        rewrite_tool_call_for_model(message.tool_calls);
    }
}

void rewrite_tool_calls_for_model(std::vector<ChatMessage>& messages) {
    for (auto& message : messages) {
        rewrite_tool_calls_for_model(message);
    }
}

} // namespace acecode
