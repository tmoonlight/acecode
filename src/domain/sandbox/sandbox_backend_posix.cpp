// macOS / Linux 沙盒后端探测(openspec add-auto-mode-sandbox)。argv 组装在
// sandbox_backend_args.cpp(纯逻辑,所有平台编译);这里只做碰 OS 的部分。

#ifndef _WIN32

#include "sandbox_backend.hpp"

#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <cerrno>
#include <filesystem>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace acecode::sandbox {

namespace {

bool executable_exists(const std::string& path) {
    return !path.empty() && access(path.c_str(), X_OK) == 0;
}

std::string find_on_path(const std::string& program) {
    const char* raw = std::getenv("PATH");
    if (!raw) return {};
    std::string path_env = raw;
    std::size_t start = 0;
    while (start <= path_env.size()) {
        std::size_t end = path_env.find(':', start);
        if (end == std::string::npos) end = path_env.size();
        const std::string dir = path_env.substr(start, end - start);
        if (!dir.empty() && std::filesystem::path(dir).is_absolute()) {
            const std::string candidate = dir + "/" + program;
            if (executable_exists(candidate)) return candidate;
        }
        if (end == path_env.size()) break;
        start = end + 1;
    }
    return {};
}

#if !defined(__APPLE__)
// 探测与执行复用相同的命名空间/挂载参数;所有字符串在 fork 前准备好。
bool bwrap_probe_succeeds(const std::string& bwrap) {
    SandboxPolicy policy;
    policy.mode = SandboxMode::ReadOnly;
    auto arguments = build_bwrap_argv(policy);
    arguments.front() = bwrap;
    arguments.push_back("/bin/true");
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        const int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execv(bwrap.c_str(), argv.data());
        _exit(127);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (waited < 0 && errno != EINTR) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return false;
}
#endif

} // namespace

BackendProbe probe_backend(WindowsBackendChoice /*windows_backend*/) {
    BackendProbe probe;
#if defined(__APPLE__)
    probe.kind = BackendKind::MacosSeatbelt;
    probe.network_enforced = true;
    if (executable_exists("/usr/bin/sandbox-exec")) {
        probe.available = true;
        probe.executable_path = "/usr/bin/sandbox-exec";
    } else {
        probe.available = false;
        probe.reason = "/usr/bin/sandbox-exec not found";
    }
#else
    probe.kind = BackendKind::LinuxBwrap;
    probe.network_enforced = true;
    const std::string bwrap = find_on_path("bwrap");
    if (bwrap.empty()) {
        probe.available = false;
        probe.reason = "bwrap (bubblewrap) is not installed; install it with your package manager";
    } else if (!bwrap_probe_succeeds(bwrap)) {
        probe.available = false;
        probe.reason = "bwrap cannot create user namespaces on this system";
    } else {
        probe.available = true;
        probe.executable_path = bwrap;
    }
#endif
    return probe;
}

} // namespace acecode::sandbox

#endif // !_WIN32
