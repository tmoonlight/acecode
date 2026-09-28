#include "test_support/agent/agent_loop_fixture.hpp"
#ifdef _WIN32
#include <gtest/gtest.h>
#include "sandbox/sandbox_backend.hpp"
#include "sandbox/sandbox_runtime.hpp"
#include "tool/bash_tool.hpp"
#include "llm/tool_protocol_names.hpp"
#include "environment/terminal_runtime.hpp"
#include "agent/agent_loop.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "test_support/sandbox/test_support.hpp"
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <condition_variable>
#include <chrono>
#include <mutex>

using namespace acecode;
using namespace acecode::sandbox;
namespace fs = std::filesystem;

namespace {
using Token = platform::UniqueHandle;

std::wstring dacl_snapshot(const fs::path& path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto rc = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &descriptor);
    EXPECT_EQ(rc, ERROR_SUCCESS);
    if (rc != ERROR_SUCCESS) return {};
    LPWSTR text = nullptr;
    const auto ok = ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor,
        SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr);
    EXPECT_TRUE(ok);
    std::wstring result = ok ? text : L"";
    if (text) LocalFree(text);
    LocalFree(descriptor);
    return result;
}

// 仅清理本例新建、仍位于预期系统临时子目录内的树,不跟随被重定向的根。
struct ScopedSandboxTemp {
    fs::path path;
    bool owned;
    explicit ScopedSandboxTemp(const std::string& value)
        : path(path_from_utf8(value)), owned(!fs::exists(path)) {}
    ~ScopedSandboxTemp() {
        std::error_code ec;
        if (owned && !path.empty() &&
            path.parent_path() == path_from_utf8(system_temp_dir()) / "acecode-sandbox" &&
            fs::weakly_canonical(path, ec) == path && !ec) {
            fs::remove_all(path, ec);
        }
    }
};
// 本机 TEMP 被其它程序授予 Everyone FullControl。此用例创建自己的私有目录,
// 验证普通用户私有路径的隔离,不修改 TEMP 的权限或掩盖公开目录的已知限制。
bool make_private_test_root(const fs::path& root) {
    Token current;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, current.put())) return false;
    DWORD size = 0;
    GetTokenInformation(current.get(), TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> user(size);
    if (!GetTokenInformation(current.get(), TokenUser, user.data(), size, &size)) return false;
    auto* info = reinterpret_cast<TOKEN_USER*>(user.data());
    EXPLICIT_ACCESS_W entry{};
    entry.grfAccessPermissions = FILE_ALL_ACCESS;
    entry.grfAccessMode = GRANT_ACCESS;
    entry.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.ptstrName = static_cast<LPWSTR>(info->User.Sid);
    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &entry, nullptr, &acl) != ERROR_SUCCESS) return false;
    auto path = root.wstring();
    const auto result = SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    return result == ERROR_SUCCESS;
}
bool token_can_write(HANDLE token, const fs::path& path) {
    if (!ImpersonateLoggedOnUser(token)) return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool allowed = file != INVALID_HANDLE_VALUE;
    if (allowed) CloseHandle(file);
    RevertToSelf();
    return allowed;
}
bool token_can_delete(HANDLE token, const fs::path& path) {
    if (!ImpersonateLoggedOnUser(token)) return false;
    const bool allowed = DeleteFileW(path.c_str()) != FALSE;
    RevertToSelf();
    return allowed;
}
bool token_can_rename(HANDLE token, const fs::path& from, const fs::path& to) {
    if (!ImpersonateLoggedOnUser(token)) return false;
    const bool allowed = MoveFileW(from.c_str(), to.c_str()) != FALSE;
    RevertToSelf();
    return allowed;
}
bool token_can_open_access(HANDLE token, const fs::path& path, DWORD access) {
    if (!ImpersonateLoggedOnUser(token)) return false;
    auto file = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    const bool allowed = file != INVALID_HANDLE_VALUE;
    if (allowed) CloseHandle(file);
    RevertToSelf();
    return allowed;
}
}

