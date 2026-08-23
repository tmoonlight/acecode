#include <gtest/gtest.h>

#include "desktop/workspace_registry.hpp"
#include "session/session_manager.hpp"
#include "tool/workspace_tools.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path unique_temp_dir(const std::string& label) {
    const auto path = fs::temp_directory_path() /
        ("acecode_workspace_tool_" + label + "_" +
         std::to_string(std::random_device{}()));
    fs::create_directories(path);
    return path;
}

std::string utf8(const fs::path& path) {
    return acecode::path_to_utf8(path);
}

} // namespace

TEST(WorkspaceTools, RegistersExistingDirectoryAndIsIdempotent) {
    const fs::path root = unique_temp_dir("register");
    const fs::path projects_dir = root / "projects";
    const fs::path target = root / "outside workspace";
    fs::create_directories(target);

    acecode::desktop::WorkspaceRegistry registry;
    registry.scan(utf8(projects_dir));
    auto deps = std::make_shared<acecode::WorkspaceToolDeps>();
    deps->registry = &registry;
    deps->projects_dir = utf8(projects_dir);

    acecode::ToolExecutor tools;
    acecode::register_workspace_tools(tools, deps);
    ASSERT_TRUE(tools.has_tool("create_workspace"));
    EXPECT_FALSE(tools.is_read_only("create_workspace"));

    const auto definitions = tools.get_tool_definitions();
    ASSERT_EQ(definitions.size(), 1u);
    const auto& schema = definitions[0].parameters;
    EXPECT_EQ(schema["required"], nlohmann::json::array({"path"}));
    EXPECT_EQ(schema["additionalProperties"], false);

    const std::string arguments = nlohmann::json{
        {"path", utf8(target)},
    }.dump();
    acecode::SessionManager caller;
    caller.start_session(utf8(root), "test", "test-model");
    const std::string caller_id = caller.current_session_id();
    acecode::ToolContext context;
    context.cwd = utf8(root);
    context.session_manager = &caller;
    const auto first = tools.execute(
        "create_workspace", arguments, context);
    ASSERT_TRUE(first.success) << first.output;
    EXPECT_EQ(caller.current_session_id(), caller_id);
    EXPECT_EQ(context.cwd, utf8(root));
    const auto first_json = nlohmann::json::parse(first.output);
    EXPECT_EQ(first_json["cwd"], utf8(fs::weakly_canonical(target)));
    EXPECT_EQ(first_json["name"], "outside workspace");
    EXPECT_EQ(first_json["available"], true);
    ASSERT_TRUE(first_json.contains("hash"));

    const auto marker = acecode::desktop::load_workspace_metadata(
        utf8(projects_dir), first_json["hash"].get<std::string>());
    ASSERT_TRUE(marker.has_value());
    EXPECT_TRUE(marker->desktop_visible);

    const auto second = tools.execute("create_workspace", arguments);
    ASSERT_TRUE(second.success) << second.output;
    const auto second_json = nlohmann::json::parse(second.output);
    EXPECT_EQ(second_json["hash"], first_json["hash"]);
    EXPECT_EQ(registry.list().size(), 1u);

    fs::remove_all(root);
}

TEST(WorkspaceTools, RejectsRelativeMissingAndFilePathsWithoutCreatingThem) {
    const fs::path root = unique_temp_dir("invalid");
    const fs::path projects_dir = root / "projects";
    const fs::path missing = root / "missing";
    const fs::path file = root / "file.txt";
    {
        std::ofstream stream(file);
        stream << "not a directory";
    }

    acecode::desktop::WorkspaceRegistry registry;
    auto deps = std::make_shared<acecode::WorkspaceToolDeps>();
    deps->registry = &registry;
    deps->projects_dir = utf8(projects_dir);
    acecode::ToolExecutor tools;
    acecode::register_workspace_tools(tools, deps);

    const auto relative = tools.execute(
        "create_workspace", R"({"path":"relative/path"})");
    EXPECT_FALSE(relative.success);
    EXPECT_NE(relative.output.find("must be absolute"), std::string::npos);

    const auto absent = tools.execute(
        "create_workspace",
        nlohmann::json{{"path", utf8(missing)}}.dump());
    EXPECT_FALSE(absent.success);
    EXPECT_NE(absent.output.find("does not exist"), std::string::npos);
    EXPECT_FALSE(fs::exists(missing));

    const auto regular_file = tools.execute(
        "create_workspace",
        nlohmann::json{{"path", utf8(file)}}.dump());
    EXPECT_FALSE(regular_file.success);
    EXPECT_NE(regular_file.output.find("not a directory"), std::string::npos);
    EXPECT_TRUE(registry.list().empty());

    fs::remove_all(root);
}

TEST(WorkspaceTools, ReportsUnavailableRuntimeDependencies) {
    acecode::ToolExecutor tools;
    acecode::register_workspace_tools(
        tools, std::make_shared<acecode::WorkspaceToolDeps>());
    const auto result = tools.execute(
        "create_workspace", R"({"path":"C:\\"})");
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("unavailable"), std::string::npos);
}
