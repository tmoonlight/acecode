#include "shell_command_line.hpp"

#include "utils/base64.hpp"
#include "web/pty/pty_backend.hpp"

#include <cstdint>

namespace acecode::environment {

const char* terminal_family_name(TerminalFamily family) {
    switch (family) {
        case TerminalFamily::Cmd:        return "cmd";
        case TerminalFamily::PowerShell: return "powershell";
        case TerminalFamily::Bash:       return "bash";
        case TerminalFamily::Posix:      return "posix";
    }
    return "posix";
}

TerminalFamily terminal_family_for_id(const std::string& shell_id) {
    if (shell_id == "powershell") return TerminalFamily::PowerShell;
    if (shell_id == "cmd") return TerminalFamily::Cmd;
    if (shell_id == "git-bash") return TerminalFamily::Bash;
    return TerminalFamily::Posix;
}

std::string quote_windows_argument(const std::string& arg) {
    const bool needs_quotes = arg.empty() ||
        arg.find_first_of(" \t\n\v\"") != std::string::npos;
    if (!needs_quotes) return arg;
    std::string out;
    out.reserve(arg.size() + 2);
    out.push_back('"');
    std::size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            // 引号前的反斜杠加倍,再转义引号本身。
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, '\\');
        backslashes = 0;
        out.push_back(c);
    }
    // 结尾反斜杠加倍,否则会把收尾引号转义掉。
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

std::string encode_powershell_command(const std::string& utf8_command) {
    // 手写 UTF-8 → UTF-16LE:std::wstring 在 POSIX 是 32 位,不能拿来当 UTF-16 用。
    std::string bytes;
    bytes.reserve(utf8_command.size() * 2);
    auto push16 = [&](std::uint32_t unit) {
        bytes.push_back(static_cast<char>(unit & 0xFF));
        bytes.push_back(static_cast<char>((unit >> 8) & 0xFF));
    };
    std::size_t i = 0;
    while (i < utf8_command.size()) {
        const unsigned char lead = static_cast<unsigned char>(utf8_command[i]);
        std::uint32_t cp = 0;
        std::size_t len = 1;
        if (lead < 0x80) {
            cp = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            cp = lead & 0x1F; len = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            cp = lead & 0x0F; len = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            cp = lead & 0x07; len = 4;
        } else {
            cp = 0xFFFD;
        }
        if (len > 1) {
            if (i + len > utf8_command.size()) {
                cp = 0xFFFD;
                len = 1;
            } else {
                for (std::size_t k = 1; k < len; ++k) {
                    const unsigned char cc = static_cast<unsigned char>(utf8_command[i + k]);
                    if ((cc & 0xC0) != 0x80) {
                        cp = 0xFFFD;
                        len = 1;
                        break;
                    }
                    cp = (cp << 6) | (cc & 0x3F);
                }
            }
        }
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            push16(0xD800 | (cp >> 10));
            push16(0xDC00 | (cp & 0x3FF));
        } else {
            push16(cp);
        }
    }
    return base64_encode(bytes);
}

std::vector<std::string> powershell_noninteractive_args() {
    return {"-NoLogo", "-NoProfile", "-NonInteractive",
            "-ExecutionPolicy", "Bypass", "-OutputFormat", "Text"};
}

const std::string& powershell_utf8_prelude() {
    // 单行、只用 .NET 静态调用与语言结构(原因见头文件);不引入具名变量
    // (.ForEach 的 $_ 不泄漏到用户脚本),报错行号固定只加 1。
    static const std::string prelude =
        "try{[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)}catch{};"
        "try{$OutputEncoding=[Text.UTF8Encoding]::new($false)}catch{};"
        "if(-not $env:PYTHONIOENCODING){$env:PYTHONIOENCODING='utf-8'};"
        "if($PSVersionTable.PSVersion.Major -lt 6){$ProgressPreference='SilentlyContinue';"
        "try{('Get-Content','Set-Content','Add-Content','Out-File','Select-String',"
        "'Import-Csv','Export-Csv').ForEach({$PSDefaultParameterValues[$_+':Encoding']='utf8'})}"
        "catch{}}\n";
    return prelude;
}

