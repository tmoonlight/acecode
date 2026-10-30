#include <gtest/gtest.h>
#include "agent/approval/path_access_policy.hpp"
#include "agent/boundary/workspace_boundary.hpp"
#include "agent/tool_exec/tool_session_host.hpp"
#include "permissions/permissions.hpp"
#include "session/attachment_store.hpp"
#include "session/session_manager.hpp"
#include "tool/tool_executor.hpp"
#include "utils/utf8_path.hpp"
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

// 场景:IM 发来的图片存在本会话的附件目录(数据目录下,不在会话工作目录里),模型要用 show_image 截取局部。
// 期望:只读工具可以读本会话附件目录里的文件;同一目录对写工具仍按工作目录校验;别的会话 / 别处的路径照旧拒绝。
// 回归:以前 show_image 读附件目录报「Path outside working directory」,Agent 截不了用户发来的图。
TEST(PathAccessPolicy, ReadOnlyToolsMayReadTheSessionsOwnAttachments) {
    acecode_test::characterization::TemporaryDirectory temporary;
    TestToolHost host;
    host.directory = acecode::path_to_utf8(temporary.path / "project");
    acecode::SessionManager session;
    session.start_session(host.directory, "stub", "stub");
    const auto session_id = session.ensure_active_session_id();
    const auto attachments = acecode::session_attachments_dir(session.current_project_dir(), session_id);
    const auto inside = acecode::path_to_utf8(acecode::path_from_utf8(attachments) / "a1.png");
    const auto other_session =
        acecode::path_to_utf8(acecode::path_from_utf8(acecode::session_attachments_dir(
                                  session.current_project_dir(), session_id + "x")) / "a1.png");
    acecode::PermissionManager permissions;
    acecode::ToolExecutor tools;
    acecode::ToolImpl show;
    show.definition.name = "show_image";
    show.is_read_only = true;
    ASSERT_TRUE(tools.register_tool(show));
    acecode::ToolImpl write;
    write.definition.name = "file_write";
    ASSERT_TRUE(tools.register_tool(write));
    acecode::agent::WorkspaceBoundary boundary(host.directory, permissions);
    acecode::agent::PathAccessPolicy policy(tools, permissions, boundary, host, &session);
    EXPECT_TRUE(policy.path_validation_error("show_image", inside).empty());
    EXPECT_FALSE(policy.path_validation_error("file_write", inside).empty());
    EXPECT_FALSE(policy.path_validation_error("show_image", other_session).empty());
    EXPECT_FALSE(policy.path_validation_error(
                     "show_image", acecode::path_to_utf8(temporary.path / "elsewhere.png")).empty());
}
