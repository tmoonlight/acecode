#pragma once

// 命令分类器(openspec add-auto-mode-sandbox,复刻 Codex
// codex-rs/shell-command/src/command_safety 的语义):把一条 shell 命令拆成
// 段,对每段判「已知安全(只读)」/「危险」/「未知」,整体取最保守结果。
//
// 纯函数、无 I/O、三平台同一份名单:模型在 Windows 上照样会写 POSIX 命令交给
// cmd,反过来也一样,所以按 token 名判,不按当前 shell 判。
//
// 拆段规则照 Codex 文档:只有整条脚本是"普通单词"(没有重定向 / 变量或命令
// 替换 / 通配符 / 单个 & / 换行 / 控制流 / PowerShell 脚本块)时,才按
// `&&` `||` `;` `|` 拆;否则整条视为一段、且不可能是 KnownSafe。危险判定不
// 依赖拆段成功:对整条原文与尽力拆出的每一段都跑一遍。

#include <string>
#include <vector>

namespace acecode::sandbox {

enum class CommandKind { KnownSafe, Dangerous, Unknown };

enum class CommandPlatform { Posix, Cmd, PowerShell };
CommandPlatform host_command_platform();

const char* command_kind_name(CommandKind kind);

struct CommandSegment {
    std::vector<std::string> tokens;   // 去引号后的 token,不含运算符
};

struct CommandClassification {
    CommandKind kind = CommandKind::Unknown;
    // 尽力拆出的段(拆段失败时也按运算符尽力拆,供规则匹配与前缀记忆用)。
    std::vector<CommandSegment> segments;
    // 解包得到的命令仅额外参与拒绝规则,不会扩大外层 allow 的范围。
    std::vector<CommandSegment> nested_segments;
    // 是否满足 Codex 的"可安全拆段"条件;false 时 kind 不可能是 KnownSafe。
    bool split_safely = false;
    // 一句话理由,给日志 / 确认框。
    std::string reason;
};

// 单个 shell 单词(已去引号)。quoted 标记它是否整体或部分带引号 —— 带引号的
// `*` 不是通配符,带引号的 `&&` 不是运算符。
struct ShellWord {
    std::string text;
    bool quoted = false;
    bool is_operator = false;   // && || | ; & > >> < 换行 等
};

// 词法切分。POSIX / cmd / PowerShell 三种引号规则取并集:单引号字面量、双引号
// 内 \" 与 `" 转义、反斜杠只在后面跟空白 / 引号 / 运算符 / $ / ` 时才算转义
// (否则 `C:\Users` 会被吃掉)。运算符只在引号外识别。
std::vector<ShellWord> tokenize_shell_words(
    const std::string& command, CommandPlatform platform = host_command_platform());

// 主入口。
CommandClassification classify_command(
    const std::string& command, CommandPlatform platform = host_command_platform());

// 取可执行名:去目录、去 .exe/.cmd/.bat 后缀、转小写。`C:\Git\bin\git.exe` → `git`。
std::string command_basename(const std::string& token);

// 会话级「总是允许」记忆用的前缀:首 token(basename),首 token 属于多级 CLI
// (git / npm / cargo ...)时再带第二个 token。空段返回空串。
std::string always_allow_prefix_for_segment(const CommandSegment& segment);
// 同上,但返回 token 列表(规则写回用 pattern);空 = 不提供前缀。
std::vector<std::string> always_allow_prefix_tokens_for_segment(const CommandSegment& segment);

// 任一 token 引用敏感路径(.ssh / .env / id_rsa / .aws ... 与 PathValidator
// 同一份名单)。只读命令碰这些文件也不该自动跑。
bool segment_references_sensitive_path(const CommandSegment& segment);

} // namespace acecode::sandbox