// 默认配置必须覆盖真实系统 TEMP 场景:只授权工作区专用临时目录,不改整个
// 系统临时根或无关文件的 ACL;子进程实际经 TEMP/TMP/TMPDIR 写入同一目录。
TEST(SandboxBackendWin, DefaultTempWritesWithoutChangingSharedTempAcl) {
    if (!probe_backend().available) GTEST_SKIP() << "restricted tokens unavailable";
    test::TempTree tree;
    ASSERT_TRUE(make_private_test_root(tree.root));
    const auto workspace = tree.dir("workspace");
    const auto unrelated = tree.root / "unrelated.txt";
    tree.write(unrelated, "untouched");
    SandboxRuntime runtime;
    auto request = runtime.request_for(SandboxMode::WorkspaceWrite, path_to_utf8(workspace));
    ASSERT_FALSE(request.policy.temporary_directory.empty());
    ScopedSandboxTemp temporary(request.policy.temporary_directory);
    const auto shared = path_from_utf8(system_temp_dir());
    ASSERT_NE(temporary.path, shared);
    ASSERT_EQ(temporary.path.parent_path(), shared / "acecode-sandbox");
    ASSERT_TRUE(temporary.owned);
    const auto shared_before = dacl_snapshot(shared);
    const auto unrelated_before = dacl_snapshot(unrelated);
    const auto prepare_started = std::chrono::steady_clock::now();
    const auto prepare_error = runtime.prepare_request(request);
    RecordProperty("prepare_ms", static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - prepare_started).count()));
    ASSERT_TRUE(prepare_error.empty()) << prepare_error;
    EXPECT_EQ(dacl_snapshot(shared), shared_before);
    EXPECT_EQ(dacl_snapshot(unrelated), unrelated_before);

    SandboxRuntime reopened;
    const auto same = reopened.request_for(SandboxMode::WorkspaceWrite,
        path_to_utf8(workspace / "."));
    EXPECT_EQ(same.policy.temporary_directory, request.policy.temporary_directory);
    EXPECT_EQ(synthetic_sid_string(same.policy), synthetic_sid_string(request.policy));
    EXPECT_NE(reopened.request_for(SandboxMode::WorkspaceWrite, path_to_utf8(tree.dir("other")))
        .policy.temporary_directory, request.policy.temporary_directory);

    auto previous = environment::terminal().last();
    struct RestoreTerminal {
        decltype(previous) snapshot;
        ~RestoreTerminal() {
            if (snapshot) environment::terminal().publish(*snapshot);
            else environment::terminal().reset_for_test();
        }
    } restore{previous};
    environment::terminal().reset_for_test();
    ToolContext ctx;
    ctx.cwd = path_to_utf8(workspace);
    ctx.exec_sandbox = request;
    const auto result = create_bash_tool().execute(nlohmann::json{{"command",
        "echo temp>\"%TEMP%\\temp.txt\" && echo tmp>\"%TMP%\\tmp.txt\" && "
        "echo tmpdir>\"%TMPDIR%\\tmpdir.txt\""}}.dump(), ctx);
    ASSERT_TRUE(result.success) << result.output;
    for (const auto* name : {"temp.txt", "tmp.txt", "tmpdir.txt"}) {
        EXPECT_TRUE(fs::exists(temporary.path / name)) << name;
    }

    // 复现用户会话的 PowerShell 存在性检查,只读取 C:\1.txt,不创建或修改它。
    ConsoleConfig console;
    console.default_shell = "powershell";
    const auto terminal = environment::terminal().reresolve(console);
    ASSERT_TRUE(terminal.resolved.usable) << terminal.resolved.fallback_reason;
    ASSERT_EQ(terminal.resolved.id, "powershell");
    const auto shell_started = std::chrono::steady_clock::now();
    const auto check = create_bash_tool().execute(nlohmann::json{{"command",
        R"(if (Test-Path 'C:\1.txt') { Write-Output 'EXISTS' } else { Write-Output 'MISSING' })"}}.dump(), ctx);
    RecordProperty("powershell_probe_ms", static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - shell_started).count()));
    EXPECT_TRUE(check.success) << check.output;
    EXPECT_TRUE(check.output.find("EXISTS") != std::string::npos ||
                check.output.find("MISSING") != std::string::npos) << check.output;
    const auto powershell_write = create_bash_tool().execute(nlohmann::json{{"command",
        R"([System.IO.File]::WriteAllText((Join-Path $env:TEMP 'powershell.txt'), 'hello'))"}}.dump(), ctx);
    EXPECT_TRUE(powershell_write.success) << powershell_write.output;
    EXPECT_TRUE(fs::exists(temporary.path / "powershell.txt"));
    EXPECT_EQ(dacl_snapshot(shared), shared_before);
    EXPECT_EQ(dacl_snapshot(unrelated), unrelated_before);
}

