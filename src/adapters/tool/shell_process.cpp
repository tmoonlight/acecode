#include "shell_process.hpp"

#include "environment/shell_command_line.hpp"
#include "environment/terminal_runtime.hpp"
#include "sandbox/sandbox_backend.hpp"
#include "utils/encoding.hpp"
#include "utils/stream_processing.hpp"

#include <algorithm>
#include <cstdlib>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace acecode {

namespace {

bool sandboxed_spec(const ShellLaunchSpec& spec) {
    return spec.sandbox && spec.sandbox->policy.mode != sandbox::SandboxMode::FullAccess;
}

#ifdef _WIN32
std::wstring env_name_prefix(const std::wstring& name) {
    return name + L"=";
}

bool starts_with_env_name_ci(const std::wstring& value, const std::wstring& prefix) {
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

std::vector<wchar_t> build_environment_block_with_var(
    const std::vector<std::pair<std::string, std::string>>& variables) {
    std::vector<std::wstring> entries;
    LPWCH env = GetEnvironmentStringsW();
    if (env) {
        for (LPWCH p = env; *p != L'\0';) {
            std::wstring entry(p);
            const size_t entry_len = entry.size();
            bool replaced = false;
            for (const auto& variable : variables) {
                if (starts_with_env_name_ci(entry, env_name_prefix(utf8_to_wide(variable.first)))) replaced = true;
            }
            if (!replaced) entries.push_back(std::move(entry));
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

} // namespace

std::string clean_shell_output(const std::string& text) {
    if (text.empty()) return {};
#ifdef _WIN32
    std::string normalized;
    normalized.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            normalized += '\n';
            ++i;
        } else {
            normalized += text[i];
        }
    }
    return strip_ansi(normalized);
#else
    return strip_ansi(text);
#endif
}

#ifdef _WIN32

std::unique_ptr<ShellProcess> ShellProcess::launch(const ShellLaunchSpec& spec,
                                                   ShellLaunchFailure& failure) {
    const bool sandboxed = sandboxed_spec(spec);
    auto sandbox_failure = [&failure](std::string message) -> std::unique_ptr<ShellProcess> {
        failure.message = std::move(message);
        failure.sandbox_unavailable = true;
        return nullptr;
    };
    if (sandboxed) {
        if (spec.sandbox->backend == sandbox::BackendKind::WindowsMxc) {
            // MXC 口子(align-codex-sandboxing D9):探测恒不可用,正常不会走到这里;
            // 真接入时把 SandboxPolicy 翻译成 MXC ExecutionRequest 后在此启动。
            return sandbox_failure("MXC backend is not bundled in this build");
        }
        if (spec.sandbox->backend != sandbox::BackendKind::WindowsRestrictedToken) {
            return sandbox_failure("Invalid backend for Windows");
        }
        std::string error;
        if (!sandbox::ensure_windows_acl_grants(spec.sandbox->policy, &error)) return sandbox_failure(error);
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    platform::UniqueHandle read_pipe, write_pipe;
    if (!CreatePipe(read_pipe.put(), write_pipe.put(), &sa, 0)) {
        failure.message = "Failed to create pipe.";
        return nullptr;
    }
    SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe.get();
    si.hStdError = write_pipe.get();

    PROCESS_INFORMATION pi = {};

    // 默认终端(openspec: agent-default-terminal):运行时已解析出可用终端就按其家族
    // 构造命令行(cmd / PowerShell EncodedCommand / Git Bash -c);未 bootstrap 或
    // 全部候选不可用时维持改动前的 `cmd.exe /c`。
    std::string windows_command_line = "cmd.exe /d /c " + spec.command;
    if (const auto terminal_snapshot = acecode::environment::terminal().last()) {
        windows_command_line = acecode::environment::build_shell_command_line(
            terminal_snapshot->resolved, spec.command).windows_command_line;
    }
    std::wstring full_cmd = utf8_to_wide(windows_command_line);
    std::vector<wchar_t> full_cmd_buffer(full_cmd.begin(), full_cmd.end());
    full_cmd_buffer.push_back(L'\0');

    std::wstring wide_cwd;
    if (!spec.cwd.empty()) wide_cwd = utf8_to_wide(spec.cwd);
    const wchar_t* cwd_ptr = wide_cwd.empty() ? nullptr : wide_cwd.c_str();

    std::vector<wchar_t> env_block;
    void* env_ptr = nullptr;
    if (!spec.environment.empty()) {
        env_block = build_environment_block_with_var(spec.environment);
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
        auto token = sandbox::create_restricted_token(spec.sandbox->policy, &sandbox_error);
        if (token) {
            std::wstring desktop = L"winsta0\\default";
            si.lpDesktop = desktop.data();
            ok = CreateProcessAsUserW(token.get(), nullptr, full_cmd_buffer.data(), nullptr, nullptr, TRUE,
                                      creation_flags, env_ptr, cwd_ptr, &si, &pi);
            if (!ok) sandbox_error = "CreateProcessAsUserW failed: " + std::to_string(GetLastError());
        }
    } else {
        ok = CreateProcessW(nullptr, full_cmd_buffer.data(), nullptr, nullptr, TRUE,
                            creation_flags, env_ptr, cwd_ptr, &si, &pi);
    }

    write_pipe.reset();

    if (!ok) {
        if (sandboxed) return sandbox_failure(sandbox_error);
        failure.message = "Failed to execute command.";
        return nullptr;
    }

    auto process = std::make_unique<ShellProcess>(ConstructionKey{});
    process->read_pipe_ = std::move(read_pipe);
    process->process_ = platform::UniqueHandle(pi.hProcess);
    process->thread_ = platform::UniqueHandle(pi.hThread);
    process->job_ = sandbox::create_process_tree_job();
    if (process->job_ && !sandbox::assign_process_to_job(process->job_, pi.hProcess)) {
        // 嵌套 Job 被拒(旧系统 / 受限宿主 Job):退回只杀直接子进程的旧行为。
        process->job_.reset();
    }
    ResumeThread(pi.hThread);
    return process;
}

ShellProcess::~ShellProcess() = default;

std::size_t ShellProcess::read_available(std::string& raw) {
    if (!read_pipe_) return 0;
    DWORD avail = 0;
    if (!PeekNamedPipe(read_pipe_.get(), nullptr, 0, nullptr, &avail, nullptr) || avail == 0) return 0;
    char buffer[4096];
    DWORD bytes_read = 0;
    if (!ReadFile(read_pipe_.get(), buffer, sizeof(buffer), &bytes_read, nullptr) || bytes_read == 0) return 0;
    raw.append(buffer, bytes_read);
    return bytes_read;
}

bool ShellProcess::poll_exited() {
    if (exited_) return true;
    if (!process_) return false;
    if (WaitForSingleObject(process_.get(), 0) != WAIT_OBJECT_0) return false;
    DWORD code = 0;
    GetExitCodeProcess(process_.get(), &code);
    exit_code_ = static_cast<int>(code);
    exited_ = true;
    return true;
}

void ShellProcess::drain(std::string& raw) {
    // **不能**裸 ReadFile:同步管道只要还有进程持有写端,数据耗尽时 ReadFile 会
    // 永久阻塞(典型:命令拉起的 Chrome / 后台 daemon 继承了同一根管道)。只读
    // 当前已有的字节,读空立即返回,把后台孙进程交给 OS。
    while (read_available(raw) > 0) {
    }
}

void ShellProcess::kill_tree(int grace_ms, bool /*graceful*/) {
    if (exited_ || !process_) return;
    if (job_) sandbox::terminate_job_tree(job_);
    else TerminateProcess(process_.get(), 1);
    WaitForSingleObject(process_.get(), static_cast<DWORD>(grace_ms > 0 ? grace_ms : 0));
    poll_exited();
}

std::string ShellProcess::launch_error() {
    return {};
}

#else

std::unique_ptr<ShellProcess> ShellProcess::launch(const ShellLaunchSpec& spec,
                                                   ShellLaunchFailure& failure) {
    const bool sandboxed = sandboxed_spec(spec);
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        failure.message = "Failed to create pipe.";
        return nullptr;
    }
    // stdin 管道总是创建,子进程才有确定的 stdin;没有输入时立即关闭写端让它读到 EOF。
    int stdin_pipefd[2];
    if (pipe(stdin_pipefd) == -1) {
        close(pipefd[0]); close(pipefd[1]);
        failure.message = "Failed to create stdin pipe.";
        return nullptr;
    }
    auto close_all = [&pipefd, &stdin_pipefd]() {
        close(pipefd[0]); close(pipefd[1]); close(stdin_pipefd[0]); close(stdin_pipefd[1]);
    };

    // 默认终端(openspec: agent-default-terminal):argv 在 fork **之前**准备好 ——
    // daemon 是多线程进程,子进程里不能再分配内存(malloc 锁可能被别的线程持有)。
    // 运行时没有可用终端时维持改动前的 /bin/sh -c。
    std::string shell_program = "/bin/sh";
    std::vector<std::string> shell_argv = {"/bin/sh", "-c", spec.command};
    if (const auto terminal_snapshot = acecode::environment::terminal().last()) {
        auto line = acecode::environment::build_shell_command_line(terminal_snapshot->resolved, spec.command);
        shell_program = line.program;
        shell_argv = line.argv;
    }
    if (sandboxed) {
        std::vector<std::string> prefix;
        if (spec.sandbox->backend == sandbox::BackendKind::MacosSeatbelt) {
            prefix = sandbox::build_seatbelt_argv(spec.sandbox->policy);
        } else if (spec.sandbox->backend == sandbox::BackendKind::LinuxBwrap) {
            if (spec.sandbox->backend_executable.empty()) {
                close_all();
                failure.message = "No verified bubblewrap executable";
                failure.sandbox_unavailable = true;
                return nullptr;
            }
            prefix = sandbox::build_bwrap_argv(spec.sandbox->policy);
            prefix.front() = spec.sandbox->backend_executable;
        } else {
            close_all();
            failure.message = "Invalid backend for POSIX";
            failure.sandbox_unavailable = true;
            return nullptr;
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
        const bool overridden = std::any_of(spec.environment.begin(), spec.environment.end(),
            [&name](const auto& entry) { return entry.first == name; });
        if (!overridden) environment_storage.push_back(std::move(value));
    }
    for (const auto& entry : spec.environment) environment_storage.push_back(entry.first + "=" + entry.second);
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
            if (directory.empty()) directory = spec.cwd.empty() ? "." : spec.cwd;
            const auto candidate = directory + "/" + shell_program;
            if (access(candidate.c_str(), X_OK) == 0) { shell_program = candidate; break; }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    // CLOEXEC 让父进程区分后端根本没启动与已运行命令自行返回 127。
    int exec_error_pipe[2];
    if (pipe(exec_error_pipe) == -1) {
        close_all();
        failure.message = "Failed to create launch status pipe.";
        return nullptr;
    }
    if (fcntl(exec_error_pipe[1], F_SETFD, FD_CLOEXEC) == -1 ||
        fcntl(exec_error_pipe[0], F_SETFL, O_NONBLOCK) == -1) {
        close_all();
        close(exec_error_pipe[0]); close(exec_error_pipe[1]);
        failure.message = "Failed to configure launch status pipe.";
        return nullptr;
    }
    const std::string cwd = spec.cwd;
    pid_t pid = fork();
    if (pid == -1) {
        close_all();
        close(exec_error_pipe[0]); close(exec_error_pipe[1]);
        failure.message = "Failed to fork.";
        return nullptr;
    }

    if (pid == 0) {
        close(exec_error_pipe[0]);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        close(stdin_pipefd[1]);
        dup2(stdin_pipefd[0], STDIN_FILENO);
        close(stdin_pipefd[0]);

        // 新进程组,父进程才能向整棵树发信号。
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

    close(exec_error_pipe[1]);
    close(pipefd[1]);
    close(stdin_pipefd[0]);

    auto process = std::make_unique<ShellProcess>(ConstructionKey{});
    process->pid_ = pid;
    process->read_fd_ = pipefd[0];
    process->exec_error_fd_ = exec_error_pipe[0];
    const int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    if (!spec.stdin_inputs.empty()) {
        const int write_fd = stdin_pipefd[1];
        process->stdin_writer_ = acecode::JoiningThread([write_fd, inputs = spec.stdin_inputs]() {
            for (const auto& line : inputs) {
                std::string data = line + "\n";
                const char* p = data.c_str();
                size_t remaining = data.size();
                while (remaining > 0) {
                    ssize_t n = write(write_fd, p, remaining);
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        close(write_fd);
                        return; // 子进程已退出或管道已断
                    }
                    p += n;
                    remaining -= static_cast<size_t>(n);
                }
            }
            close(write_fd);
        });
    } else {
        close(stdin_pipefd[1]);
    }
    return process;
}

ShellProcess::~ShellProcess() {
    if (read_fd_ >= 0) close(read_fd_);
    if (exec_error_fd_ >= 0) close(exec_error_fd_);
    if (stdin_writer_.joinable()) stdin_writer_.join();
    // 已结束的子进程需要回收,避免僵尸;仍在运行的(调用方有意留下)不在这里阻塞。
    if (pid_ > 0 && !exited_) {
        int status = 0;
        waitpid(pid_, &status, WNOHANG);
    }
}

std::size_t ShellProcess::read_available(std::string& raw) {
    if (read_fd_ < 0) return 0;
    char buffer[4096];
    const ssize_t n = read(read_fd_, buffer, sizeof(buffer));
    if (n <= 0) return 0;
    raw.append(buffer, static_cast<size_t>(n));
    return static_cast<size_t>(n);
}

bool ShellProcess::poll_exited() {
    if (exited_) return true;
    if (pid_ <= 0) return false;
    int status = 0;
    if (waitpid(pid_, &status, WNOHANG) != pid_) return false;
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status)
                                   : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    exited_ = true;
    return true;
}

void ShellProcess::drain(std::string& raw) {
    while (read_available(raw) > 0) {
    }
}

void ShellProcess::kill_tree(int grace_ms, bool graceful) {
    if (exited_ || pid_ <= 0) return;
    if (graceful) {
        kill(-pid_, SIGTERM);
        for (int waited = 0; waited < grace_ms; waited += 10) {
            if (poll_exited()) return;
            usleep(10000);
        }
    }
    kill(-pid_, SIGKILL);
    int status = 0;
    if (waitpid(pid_, &status, 0) == pid_) {
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status)
                                       : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
        exited_ = true;
    }
}

std::string ShellProcess::launch_error() {
    if (exec_error_fd_ < 0) return {};
    int launch_error = 0;
    ssize_t launch_status;
    do { launch_status = read(exec_error_fd_, &launch_error, sizeof(launch_error)); }
    while (launch_status < 0 && errno == EINTR);
    close(exec_error_fd_);
    exec_error_fd_ = -1;
    if (launch_status == static_cast<ssize_t>(sizeof(launch_error)) && launch_error != 0) {
        return "Cannot start the shell/backend: " +
               std::error_code(launch_error, std::generic_category()).message();
    }
    return {};
}

#endif

} // namespace acecode
