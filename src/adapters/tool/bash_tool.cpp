#include "bash_tool.hpp"
#include "llm/tool_icons.hpp"
#include "environment/shell_command_line.hpp"
#include "environment/terminal_runtime.hpp"
#include "utils/logger.hpp"
#include "utils/encoding.hpp"
#include "utils/stream_processing.hpp"
#include "tool/tool_errors.hpp"
#include "utils/utf8_path.hpp"
#include "sandbox/exec_permission.hpp"
#include "sandbox/sandbox_backend.hpp"
#include "sandbox/sandbox_denial.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <deque>
#include <chrono>
#include <filesystem>
#include <thread>
#include <atomic>
#include <set>
#include <system_error>
#include <cstdlib>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
extern char** environ;
#endif

namespace acecode {

static constexpr int DEFAULT_TIMEOUT_MS = 120000;    // 2 minutes
static constexpr size_t MAX_OUTPUT_SIZE = 100 * 1024; // 100KB

static std::string ascii_lower_copy(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

static bool is_root_script_extension(const std::filesystem::path& path) {
    std::string ext = ascii_lower_copy(path_to_utf8(path.extension()));
    return ext == ".py" || ext == ".ps1" || ext == ".js" ||
           ext == ".bat" || ext == ".cmd";
}

static std::set<std::string> list_root_script_files(const std::filesystem::path& root) {
    std::set<std::string> out;
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec) || ec) return out;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        const auto path = entry.path();
        if (is_root_script_extension(path)) {
            out.insert(path_to_utf8(path.filename()));
        }
    }
    return out;
}

static std::vector<std::string> new_root_script_files(
    const std::set<std::string>& before,
    const std::set<std::string>& after) {
    std::vector<std::string> created;
    for (const auto& name : after) {
        if (before.find(name) == before.end()) created.push_back(name);
    }
    return created;
}

static void append_root_script_warning(ToolResult& result,
                                       const std::vector<std::string>& created,
                                       const std::string& scratch_dir) {
    if (created.empty()) return;
    if (!result.output.empty() && result.output.back() != '\n') result.output += "\n";
    result.output += "[Warning] Shell command created script file(s) in the workspace root: ";
    for (size_t i = 0; i < created.size(); ++i) {
        if (i > 0) result.output += ", ";
        result.output += created[i];
    }
    result.output += ". Temporary helper scripts should be written under ACECODE_TMPDIR";
    if (!scratch_dir.empty()) result.output += " (" + scratch_dir + ")";
    result.output += ".";
}

#ifdef _WIN32
static std::wstring env_name_prefix(const std::wstring& name) {
    return name + L"=";
}

static bool starts_with_env_name_ci(const std::wstring& value,
                                    const std::wstring& prefix) {
    if (value.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        wchar_t a = value[i];
        wchar_t b = prefix[i];
        if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b - L'A' + L'a');
        if (a != b) return false;
    }
    return true;
}

static std::vector<wchar_t> build_environment_block_with_var(
    const std::vector<std::pair<std::string, std::string>>& variables) {
    std::vector<std::wstring> entries;
    LPWCH env = GetEnvironmentStringsW();
    if (env) {
        for (LPWCH p = env; *p != L'\0'; ) {
            std::wstring entry(p);
            const size_t entry_len = entry.size();
            bool replaced = false;
            for (const auto& variable : variables) {
                if (starts_with_env_name_ci(entry, env_name_prefix(utf8_to_wide(variable.first)))) replaced = true;
            }
            if (!replaced) {
                entries.push_back(std::move(entry));
            }
            p += entry_len + 1;
        }
        FreeEnvironmentStringsW(env);
    }
    for (const auto& variable : variables) entries.push_back(utf8_to_wide(variable.first + "=" + variable.second));
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });

    std::vector<wchar_t> block;
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}
#endif

// Normalize Windows CRLF to LF before feeding the line state machine, so the
// \r does not clobber `current_line` mid-way through a proper newline.
#ifdef _WIN32
static std::string normalize_crlf(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') {
            out += '\n';
            i++;
        } else {
            out += s[i];
        }
    }
    return out;
}
#endif