// 专用临时目录若是指向外部目录的链接,准备必须先拒绝,不能向目标传播 ACL。
TEST(SandboxBackendWin, RejectsRedirectedTemporaryDirectoryBeforeGrantingAcl) {
    if (!probe_backend().available) GTEST_SKIP() << "restricted tokens unavailable";
    test::TempTree tree;
    auto workspace = tree.dir("workspace");
    auto outside = tree.dir("outside");
    SandboxRuntime runtime;
    auto request = runtime.request_for(SandboxMode::WorkspaceWrite, path_to_utf8(workspace));
    ASSERT_FALSE(request.policy.temporary_directory.empty());
    ScopedSandboxTemp temporary(request.policy.temporary_directory);
    ASSERT_TRUE(temporary.owned);
    fs::create_directories(temporary.path.parent_path());
    if (!CreateSymbolicLinkW(temporary.path.c_str(), outside.c_str(),
                            SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2)) {
        GTEST_SKIP() << "directory symlinks unavailable: " << GetLastError();
    }
    const auto before = dacl_snapshot(outside);
    const auto error = runtime.prepare_request(request);
    EXPECT_NE(error.find("redirected"), std::string::npos) << error;
    EXPECT_EQ(dacl_snapshot(outside), before);
    EXPECT_FALSE(fs::exists(outside / ".acecode"));
    // RemoveDirectoryW 只删除目录链接本身,不递归到链接目标。
    EXPECT_TRUE(RemoveDirectoryW(temporary.path.c_str()));
}

// 真机回归:工作区 A 的 ACE 不能让 B 或只读令牌继续写 A。
TEST(SandboxBackendWin, IsolatesWorkspaceDataWritesAndProtectsSensitiveContents) {
    const auto probe = probe_backend();
    if (!probe.available) GTEST_SKIP() << probe.reason;
    test::TempTree tree;
    ASSERT_TRUE(make_private_test_root(tree.root));
    auto a = tree.dir("a");
    auto b = tree.dir("b");
    auto outside = tree.dir("outside");
    tree.dir("a/.git");
    SandboxRuntime runtime;
    SandboxRuntimeConfig config;
    config.exclude_tmpdir = true;
    runtime.configure(config);
    auto request_a = runtime.request_for(SandboxMode::WorkspaceWrite, path_to_utf8(a));
    auto request_b = runtime.request_for(SandboxMode::WorkspaceWrite, path_to_utf8(b));
    ASSERT_TRUE(runtime.prepare_request(request_a).empty());
    ASSERT_TRUE(runtime.prepare_request(request_b).empty());
    std::string error;
    auto token_a = create_restricted_token(request_a.policy, &error);
    auto token_b = create_restricted_token(request_b.policy, &error);
    auto read_only = make_sandbox_policy(SandboxMode::ReadOnly, path_to_utf8(a), {});
    auto token_read = create_restricted_token(read_only, &error);
    ASSERT_NE(token_a.get(), nullptr) << error;
    ASSERT_NE(token_b.get(), nullptr) << error;
    ASSERT_NE(token_read.get(), nullptr) << error;
    EXPECT_NE(synthetic_sid_string(request_a.policy), synthetic_sid_string(request_b.policy));
    EXPECT_NE(synthetic_sid_string(request_a.policy), synthetic_sid_string(read_only));
    EXPECT_TRUE(token_can_write(token_a.get(), a / "allowed.txt"));
    EXPECT_FALSE(token_can_write(token_a.get(), b / "denied.txt"));
    EXPECT_FALSE(token_can_write(token_a.get(), outside / "denied.txt"));
    EXPECT_FALSE(token_can_write(token_b.get(), a / "denied.txt"));
    EXPECT_FALSE(token_can_write(token_read.get(), a / "denied-readonly.txt"));
    for (const char* child : {".git/config", ".git/config.worktree", ".git/hooks/pre-commit",
                             ".git/modules/attack", ".acecode/rules/attack.rules"}) {
        EXPECT_FALSE(token_can_write(token_a.get(), a / child)) << child;
    }
    EXPECT_TRUE(token_can_open_access(token_a.get(), a / ".git/config", GENERIC_READ));
    EXPECT_TRUE(token_can_delete(token_a.get(), a / "allowed.txt"));
    // 正常宿主仍可编辑规则,沙盒初始化不应误伤当前用户。
    tree.write(a / ".acecode/rules/user.rules", "# user rule");
    EXPECT_TRUE(fs::exists(a / ".acecode/rules/user.rules"));
    EXPECT_TRUE(ensure_windows_acl_grants(request_a.policy, &error));
}

