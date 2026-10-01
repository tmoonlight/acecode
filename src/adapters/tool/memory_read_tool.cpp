#include "memory_read_tool.hpp"

#include "memory_tool_scope.hpp"

#include "memory/memory_service.hpp"
#include "memory/memory_types.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace acecode {

namespace {

struct ScopedEntry {
    MemoryScope scope = MemoryScope::Global;
    MemoryEntry entry;
};

std::string ascii_lower(std::string text) {
    for (auto& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

bool is_utf8_continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// 命中位置前后各约 60 字节的片段,按 UTF-8 字符边界对齐,换行折成空格。
std::string snippet_around(const std::string& text, std::size_t pos, std::size_t len) {
    std::size_t start = pos > 60 ? pos - 60 : 0;
    std::size_t end = std::min(text.size(), pos + len + 60);
    while (start > 0 && start < text.size() && is_utf8_continuation(text[start])) --start;
    while (end < text.size() && is_utf8_continuation(text[end])) ++end;
    std::string out = text.substr(start, end - start);
    for (auto& c : out) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    if (start > 0) out = "..." + out;
    if (end < text.size()) out += "...";
    return out;
}

nlohmann::json summary_json(const ScopedEntry& item) {
    return {
        {"scope", memory_scope_to_string(item.scope)},
        {"name", item.entry.name},
        {"description", item.entry.description},
        {"type", memory_type_to_string(item.entry.type)},
        {"updated_at", item.entry.updated_at},
    };
}

nlohmann::json error_json(const std::string& message) {
    return {{"success", false}, {"error", message}};
}

} // namespace

ToolImpl create_memory_read_tool(std::shared_ptr<MemoryService> memory) {
    ToolDef def;
    def.name = "memory_read";
    def.description =
        "Read persistent memory. Two scopes exist: 'global' (the user's cross-project "
        "preferences) and 'workspace' (memory for the current workspace). With no "
        "arguments, lists the entries of both scopes (scope, name, description, type, "
        "updated_at). 'scope' limits the listing to global, workspace or all (default). "
        "'type' filters by user|feedback|project|reference. 'name' returns one entry's "
        "full body, checking the workspace scope before the global scope unless 'scope' "
        "is given. 'query' does a case-insensitive substring search over names, "
        "descriptions and bodies and returns matching entries with a snippet. Missing "
        "entries return {found:false} rather than erroring. Read-only.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"scope", {
                {"type", "string"},
                {"enum", nlohmann::json::array({"global", "workspace", "all"})},
                {"description", "Which scope to read. Default 'all'."}
            }},
            {"name", {
                {"type", "string"},
                {"description", "Memory entry file stem (the <name> in <name>.md)."}
            }},
            {"type", {
                {"type", "string"},
                {"enum", nlohmann::json::array({"user", "feedback", "project", "reference"})},
                {"description", "Filter entries by type when 'name' is absent."}
            }},
            {"query", {
                {"type", "string"},
                {"description", "Case-insensitive text to search for in names, descriptions and bodies."}
            }}
        }},
        {"required", nlohmann::json::array()}
    });

    auto execute = [memory](const std::string& arguments_json,
                            const ToolContext& ctx) -> ToolResult {
        std::string scope_str, name, type_str, query;
        try {
            if (!arguments_json.empty()) {
                auto args = nlohmann::json::parse(arguments_json);
                scope_str = args.value("scope", "");
                name = args.value("name", "");
                type_str = args.value("type", "");
                query = args.value("query", "");
            }
        } catch (...) {
            return ToolResult{"[Error] Failed to parse tool arguments.", false};
        }
        if (!memory || !memory->enabled()) {
            return ToolResult{error_json("memory is disabled in settings").dump(), false};
        }

        std::vector<MemoryScope> scopes;
        if (scope_str.empty() || scope_str == "all") {
            scopes = {MemoryScope::Workspace, MemoryScope::Global};
        } else if (auto parsed = parse_memory_scope(scope_str)) {
            scopes = {*parsed};
        } else {
            return ToolResult{error_json("invalid scope: " + scope_str +
                                         " (allowed: global|workspace|all)").dump(), false};
        }
        std::optional<MemoryType> type_filter;
        if (!type_str.empty()) {
            type_filter = parse_memory_type(type_str);
            if (!type_filter) {
                return ToolResult{error_json("invalid type: " + type_str +
                                             " (allowed: user|feedback|project|reference)").dump(),
                                  false};
            }
        }

        // 另一个进程(别的工作区的 daemon、TUI)可能刚写过新条目;目录里只有
        // 几个小文件,每次读前重扫的代价可以忽略。
        const MemoryToolScope where = resolve_memory_tool_scope(ctx);
        std::vector<ScopedEntry> entries;
        for (MemoryScope scope : scopes) {
            auto registry = memory->scope(scope, where.project_dir);
            if (!registry) continue;
            registry->reload();
            for (auto& entry : registry->list(type_filter)) {
                entries.push_back({scope, std::move(entry)});
            }
        }

        nlohmann::json out;
        if (!name.empty()) {
            for (const auto& item : entries) {
                if (item.entry.name != name) continue;
                out = summary_json(item);
                out["found"] = true;
                out["created_at"] = item.entry.created_at;
                out["source"] = item.entry.source;
                out["source_sessions"] = item.entry.source_sessions;
                out["body"] = item.entry.body;
                out["path"] = path_to_utf8_generic(item.entry.path);
                return ToolResult{out.dump(), true};
            }
            out["found"] = false;
            out["name"] = name;
            out["hint"] = "Use memory_read() with no args to list available entries.";
            LOG_DEBUG("[memory_read] entry not found: " + name);
            return ToolResult{out.dump(), true};
        }

        nlohmann::json arr = nlohmann::json::array();
        const std::string needle = ascii_lower(query);
        for (const auto& item : entries) {
            if (needle.empty()) {
                arr.push_back(summary_json(item));
                continue;
            }
            const std::string fields[] = {item.entry.name, item.entry.description, item.entry.body};
            for (const auto& field : fields) {
                const std::size_t pos = ascii_lower(field).find(needle);
                if (pos == std::string::npos) continue;
                auto hit = summary_json(item);
                hit["snippet"] = snippet_around(field, pos, needle.size());
                arr.push_back(std::move(hit));
                break;
            }
        }
        out["success"] = true;
        out["workspace_available"] = !where.project_dir.empty();
        if (!query.empty()) out["query"] = query;
        out["count"] = arr.size();
        out["entries"] = std::move(arr);
        if (out["count"].get<std::size_t>() == 0) {
            out["message"] = !query.empty() ? "No memory entries match '" + query + "'."
                : type_filter ? "No memory entries of type '" + type_str + "'."
                : "No memory entries yet. Use memory_write to create one.";
        }
        return ToolResult{out.dump(), true};
    };

    return ToolImpl{def, execute, /*is_read_only=*/true};
}

} // namespace acecode