static ToolResult execute_bash(const std::string& arguments_json, const ToolContext& ctx) {
    std::string command;
    int timeout_ms = DEFAULT_TIMEOUT_MS;
    std::string cwd;
    std::vector<std::string> stdin_inputs;

    try {
        auto args = nlohmann::json::parse(arguments_json);
        const auto error = sandbox::validate_escalation_arguments(args);
        if (!error.empty()) return ToolResult{"[Error] " + error, false};
        command = args.value("command", "");
        timeout_ms = args.value("timeout_ms", DEFAULT_TIMEOUT_MS);
        cwd = args.value("cwd", "");
        if (args.contains("stdin_inputs") && args["stdin_inputs"].is_array()) {
            for (const auto& el : args["stdin_inputs"]) {
                if (el.is_string()) stdin_inputs.push_back(el.get<std::string>());
            }
        }
    } catch (...) {
        return ToolResult{"[Error] Failed to parse tool arguments.", false};
    }

    if (command.empty()) {
        return ToolResult{"[Error] No command provided.", false};
    }

    if (cwd.empty() && !ctx.cwd.empty()) {
        cwd = ctx.cwd;
    }

    const bool sandboxed = ctx.exec_sandbox && ctx.exec_sandbox->policy.mode != sandbox::SandboxMode::FullAccess;
    auto sandbox_failure = [&](const std::string& message) {
        ToolResult result{"[Sandbox unavailable] " + message + ". The command was not executed.", false};
        result.metadata["sandbox_unavailable"] = true;
        // 单行原因给 AgentLoop 记进 /sandbox 状态与 system prompt;正文里的
        // 引导句只给模型看。
        result.metadata["sandbox_unavailable_reason"] = message;
        result.output += "\nRequest with_escalated_permissions=true with a non-empty justification if full access is needed.";
        return result;
    };
    if (sandboxed && ctx.exec_sandbox->backend == sandbox::BackendKind::None) {
        return sandbox_failure("No sandbox backend selected");
    }
    std::vector<std::pair<std::string, std::string>> child_environment;
    if (!ctx.scratch_dir.empty()) child_environment.emplace_back("ACECODE_TMPDIR", ctx.scratch_dir);
    if (sandboxed) {
        const auto additions = sandbox::sandbox_environment(ctx.exec_sandbox->backend,
            ctx.exec_sandbox->policy, ctx.exec_sandbox->network_enforced,
            ctx.exec_sandbox->denybin_dir);
        child_environment.insert(child_environment.end(), additions.begin(), additions.end());
    }

    auto t_start = std::chrono::steady_clock::now();
    auto make_summary = [&](const std::string& cmd, long long duration_ms,
                            size_t total_bytes_out, int exit_code,
                            bool is_success, bool was_truncated,
                            bool was_aborted, bool was_timed_out) {
        ToolSummary s;
        s.verb = "Ran";
        s.object = truncate_utf8_prefix(cmd, 60);
        s.metrics.emplace_back("time", format_duration_compact(duration_ms));
        s.metrics.emplace_back("bytes", format_bytes_compact(total_bytes_out));
        if (sandboxed) s.metrics.emplace_back("sandbox", sandbox::sandbox_mode_name(ctx.exec_sandbox->policy.mode));
        if (!is_success && exit_code != 0) {
            s.metrics.emplace_back("exit", std::to_string(exit_code));
        }
        if (was_truncated) s.metrics.emplace_back("truncated", "true");
        if (was_aborted) s.metrics.emplace_back("aborted", "true");
        if (was_timed_out) s.metrics.emplace_back("timeout", "true");
        s.icon = tool_icon("bash");
        return s;
    };

    const std::string scratch_dir = ctx.scratch_dir;
    if (scratch_dir.empty() &&
        ToolContext::references_scratch_path_alias(command)) {
        ToolResult r{
            ToolErrors::invalid_parameter(
                "command",
                "references ACECODE_TMPDIR, but no session scratch directory is available. "
                "Retry in an active session or use an explicit path."),
            false};
        r.summary = make_summary(command, 0, 0, -1, false,
                                 false, false, false);
        return r;
    }

    LOG_INFO("bash: cmd=" + log_truncate(command, 200) + " cwd=" + cwd +
             " timeout=" + std::to_string(timeout_ms) +
             " stdin_inputs=" + std::to_string(stdin_inputs.size()));

#ifdef _WIN32
    std::filesystem::path cwd_path = cwd.empty()
        ? std::filesystem::path{}
        : std::filesystem::path(utf8_to_wide(cwd));
#else
    std::filesystem::path cwd_path = cwd;
#endif

    if (!cwd.empty() && !std::filesystem::is_directory(cwd_path)) {
        ToolResult r{"[Error] Working directory does not exist: " + cwd, false};
        r.summary = make_summary(command, 0, 0, -1, false, false, false, false);
        return r;
    }

    std::error_code cwd_ec;
    const std::filesystem::path root_for_script_warning = cwd_path.empty()
        ? std::filesystem::current_path(cwd_ec)
        : cwd_path;
    const auto root_scripts_before = list_root_script_files(root_for_script_warning);

    if (!scratch_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path_from_utf8(scratch_dir), ec);
        if (ec) {
            ToolResult r{"[Error] Failed to create ACECODE_TMPDIR: " + scratch_dir +
                         " (" + ec.message() + ")", false};
            r.summary = make_summary(command, 0, 0, -1, false, false, false, false);
            return r;
        }
    }

    const auto terminal_snapshot = acecode::environment::terminal().last();
    if (terminal_snapshot && !terminal_snapshot->resolved.usable) {
        return ToolResult{"[Error] No usable terminal. Check Settings > Configuration: " +
                          terminal_snapshot->resolved.fallback_reason, false};
    }

    // Shared streaming state across both OS branches.
    std::string full_output;
    std::string current_line;
    std::deque<std::string> tail_lines;
    int total_lines = 0;

    // Incremental decoder: turns raw subprocess bytes into valid UTF-8, holding
    // back any trailing partial character across chunk boundaries. On Windows it
    // falls back to the console codepage (e.g. GBK/CP936) so legacy cmd.exe
    // output decodes correctly instead of corrupting JSON serialization with a
    // stray byte like 0xF7. Output here is always safe to stream and to embed in
    // the tool result JSON.
    IncrementalTextDecoder decoder;

    // Emit already-decoded UTF-8 text into the output/stream pipeline.
    auto emit_text = [&](std::string text) {
        if (text.empty()) return;
#ifdef _WIN32
        text = normalize_crlf(text);
#endif
        std::string clean = strip_ansi(text);
        if (clean.empty()) return;
        full_output += clean;
        feed_line_state(clean, current_line, tail_lines, total_lines);
        if (ctx.stream) ctx.stream(clean);
    };

    auto process_raw = [&](const char* data, size_t len) {
        if (len == 0) return;
        emit_text(decoder.push(data, len));
    };

    // Drain any bytes the decoder is still holding (called once after EOF).
    auto flush_decoder = [&]() {
        emit_text(decoder.flush());
    };

    bool aborted = false;
    bool timed_out = false;