// 真机通过实际 Bash 工具创建受限子进程,验证拒绝提示及显式完整访问。
TEST(SandboxBackendWin, BashChildEnforcesBoundaryAndReportsDenial) {
    if (!probe_backend().available) GTEST_SKIP() << "restricted tokens unavailable";
    test::TempTree tree;
    ASSERT_TRUE(make_private_test_root(tree.root));
    auto workspace = tree.dir("workspace");
    auto outside = tree.dir("outside");
    tree.write(workspace / ".acecode/rules/probe.rules", "# protected");
    SandboxRuntime runtime;
    SandboxRuntimeConfig config;
    config.exclude_tmpdir = true;
    runtime.configure(config);
    ToolContext ctx;
    ctx.cwd = path_to_utf8(workspace);
    ctx.exec_sandbox = runtime.request_for(SandboxMode::WorkspaceWrite, ctx.cwd);
    ASSERT_TRUE(runtime.prepare_request(*ctx.exec_sandbox).empty());
    auto previous = environment::terminal().last();
    struct RestoreTerminal {
        decltype(previous) snapshot;
        ~RestoreTerminal() {
            if (snapshot) environment::terminal().publish(*snapshot);
            else environment::terminal().reset_for_test();
        }
    } restore{previous};
    environment::terminal().reset_for_test();
    auto bash = create_bash_tool();
    auto inside = bash.execute(nlohmann::json{{"command", "echo allowed > allowed.txt"}}.dump(), ctx);
    ASSERT_TRUE(inside.success) << inside.output << " " << inside.metadata.dump();
    EXPECT_TRUE(fs::exists(workspace / "allowed.txt"));
    auto protected_write = bash.execute(nlohmann::json{{"command", "echo denied >> .acecode\\rules\\probe.rules"}}.dump(), ctx);
    EXPECT_FALSE(protected_write.success) << protected_write.output;
    EXPECT_TRUE(fs::exists(workspace / ".acecode/rules/probe.rules"));
    const std::string command = "echo denied > \"" + path_to_utf8(outside / "denied.txt") + "\"";
    auto denied = bash.execute(nlohmann::json{{"command", command}}.dump(), ctx);
    EXPECT_FALSE(denied.success);
    EXPECT_FALSE(fs::exists(outside / "denied.txt"));
    EXPECT_TRUE(denied.metadata.value("sandbox_denied", false)) << denied.output;
    EXPECT_NE(denied.output.find("require_escalated"), std::string::npos);
    EXPECT_TRUE(denied.metadata.contains("sandbox_violation")) << denied.metadata.dump();
    ctx.exec_sandbox.reset(); // 模拟用户已批准的完整访问执行上下文。
    auto approved = bash.execute(nlohmann::json{{"command", command},
        {"with_escalated_permissions", true}, {"justification", "Write the requested external file."}}.dump(), ctx);
    EXPECT_TRUE(approved.success) << approved.output;
    EXPECT_TRUE(fs::exists(outside / "denied.txt"));
}

