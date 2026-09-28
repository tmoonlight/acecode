#include <gtest/gtest.h>
#include "agent/approval/path_access_policy.hpp"
#include "agent/boundary/workspace_boundary.hpp"
#include "agent/tool_exec/tool_session_host.hpp"
#include "permissions/permissions.hpp"
#include "tool/tool_executor.hpp"
#include "test_support/agent_loop/characterization_fixture.hpp"

namespace {
class TestToolHost final : public acecode::agent::ToolSessionHost {
public:
    std::string directory;
    std::string root;
    std::string cwd() const override { return directory; }
    std::string write_root() const override { return root; }
    std::vector<std::string> writable_workspace_folders() const override { return {}; }
    bool path_in_workspace_folders(const std::string&) const override { return false; }
    void switch_cwd(const std::string& cwd) override { directory = cwd; }
};
}

TEST(PathAccessPolicy, BoundedYoloAllowsReadsButRejectsWritesOutsideRoot) {
    acecode_test::characterization::TemporaryDirectory temporary;
    TestToolHost host;
    host.directory = acecode::path_to_utf8(temporary.path / "project");
    host.root = host.directory;
    const auto outside = acecode::path_to_utf8(temporary.path / "outside.txt");
    acecode::PermissionManager permissions;
    permissions.set_mode(acecode::PermissionMode::Yolo);
    acecode::ToolExecutor tools;
    acecode::ToolImpl read;
    read.definition.name = "file_read";
    read.is_read_only = true;
    ASSERT_TRUE(tools.register_tool(read));
    acecode::agent::WorkspaceBoundary boundary(host.directory, permissions);
    acecode::agent::PathAccessPolicy policy(tools, permissions, boundary, host, nullptr);
    EXPECT_TRUE(policy.path_validation_error("file_read", outside).empty());
    EXPECT_NE(policy.path_validation_error("file_write", outside).find("Write boundary blocked"),
        std::string::npos);
    host.root.clear();
    EXPECT_TRUE(policy.path_validation_error("file_write", outside).empty());
}
