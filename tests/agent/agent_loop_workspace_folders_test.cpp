// 覆盖 AgentLoop 对「编辑项目」附加文件夹的消费(多文件夹项目):
// - 每回合开头从会话 project dir 的 workspace.json 读 extra_folders;
// - 文件工具的路径校验把附加文件夹当作工作目录放行,之外的路径照旧拒绝;
// - 系统提示 # Environment 列出附加工作目录;
// - 有写边界(worktree)时,与主 checkout 重叠的附加文件夹不放行写入,否则附加一个
//   主仓子目录 / 上级目录就能绕开 worktree 隔离。
//
// workspace.json 写在 SessionStorage::get_project_dir(cwd) 下(与会话文件同目录),
// 与同目录其它 AgentLoop 测试一样用临时 cwd,结束时清掉对应 project dir。

#include <gtest/gtest.h>

#include "agent/agent_loop.hpp"
#include "permissions/permissions.hpp"
#include "session/event_dispatcher.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "tool/tool_executor.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using acecode::AgentCallbacks;
using acecode::AgentLoop;
using acecode::PermissionManager;
using acecode::PermissionMode;
using acecode::PermissionResult;
using acecode::SessionEvent;
using acecode::SessionEventKind;
using acecode::ToolContext;
using acecode::ToolDef;
using acecode::ToolExecutor;
using acecode::ToolImpl;
using acecode::ToolResult;
using acecode::ToolSource;
using acecode::path_to_utf8;
using acecode_test::ScriptedResponse;
using acecode_test::StubLlmProvider;

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

ToolImpl make_write_probe(std::atomic<int>* calls) {
    ToolDef def;
    def.name = "write_path_probe";
    def.description = "Workspace folder write probe";
    def.parameters = {
        {"type", "object"},
        {"properties", nlohmann::json::object({{"file_path", {{"type", "string"}}}})},
    };
    ToolImpl impl;
    impl.definition = def;
    impl.is_read_only = false;
    impl.source = ToolSource::Builtin;
    impl.execute = [calls](const std::string&, const ToolContext&) -> ToolResult {
        calls->fetch_add(1);
        return ToolResult{"probe ok", true};
    };
    return impl;
}

fs::path make_temp_dir(const std::string& name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto p = fs::temp_directory_path() / (name + "_" + std::to_string(stamp));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

// 模拟「编辑项目」保存后的 workspace.json。
void write_workspace_marker(const fs::path& main, const std::vector<fs::path>& extras) {
    const auto project_dir = acecode::SessionStorage::get_project_dir(path_to_utf8(main));
    fs::create_directories(acecode::path_from_utf8(project_dir));
    nlohmann::json folders = nlohmann::json::array();
    for (const auto& extra : extras) folders.push_back(path_to_utf8(extra));
    std::ofstream out(acecode::path_from_utf8(project_dir) / "workspace.json");
    out << nlohmann::json{
        {"cwd", path_to_utf8(main)},
        {"name", "main"},
        {"desktop_visible", true},
        {"extra_folders", folders},
    }.dump();
}

class Harness {
public:
    Harness(std::string cwd, PermissionMode mode)
        : cwd_(std::move(cwd)) {
        AgentCallbacks cb;
        cb.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lk(busy_mu_);
            busy_ = busy;
            if (!busy) busy_cv_.notify_all();
        };
        cb.on_tool_confirm = [](const std::string&, const std::string&) {
            return PermissionResult::Allow;
        };
        auto accessor = [this]() -> std::shared_ptr<acecode::LlmProvider> { return provider_; };
        perms_.set_mode(mode);
        loop_ = std::make_unique<AgentLoop>(accessor, tools_, cb, cwd_, perms_);
        sub_ = loop_->events().subscribe([this](const SessionEvent& e) {
            std::lock_guard<std::mutex> lk(events_mu_);
            events_.push_back(e);
        });
    }
    ~Harness() {
        if (loop_ && sub_ != 0) loop_->events().unsubscribe(sub_);
        loop_.reset();
    }

    ToolExecutor& tools() { return tools_; }
    StubLlmProvider& provider() { return *provider_; }
    AgentLoop& loop() { return *loop_; }

    void enter_worktree(const std::string& worktree_path, const std::string& original_cwd) {
        session_manager_.start_session(cwd_, "stub", "stub-model");
        loop_->set_session_manager(&session_manager_);
        acecode::WorktreeSessionInfo info;
        info.original_cwd = original_cwd;
        info.worktree_path = worktree_path;
        info.worktree_name = "wt";
        info.worktree_branch = "worktree-wt";
        session_manager_.set_active_worktree(info);
        loop_->set_cwd(worktree_path);
    }

    bool submit_and_wait() {
        {
            std::lock_guard<std::mutex> lk(busy_mu_);
            busy_ = true;
        }
        loop_->submit("go");
        std::unique_lock<std::mutex> lk(busy_mu_);
        return busy_cv_.wait_for(lk, 5s, [this] { return !busy_; });
    }

    std::vector<SessionEvent> tool_ends() const {
        std::lock_guard<std::mutex> lk(events_mu_);
        std::vector<SessionEvent> out;
        for (const auto& e : events_) {
            if (e.kind == SessionEventKind::ToolEnd) out.push_back(e);
        }
        return out;
    }

private:
    std::string cwd_;
    std::shared_ptr<StubLlmProvider> provider_ = std::make_shared<StubLlmProvider>();
    ToolExecutor tools_;
    PermissionManager perms_;
    acecode::SessionManager session_manager_;
    std::unique_ptr<AgentLoop> loop_;
    acecode::EventDispatcher::SubscriptionId sub_ = 0;
    mutable std::mutex events_mu_;
    std::vector<SessionEvent> events_;
    std::mutex busy_mu_;
    std::condition_variable busy_cv_;
    bool busy_ = false;
};

