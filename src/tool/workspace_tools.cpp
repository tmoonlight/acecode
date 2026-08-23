#include "workspace_tools.hpp"

#include "../desktop/workspace_registry.hpp"
#include "../utils/tool_args_parser.hpp"
#include "../utils/utf8_path.hpp"

#include <filesystem>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace acecode {

namespace {

ToolResult tool_error(const std::string& message) {
    return ToolResult{"[Error] " + message, false};
}

ToolImpl create_workspace_tool(std::shared_ptr<WorkspaceToolDeps> deps) {
    ToolImpl tool;
    tool.definition.name = "create_workspace";
    tool.definition.description =
        "Register an existing absolute directory as an ACECode workspace. "
        "This does not create the directory, change the calling thread's "
        "working directory, or create a thread.";
    tool.definition.parameters = nlohmann::json{
        {"type", "object"},
        {"properties",
         nlohmann::json{
             {"path",
              nlohmann::json{
                  {"type", "string"},
                  {"minLength", 1},
                  {"description", "Absolute path of an existing directory"},
              }},
         }},
        {"required", nlohmann::json::array({"path"})},
        {"additionalProperties", false},
    };
    tool.is_read_only = false;
    tool.execute = [deps](const std::string& arguments,
                          const ToolContext&) -> ToolResult {
        if (!deps || !deps->registry || deps->projects_dir.empty()) {
            return tool_error(
                "workspace tools are unavailable in this runtime");
        }

        ToolArgsParser parser(arguments);
        if (parser.has_error()) return tool_error(parser.error());
        auto raw_path = parser.get<std::string>("path");
        if (!raw_path || raw_path->empty()) {
            return tool_error("path is required and must be a non-empty string");
        }

        fs::path native_path = path_from_utf8(*raw_path);
        if (!native_path.is_absolute()) {
            return tool_error("path must be absolute");
        }

        std::error_code ec;
        const bool exists = fs::exists(native_path, ec);
        if (ec || !exists) {
            return tool_error("path does not exist: " + *raw_path);
        }
        if (!fs::is_directory(native_path, ec) || ec) {
            return tool_error("path is not a directory: " + *raw_path);
        }

        fs::path canonical_path = fs::weakly_canonical(native_path, ec);
        if (ec || canonical_path.empty()) {
            return tool_error("failed to resolve path: " + *raw_path);
        }
        const std::string cwd = path_to_utf8(canonical_path);
        auto meta = deps->registry->register_new(deps->projects_dir, cwd);

        // register_new keeps the current process usable when persistence
        // fails. A model-visible creation API must not report that transient
        // cache entry as a durable workspace, so verify the marker here.
        const auto persisted = desktop::load_workspace_metadata(
            deps->projects_dir, meta.hash);
        if (!persisted || !persisted->desktop_visible) {
            return tool_error("failed to persist workspace registration");
        }

        nlohmann::json output{
            {"hash", meta.hash},
            {"cwd", meta.cwd},
            {"name", meta.name},
            {"available", true},
        };
        ToolResult result;
        result.output = output.dump();
        result.summary = ToolSummary{"Registered", meta.cwd, {}, {}};
        return result;
    };
    return tool;
}

} // namespace

void register_workspace_tools(
    ToolExecutor& tools,
    std::shared_ptr<WorkspaceToolDeps> deps) {
    tools.register_tool(create_workspace_tool(std::move(deps)));
}

} // namespace acecode
