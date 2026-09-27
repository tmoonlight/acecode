#pragma once

// 默认终端解析(openspec: agent-default-terminal)。
//
// detect_console_shells 只知道"文件在不在";这里再把每个候选真的拉起来跑一次
// `exit 0`(与 bash 工具相同的启动形态),按顺序找第一个**能启动**的:
//   配置的类型(显式路径 → 探测路径 → 同类备选)→ 平台回退顺序
//   Windows: powershell(pwsh → powershell.exe)→ git-bash → cmd
//   POSIX  : shell(登录 shell)→ bash → zsh → fish
// 被策略拒绝(拒绝访问)、文件缺失、退出码非零、超时都算不可用,原因逐条累积
// 到 fallback_reason,UI 据此显示「已回退: <类型>」与提示。
//
// 启动探测通过 LaunchProbe 注入,单测用假探测覆盖回退链;生产用 run_hook_process。

#include "shell_command_line.hpp"
#include "config/config.hpp"
#include "web/pty/pty_backend.hpp"

#include <functional>
#include <string>
#include <vector>

namespace acecode::environment {

struct TerminalCandidate {
    std::string id;
    std::string label;
    TerminalFamily family = TerminalFamily::Posix;
    std::vector<std::string> programs;   // 尝试顺序:显式路径 → 探测路径 → 同类备选
    std::string configured_path;         // 用户显式路径(可空)
    std::string detected_path;           // 自动探测路径(可空 / 裸名)
    std::string program;                 // 通过探测的程序;未探测时为首选程序
    bool available = false;              // 目录层面存在
    bool needs_path = false;             // 探测不到、需要用户指定路径(git-bash)
    bool probed = false;                 // 至少尝试启动过一个程序(解析在首个可用项停下,后面的不探)
    bool usable = false;                 // 通过了启动探测
    std::string probe_error;             // 该候选所有程序的失败原因(分号分隔)
};

struct TerminalResolution {
    ResolvedTerminal resolved;
    std::vector<TerminalCandidate> candidates;
    std::string configured_id;           // console.default_shell(可空)
};

struct LaunchProbeResult {
    bool ok = false;
    std::string error;
};

// 启动探测:拉起 program + args,返回是否以退出码 0 结束。
using LaunchProbe = std::function<LaunchProbeResult(const std::string& program,
                                                    const std::vector<std::string>& args)>;

// 生产探测:run_hook_process,超时 timeout_ms(默认 5 秒),不继承 stdin。
LaunchProbe default_launch_probe(int timeout_ms = 5000);

// 平台回退顺序(类型 id)。
std::vector<std::string> platform_fallback_order();

// 控制台停靠区用的启动命令行(含参数):git-bash 带 --login -i,其余只是程序本身。
std::string console_command_for(TerminalFamily family, const std::string& program);

TerminalResolution resolve_terminal(const ConsoleConfig& console,
                                    const ShellProbe& probe,
                                    const LaunchProbe& launch);
TerminalResolution resolve_terminal(const ConsoleConfig& console);  // 真实探测

}  // namespace acecode::environment
