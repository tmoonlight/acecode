#include "memory_command.hpp"

#include "memory_runtime.hpp"
#include "memory_scheduler.hpp"

#include "memory/memory_frontmatter.hpp"
#include "memory/memory_service.hpp"
#include "prompt/memory_prompt.hpp"
#include "session/session_manager.hpp"
#include "utils/utf8_path.hpp"

#include <chrono>
#include <optional>
#include <sstream>
#include <vector>

namespace acecode {

namespace {

struct ParsedArgs {
    std::string sub;
    std::string name;
    std::string scope;
    std::string type;
};

std::string trim(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

ParsedArgs parse_args(const std::string& args) {
    ParsedArgs out;
    std::istringstream iss(trim(args));
    std::string token;
    while (iss >> token) {
        if (token.rfind("--scope=", 0) == 0) out.scope = token.substr(8);
        else if (token.rfind("--type=", 0) == 0) out.type = token.substr(7);
        else if (out.sub.empty()) out.sub = token;
        else if (out.name.empty()) out.name = token;
    }
    if (out.sub.empty()) out.sub = "list";
    return out;
}

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string project_dir_of(SessionManager* session) {
    if (!session || session->is_no_workspace()) return {};
    return session->current_project_dir();
}

const char* usage_text() {
    return "Usage:\n"
           "  /memory list [--scope=global|workspace] [--type=<type>]\n"
           "  /memory view <name>\n"
           "  /memory edit <name>\n"
           "  /memory forget <name>\n"
           "  /memory flush\n"
           "  /memory off | /memory on\n"
           "  /memory reload";
}

struct Found {
    MemoryScope scope = MemoryScope::Global;
    MemoryEntry entry;
};

// 按名字查找:未指定作用域时先工作区后全局(与 memory_read 一致)。
std::optional<Found> find_entry(MemoryService& memory, const std::string& project_dir,
                                const std::string& name, const std::string& scope_filter) {
    const MemoryScope order[] = {MemoryScope::Workspace, MemoryScope::Global};
    for (MemoryScope scope : order) {
        if (!scope_filter.empty() && memory_scope_to_string(scope) != scope_filter) continue;
        auto registry = memory.scope(scope, project_dir);
        if (!registry) continue;
        registry->reload();
        if (auto entry = registry->find(name)) return Found{scope, std::move(*entry)};
    }
    return std::nullopt;
}

void append_scope_listing(std::ostringstream& out, const char* title,
                          const std::shared_ptr<MemoryRegistry>& registry,
                          const std::optional<MemoryType>& type_filter) {
    if (!registry) {
        out << title << ": not available (this session has no workspace)\n";
        return;
    }
    registry->reload();
    const auto entries = registry->list(type_filter);
    out << title << " (" << path_to_utf8_generic(registry->dir()) << "): ";
    if (entries.empty()) {
        out << "no entries\n";
        return;
    }
    out << entries.size() << (entries.size() == 1 ? " entry\n" : " entries\n");
    const std::int64_t now = now_seconds();
    for (const auto& e : entries) {
        out << "  [" << memory_type_to_string(e.type) << "] " << e.name << " \xE2\x80\x94 "
            << e.description;
        const std::string when = !e.updated_at.empty() ? e.updated_at
                                 : !e.created_at.empty() ? e.created_at
                                 : memory_file_mtime_iso8601(e.path);
        const std::string age = memory_age_label(when, now);
        if (!age.empty()) out << " (" << age << ")";
        if (e.source == kMemorySourceSummary) out << " [summary]";
        out << "\n";
    }
}

std::string handle_list(MemoryService& memory, const std::string& project_dir,
                        const ParsedArgs& args) {
    std::optional<MemoryType> type_filter;
    if (!args.type.empty()) {
        type_filter = parse_memory_type(args.type);
        if (!type_filter) return "Invalid type filter: " + args.type;
    }
    if (!args.scope.empty() && !parse_memory_scope(args.scope)) {
        return "Invalid scope: " + args.scope + " (allowed: global|workspace)";
    }
    std::ostringstream out;
    if (args.scope.empty() || args.scope == "global") {
        append_scope_listing(out, "Global memory", memory.scope(MemoryScope::Global, project_dir),
                             type_filter);
    }
    if (args.scope.empty() || args.scope == "workspace") {
        append_scope_listing(out, "Workspace memory",
                             memory.scope(MemoryScope::Workspace, project_dir), type_filter);
    }
    std::string text = out.str();
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

std::string handle_view(MemoryService& memory, const std::string& project_dir,
                        const ParsedArgs& args) {
    if (args.name.empty()) return "Usage: /memory view <name>";
    const auto found = find_entry(memory, project_dir, args.name, args.scope);
    if (!found) return "No memory entry named '" + args.name + "'.";
    const auto& e = found->entry;
    std::ostringstream out;
    out << "[" << memory_scope_to_string(found->scope) << "] ["
        << memory_type_to_string(e.type) << "] " << e.name << "\n"
        << "Description: " << e.description << "\n";
    if (!e.updated_at.empty()) out << "Updated: " << e.updated_at << "\n";
    if (!e.source.empty()) out << "Source: " << e.source << "\n";
    out << "Path: " << path_to_utf8_generic(e.path) << "\n\n" << e.body;
    std::string text = out.str();
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

std::string handle_forget(MemoryService& memory, const std::string& project_dir,
                          const ParsedArgs& args) {
    if (args.name.empty()) return "Usage: /memory forget <name>";
    const auto found = find_entry(memory, project_dir, args.name, args.scope);
    if (!found) return "No memory entry named '" + args.name + "'.";
    std::string error;
    if (!memory.delete_entry(found->scope, project_dir, args.name, error)) {
        return "Failed to forget '" + args.name + "': " + error;
    }
    return "Forgot '" + args.name + "' from the " + memory_scope_to_string(found->scope) +
           " memory.";
}

std::string handle_reload(MemoryService& memory, const std::string& project_dir) {
    memory.global().reload();
    const std::size_t global = memory.global().size();
    std::size_t workspace = 0;
    if (auto registry = memory.workspace(project_dir)) {
        registry->reload();
        workspace = registry->size();
    }
    return "Reloaded " + std::to_string(global) + " global and " + std::to_string(workspace) +
           " workspace memory entr" + (global + workspace == 1 ? "y." : "ies.");
}

std::string handle_flush(MemoryRuntime& runtime, SessionManager* session,
                         const std::string& project_dir) {
    const MemoryConfig cfg = runtime.service()->config();
    if (!cfg.summary.enabled) {
        return "Memory summarization is off, so there is nothing to flush. Turn it on in "
               "Settings > Personalization > Memory to let ACECode extract and consolidate "
               "memories from idle sessions.";
    }
    if (!runtime.scheduler()) return "Memory summarization is not available in this mode.";
    if (!session || session->current_session_id().empty() || project_dir.empty()) {
        return "/memory flush needs a saved workspace session.";
    }
    if (!session->memory_enabled()) {
        return "Memory is off for this session. Use /memory on before /memory flush.";
    }
    return runtime.scheduler()->request_flush(session->current_session_id(), project_dir).message;
}

} // namespace

MemoryCommandResult dispatch_memory_command(const std::string& args,
                                            const MemoryCommandContext& ctx) {
    MemoryCommandResult result;
    const ParsedArgs parsed = parse_args(args);
    if (!ctx.runtime || !ctx.runtime->service()) {
        result.text = "Memory is not available in this session.";
        return result;
    }
    MemoryService& memory = *ctx.runtime->service();
    const std::string project_dir = project_dir_of(ctx.session);

    if (parsed.sub == "off" || parsed.sub == "on") {
        if (!ctx.session) {
            result.text = "There is no active session.";
            return result;
        }
        const bool enable = parsed.sub == "on";
        ctx.session->set_memory_enabled(enable);
        result.text = enable
            ? "Memory is on for this session."
            : "Memory is off for this session: no memory context or memory tools, and the "
              "session is skipped by memory summarization. Use /memory on to turn it back on.";
        return result;
    }
    if (!memory.enabled()) {
        result.text = "Memory is turned off in Settings > Personalization > Memory.";
        return result;
    }
    if (parsed.sub == "list") {
        result.text = handle_list(memory, project_dir, parsed);
        if (ctx.session && !ctx.session->memory_enabled()) {
            result.text += "\nMemory is off for this session (/memory on to turn it back on).";
        }
    } else if (parsed.sub == "view") {
        result.text = handle_view(memory, project_dir, parsed);
    } else if (parsed.sub == "forget") {
        result.text = handle_forget(memory, project_dir, parsed);
    } else if (parsed.sub == "reload") {
        result.text = handle_reload(memory, project_dir);
    } else if (parsed.sub == "flush") {
        result.text = handle_flush(*ctx.runtime, ctx.session, project_dir);
    } else if (parsed.sub == "edit") {
        if (parsed.name.empty()) {
            result.text = "Usage: /memory edit <name>";
        } else if (ctx.web) {
            result.text = "Edit memory entries in Settings > Personalization > Memory.";
        } else if (const auto found = find_entry(memory, project_dir, parsed.name, parsed.scope)) {
            result.edit_path = path_to_utf8(found->entry.path);
            result.text = "Opening " + path_to_utf8_generic(found->entry.path) + " in the editor.";
        } else {
            result.text = "No memory entry named '" + parsed.name + "'.";
        }
    } else {
        result.text = usage_text();
    }
    return result;
}

} // namespace acecode
