#pragma once

// bash 工具与终端探测共用的命令行构造(openspec: agent-default-terminal)。
// 纯函数,无平台依赖,便于单测逐字节比对。
//
// 各终端家族的启动形态:
//   cmd        : <program> /c <command>                    (与改动前的 `cmd.exe /c` 逐字节一致)
//   powershell : <program> -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
//                -OutputFormat Text -EncodedCommand <UTF-16LE base64 of "<prelude>\n<command>">
//                (prelude 见 powershell_utf8_prelude;只有 bash 工具的执行路径加,探测不加)
//                —— 用 EncodedCommand 是因为 `-Command` 的剩余参数会先过一遍 Windows argv
//                解析,引号被吞得不可预测;EncodedCommand 不经过任何引号处理。
//   bash/posix : <program> -c <command>                    (非登录、非交互)

#include <string>
#include <vector>

namespace acecode::environment {

// 终端家族。命名对应 system prompt `Shell:` 行与语法指引的切换依据:
//   Cmd = Windows 命令提示符;PowerShell = pwsh / Windows PowerShell;
//   Bash = Windows 上的 Git Bash;Posix = Linux/macOS 的登录 shell 家族。
enum class TerminalFamily { Cmd, PowerShell, Bash, Posix };

const char* terminal_family_name(TerminalFamily family);  // "cmd" / "powershell" / "bash" / "posix"

// 终端类型 id → 家族:powershell → PowerShell;cmd → Cmd;git-bash → Bash;其余 → Posix。
TerminalFamily terminal_family_for_id(const std::string& shell_id);

// 已解析的默认终端。program 为裸名(走 PATH)或绝对路径。
struct ResolvedTerminal {
    std::string id;                       // 终端类型 id(空 = 未解析出可用终端)
    TerminalFamily family = TerminalFamily::Cmd;
    std::string program;                  // 实际启动的程序
    std::string console_command;          // 控制台停靠区新建 tab 用的命令行(含参数)
    std::string fallback_reason;          // 非空 = 发生了回退(或显式路径不可用),给 UI 展示
    bool usable = false;                  // false = 没有任何候选通过启动探测
};

struct ShellCommandLine {
    std::string program;                  // argv[0]
    std::vector<std::string> argv;        // POSIX 用(argv[0] = program)
    std::string windows_command_line;     // Windows 用(交给 CreateProcessW 的完整命令行)
};

// 按 MSVCRT argv 规则给单个参数加引号:含空格 / 制表符 / 引号时包双引号,
// 引号前的反斜杠加倍,引号本身转义。
std::string quote_windows_argument(const std::string& arg);

// UTF-8 → UTF-16LE → base64,PowerShell -EncodedCommand 的参数。
std::string encode_powershell_command(const std::string& utf8_command);

// PowerShell 非交互启动的固定参数(不含 -EncodedCommand)。
std::vector<std::string> powershell_noninteractive_args();

// 启动探测用的"exit 0"参数(每个家族各自的语法)。探测不加编码前置脚本。
std::vector<std::string> probe_arguments(TerminalFamily family);

// bash 工具执行 PowerShell 家族命令前拼在前面的一行编码前置脚本(以 "\n" 结尾)。
// 起因(fix-feedback-0924 第 4 条):Windows PowerShell 5.1 的 Get-Content 按 ANSI
// 代码页(中文系统 = GBK)读无 BOM 的 UTF-8 源码,agent 再 WriteAllText 写回,
// 整份中文被永久写坏;`$x = git show ...` 按 [Console]::OutputEncoding 解码,
// bash 工具开的隐藏控制台是 936,pwsh 7 也一样。前置脚本:
//   - 所有版本:[Console]::OutputEncoding 与 $OutputEncoding(往原生命令 stdin
//     写的编码)各自在独立 try 里设成无 BOM 的 UTF-8 —— 合并成一个 try 时
//     SetConsoleOutputCP 失败会连带 stdin 编码也没设上;PYTHONIOENCODING 未设置
//     时补 utf-8(否则 Python ≤3.14 往管道写 GBK,与 UTF-8 控制台混在一起);
//   - 仅 5.1(Major -lt 6):关闭进度记录,并把 7 个文件 cmdlet 的 -Encoding
//     默认值设为 utf8(pwsh 7 默认已是无 BOM UTF-8,不碰)。5.1 写入会带 BOM,
//     这是用户拍板的取舍:只改读取侧会让「读→写」把 UTF-8 源码静默转成 GBK。
// 只能用 .NET 静态调用与语言结构:调用 New-Object 等会触发模块自动加载的 cmdlet
// 时,5.1 每条命令的 stderr 都会多出一段 GBK 编码的 CLIXML 进度记录。用户命令有
// 解析错误时整段脚本都不执行(PowerShell 先解析后执行),错误输出仍是 GBK,由
// bash 工具的 IncrementalTextDecoder 按 ACP 回退解码。
const std::string& powershell_utf8_prelude();

// 在命令前拼上 powershell_utf8_prelude()。以下只能出现在脚本开头的写法原样
// 返回(拼上前置脚本会把能运行的命令变成解析错误):using 语句、param 块、
// [特性]param、begin/process/end/dynamicparam/clean 具名块;判定前先跳过空白、
// # 行注释(含 #requires)与 <# #> 块注释。误判为「不加」的唯一后果是退回
// 改动前的行为,所以判定宁可偏向不加。
std::string with_powershell_utf8_prelude(const std::string& command);

// 构造 bash 工具执行 `command` 的命令行。
ShellCommandLine build_shell_command_line(const ResolvedTerminal& terminal,
                                          const std::string& command);

}  // namespace acecode::environment