#ifdef _WIN32
    // stdin_inputs not implemented on Windows in this release; it's accepted
    // but silently ignored (see proposal — deferred to future MCP work).
    (void)stdin_inputs;

    if (sandboxed) {
        if (ctx.exec_sandbox->backend == sandbox::BackendKind::WindowsMxc) {
            // MXC 口子(align-codex-sandboxing D9):探测恒不可用,正常不会走到这里;
            // 真接入时把 SandboxPolicy 翻译成 MXC ExecutionRequest 后在此启动。
            return sandbox_failure("MXC backend is not bundled in this build");
        }
        if (ctx.exec_sandbox->backend != sandbox::BackendKind::WindowsRestrictedToken) {
            return sandbox_failure("Invalid backend for Windows");
        }
        std::string error;
        if (!sandbox::ensure_windows_acl_grants(ctx.exec_sandbox->policy, &error)) return sandbox_failure(error);
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return ToolResult{"[Error] Failed to create pipe.", false};
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;

    PROCESS_INFORMATION pi = {};

    // 默认终端(openspec: agent-default-terminal):运行时已解析出可用终端就按其家族
    // 构造命令行(cmd / PowerShell EncodedCommand / Git Bash -c);未 bootstrap 或
    // 全部候选不可用时维持改动前的 `cmd.exe /c`。
    std::string windows_command_line = "cmd.exe /d /c " + command;
    if (terminal_snapshot) {
        windows_command_line = acecode::environment::build_shell_command_line(
            terminal_snapshot->resolved, command).windows_command_line;
    }
    std::wstring full_cmd = utf8_to_wide(windows_command_line);
    std::vector<wchar_t> full_cmd_buffer(full_cmd.begin(), full_cmd.end());
    full_cmd_buffer.push_back(L'\0');

    std::wstring wide_cwd;
    if (!cwd.empty()) {
        wide_cwd = utf8_to_wide(cwd);
    }
    const wchar_t* cwd_ptr = wide_cwd.empty() ? nullptr : wide_cwd.c_str();

    std::vector<wchar_t> env_block;
    void* env_ptr = nullptr;
    if (!child_environment.empty()) {
        env_block = build_environment_block_with_var(child_environment);
        env_ptr = env_block.data();
    }

    BOOL ok = FALSE;
    std::string sandbox_error;
    // 挂起启动 → 挂进 Job Object → 再放行(align-codex-sandboxing D7):超时 / 中止时
    // TerminateJobObject 能杀掉整棵进程树(cmd → node → 子工具),而不是只杀 cmd。
    // Job 不设 KILL_ON_JOB_CLOSE,正常结束关闭句柄不影响有意留下的后台孙进程。
    const DWORD creation_flags = CREATE_NO_WINDOW | CREATE_SUSPENDED |
                                 (env_ptr ? CREATE_UNICODE_ENVIRONMENT : 0);
    if (sandboxed) {
        HANDLE token = static_cast<HANDLE>(sandbox::create_restricted_token(ctx.exec_sandbox->policy, &sandbox_error));
        if (token) {
            std::wstring desktop = L"winsta0\\default";
            si.lpDesktop = desktop.data();
            ok = CreateProcessAsUserW(token, nullptr, full_cmd_buffer.data(), nullptr, nullptr, TRUE,
                creation_flags, env_ptr, cwd_ptr, &si, &pi);
            if (!ok) sandbox_error = "CreateProcessAsUserW failed: " + std::to_string(GetLastError());
            CloseHandle(token);
        }
    } else {
        ok = CreateProcessW(nullptr, full_cmd_buffer.data(), nullptr, nullptr, TRUE,
            creation_flags, env_ptr, cwd_ptr, &si, &pi);
    }

    CloseHandle(hWritePipe);

    if (!ok) {
        CloseHandle(hReadPipe);
        if (sandboxed) return sandbox_failure(sandbox_error);
        return ToolResult{"[Error] Failed to execute command.", false};
    }

    void* process_job = sandbox::create_process_tree_job();
    if (process_job && !sandbox::assign_process_to_job(process_job, pi.hProcess)) {
        // 嵌套 Job 被拒(旧系统 / 受限宿主 Job):退回只杀直接子进程的旧行为。
        sandbox::close_job(process_job);
        process_job = nullptr;
    }
    ResumeThread(pi.hThread);
    auto kill_process_tree = [&]() {
        if (process_job) sandbox::terminate_job_tree(process_job);
        else TerminateProcess(pi.hProcess, 1);
    };

    char buffer[4096];
    DWORD bytes_read;
    auto start = std::chrono::steady_clock::now();

    while (true) {
        DWORD avail = 0;
        PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &avail, nullptr);
        if (avail > 0) {
            if (ReadFile(hReadPipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
                process_raw(buffer, bytes_read);
            }
        }

        DWORD wait_result = WaitForSingleObject(pi.hProcess, 0);
        if (wait_result == WAIT_OBJECT_0) {
            // Process finished - drain remaining output.
            // **不能**裸 ReadFile:Windows 管道在同步模式下,只要还有任何进程
            // 持有写端,ReadFile 在数据耗尽时会**永久阻塞**。直接子进程(cmd.exe)
            // 已退出,但孙进程(典型场景:`agent-browser open` 启动的 Chrome,
            // 后台 daemon,detached 的 powershell 子任务……)如果继承了同一根管道,
            // 仍 hold 着写端,ReadFile 永远等不到 EOF。这条循环之外才有 abort /
            // timeout 检查,一旦在这里 block 住,Esc / Ctrl+C / 120s 超时全部失效,
            // TUI 表现为"卡死"。修复:沿用下方 abort / timeout drain 一样的模式,
            // 只 drain 当前可读字节,管道里没数据立刻 break,把后台孙进程的命运
            // 交给 OS。
            while (true) {
                DWORD drain_avail = 0;
                if (!PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &drain_avail, nullptr)) break;
                if (drain_avail == 0) break;
                if (!ReadFile(hReadPipe, buffer, sizeof(buffer), &bytes_read, nullptr)) break;
                if (bytes_read == 0) break;
                process_raw(buffer, bytes_read);
            }
            break;
        }

        // Abort check
        if (ctx.abort_flag && ctx.abort_flag->load()) {
            kill_process_tree();
            WaitForSingleObject(pi.hProcess, 1000);
            while (PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                if (ReadFile(hReadPipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
                    process_raw(buffer, bytes_read);
                } else break;
            }
            aborted = true;
            break;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) {
            kill_process_tree();
            WaitForSingleObject(pi.hProcess, 1000);
            while (PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                if (ReadFile(hReadPipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
                    process_raw(buffer, bytes_read);
                } else break;
            }
            timed_out = true;
            break;
        }

        Sleep(10);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    sandbox::close_job(process_job);
    CloseHandle(hReadPipe);

#else
    // POSIX: fork/exec with streaming, stdin injection, and abort support.
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        return ToolResult{"[Error] Failed to create pipe.", false};
    }

    // stdin pipe (read by child, written by parent). Always created so the
    // child has a well-defined stdin; if stdin_inputs is empty we close the
    // write end immediately so the child sees EOF.
    int stdin_pipefd[2];
    if (pipe(stdin_pipefd) == -1) {
        close(pipefd[0]); close(pipefd[1]);
        return ToolResult{"[Error] Failed to create stdin pipe.", false};
    }

    // 默认终端(openspec: agent-default-terminal):argv 在 fork **之前**准备好 ——
    // daemon 是多线程进程,子进程里不能再分配内存(malloc 锁可能被别的线程持有)。
    // 运行时没有可用终端时维持改动前的 /bin/sh -c。
    std::string shell_program = "/bin/sh";
    std::vector<std::string> shell_argv = {"/bin/sh", "-c", command};
    if (terminal_snapshot) {
        auto line = acecode::environment::build_shell_command_line(terminal_snapshot->resolved, command);
        shell_program = line.program;
        shell_argv = line.argv;
    }
    if (sandboxed) {
        std::vector<std::string> prefix;
        if (ctx.exec_sandbox->backend == sandbox::BackendKind::MacosSeatbelt) {
            prefix = sandbox::build_seatbelt_argv(ctx.exec_sandbox->policy);
        } else if (ctx.exec_sandbox->backend == sandbox::BackendKind::LinuxBwrap) {
            if (ctx.exec_sandbox->backend_executable.empty()) {
                close(pipefd[0]); close(pipefd[1]); close(stdin_pipefd[0]); close(stdin_pipefd[1]);
                return sandbox_failure("No verified bubblewrap executable");
            }
            prefix = sandbox::build_bwrap_argv(ctx.exec_sandbox->policy);
            prefix.front() = ctx.exec_sandbox->backend_executable;
        } else {
            close(pipefd[0]); close(pipefd[1]); close(stdin_pipefd[0]); close(stdin_pipefd[1]);
            return sandbox_failure("Invalid backend for POSIX");
        }
        prefix.insert(prefix.end(), shell_argv.begin(), shell_argv.end());
        shell_argv = std::move(prefix);
        shell_program = shell_argv.front();
    }
    std::vector<char*> shell_argv_c;
    shell_argv_c.reserve(shell_argv.size() + 1);
    for (auto& arg : shell_argv) shell_argv_c.push_back(const_cast<char*>(arg.c_str()));
    shell_argv_c.push_back(nullptr);

    // 在父进程准备环境和可执行路径。多线程 daemon fork 后只能调用异步信号安全 API。
    std::vector<std::string> environment_storage;
    for (char** variable = environ; variable && *variable; ++variable) {
        std::string value(*variable);
        const auto separator = value.find('=');
        const auto name = value.substr(0, separator);
        const bool overridden = std::any_of(child_environment.begin(), child_environment.end(),
            [&](const auto& entry) { return entry.first == name; });
        if (!overridden) environment_storage.push_back(std::move(value));
    }
    for (const auto& entry : child_environment) environment_storage.push_back(entry.first + "=" + entry.second);
    std::vector<char*> environment_pointers;
    for (auto& variable : environment_storage) environment_pointers.push_back(variable.data());
    environment_pointers.push_back(nullptr);
    if (shell_program.find('/') == std::string::npos) {
        const char* raw_path = std::getenv("PATH");
        const std::string search_path = raw_path ? raw_path : "/usr/bin:/bin";
        std::size_t start = 0;
        while (start <= search_path.size()) {
            const auto end = search_path.find(':', start);
            auto directory = search_path.substr(start, end == std::string::npos ? end : end - start);
            if (directory.empty()) directory = cwd.empty() ? "." : cwd;
            const auto candidate = directory + "/" + shell_program;
            if (access(candidate.c_str(), X_OK) == 0) { shell_program = candidate; break; }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    // CLOEXEC 让父进程区分后端根本没启动与已运行命令自行返回 127。
    int exec_error_pipe[2];
    if (pipe(exec_error_pipe) == -1) {
        close(pipefd[0]); close(pipefd[1]); close(stdin_pipefd[0]); close(stdin_pipefd[1]);
        return ToolResult{"[Error] Failed to create launch status pipe.", false};
    }
    if (fcntl(exec_error_pipe[1], F_SETFD, FD_CLOEXEC) == -1 ||
        fcntl(exec_error_pipe[0], F_SETFL, O_NONBLOCK) == -1) {
        close(pipefd[0]); close(pipefd[1]); close(stdin_pipefd[0]); close(stdin_pipefd[1]);
        close(exec_error_pipe[0]); close(exec_error_pipe[1]);
        return ToolResult{"[Error] Failed to configure launch status pipe.", false};
    }
    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]); close(pipefd[1]);
        close(stdin_pipefd[0]); close(stdin_pipefd[1]);
        close(exec_error_pipe[0]); close(exec_error_pipe[1]);
        return ToolResult{"[Error] Failed to fork.", false};
    }

    if (pid == 0) {
        // Child
        close(exec_error_pipe[0]);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        close(stdin_pipefd[1]);
        dup2(stdin_pipefd[0], STDIN_FILENO);
        close(stdin_pipefd[0]);

        // New process group so parent can signal the whole tree
        setpgid(0, 0);

        if (!cwd.empty()) {
            if (chdir(cwd.c_str()) != 0) {
                const int launch_error = errno;
                (void)write(exec_error_pipe[1], &launch_error, sizeof(launch_error));
                _exit(127);
            }
        }
        execve(shell_program.c_str(), shell_argv_c.data(), environment_pointers.data());
        const int launch_error = errno;
        (void)write(exec_error_pipe[1], &launch_error, sizeof(launch_error));
        _exit(127);
    }

    // Parent
    close(exec_error_pipe[1]);
    close(pipefd[1]);
    close(stdin_pipefd[0]);

    // stdin writer thread (only if we have inputs to send)
    std::thread stdin_writer;
    if (!stdin_inputs.empty()) {
        int write_fd = stdin_pipefd[1];
        stdin_writer = std::thread([write_fd, inputs = stdin_inputs]() {
            for (const auto& line : inputs) {
                std::string data = line + "\n";
                const char* p = data.c_str();
                size_t remaining = data.size();
                while (remaining > 0) {
                    ssize_t n = write(write_fd, p, remaining);
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        return; // pipe closed (child exited) or other error
                    }
                    p += n;
                    remaining -= n;
                }
            }
            close(write_fd);
        });
    } else {
        // No inputs: close immediately so child sees EOF on stdin
        close(stdin_pipefd[1]);
    }

    char buffer[4096];
    ssize_t n;
    auto start = std::chrono::steady_clock::now();

    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    int status = 0;
    while (true) {
        n = read(pipefd[0], buffer, sizeof(buffer));
        if (n > 0) {
            process_raw(buffer, n);
        }

        int wr = waitpid(pid, &status, WNOHANG);
        if (wr == pid) {
            while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
                process_raw(buffer, n);
            }
            break;
        }

        if (ctx.abort_flag && ctx.abort_flag->load()) {
            kill(-pid, SIGTERM);
            // Give it up to 500ms to exit gracefully
            for (int i = 0; i < 50; ++i) {
                if (waitpid(pid, &status, WNOHANG) == pid) { aborted = true; break; }
                usleep(10000);
            }
            if (!aborted) {
                kill(-pid, SIGKILL);
                waitpid(pid, &status, 0);
                aborted = true;
            }
            while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
                process_raw(buffer, n);
            }
            break;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) {
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
                process_raw(buffer, n);
            }
            break;
        }

        usleep(10000); // 10ms
    }

    close(pipefd[0]);
    // If writer thread still running, close the write end to make it exit
    // (the child is dead; further writes would EPIPE).
    if (stdin_writer.joinable()) {
        // Not safe to close twice — the thread itself closes after finishing.
        stdin_writer.join();
    }

    int launch_error = 0;
    ssize_t launch_status;
    do { launch_status = read(exec_error_pipe[0], &launch_error, sizeof(launch_error)); }
    while (launch_status < 0 && errno == EINTR);
    close(exec_error_pipe[0]);
    if (launch_status == sizeof(launch_error) && launch_error != 0) {
        const auto message = "Cannot start the shell/backend: " +
            std::error_code(launch_error, std::generic_category()).message();
        return sandboxed ? sandbox_failure(message) : ToolResult{"[Error] " + message, false};
    }
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) :
                   (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
#endif

    // EOF reached: drain any bytes the decoder held back (incomplete trailing
    // character or undecodable tail) so they are not silently dropped.
    flush_decoder();

    // Flush any trailing partial line into full_output and tail_lines so it's
    // visible even without a trailing newline.
    if (!current_line.empty()) {
        full_output += current_line;
        tail_lines.push_back(current_line);
        while (tail_lines.size() > 5) tail_lines.pop_front();
        current_line.clear();
    }

    const size_t raw_bytes = full_output.size();

    // AgentLoop 会把大结果交给 tool_result_storage 落盘并生成 preview,所以
    // 模型驱动路径必须保留完整输出;直接单测/嵌入式调用默认仍走旧的 100KB 保护。
    const bool should_truncate_inline = !ctx.preserve_full_output;

    // Head+tail truncation: keep first 40% and last 60% of MAX_OUTPUT_SIZE,
    // joined by a one-line marker reporting the omitted byte count. This
    // preserves early context (e.g. build args, cwd, path) which a pure
    // tail-only policy loses.
    bool was_truncated = false;
    if (should_truncate_inline && full_output.size() > MAX_OUTPUT_SIZE) {
        const size_t head_cap = static_cast<size_t>(MAX_OUTPUT_SIZE * 0.4);
        const size_t tail_cap = MAX_OUTPUT_SIZE - head_cap; // ~60%
        const size_t omitted = full_output.size() - head_cap - tail_cap;
        std::string head = full_output.substr(0, head_cap);
        std::string tail = full_output.substr(full_output.size() - tail_cap);
        // Ensure the marker sits on its own line.
        if (!head.empty() && head.back() != '\n') head += "\n";
        full_output = head + "[... " + std::to_string(omitted) + " bytes omitted ...]\n" + tail;
        was_truncated = true;
    }

    full_output = ensure_utf8(full_output);

    auto t_end = std::chrono::steady_clock::now();
    long long duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        t_end - t_start).count();

    if (aborted) {
        if (full_output.empty() || full_output.back() != '\n') full_output += "\n";
        full_output += "[Aborted]";
        ToolResult r{full_output, false};
        r.summary = make_summary(command, duration_ms, raw_bytes, -1, false,
                                 was_truncated, true, false);
        append_root_script_warning(r,
            new_root_script_files(root_scripts_before,
                                  list_root_script_files(root_for_script_warning)),
            scratch_dir);
        return r;
    }
    if (timed_out) {
        ToolResult r{full_output + "\n[Error] Command timed out after " +
            std::to_string(timeout_ms / 1000) + " seconds.", false};
        r.summary = make_summary(command, duration_ms, raw_bytes, -1, false,
                                 was_truncated, false, true);
        append_root_script_warning(r,
            new_root_script_files(root_scripts_before,
                                  list_root_script_files(root_for_script_warning)),
            scratch_dir);
        return r;
    }

    if (full_output.empty()) {
        full_output = "(no output)";
    }

    bool is_ok = (exit_code == 0);
    ToolResult r{full_output, is_ok};
    r.metadata["exit_code"] = static_cast<int>(exit_code);
    if (sandboxed) {
        r.metadata["sandbox"] = sandbox::sandbox_mode_name(ctx.exec_sandbox->policy.mode);
        if (const auto violation = sandbox::classify_sandbox_violation(static_cast<int>(exit_code), full_output)) {
            r.output += sandbox::escalation_hint(ctx.exec_sandbox->policy, ctx.exec_sandbox->network_enforced,
                                                 &*violation, ctx.exec_sandbox->network_best_effort);
            r.metadata["sandbox_denied"] = true;
            r.metadata["sandbox_violation"] = sandbox::violation_to_json(*violation);
            LOG_WARN("[sandbox] violation reason=" + violation->reason +
                     (violation->path.empty() ? std::string{} : " path=" + violation->path));
        }
    }
    r.summary = make_summary(command, duration_ms, raw_bytes,
                             static_cast<int>(exit_code), is_ok,
                             was_truncated, false, false);
    append_root_script_warning(r,
        new_root_script_files(root_scripts_before,
                              list_root_script_files(root_for_script_warning)),
        scratch_dir);
    return r;
}