// 已接受的 Codex unelevated 边界,单独记录,不能把它算作删除隔离通过。
// 对照源码 dfaf451426868c22e6859f5494150fd6338c3257,详见 docs/sandbox.md。
TEST(SandboxBackendWin, DocumentsUnelevatedDeleteAndRenameLimitation) {
    if (!probe_backend().available) GTEST_SKIP() << "restricted tokens unavailable";
    test::TempTree tree;
    ASSERT_TRUE(make_private_test_root(tree.root));
    const auto workspace = tree.dir("workspace");
    const auto outside = tree.dir("outside");
    tree.write(workspace / ".acecode/rules/probe.rules", "# protected");
    tree.write(outside / "existing.txt", "outside");
    SandboxRuntime runtime;
    SandboxRuntimeConfig config;
    config.exclude_tmpdir = true;
    runtime.configure(config);
    auto request = runtime.request_for(SandboxMode::WorkspaceWrite, path_to_utf8(workspace));
    ASSERT_TRUE(runtime.prepare_request(request).empty());
    std::string error;
    auto token = create_restricted_token(request.policy, &error);
    ASSERT_NE(token.get(), nullptr) << error;
    EXPECT_FALSE(token_can_write(token.get(), outside / "new.txt"));
    EXPECT_TRUE(token_can_delete(token.get(), outside / "existing.txt"));
    EXPECT_TRUE(token_can_delete(token.get(), workspace / ".acecode/rules/probe.rules"));
    EXPECT_TRUE(token_can_rename(token.get(), workspace / ".acecode", workspace / ".acecode-old"));
    EXPECT_NE(runtime.status_text(PermissionMode::Auto, path_to_utf8(workspace), false)
        .find("delete/rename are not fully restricted"), std::string::npos);
}

// 场景:Windows 受限令牌 + network_access=false 的真实子进程(align-codex-sandboxing
// D7)。期望:子进程看到准断网环境(HTTPS_PROXY 指向死端口、NPM_CONFIG_OFFLINE)、
// PATH 最前是 ssh / scp 桩目录且 `ssh` 直接失败退出;放行网络(会话授权)后这些
// 变量不出现。
TEST(SandboxBackendWin, OfflineEnvironmentReachesRestrictedChild) {
    if (!probe_backend().available) GTEST_SKIP() << "restricted tokens unavailable";
    test::TempTree tree;
    ASSERT_TRUE(make_private_test_root(tree.root));
    const auto workspace = tree.dir("workspace");
    SandboxRuntime runtime;
    SandboxRuntimeConfig config;
    config.exclude_tmpdir = true;
    config.acecode_home = path_to_utf8(tree.dir("home"));
    runtime.configure(config);
    ToolContext ctx;
    ctx.cwd = path_to_utf8(workspace);
    ctx.exec_sandbox = runtime.request_for(SandboxMode::WorkspaceWrite, ctx.cwd);
    ASSERT_TRUE(runtime.prepare_request(*ctx.exec_sandbox).empty());
    EXPECT_FALSE(ctx.exec_sandbox->denybin_dir.empty());
    auto previous = environment::terminal().last();
    struct RestoreTerminal {
        decltype(previous) snapshot;
        ~RestoreTerminal() {
            if (snapshot) environment::terminal().publish(*snapshot);
            else environment::terminal().reset_for_test();
        }
    } restore{previous};
    environment::terminal().reset_for_test();
    auto bash = create_bash_tool();
    auto env = bash.execute(nlohmann::json{{"command", "echo %HTTPS_PROXY% %NPM_CONFIG_OFFLINE% %SBX_NONET_ACTIVE%"}}.dump(), ctx);
    ASSERT_TRUE(env.success) << env.output;
    EXPECT_NE(env.output.find("http://127.0.0.1:9 true 1"), std::string::npos) << env.output;
    auto ssh = bash.execute(nlohmann::json{{"command", "ssh example.com"}}.dump(), ctx);
    EXPECT_FALSE(ssh.success) << ssh.output;
    AdditionalPermissions network;
    network.network = true;
    ctx.exec_sandbox = runtime.request_for(SandboxMode::WorkspaceWrite, ctx.cwd, &network);
    ASSERT_TRUE(runtime.prepare_request(*ctx.exec_sandbox).empty());
    auto online = bash.execute(nlohmann::json{{"command", "echo [%HTTPS_PROXY%]"}}.dump(), ctx);
    ASSERT_TRUE(online.success) << online.output;
    EXPECT_EQ(online.output.find("127.0.0.1:9"), std::string::npos) << online.output;
}