std::string end_output(const SessionEvent& e) {
    return e.payload.value("output", std::string{});
}

} // namespace

// 场景:项目主文件夹 main,「编辑项目」加了附加文件夹 shared;模型在一个回合里先写
// shared 里的文件,再写一个不属于项目的 other 目录。
// 期望:写 shared 通过路径校验并执行;写 other 仍报 "Path outside working directory";
// 本回合的系统提示列出附加工作目录。
TEST(AgentLoopWorkspaceFolders, AdditionalFolderIsWritableAndListedInPrompt) {
    const auto root = make_temp_dir("acecode_ws_folders_write");
    const auto main = root / "main";
    const auto shared = root / "shared";
    const auto other = root / "other";
    fs::create_directories(main);
    fs::create_directories(shared);
    fs::create_directories(other);
    write_workspace_marker(main, {shared});
    const auto project_dir = acecode::SessionStorage::get_project_dir(path_to_utf8(main));

    {
        std::atomic<int> calls{0};
        Harness h(path_to_utf8(main), PermissionMode::Default);
        h.tools().register_tool(make_write_probe(&calls));
        ScriptedResponse turn;
        turn.tool_calls.push_back({"call-shared", "write_path_probe",
            nlohmann::json{{"file_path", path_to_utf8(shared / "new.txt")}}.dump()});
        turn.tool_calls.push_back({"call-other", "write_path_probe",
            nlohmann::json{{"file_path", path_to_utf8(other / "x.txt")}}.dump()});
        h.provider().push_response(std::move(turn));
        h.provider().push_text("done");

        ASSERT_TRUE(h.submit_and_wait());
        EXPECT_EQ(calls.load(), 1);
        const auto ends = h.tool_ends();
        ASSERT_EQ(ends.size(), 2u);
        for (const auto& end : ends) {
            const auto id = end.payload.value("tool_call_id", std::string{});
            if (id == "call-shared") {
                EXPECT_TRUE(end.payload.value("success", false)) << end_output(end);
            } else {
                EXPECT_FALSE(end.payload.value("success", true));
                EXPECT_NE(end_output(end).find("Path outside working directory"), std::string::npos);
            }
        }

        const auto request = h.provider().messages_for_turn(0);
        ASSERT_FALSE(request.empty());
        EXPECT_NE(request[0].content.find("- Additional working directories: " + path_to_utf8(shared)),
                  std::string::npos);
    }

    fs::remove_all(acecode::path_from_utf8(project_dir));
    fs::remove_all(root);
}