ToolImpl create_bash_tool() {
    ToolDef def;
    def.name = "bash";
    def.description = "Execute a shell command and return its output. "
                      "Use this to run commands, check files, install packages, etc. "
                      "For programs that prompt for input (e.g. 'apt install' confirming, "
                      "'npm login' asking for credentials), pass stdin_inputs with the "
                      "answers to pipe into the command's stdin in order. "
                      "Commands may run in a filesystem/network sandbox. If a command is denied by the sandbox, "
                      "prefer the smallest request: sandbox_permissions=\"with_additional_permissions\" plus "
                      "additional_permissions listing only the extra paths (or network) it needs; use "
                      "sandbox_permissions=\"require_escalated\" only when unrestricted access is genuinely "
                      "required. Both need a non-empty justification and are subject to user approval.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"sandbox_permissions", {
                {"type", "string"},
                {"enum", nlohmann::json::array({"use_default", "with_additional_permissions", "require_escalated"})},
                {"description", "use_default (default): run under the current sandbox policy. "
                                "with_additional_permissions: stay sandboxed but also grant additional_permissions "
                                "(user approval required). require_escalated: ask the user to run outside the sandbox."}
            }},
            {"additional_permissions", {
                {"type", "object"},
                {"description", "Extra permissions for sandbox_permissions=with_additional_permissions: "
                                "{\"file_system\": {\"read\": [absolute paths], \"write\": [absolute paths]}, "
                                "\"network\": {\"enabled\": true}}. Paths may start with ~. Denied secret stores "
                                "(e.g. ~/.ssh) cannot be requested."},
                {"properties", {
                    {"file_system", {
                        {"type", "object"},
                        {"properties", {
                            {"read", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                            {"write", {{"type", "array"}, {"items", {{"type", "string"}}}}}
                        }}
                    }},
                    {"network", {
                        {"type", "object"},
                        {"properties", {{"enabled", {{"type", "boolean"}}}}}
                    }}
                }}
            }},
            {"with_escalated_permissions", {
                {"type", "boolean"}, {"description", "Legacy alias for sandbox_permissions=require_escalated (default: false)"}
            }},
            {"justification", {
                {"type", "string"}, {"description", "One sentence explaining why extra or escalated permissions are necessary"}
            }},
            {"prefix_rule", {
                {"type", "array"}, {"items", {{"type", "string"}}},
                {"description", "Optional command prefix tokens (e.g. [\"pnpm\", \"install\"]) the user may choose "
                                "to always allow; it must cover every command segment and cannot be a shell, "
                                "interpreter, rm or sudo."}
            }},
            {"command", {
                {"type", "string"},
                {"description", "The shell command to execute"}
            }},
            {"timeout_ms", {
                {"type", "integer"},
                {"description", "Timeout in milliseconds (default: 120000)"}
            }},
            {"cwd", {
                {"type", "string"},
                {"description", "Working directory for the command (default: agent CWD)"}
            }},
            {"stdin_inputs", {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "Optional: lines to feed to the command's stdin in order, "
                                "each followed by a newline. Use for programs that prompt "
                                "interactively, e.g. [\"y\"] for an 'apt install' confirmation. "
                                "On Windows this parameter is currently accepted but ignored."}
            }}
        }},
        {"required", nlohmann::json::array({"command"})}
    });

    return ToolImpl{def, execute_bash, /*is_read_only=*/false};
}

} // namespace acecode
