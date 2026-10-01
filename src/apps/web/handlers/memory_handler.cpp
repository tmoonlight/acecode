#include "web/handlers/memory_handler.hpp"

#include "utils/utf8_path.hpp"

namespace acecode::web {

using nlohmann::json;

json memory_settings_json(const MemoryConfig& cfg, bool summary_available) {
    return {
        {"enabled", cfg.enabled},
        {"max_index_bytes", cfg.max_index_bytes},
        {"summary", {
            {"enabled", cfg.summary.enabled},
            {"model_name", cfg.summary.model_name},
            {"idle_minutes", cfg.summary.idle_minutes},
            {"max_session_age_days", cfg.summary.max_session_age_days},
        }},
        {"summary_available", summary_available},
    };
}

bool parse_memory_settings_request(const json& body, const MemoryConfig& current,
                                   MemoryConfig& out, std::string& error) {
    if (!body.is_object()) {
        error = "request body must be a JSON object";
        return false;
    }
    MemoryConfig next = current;
    if (body.contains("enabled")) {
        if (!body["enabled"].is_boolean()) {
            error = "enabled must be a boolean";
            return false;
        }
        next.enabled = body["enabled"].get<bool>();
    }
    if (body.contains("max_index_bytes")) {
        if (!body["max_index_bytes"].is_number_integer() || body["max_index_bytes"].get<long long>() <= 0) {
            error = "max_index_bytes must be a positive integer";
            return false;
        }
        next.max_index_bytes = static_cast<std::size_t>(body["max_index_bytes"].get<long long>());
    }
    if (body.contains("summary")) {
        const auto& summary = body["summary"];
        if (!summary.is_object()) {
            error = "summary must be an object";
            return false;
        }
        if (summary.contains("enabled")) {
            if (!summary["enabled"].is_boolean()) {
                error = "summary.enabled must be a boolean";
                return false;
            }
            next.summary.enabled = summary["enabled"].get<bool>();
        }
        if (summary.contains("model_name")) {
            if (!summary["model_name"].is_string()) {
                error = "summary.model_name must be a string";
                return false;
            }
            next.summary.model_name = summary["model_name"].get<std::string>();
        }
        for (const char* key : {"idle_minutes", "max_session_age_days"}) {
            if (!summary.contains(key)) continue;
            if (!summary[key].is_number_integer()) {
                error = std::string("summary.") + key + " must be an integer";
                return false;
            }
            (std::string(key) == "idle_minutes" ? next.summary.idle_minutes
                                                : next.summary.max_session_age_days) =
                summary[key].get<int>();
        }
    }
    out = next;
    return true;
}

json memory_entry_json(MemoryScope scope, const MemoryEntry& entry, bool include_body) {
    json out = {
        {"scope", memory_scope_to_string(scope)},
        {"name", entry.name},
        {"description", entry.description},
        {"type", memory_type_to_string(entry.type)},
        {"created_at", entry.created_at},
        {"updated_at", entry.updated_at},
        {"source", entry.source.empty() ? std::string(kMemorySourceManual) : entry.source},
        {"source_sessions", entry.source_sessions},
    };
    if (include_body) {
        out["body"] = entry.body;
        out["path"] = path_to_utf8_generic(entry.path);
    }
    return out;
}

json memory_overview_json(MemoryService& memory, const std::string& project_dir,
                          const MemorySummaryStatus& status) {
    json scopes = json::object();
    const MemoryScope order[] = {MemoryScope::Global, MemoryScope::Workspace};
    for (MemoryScope scope : order) {
        json item = {{"available", false}, {"dir", ""}, {"entries", json::array()}};
        if (auto registry = memory.scope(scope, project_dir)) {
            registry->reload();
            item["available"] = true;
            item["dir"] = path_to_utf8_generic(registry->dir());
            for (const auto& entry : registry->list()) {
                item["entries"].push_back(memory_entry_json(scope, entry, false));
            }
        }
        scopes[memory_scope_to_string(scope)] = std::move(item);
    }
    return {
        {"enabled", memory.enabled()},
        {"scopes", std::move(scopes)},
        {"status", {
            {"summary_enabled", status.enabled},
            {"global_inbox", status.global_inbox},
            {"workspace_inbox", status.workspace_inbox},
            {"last_extraction_ms", status.last_extraction_ms},
            {"last_consolidation_ms", status.last_consolidation_ms},
            {"last_error", status.last_error},
            {"last_error_ms", status.last_error_ms},
        }},
    };
}

bool parse_memory_entry_edit(const json& body, MemoryEntryEdit& out, std::string& error) {
    if (!body.is_object() || !body.contains("description") || !body["description"].is_string() ||
        !body.contains("body") || !body["body"].is_string()) {
        error = "description and body must be strings";
        return false;
    }
    MemoryEntryEdit edit;
    edit.description = body["description"].get<std::string>();
    edit.body = body["body"].get<std::string>();
    if (edit.description.find_first_not_of(" \t\r\n") == std::string::npos) {
        error = "description must not be empty";
        return false;
    }
    if (body.contains("type")) {
        if (!body["type"].is_string() || !(edit.type = parse_memory_type(body["type"].get<std::string>()))) {
            error = "type must be one of user|feedback|project|reference";
            return false;
        }
    }
    out = std::move(edit);
    return true;
}

} // namespace acecode::web
