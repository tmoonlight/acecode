#include "memory_write_tool.hpp"

#include "memory_tool_scope.hpp"

#include "memory/memory_paths.hpp"
#include "memory/memory_service.hpp"
#include "memory/memory_types.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

namespace acecode {

namespace {

MemoryWriteMode parse_mode(const std::string& s) {
    if (s == "create") return MemoryWriteMode::Create;
    if (s == "update") return MemoryWriteMode::Update;
    return MemoryWriteMode::Upsert;
}

ToolResult fail(const std::string& message) {
    nlohmann::json err;
    err["success"] = false;
    err["error"] = message;
    return ToolResult{err.dump(), false};
}

} // namespace

ToolImpl create_memory_write_tool(std::shared_ptr<MemoryService> memory) {
    ToolDef def;
    def.name = "memory_write";
    def.description =
        "Persist a memory entry. Two scopes exist: 'global' for the user's cross-project "
        "preferences and 'workspace' for knowledge about the current workspace. Without "
        "'scope', user/feedback entries go to global and project/reference entries go to "
        "the workspace (sessions without a workspace always write global). Writes are "
        "atomic, the scope's MEMORY.md index is updated automatically, and secrets such as "
        "API keys, tokens and passwords are replaced with [REDACTED] before saving. 'name' "
        "must match [A-Za-z0-9_-] (1-64 chars). 'type' must be one of "
        "user|feedback|project|reference. 'mode' defaults to 'upsert' (also 'create' or "
        "'update').";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"name", {
                {"type", "string"},
                {"description", "File-stem identifier ([A-Za-z0-9_-]{1,64})."}
            }},
            {"type", {
                {"type", "string"},
                {"enum", nlohmann::json::array({"user", "feedback", "project", "reference"})},
                {"description", "Category of the memory."}
            }},
            {"description", {
                {"type", "string"},
                {"description", "One-line description shown in the memory index; put the actionable rule here."}
            }},
            {"body", {
                {"type", "string"},
                {"description", "Markdown body of the memory entry."}
            }},
            {"mode", {
                {"type", "string"},
                {"enum", nlohmann::json::array({"create", "update", "upsert"})},
                {"description", "Write semantics. Default 'upsert' (create-or-replace)."}
            }},
            {"scope", {
                {"type", "string"},
                {"enum", nlohmann::json::array({"global", "workspace"})},
                {"description", "Target scope. Default: global for user/feedback, workspace for project/reference."}
            }}
        }},
        {"required", nlohmann::json::array({"name", "type", "description", "body"})}
    });

    auto execute = [memory](const std::string& arguments_json,
                            const ToolContext& ctx) -> ToolResult {
        std::string name, type_str, description, body, mode_str, scope_str;
        try {
            if (arguments_json.empty()) {
                return ToolResult{"[Error] memory_write requires arguments.", false};
            }
            auto args = nlohmann::json::parse(arguments_json);
            name = args.value("name", "");
            type_str = args.value("type", "");
            description = args.value("description", "");
            body = args.value("body", "");
            mode_str = args.value("mode", "upsert");
            scope_str = args.value("scope", "");
        } catch (...) {
            return ToolResult{"[Error] Failed to parse tool arguments.", false};
        }
        if (!memory || !memory->enabled()) return fail("memory is disabled in settings");

        std::string name_err = validate_memory_name(name);
        if (!name_err.empty()) return fail(name_err);
        auto parsed_type = parse_memory_type(type_str);
        if (!parsed_type.has_value()) {
            return fail("invalid type: " + type_str + " (allowed: user|feedback|project|reference)");
        }
        if (description.empty()) return fail("description must not be empty");

        MemoryScope scope = default_memory_scope_for_type(*parsed_type);
        if (!scope_str.empty()) {
            auto parsed_scope = parse_memory_scope(scope_str);
            if (!parsed_scope) return fail("invalid scope: " + scope_str + " (allowed: global|workspace)");
            scope = *parsed_scope;
        }
        const MemoryToolScope where = resolve_memory_tool_scope(ctx);
        std::string notice;
        if (scope == MemoryScope::Workspace && where.project_dir.empty()) {
            scope = MemoryScope::Global;
            notice = "This session has no workspace, so the entry was saved to the global scope.";
        }
        auto registry = memory->scope(scope, where.project_dir);
        if (!registry) return fail("memory scope is unavailable");

        MemoryWriteRequest request;
        request.name = name;
        request.type = *parsed_type;
        request.description = description;
        request.body = body;
        request.mode = parse_mode(mode_str);
        request.source = kMemorySourceManual;
        if (!where.session_id.empty()) request.source_sessions.push_back(where.session_id);

        std::string err_msg;
        MemoryWriteOutcome outcome;
        auto written = registry->upsert(request, err_msg, &outcome);
        if (!written) return fail(err_msg);

        nlohmann::json out;
        out["success"] = true;
        out["scope"] = memory_scope_to_string(scope);
        out["name"] = written->name;
        out["description"] = written->description;
        out["type"] = memory_type_to_string(written->type);
        out["created"] = outcome.created;
        out["path"] = path_to_utf8_generic(written->path);
        if (outcome.redactions > 0) {
            out["redactions"] = outcome.redactions;
            const std::string redaction_notice =
                std::to_string(outcome.redactions) +
                " secret value(s) were replaced with [REDACTED] before saving.";
            notice = notice.empty() ? redaction_notice : notice + " " + redaction_notice;
        }
        if (!notice.empty()) out["notice"] = notice;
        LOG_INFO("[memory_write] persisted " + path_to_utf8_generic(written->path));
        return ToolResult{out.dump(), true};
    };

    return ToolImpl{def, execute, /*is_read_only=*/false};
}

} // namespace acecode