// 场景:Job Object 杀树(D7)。期望:cmd 启动一个 30 秒的 ping 孙进程后,
// TerminateJobObject 让整棵树在 2 秒内退出;不设 KILL_ON_JOB_CLOSE,所以只
// 关闭 Job 句柄不会杀进程。
TEST(SandboxBackendWin, ProcessTreeJobTerminatesGrandchildren) {
    auto job = create_process_tree_job();
    ASSERT_TRUE(job);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring command = L"cmd.exe /d /c ping -n 30 127.0.0.1 > nul";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    ASSERT_TRUE(CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi));
    const bool assigned = assign_process_to_job(job, pi.hProcess);
    ResumeThread(pi.hThread);
    if (!assigned) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess); job.reset();
        GTEST_SKIP() << "nested job assignment refused on this host";
    }
    Sleep(500);
    EXPECT_EQ(WaitForSingleObject(pi.hProcess, 0), WAIT_TIMEOUT) << "命令应仍在跑";
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    ASSERT_TRUE(QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr));
    EXPECT_GE(accounting.ActiveProcesses, 2u) << "cmd + ping 都应在 Job 里";
    terminate_job_tree(job);
    EXPECT_EQ(WaitForSingleObject(pi.hProcess, 2000), WAIT_OBJECT_0);
    ASSERT_TRUE(QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr));
    EXPECT_EQ(accounting.ActiveProcesses, 0u);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    job.reset();
}

// 真机完整链路:模型调用 -> AgentLoop 审批 -> Bash 受限/完整访问子进程。
// LLM 使用确定性 provider,Shell、git、令牌和文件系统全部真实执行。
// 同时覆盖工具重写关闭与 bash 改名为 run_shell,两种入口必须遵守相同边界。
class SandboxBackendWinAgentLoop : public testing::TestWithParam<bool> {};

