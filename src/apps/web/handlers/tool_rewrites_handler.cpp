#include "tool_rewrites_handler.hpp"

#include <algorithm>

namespace acecode::web {
using nlohmann::json;

namespace {

json mappings_to_object(const ToolProtocolNameMappings& mappings) {
    json object = json::object();
    for (const auto& mapping : mappings) {
        object[mapping.native_name] = mapping.public_name;
    }
    return object;
}

std::vector<std::string> registered_names(
    const std::vector<RegisteredToolInfo>& registered_tools) {
    std::vector<std::string> names;
    names.reserve(registered_tools.size());
    for (const auto& tool : registered_tools) names.push_back(tool.definition.name);
    return names;
}

} // namespace

json tool_rewrites_snapshot(
    const tool_rewrites::ToolRewriteSettings& settings,
    const std::vector<RegisteredToolInfo>& registered_tools,
    const std::string& path,
    const std::string& load_warning) {
    std::vector<const RegisteredToolInfo*> builtin;
    for (const auto& tool : registered_tools) {
        if (tool.source == ToolSource::Builtin) builtin.push_back(&tool);
    }
    std::sort(builtin.begin(), builtin.end(),
              [](const RegisteredToolInfo* a, const RegisteredToolInfo* b) {
                  return a->definition.name < b->definition.name;
              });

    json tools = json::array();
    for (const auto* tool : builtin) {
        tools.push_back({
            {"name", tool->definition.name},
            {"description", tool->definition.description},
            {"read_only", tool->is_read_only},
        });
    }

    json snapshot{
        {"enabled", settings.enabled},
        {"rewrites", mappings_to_object(settings.rewrites)},
        {"defaults", mappings_to_object(default_model_tool_name_mappings())},
        {"tools", std::move(tools)},
        {"path", path},
    };
    if (!load_warning.empty()) snapshot["warning"] = load_warning;
    return snapshot;
}

bool parse_tool_rewrites_request(
    const json& body,
    const std::vector<RegisteredToolInfo>& registered_tools,
    tool_rewrites::ToolRewriteSettings& out,
    std::string& error) {
    if (!body.is_object()) {
        error = "request body must be an object";
        return false;
    }
    // 复用文件解析器:PUT body 与磁盘格式同构,校验规则只维护一份。
    tool_rewrites::ToolRewriteSettings parsed;
    if (!tool_rewrites::parse_settings(body.dump(), parsed, &error)) return false;
    if (!body.contains("enabled")) {
        error = "'enabled' is required";
        return false;
    }
    if (!tool_rewrites::validate_settings_against_tools(
            parsed, registered_names(registered_tools), &error)) {
        return false;
    }
    out = std::move(parsed);
    error.clear();
    return true;
}

} // namespace acecode::web