// 回归场景:worktree 会话(写边界 = worktree)。附加文件夹里一个是主 checkout 的
// 子目录 main/vendor,一个是项目外的 shared。
// 期望:shared 可写;main/vendor 被收回写权限(只读,系统提示单独标注)——否则 Yolo
// 子代理能借附加文件夹把改动写回主 checkout,绕开 worktree 隔离。
TEST(AgentLoopWorkspaceFolders, WorktreeBoundaryDropsFolderOverlappingMainCheckout) {
    const auto root = make_temp_dir("acecode_ws_folders_worktree");
    const auto main = root / "main";
    const auto vendor = main / "vendor";
    const auto worktree = main / ".acecode" / "worktrees" / "wt";
    const auto shared = root / "shared";
    fs::create_directories(vendor);
    fs::create_directories(worktree);
    fs::create_directories(shared);
    write_workspace_marker(main, {vendor, shared});
    const auto project_dir = acecode::SessionStorage::get_project_dir(path_to_utf8(main));

    {
        std::atomic<int> calls{0};
        Harness h(path_to_utf8(main), PermissionMode::Yolo);
        h.tools().register_tool(make_write_probe(&calls));
        h.enter_worktree(path_to_utf8(worktree), path_to_utf8(main));

        h.loop().refresh_workspace_folders();
        EXPECT_EQ(h.loop().workspace_extra_folders().size(), 2u);
        EXPECT_EQ(h.loop().writable_workspace_folders(),
                  (std::vector<std::string>{path_to_utf8(shared)}));

        ScriptedResponse turn;
        turn.tool_calls.push_back({"call-shared", "write_path_probe",
            nlohmann::json{{"file_path", path_to_utf8(shared / "ok.txt")}}.dump()});
        turn.tool_calls.push_back({"call-vendor", "write_path_probe",
            nlohmann::json{{"file_path", path_to_utf8(vendor / "escape.txt")}}.dump()});
        h.provider().push_response(std::move(turn));
        h.provider().push_text("done");

        ASSERT_TRUE(h.submit_and_wait());
        EXPECT_EQ(calls.load(), 1);
        for (const auto& end : h.tool_ends()) {
            const auto id = end.payload.value("tool_call_id", std::string{});
            if (id == "call-shared") {
                EXPECT_TRUE(end.payload.value("success", false)) << end_output(end);
            } else {
                EXPECT_FALSE(end.payload.value("success", true));
                EXPECT_NE(end_output(end).find("Write boundary blocked"), std::string::npos);
            }
        }

        const auto request = h.provider().messages_for_turn(0);
        ASSERT_FALSE(request.empty());
        EXPECT_NE(request[0].content.find("readable but not writable in this session"),
                  std::string::npos);
    }

    fs::remove_all(acecode::path_from_utf8(project_dir));
    fs::remove_all(root);
}

// 场景:保存之后才开的会话 vs 已打开的会话 —— 附加文件夹在回合之间被移除。
// 期望:下一回合开头重读,被移除的文件夹立即不再放行(不必重开会话)。
TEST(AgentLoopWorkspaceFolders, RemovedFolderStopsApplyingNextTurn) {
    const auto root = make_temp_dir("acecode_ws_folders_refresh");
    const auto main = root / "main";
    const auto shared = root / "shared";
    fs::create_directories(main);
    fs::create_directories(shared);
    write_workspace_marker(main, {shared});
    const auto project_dir = acecode::SessionStorage::get_project_dir(path_to_utf8(main));

    {
        std::atomic<int> calls{0};
        Harness h(path_to_utf8(main), PermissionMode::Default);
        h.tools().register_tool(make_write_probe(&calls));
        h.loop().refresh_workspace_folders();
        ASSERT_EQ(h.loop().writable_workspace_folders().size(), 1u);

        write_workspace_marker(main, {});
        h.provider().push_tool_call("write_path_probe",
            nlohmann::json{{"file_path", path_to_utf8(shared / "x.txt")}}.dump(), "call-x");
        h.provider().push_text("done");
        ASSERT_TRUE(h.submit_and_wait());
        EXPECT_EQ(calls.load(), 0);
        EXPECT_TRUE(h.loop().writable_workspace_folders().empty());
    }

    fs::remove_all(acecode::path_from_utf8(project_dir));
    fs::remove_all(root);
}