namespace {

bool is_ps_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

char ascii_lower_char(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// 跳过空白、# 行注释(含 #requires)与 <# #> 块注释。块注释没有闭合时返回 npos。
std::size_t skip_ws_and_comments(const std::string& s, std::size_t i) {
    while (i < s.size()) {
        const char c = s[i];
        if (is_ps_space(c)) {
            ++i;
            continue;
        }
        if (c == '<' && i + 1 < s.size() && s[i + 1] == '#') {
            const auto end = s.find("#>", i + 2);
            if (end == std::string::npos) return std::string::npos;
            i = end + 2;
            continue;
        }
        if (c == '#') {
            const auto nl = s.find('\n', i);
            if (nl == std::string::npos) return s.size();
            i = nl + 1;
            continue;
        }
        break;
    }
    return i;
}

// 跳过一个 [...] 特性块:方括号配对计数,单双引号里的方括号不计。
// 返回块后的位置;没有闭合返回 npos。
std::size_t skip_attribute_block(const std::string& s, std::size_t i) {
    int depth = 0;
    char quote = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (quote != 0) {
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '[') {
            ++depth;
        } else if (c == ']') {
            if (--depth == 0) return i + 1;
        }
    }
    return std::string::npos;
}

// s[i..] 以 word(小写)开头(不区分大小写),其后是结尾、空白或 followers 之一。
bool starts_with_word_ci(const std::string& s, std::size_t i, const char* word,
                         const char* followers) {
    std::size_t k = 0;
    for (; word[k] != '\0'; ++k) {
        if (i + k >= s.size() || ascii_lower_char(s[i + k]) != word[k]) return false;
    }
    const std::size_t after = i + k;
    if (after >= s.size() || is_ps_space(s[after])) return true;
    for (const char* f = followers; *f != '\0'; ++f) {
        if (s[after] == *f) return true;
    }
    return false;
}

bool starts_with_leading_only_statement(const std::string& s) {
    std::size_t i = skip_ws_and_comments(s, 0);
    // 块注释未闭合:原命令本身就会报同样的解析错误,原样交给 PowerShell。
    if (i == std::string::npos) return true;
    if (i >= s.size()) return false;
    if (starts_with_word_ci(s, i, "using", "")) return true;
    bool saw_attribute = false;
    while (i < s.size() && s[i] == '[') {
        const std::size_t after = skip_attribute_block(s, i);
        if (after == std::string::npos) return false;
        saw_attribute = true;
        i = skip_ws_and_comments(s, after);
        if (i == std::string::npos) return true;
    }
    if (i >= s.size()) return false;
    if (starts_with_word_ci(s, i, "param", "(")) return true;
    // 特性后面不是 param(例如 `[int]$x = 1` 类型转换、`[IO.File]::...`)→ 普通语句。
    if (saw_attribute) return false;
    for (const char* keyword : {"begin", "process", "end", "dynamicparam", "clean"}) {
        if (starts_with_word_ci(s, i, keyword, "{")) return true;
    }
    return false;
}

}  // namespace

std::string with_powershell_utf8_prelude(const std::string& command) {
    if (starts_with_leading_only_statement(command)) return command;
    return powershell_utf8_prelude() + command;
}

std::vector<std::string> probe_arguments(TerminalFamily family) {
    switch (family) {
        case TerminalFamily::Cmd:
            return {"/d", "/c", "exit", "0"};
        case TerminalFamily::PowerShell: {
            auto args = powershell_noninteractive_args();
            args.push_back("-EncodedCommand");
            args.push_back(encode_powershell_command("exit 0"));
            return args;
        }
        case TerminalFamily::Bash:
        case TerminalFamily::Posix:
            return {"-c", "exit 0"};
    }
    return {"-c", "exit 0"};
}

ShellCommandLine build_shell_command_line(const ResolvedTerminal& terminal,
                                          const std::string& command) {
    ShellCommandLine out;
    out.program = terminal.program;
    const std::string quoted_program = quote_shell_path_if_needed(terminal.program);
    switch (terminal.family) {
        case TerminalFamily::Cmd:
            // 与改动前的 `cmd.exe /c <command>` 逐字节一致:cmd 自己解析剩余文本。
            out.windows_command_line = quoted_program + " /c " + command;
            out.argv = {terminal.program, "/c", command};
            break;
        case TerminalFamily::PowerShell: {
            out.argv.push_back(terminal.program);
            std::string line = quoted_program;
            for (const auto& a : powershell_noninteractive_args()) {
                out.argv.push_back(a);
                line += " " + a;
            }
            // 编码前置脚本在权限审批之后拼接:分类器、审计、hooks 看到的仍是原始命令。
            const std::string encoded =
                encode_powershell_command(with_powershell_utf8_prelude(command));
            out.argv.push_back("-EncodedCommand");
            out.argv.push_back(encoded);
            line += " -EncodedCommand " + encoded;
            out.windows_command_line = line;
            break;
        }
        case TerminalFamily::Bash:
        case TerminalFamily::Posix:
            out.argv = {terminal.program, "-c", command};
            out.windows_command_line =
                quoted_program + " -c " + quote_windows_argument(command);
            break;
    }
    return out;
}

}  // namespace acecode::environment