TEST_P(SandboxBackendWinAgentLoop, AutoAgentLoopRunsGitAndRequiresApprovalForEscalationAndDanger) {
    if (!probe_backend().available) GTEST_SKIP() << "restricted tokens unavailable";
    ScopedModelToolNameMappings mappings(GetParam()
        ? ToolProtocolNameMappings{{"bash", "run_shell"}} : ToolProtocolNameMappings{});
    const std::string model_tool_name = GetParam() ? "run_shell" : "bash";
    test::TempTree tree;
    ASSERT_TRUE(make_private_test_root(tree.root));
    const auto workspace = tree.dir("workspace");
    const auto outside = tree.dir("outside");
    auto previous = environment::terminal().last();
    struct RestoreTerminal {
        decltype(previous) snapshot;
        ~RestoreTerminal() {
            if (snapshot) environment::terminal().publish(*snapshot);
            else environment::terminal().reset_for_test();
        }
    } restore{previous};
    environment::terminal().reset_for_test();
    auto bash = create_bash_tool();
    ToolContext init_context;
    init_context.cwd = path_to_utf8(workspace);
    const auto initialized = bash.execute(nlohmann::json{
        {"command", "git -c init.templateDir= -c init.defaultBranch=main init -q"}}.dump(), init_context);
    ASSERT_TRUE(initialized.success) << initialized.output;
    std::vector<ToolResult> results;
    std::vector<SandboxMode> execution_modes;
    const auto real_execute = bash.execute;
    bash.execute = [&](const std::string& args, const ToolContext& ctx) {
        execution_modes.push_back(ctx.exec_sandbox ? ctx.exec_sandbox->policy.mode : SandboxMode::FullAccess);
        auto result = real_execute(args, ctx);
        results.push_back(result);
        return result;
    };
    ToolExecutor tools;
    tools.register_tool(bash);
    PermissionManager permissions;
    permissions.set_mode(PermissionMode::Auto);
    std::vector<nlohmann::json> prompts;
    PermissionResult answer = PermissionResult::Allow;
    std::mutex mutex;
    std::condition_variable cv;
    bool busy = false;
    AgentCallbacks callbacks;
    callbacks.on_busy_changed = [&](bool value) {
        std::lock_guard<std::mutex> lock(mutex);
        busy = value;
        if (!busy) cv.notify_all();
    };
    callbacks.on_tool_confirm = [&](const std::string& tool_name, const std::string& args) {
        EXPECT_EQ(tool_name, "bash");
        prompts.push_back(nlohmann::json::parse(args));
        return answer;
    };
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    AgentLoop loop(
        acecode_test::AgentLoopFixture::dependencies([&]() -> std::shared_ptr<LlmProvider> { return provider; }, tools, callbacks, permissions),
        acecode_test::AgentLoopFixture::configuration(path_to_utf8(workspace)));
    loop.start();
    SandboxConfig config;
    config.exclude_tmpdir = true;
    loop.set_sandbox_config(config);
    loop.set_exec_rules({});
    auto run = [&](nlohmann::json args) {
        provider->push_tool_call(model_tool_name, args.dump(), "native-" + std::to_string(provider->turn_count()));
        provider->push_text("done");
        { std::lock_guard<std::mutex> lock(mutex); busy = true; }
        loop.submit("run");
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(15), [&] { return !busy; });
    };
    ASSERT_TRUE(run({{"command", "git status --short"}}));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(prompts.empty());
    EXPECT_TRUE(results.back().success) << results.back().output;
    EXPECT_EQ(execution_modes.back(), SandboxMode::WorkspaceWrite);
    const auto advertised_tools = provider->tools_for_turn(0);
    ASSERT_EQ(advertised_tools.size(), 1u);
    EXPECT_EQ(advertised_tools.front().name, model_tool_name);
    const auto command = "echo denied > \"" + path_to_utf8(outside / "probe.txt") + "\"";
    ASSERT_TRUE(run({{"command", command}}));
    ASSERT_EQ(results.size(), 2u);
    EXPECT_TRUE(prompts.empty());
    EXPECT_FALSE(results.back().success);
    EXPECT_TRUE(results.back().metadata.value("sandbox_denied", false));
    EXPECT_FALSE(fs::exists(outside / "probe.txt"));
    ASSERT_TRUE(run({{"command", command}, {"with_escalated_permissions", true},
        {"justification", "Write the requested file outside this workspace."}}));
    ASSERT_EQ(prompts.size(), 1u);
    EXPECT_EQ(prompts.back()["permission"]["reason"], "escalation_requested");
    EXPECT_EQ(prompts.back()["permission"]["sandbox"], "full-access");
    ASSERT_EQ(results.size(), 3u);
    EXPECT_TRUE(results.back().success) << results.back().output;
    EXPECT_EQ(execution_modes.back(), SandboxMode::FullAccess);
    EXPECT_TRUE(fs::exists(outside / "probe.txt"));
    answer = PermissionResult::Deny;
    ASSERT_TRUE(run({{"command", "rm -rf output"}}));
    ASSERT_EQ(prompts.size(), 2u);
    EXPECT_EQ(prompts.back()["permission"]["reason"], "dangerous_command");
    EXPECT_EQ(results.size(), 3u);

    ExecRules forbidden_rules;
    PrefixRule forbidden;
    forbidden.pattern = {{"git"}, {"status"}};
    forbidden.decision = RuleDecision::Forbidden;
    forbidden_rules.add_rule(std::move(forbidden));
    loop.set_exec_rules(std::move(forbidden_rules));
    answer = PermissionResult::Allow;
    ASSERT_TRUE(run({{"command", "git status --short"}}));
    EXPECT_EQ(prompts.size(), 2u);
    EXPECT_EQ(results.size(), 3u);
}

INSTANTIATE_TEST_SUITE_P(SandboxToolNames, SandboxBackendWinAgentLoop,
    testing::Bool(), [](const testing::TestParamInfo<bool>& info) {
        return info.param ? "Rewritten" : "Native";
    });

TEST(SandboxBackendWin, JobRaiiKeepsExistingLifetimeFlags) {
    // 场景:通过 RAII 工厂创建 Job。期望仍不启用关闭杀树和静默脱离;
    // 这锁定重构前的沙盒行为,避免复用 Computer Use 的 Job 策略。
    auto job = create_process_tree_job();
    ASSERT_TRUE(job);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    ASSERT_TRUE(QueryInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
        &limits, sizeof(limits), nullptr));
    EXPECT_EQ(limits.BasicLimitInformation.LimitFlags &
        (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK), 0u);
}

#endif
