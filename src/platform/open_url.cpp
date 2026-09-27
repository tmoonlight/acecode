#include "open_url.hpp"

#include "utils/encoding.hpp"

#include <algorithm>
#include <cctype>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <cerrno>
#  include <signal.h>
#  include <sys/wait.h>
#  include <thread>
#  include <unistd.h>
#endif

namespace acecode {

namespace {

#ifndef _WIN32
void wait_for_child_process(pid_t pid) noexcept {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}
#endif

// 默认平台打开器:POSIX 用 open(macOS) / xdg-open(Linux),fork+execlp 不经
// shell;Windows 用 ShellExecuteW 的 "open" verb(等价于 `start`,但无 cmd
// 转义面)。URL 原样作参数,带空格/引号也安全。
bool platform_open_url(const std::string& url, std::string& error) {
#ifdef _WIN32
    const std::wstring wide_url(acecode::utf8_to_wide(url));
    HINSTANCE result = ::ShellExecuteW(
        nullptr, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    const auto code = reinterpret_cast<intptr_t>(result);
    if (code > 32) return true;
    error = "ShellExecute failed: " + std::to_string(code);
    return false;
#else
#  ifdef __APPLE__
    const char* opener = "open";
#  else
    const char* opener = "xdg-open";
#  endif
    const pid_t pid = ::fork();
    if (pid < 0) {
        error = "fork failed";
        return false;
    }
    if (pid == 0) {
        ::execlp(opener, opener, url.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }

    // 不修改进程级 SIGCHLD 策略(ACECode 的其它子进程也依赖它);只为本次
    // opener 启动一个短生命周期 waiter,确保长时间运行的 TUI 不积累 zombie。
    std::thread reaper;
    try {
        reaper = std::thread([pid]() { wait_for_child_process(pid); });
    } catch (...) {
        ::kill(pid, SIGTERM);
        wait_for_child_process(pid);
        error = "failed to start URL process reaper";
        return false;
    }
    try {
        reaper.detach();
    } catch (...) {
        ::kill(pid, SIGTERM);
        if (reaper.joinable()) {
            reaper.join();
        }
        error = "failed to detach URL process reaper";
        return false;
    }
    return true;
#endif
}

} // namespace

bool is_openable_http_url(const std::string& url) {
    // 前导空白/空串/控制字符一律拒绝。控制字符(尤其 ESC 0x1B)是终端转义
    // 注入通道,必须拦。
    if (url.empty() ||
        static_cast<unsigned char>(url.front()) < 0x21 ||
        static_cast<unsigned char>(url.back()) < 0x21) {
        return false;
    }
    for (const char c : url) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) {
            return false;  // 控制字符 / DEL
        }
    }

    // 仅 http/https(大小写不敏感),且 scheme 后必须有内容(host 至少 1 字符,
    // "http://" 裸 scheme 打开无意义)。
    const bool http = url.size() > 7 &&
        std::equal(url.begin(), url.begin() + 7, "http://",
                   [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a)) == b;
                   });
    const bool https = url.size() > 8 &&
        std::equal(url.begin(), url.begin() + 8, "https://",
                   [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a)) == b;
                   });
    return http || https;
}

OpenUrlResult open_url_in_browser(const std::string& url,
                                  OpenUrlLauncher launcher) {
    if (!is_openable_http_url(url)) {
        return {false, "URL must be an http/https link"};
    }

    std::string error;
    auto launch = launcher ? std::move(launcher)
                           : OpenUrlLauncher(platform_open_url);
    if (!launch(url, error)) {
        if (error.empty()) error = "failed to open URL in browser";
        return {false, error};
    }
    return {true, {}};
}

} // namespace acecode
