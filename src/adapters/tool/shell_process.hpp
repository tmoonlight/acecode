#pragma once

// 一条 shell 命令子进程的生命周期:启动(含沙箱令牌 / bwrap / Seatbelt 包装)、
// 非阻塞读取输出、检查退出、杀整棵进程树。
//
// 从 bash_tool 拆出来,是为了让「还在跑的前台命令」可以整体交给后台任务登记表
// 继续读:进程句柄、管道、进程树 Job 都归这个对象,谁持有它谁负责读完。
// 析构只关闭句柄、不杀进程;要结束进程先调 kill_tree()。

#include "platform/process/unique_resources.hpp"
#include "sandbox/sandbox_policy.hpp"
#include "utils/joining_thread.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace acecode {

// 解码后的子进程文本:Windows 下 CRLF 统一成 LF,去掉 ANSI 控制序列。
std::string clean_shell_output(const std::string& text);

struct ShellLaunchSpec {
    std::string command;
    std::string cwd;
    // 追加 / 覆盖到子进程环境里的变量(ACECODE_TMPDIR、沙箱环境等)。
    std::vector<std::pair<std::string, std::string>> environment;
    // 依次写入 stdin 的行(只在 POSIX 生效;Windows 接受但忽略)。
    std::vector<std::string> stdin_inputs;
    // 空或 FullAccess = 不进沙箱。
    std::optional<sandbox::ExecSandboxRequest> sandbox;
};

struct ShellLaunchFailure {
    std::string message;
    // true:沙箱后端起不来(调用方据此提示申请越权),false:普通启动失败。
    bool sandbox_unavailable = false;
};

class ShellProcess {
public:
    // 失败返回 nullptr 并填写 failure。
    static std::unique_ptr<ShellProcess> launch(const ShellLaunchSpec& spec,
                                                ShellLaunchFailure& failure);

    ShellProcess(const ShellProcess&) = delete;
    ShellProcess& operator=(const ShellProcess&) = delete;
    ~ShellProcess();

    // 把当前已经可读的字节追加到 raw(不阻塞)。返回读到的字节数。
    std::size_t read_available(std::string& raw);
    // 不阻塞地检查直接子进程是否已退出;退出后 exit_code() 有效。
    bool poll_exited();
    // 退出后把管道里还能立即读到的字节读完(孙进程可能仍持有写端,所以绝不阻塞等 EOF)。
    void drain(std::string& raw);
    // 结束整棵进程树,最多等 grace_ms 让它退出。graceful=true 时 POSIX 先 SIGTERM。
    void kill_tree(int grace_ms = 1000, bool graceful = false);
    bool exited() const { return exited_; }
    int exit_code() const { return exit_code_; }
    // POSIX:shell / 沙箱后端根本没启动起来时的原因(只在退出后读取);其余情况为空。
    std::string launch_error();

private:
    struct ConstructionKey {};

public:
    // 只能经 launch() 创建;key 类型外部不可见。
    explicit ShellProcess(ConstructionKey) {}

private:

#ifdef _WIN32
    platform::UniqueHandle read_pipe_;
    platform::UniqueHandle process_;
    platform::UniqueHandle thread_;
    platform::UniqueHandle job_;
#else
    int read_fd_ = -1;
    int exec_error_fd_ = -1;
    int pid_ = -1;
    acecode::JoiningThread stdin_writer_;
#endif
    bool exited_ = false;
    int exit_code_ = -1;
};

} // namespace acecode
