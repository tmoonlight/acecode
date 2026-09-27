#include "command_classifier.hpp"

#include "permissions/path_validator.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <vector>

namespace acecode::sandbox {

namespace {

constexpr int kMaxWrapperDepth = 8;

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool in(const std::string& value, std::initializer_list<const char*> set) {
    for (const char* item : set) {
        if (value == item) return true;
    }
    return false;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool is_url_token(const std::string& token) {
    const std::string t = lower(token);
    return starts_with(t, "http://") || starts_with(t, "https://");
}

// ---------------------------------------------------------------------------
// 词法
// ---------------------------------------------------------------------------

struct ScanFacts {
    bool malformed = false;
    bool has_dollar = false;        // $VAR / $(...) / $_(PowerShell)
    bool has_backtick = false;      // `cmd` / PowerShell 转义
    bool has_percent_var = false;   // %VAR%(cmd)
    bool has_unquoted_glob = false; // * ? [ 在引号外
    bool has_grouping = false;      // { } ( ) 在引号外(子 shell / 脚本块 / 函数)
    bool has_redirect = false;      // > >> < 2> &>
    bool has_background = false;    // 单个 &
    bool has_newline = false;
};

struct Scanned {
    std::vector<ShellWord> words;
    ScanFacts facts;
};

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

Scanned scan(const std::string& command, CommandPlatform platform) {
    Scanned out;
    std::string cur;
    bool cur_quoted = false;
    bool have_cur = false;
    int percent_count = 0;

    auto flush = [&] {
        if (have_cur) {
            out.words.push_back({cur, cur_quoted, false});
        }
        cur.clear();
        cur_quoted = false;
        have_cur = false;
    };
    auto push_op = [&](std::string op) {
        flush();
        out.words.push_back({std::move(op), false, true});
    };

    for (std::size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];
        if (c == '\'' && platform != CommandPlatform::Cmd) {
            // 单引号:字面量到下一个单引号。
            std::size_t j = command.find('\'', i + 1);
            if (j == std::string::npos) { j = command.size(); out.facts.malformed = true; }
            cur += command.substr(i + 1, j - i - 1);
            cur_quoted = true;
            have_cur = true;
            i = j;
            continue;
        }
        if (c == '"') {
            std::size_t j = i + 1;
            while (j < command.size() && command[j] != '"') {
                if (((platform == CommandPlatform::Posix && command[j] == '\\') ||
                     (platform == CommandPlatform::PowerShell && command[j] == '`')) && j + 1 < command.size() &&
                    command[j + 1] == '"') {
                    cur += '"';
                    j += 2;
                    continue;
                }
                if (command[j] == '$') out.facts.has_dollar = true;
                if (command[j] == '`') out.facts.has_backtick = true;
                if (command[j] == '%') ++percent_count;
                if (platform == CommandPlatform::Cmd && command[j] == '!') out.facts.malformed = true;
                cur += command[j];
                ++j;
            }
            cur_quoted = true;
            have_cur = true;
            if (j == command.size()) out.facts.malformed = true;
            i = j;
            continue;
        }
        if (c == '\\' && platform == CommandPlatform::Posix && i + 1 < command.size()) {
            const char n = command[i + 1];
            if (is_space(n) || n == '"' || n == '\'' || n == '$' || n == '`' ||
                n == '|' || n == '&' || n == ';' || n == '>' || n == '<' || n == '\n') {
                cur += n;
                have_cur = true;
                ++i;
                continue;
            }
        }
        if (c == '\n') {
            out.facts.has_newline = true;
            push_op("\n");
            continue;
        }
        if (is_space(c)) {
            flush();
            continue;
        }
        if (c == '&') {
            if (i + 1 < command.size() && command[i + 1] == '&') {
                push_op("&&");
                ++i;
            } else if (i + 1 < command.size() && command[i + 1] == '>') {
                out.facts.has_redirect = true;
                push_op("&>");
                ++i;
            } else {
                out.facts.has_background = true;
                push_op("&");
            }
            continue;
        }
        if (c == '|') {
            if (i + 1 < command.size() && command[i + 1] == '|') {
                push_op("||");
                ++i;
            } else if (i + 1 < command.size() && command[i + 1] == '&') {
                out.facts.has_redirect = true;
                push_op("|&");
                ++i;
            } else {
                push_op("|");
            }
            continue;
        }
        if (c == ';') {
            push_op(";");
            continue;
        }
        if (c == '>') {
            out.facts.has_redirect = true;
            // `2>` / `1>`:前面紧贴的裸数字 token 是 fd,不是参数。
            if (have_cur && !cur_quoted && cur.size() == 1 && std::isdigit(static_cast<unsigned char>(cur[0]))) {
                cur.clear();
                have_cur = false;
            }
            if (i + 1 < command.size() && command[i + 1] == '>') {
                push_op(">>");
                ++i;
            } else {
                push_op(">");
            }
            continue;
        }
        if (c == '<') {
            out.facts.has_redirect = true;
            push_op("<");
            continue;
        }
        if (c == '$') out.facts.has_dollar = true;
        if (c == '`') out.facts.has_backtick = true;
        if (c == '%') ++percent_count;
        if ((platform == CommandPlatform::Cmd && (c == '^' || c == '!')) || c == '#') {
            out.facts.malformed = true;
        }
        if (c == '*' || c == '?' || c == '[') out.facts.has_unquoted_glob = true;
        if (c == '{' || c == '}' || c == '(' || c == ')') out.facts.has_grouping = true;
        cur += c;
        have_cur = true;
    }
    flush();
    out.facts.has_percent_var = percent_count >= 2;
    return out;
}

bool is_control_keyword(const std::string& token) {
    return in(lower(token), {"if", "then", "else", "elif", "fi", "for", "while", "until",
                             "do", "done", "case", "esac", "function", "select", "in",
                             "foreach", "switch", "try", "catch", "finally"});
}

// 尽力拆段:按 && || | ; & 换行 |& 切;重定向运算符与其目标从段里剔除,
// 目标单独记下(供 /dev/sd* 判定)。
struct RawSegment {
    std::vector<std::string> tokens;
    std::string leading_operator;              // 与前一段之间的运算符,首段为空
    std::vector<std::string> redirect_targets;
};

std::vector<RawSegment> split_segments(const std::vector<ShellWord>& words) {
    std::vector<RawSegment> segments;
    RawSegment cur;
    bool expect_redirect_target = false;
    for (const auto& w : words) {
        if (w.is_operator) {
            if (w.text == ">" || w.text == ">>" || w.text == "<" || w.text == "&>") {
                expect_redirect_target = true;
                continue;
            }
            segments.push_back(std::move(cur));
            cur = RawSegment{};
            cur.leading_operator = w.text;
            expect_redirect_target = false;
            continue;
        }
        if (expect_redirect_target) {
            cur.redirect_targets.push_back(w.text);
            expect_redirect_target = false;
            continue;
        }
        cur.tokens.push_back(w.text);
    }
    segments.push_back(std::move(cur));
    // 丢掉空段(例如尾随 `;`),但保留至少一段。
    std::vector<RawSegment> out;
    for (auto& s : segments) {
        if (!s.tokens.empty() || !s.redirect_targets.empty()) out.push_back(std::move(s));
    }
    if (out.empty()) out.push_back(RawSegment{});
    return out;
}

// ---------------------------------------------------------------------------
// 危险判定
// ---------------------------------------------------------------------------

bool has_flag_char(const std::string& token, char flag) {
    // `-rf` / `-fr` / `-Rf` 这类合并短选项。
    if (token.size() < 2 || token[0] != '-' || token[1] == '-') return false;
    return token.find(flag, 1) != std::string::npos;
}

bool rm_is_dangerous(const std::vector<std::string>& tokens) {
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        const std::string t = lower(tokens[i]);
        if (t == "--force" || t == "--recursive" || t == "--no-preserve-root") return true;
        if (has_flag_char(t, 'f') || has_flag_char(t, 'r')) return true;
    }
    return false;
}

bool git_is_dangerous(const std::vector<std::string>& tokens) {
    if (tokens.size() < 2) return false;
    // 跳过全局选项(-C dir / -c k=v / --no-pager ...)。
    std::size_t i = 1;
    while (i < tokens.size() && !tokens[i].empty() && tokens[i][0] == '-') {
        if (tokens[i] == "-C" || tokens[i] == "-c") i += 2; else i += 1;
    }
    if (i >= tokens.size()) return false;
    const std::string sub = lower(tokens[i]);
    std::vector<std::string> args(tokens.begin() + static_cast<std::ptrdiff_t>(i + 1), tokens.end());
    auto has = [&](std::initializer_list<const char*> set) {
        for (const auto& a : args) {
            if (in(lower(a), set)) return true;
        }
        return false;
    };
    if (sub == "reset") return has({"--hard", "--merge"});
    if (sub == "clean") {
        for (const auto& a : args) {
            const std::string l = lower(a);
            if (l == "--force" || has_flag_char(l, 'f')) return true;
        }
        return false;
    }
    if (sub == "checkout" || sub == "restore") {
        for (const auto& a : args) {
            if (a == "." || a == "*") return true;
        }
        return false;
    }
    if (sub == "push") return has({"--force", "-f", "--force-with-lease", "--delete", "-d"});
    if (sub == "branch") {
        for (const auto& a : args) {
            if (a == "-D" || lower(a) == "--delete" || lower(a) == "--force" || a == "-f") return true;
            if (has_flag_char(a, 'D')) return true;
        }
        return false;
    }
    if (sub == "stash") return has({"drop", "clear"});
    if (sub == "reflog") return has({"expire", "delete"});
    if (sub == "gc") return has({"--prune=now", "--prune"});
    if (sub == "filter-branch" || sub == "filter-repo") return true;
    return false;
}

bool powershell_remove_is_dangerous(const std::vector<std::string>& tokens) {
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        const std::string t = lower(tokens[i]);
        if (starts_with(t, "-rec") || starts_with(t, "-fo") || t == "-r" || t == "-f") return true;
    }
    return false;
}

bool cmd_del_is_dangerous(const std::vector<std::string>& tokens) {
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        const std::string t = lower(tokens[i]);
        if (t == "/f" || t == "/s" || t == "/q") return true;
    }
    return false;
}

bool any_arg_in(const std::vector<std::string>& tokens, std::initializer_list<const char*> set) {
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        if (in(lower(tokens[i]), set)) return true;
    }
    return false;
}

bool any_arg_is_url(const std::vector<std::string>& tokens) {
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        if (is_url_token(tokens[i])) return true;
    }
    return false;
}

bool any_arg_starts_with(const std::vector<std::string>& tokens, const char* prefix) {
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        if (starts_with(lower(tokens[i]), prefix)) return true;
    }
    return false;
}

// 单段危险判定(不解包)。
bool segment_is_dangerous(const std::vector<std::string>& tokens, std::string* reason) {
    if (tokens.empty()) return false;
    const std::string base = command_basename(tokens[0]);
    auto hit = [&](const char* why) {
        if (reason) *reason = why;
        return true;
    };

    if (base == "rm" && rm_is_dangerous(tokens)) return hit("rm with force/recursive flags");
    if (base == "git" && git_is_dangerous(tokens)) return hit("destructive git subcommand");
    if (in(base, {"sudo", "su", "doas", "runas", "pkexec"})) return hit("privilege escalation");
    if (base == "chmod" && any_arg_in(tokens, {"-r", "--recursive"}) && any_arg_in(tokens, {"777", "-r", "--recursive"})) {
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i] == "777" || tokens[i] == "a+rwx") return hit("chmod -R 777");
        }
    }
    if (base == "chown" && any_arg_in(tokens, {"-r", "--recursive"})) return hit("chown -R");
    if (base == "dd" && (any_arg_starts_with(tokens, "if=") || any_arg_starts_with(tokens, "of="))) {
        return hit("dd raw device copy");
    }
    if (starts_with(base, "mkfs") || in(base, {"fdisk", "parted", "wipefs", "diskpart", "format",
                                                "bcdedit", "shutdown", "reboot", "halt", "poweroff",
                                                "killall", "mshta", "bitsadmin", "takeown",
                                                "stop-computer", "restart-computer",
                                                "set-executionpolicy", "invoke-expression", "iex",
                                                "format-volume", "clear-disk", "remove-partition",
                                                "remove-itemproperty"})) {
        return hit("destructive system command");
    }
    if (base == "kill" && (any_arg_in(tokens, {"-1"}) || (any_arg_in(tokens, {"-9"}) && any_arg_in(tokens, {"-1"})))) {
        return hit("kill all processes");
    }
    if (base == "crontab" && any_arg_in(tokens, {"-r"})) return hit("crontab -r");
    if (base == "vssadmin" && any_arg_in(tokens, {"delete"})) return hit("vssadmin delete");
    if (base == "wmic" && any_arg_in(tokens, {"delete", "terminate"})) return hit("wmic delete");
    if (base == "icacls" && (any_arg_starts_with(tokens, "/grant") || any_arg_starts_with(tokens, "/deny") ||
                             any_arg_starts_with(tokens, "/reset") || any_arg_starts_with(tokens, "/setowner"))) {
        return hit("icacls permission change");
    }
    if (base == "cipher" && any_arg_in(tokens, {"/w", "/w:"})) return hit("cipher /w");
    if (base == "certutil" && (any_arg_in(tokens, {"-decode", "-urlcache", "-encode"}))) {
        return hit("certutil download/decode");
    }
    if (base == "rundll32" && any_arg_starts_with(tokens, "url.dll")) return hit("rundll32 url handler");
    if (base == "reg" && any_arg_in(tokens, {"add", "delete", "import", "restore", "load", "unload"})) {
        return hit("registry write");
    }
    if (base == "regedit" && any_arg_in(tokens, {"/s", "/i"})) return hit("regedit import");
    if (base == "sc" && any_arg_in(tokens, {"create", "delete", "config", "start", "stop", "failure"})) {
        return hit("service control");
    }
    if (base == "schtasks" && any_arg_in(tokens, {"/create", "/delete", "/change", "/run", "/end"})) {
        return hit("scheduled task change");
    }
    if (base == "net" && any_arg_in(tokens, {"user", "localgroup", "group", "share", "start", "stop", "use"})) {
        return hit("net account/share change");
    }
    if (in(base, {"del", "erase"}) && cmd_del_is_dangerous(tokens)) return hit("del with /f /s /q");
    if (in(base, {"rd", "rmdir"}) && any_arg_in(tokens, {"/s"})) return hit("rd /s");
    if (in(base, {"remove-item", "ri", "del", "erase", "rd", "rmdir", "rm"}) && powershell_remove_is_dangerous(tokens)) {
        return hit("Remove-Item -Recurse/-Force");
    }
    if (in(base, {"start", "start-process", "saps", "invoke-item", "ii", "explorer", "xdg-open", "open"}) &&
        any_arg_is_url(tokens)) {
        return hit("launches a URL");
    }
    if (in(base, {"set-itemproperty", "new-itemproperty", "new-item", "remove-item", "clear-item"}) &&
        (any_arg_starts_with(tokens, "hklm:") || any_arg_starts_with(tokens, "hkcu:") ||
         any_arg_starts_with(tokens, "registry::"))) {
        return hit("registry write");
    }
    // 浏览器直接打开 URL。
    if (in(base, {"chrome", "msedge", "firefox", "iexplore", "safari", "brave", "chromium"}) &&
        any_arg_is_url(tokens)) {
        return hit("launches a URL");
    }
    return false;
}

bool redirect_target_is_device(const std::string& target) {
    const std::string t = lower(target);
    return starts_with(t, "/dev/sd") || starts_with(t, "/dev/nvme") || starts_with(t, "/dev/hd") ||
           starts_with(t, "/dev/mmcblk") || starts_with(t, "/dev/disk") || starts_with(t, "\\\\.\\physicaldrive");
}

bool is_downloader(const std::string& base) {
    return in(base, {"curl", "wget", "iwr", "invoke-webrequest", "irm", "invoke-restmethod", "fetch"});
}

bool is_interpreter(const std::string& base) {
    return in(base, {"sh", "bash", "zsh", "dash", "ksh", "fish", "python", "python3", "pwsh",
                     "powershell", "iex", "invoke-expression", "node", "perl", "ruby", "php", "cmd"});
}

// ---------------------------------------------------------------------------
// 安全判定
// ---------------------------------------------------------------------------

bool git_is_safe(const std::vector<std::string>& tokens) {
    std::size_t i = 1;
    while (i < tokens.size() && !tokens[i].empty() && tokens[i][0] == '-') {
        const std::string t = tokens[i];
        if (t == "-C") { i += 2; continue; }
        if (t == "-c" || starts_with(t, "-c") || starts_with(t, "--config-env")) return false;
        if (t == "--no-pager" || t == "-P" || starts_with(t, "--git-dir=") || starts_with(t, "--work-tree=")) { i += 1; continue; }
        if (t == "--version" || t == "-v" || t == "--help" || t == "-h") return tokens.size() == i + 1;
        return false;
    }
    if (i >= tokens.size()) return false;
    const std::string sub = lower(tokens[i]);
    std::vector<std::string> args(tokens.begin() + static_cast<std::ptrdiff_t>(i + 1), tokens.end());
    for (const auto& a : args) {
        if (starts_with(lower(a), "--output")) return false;   // git log/diff --output=file 会写文件
        if (a == "--ext-diff" || a == "--textconv" || a == "--no-index" ||
            a == "--open-files-in-pager" || starts_with(a, "--open-files-in-pager=") || starts_with(a, "-O") ||
            a == "--exec-path" || starts_with(a, "--exec-path=")) return false;
    }
    auto has = [&](std::initializer_list<const char*> set) {
        for (const auto& a : args) {
            if (in(lower(a), set)) return true;
        }
        return false;
    };
    auto positional_count = [&] {
        std::size_t n = 0;
        for (const auto& a : args) {
            if (a.empty() || a[0] != '-') ++n;
        }
        return n;
    };
    if (in(sub, {"status", "log", "diff", "show", "rev-parse", "ls-files", "ls-tree", "blame",
                 "describe", "shortlog", "cat-file", "grep", "diff-tree", "rev-list",
                 "count-objects", "version", "check-ignore", "name-rev",
                 "for-each-ref", "show-ref", "merge-base", "var"})) {
        return true;
    }
    if (sub == "branch") {
        if (has({"-d", "-D", "-m", "-M", "-c", "-C", "-f", "--delete", "--move", "--copy",
                 "--force", "-u", "--set-upstream-to", "--unset-upstream", "--edit-description"})) {
            return false;
        }
        for (const auto& a : args) {
            if (starts_with(lower(a), "--set-upstream-to=")) return false;
        }
        return positional_count() == 0;
    }
    if (sub == "remote") {
        if (args.empty()) return true;
        return in(lower(args[0]), {"-v", "--verbose", "show", "get-url"});
    }
    if (sub == "tag") {
        if (args.empty()) return true;
        if (has({"-a", "-d", "-f", "-s", "-m", "--delete", "--force", "--annotate"})) return false;
        return has({"-l", "--list"}) || positional_count() == 0;
    }
    if (sub == "stash") return !args.empty() && in(lower(args[0]), {"list", "show"});
    if (sub == "config") {
        if (has({"--unset", "--unset-all", "--add", "--replace-all", "--edit", "-e",
                 "--rename-section", "--remove-section"})) {
            return false;
        }
        return has({"--get", "--get-all", "--get-regexp", "--list", "-l"});
    }
    if (sub == "worktree") return !args.empty() && lower(args[0]) == "list";
    if (sub == "reflog") return args.empty() || lower(args[0]) == "show";
    if (sub == "submodule") return !args.empty() && lower(args[0]) == "status";
    return false;
}

bool find_is_safe(const std::vector<std::string>& tokens) {
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        if (in(lower(tokens[i]), {"-exec", "-execdir", "-ok", "-okdir", "-delete", "-fprint",
                                  "-fprint0", "-fprintf", "-fls"})) {
            return false;
        }
    }
    return true;
}

bool sed_is_safe(const std::vector<std::string>& tokens) {
    if (tokens.size() == 2 && (tokens[1] == "--version" || tokens[1] == "--help")) return true;
    if (tokens.size() < 3) return false;
    for (std::size_t a = 3; a < tokens.size(); ++a) {
        if (!tokens[a].empty() && tokens[a][0] == '-') return false;
    }
    const std::string opt = tokens[1];
    if (opt != "-n" && opt != "-ne" && opt != "--quiet" && opt != "--silent") return false;
    // 只放行 `N p` / `N,M p` 的纯打印脚本。
    const std::string& script = tokens[2];
    std::size_t i = 0;
    auto digits = [&] {
        std::size_t start = i;
        while (i < script.size() && std::isdigit(static_cast<unsigned char>(script[i]))) ++i;
        return i > start;
    };
    if (!digits()) return false;
    if (i < script.size() && script[i] == ',') {
        ++i;
        if (!digits() && !(i < script.size() && script[i] == '$')) return false;
        if (i < script.size() && script[i] == '$') ++i;
    }
    return i + 1 == script.size() && script[i] == 'p';
}

bool version_or_help_query(const std::vector<std::string>& tokens) {
    if (tokens.size() != 2) return false;
    const std::string a = lower(tokens[1]);
    return in(a, {"--version", "-version", "--help", "-help", "/?", "version"});
}

// 单段安全判定(不解包;调用方保证该段不危险)。
bool segment_is_safe(const std::vector<std::string>& tokens) {
    if (tokens.empty()) return false;
    const std::string base = command_basename(tokens[0]);
    auto has_arg = [&](std::initializer_list<const char*> set) { return any_arg_in(tokens, set); };

    // 不把任意程序的 --version 当作安全命令:自定义脚本仍可以执行写操作。
    if (tokens[0].find('/') != std::string::npos || tokens[0].find('\\') != std::string::npos) {
        return false;
    }
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        const std::string t = lower(tokens[i]);
        if (starts_with(t, "--output") || starts_with(t, "--log") || starts_with(t, "--pre=") || t == "--pre" ||
            t == "--pre-glob" || t == "--exec" || t == "--exec-batch" ||
            t == "--in-place" || starts_with(t, "--in-place=")) return false;
    }
    if (in(base, {"uniq", "xxd", "yq", "ctags", "less", "more", "man", "help", "ldd", "cloc", "bat", "file"})) {
        return false;
    }

    // 无参数约束的只读工具(POSIX + cmd + PowerShell 别名并集)。
    if (in(base, {"cat", "ls", "pwd", "echo", "grep", "egrep", "fgrep", "rg", "head", "tail", "wc",
                  "which", "nl", "basename", "dirname", "id", "printf", "realpath", "readlink",
                  "stat", "uniq", "tr", "cut", "file", "du", "df", "diff", "comm", "column",
                  "tree", "ps", "uptime", "uname", "whoami", "true", "false", "less", "more",
                  "dir", "type", "cd", "chdir", "where", "findstr", "ver", "vol", "systeminfo",
                  "tasklist", "netstat", "md5sum", "sha1sum", "sha256sum", "printenv", "locale",
                  "strings", "hexdump", "xxd", "od", "jq", "yq", "tac", "rev", "fold", "expand",
                  "unexpand", "paste", "join", "look", "cksum", "b2sum", "nproc", "arch", "getconf",
                  "ldd", "nm", "objdump", "readelf", "ctags", "cloc", "tokei", "bat",
                  // PowerShell
                  "get-content", "gc", "get-childitem", "gci", "get-item", "gi", "get-location", "gl",
                  "get-command", "gcm", "get-process", "gps", "get-date", "get-host", "get-member",
                  "gm", "select-string", "sls", "select-object", "select", "sort-object",
                  "measure-object", "measure", "format-table", "ft", "format-list", "fl",
                  "format-wide", "fw", "out-string", "write-output", "write", "write-host",
                  "test-path", "resolve-path", "rvpa", "split-path", "join-path", "convertto-json",
                  "convertfrom-json", "compare-object", "group-object", "group", "get-variable",
                  "gv", "get-alias", "gal", "get-history", "h", "get-help", "help", "man",
                  "get-service", "gsv", "get-psdrive", "get-culture", "get-computerinfo",
                  "out-host", "oh", "get-filehash", "get-unique", "gu", "where-object", "?",
                  "get-itemproperty", "gp", "get-childitem", "convertto-csv", "convertfrom-csv",
                  "out-null", "get-psversion", "get-executionpolicy", "get-module", "gmo",
                  "measure-command"})) {
        return true;
    }
    if (base == "sort") {
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            if (starts_with(tokens[i], "-o") || starts_with(lower(tokens[i]), "/o") ||
                starts_with(tokens[i], "--output") || starts_with(tokens[i], "--compress-program")) return false;
        }
        return true;
    }
    if (base == "date") {
        if (tokens.size() == 1) return true;
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            if (starts_with(tokens[i], "-s") || starts_with(tokens[i], "--set")) return false;
        }
        return !tokens[1].empty() && (lower(tokens[1]) == "/t" || tokens[1][0] == '+' || tokens[1][0] == '-');
    }
    if (base == "time") return tokens.size() == 2 && lower(tokens[1]) == "/t";
    if (base == "hostname") return tokens.size() == 1 || (tokens.size() == 2 && in(tokens[1], {"-s", "-f", "-d", "-i", "-I"}));
    if (base == "env") return tokens.size() == 1;
    if (base == "set") {
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i].find('=') != std::string::npos) return false;
        }
        return true;
    }
    if (base == "ipconfig") return !has_arg({"/release", "/renew", "/flushdns", "/registerdns", "/setclassid", "/release6", "/renew6"});
    if (base == "find") return find_is_safe(tokens);
    if (base == "sed") return sed_is_safe(tokens);
    if (base == "git") return git_is_safe(tokens);
    if (base == "cargo") return tokens.size() == 2 && in(tokens[1], {"--version", "-V", "version", "--list"});
    if (in(base, {"node", "python", "python3", "python2", "ruby", "perl", "php", "java", "javac",
                  "go", "rustc", "gcc", "g++", "clang", "clang++", "cl", "cmake", "ctest", "ninja",
                  "make", "dotnet", "npm", "pnpm", "yarn", "npx", "pip", "pip3", "bun", "deno",
                  "docker", "kubectl", "gh", "az", "aws", "terraform", "tsc", "eslint", "prettier"})) {
        if (tokens.size() == 2 && in(lower(tokens[1]), {"--version", "-v", "-version", "version", "--help", "-h"})) {
            return true;
        }
        if (base == "npm" && tokens.size() >= 2 && in(lower(tokens[1]), {"ls", "list", "view", "outdated", "explain", "why", "prefix", "root"})) return true;
        if ((base == "pnpm" || base == "yarn") && tokens.size() >= 2 && in(lower(tokens[1]), {"ls", "list", "why", "outdated", "licenses"})) return true;
        if ((base == "pip" || base == "pip3") && tokens.size() >= 2 && in(lower(tokens[1]), {"list", "show", "freeze", "check"})) return true;
        if (base == "go" && tokens.size() >= 2 && in(lower(tokens[1]), {"version", "env"})) {
            return !has_arg({"-w", "-u"});
        }
        if (base == "dotnet" && tokens.size() == 2 && in(lower(tokens[1]), {"--info", "--list-sdks", "--list-runtimes"})) return true;
        if (base == "docker" && tokens.size() >= 2 && in(lower(tokens[1]), {"ps", "images", "version", "info"})) return true;
        if (base == "kubectl" && tokens.size() >= 2 && in(lower(tokens[1]), {"get", "describe", "version", "config"}) &&
            !(lower(tokens[1]) == "config" && tokens.size() >= 3 && !in(lower(tokens[2]), {"view", "current-context", "get-contexts"}))) {
            return true;
        }
        if (base == "gh" && tokens.size() >= 3 && in(lower(tokens[1]), {"pr", "issue", "repo", "run"}) &&
            in(lower(tokens[2]), {"list", "view", "status", "checks", "diff"})) {
            return true;
        }
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 分类(含解包)
// ---------------------------------------------------------------------------

struct SegmentVerdict {
    CommandKind kind = CommandKind::Unknown;
    std::string reason;
    std::vector<CommandSegment> nested_segments;
    bool split_safely = true;
};

CommandClassification classify_impl(const std::string& command, int depth, CommandPlatform platform);

SegmentVerdict classify_inner_script(const std::string& script, int depth, CommandPlatform platform) {
    CommandClassification inner = classify_impl(script, depth + 1, platform);
    inner.segments.insert(inner.segments.end(), inner.nested_segments.begin(), inner.nested_segments.end());
    return {inner.kind, inner.reason, std::move(inner.segments), inner.split_safely};
}

std::string join_tokens(const std::vector<std::string>& tokens, std::size_t from) {
    std::string out;
    for (std::size_t i = from; i < tokens.size(); ++i) {
        if (!out.empty()) out += ' ';
        // 重新加引号,免得内层 tokenizer 把含空格的单个 token 拆开。
        const std::string& t = tokens[i];
        if (t.find_first_of(" \t\"'") != std::string::npos) {
            std::string quoted = "\"";
            for (char c : t) {
                if (c == '"') quoted += "\\\"";
                else quoted += c;
            }
            quoted += '"';
            out += quoted;
        } else {
            out += t;
        }
    }
    return out;
}

SegmentVerdict classify_segment(const std::vector<std::string>& tokens, int depth, CommandPlatform platform) {
    if (tokens.empty()) return {CommandKind::Unknown, "empty command"};
    if (depth > kMaxWrapperDepth) return {CommandKind::Unknown, "wrapper nesting too deep"};

    std::string why;
    const std::string base = command_basename(tokens[0]);
    if (segment_is_dangerous(tokens, &why)) {
        SegmentVerdict result{CommandKind::Dangerous, why};
        if (base == "sudo" || base == "doas") {
            std::size_t start = 1;
            while (start < tokens.size() && !tokens[start].empty() && tokens[start][0] == '-') {
                if (tokens[start] == "--") { ++start; break; }
                start += in(tokens[start], {"-u", "-g", "-h", "-p", "-C", "-T", "-R", "-D", "--user", "--group"}) ? 2 : 1;
            }
            if (start < tokens.size()) {
                std::vector<std::string> inner(tokens.begin() + start, tokens.end());
                auto nested = classify_segment(inner, depth + 1, platform);
                result.nested_segments = std::move(nested.nested_segments);
                result.nested_segments.push_back({std::move(inner)});
            }
        }
        return result;
    }

    // 解包 sh -c / bash -lc。
    if (in(base, {"sh", "bash", "zsh", "dash", "ksh"})) {
        for (std::size_t i = 1; i + 1 < tokens.size(); ++i) {
            const std::string& opt = tokens[i];
            if (opt.size() >= 2 && opt[0] == '-' && opt[1] != '-' && opt.find('c') != std::string::npos) {
                auto inner = classify_inner_script(tokens[i + 1], depth, CommandPlatform::Posix);
                for (std::size_t option = 1; option < i; ++option) {
                    if (!in(tokens[option], {"--noprofile", "--norc", "-l"})) {
                        if (inner.kind == CommandKind::KnownSafe) inner.kind = CommandKind::Unknown;
                    }
                }
                if (!in(opt, {"-c", "-lc", "-cl"}) && inner.kind == CommandKind::KnownSafe) inner.kind = CommandKind::Unknown;
                return inner;
            }
        }
        return {CommandKind::Unknown, "shell without -c"};
    }
    if (base == "cmd") {
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            const std::string opt = lower(tokens[i]);
            if (opt == "/c" || opt == "/k") {
                if (i + 1 >= tokens.size()) return {CommandKind::Unknown, "cmd /c without script"};
                const std::string script = i + 2 == tokens.size() ? tokens[i + 1] : join_tokens(tokens, i + 1);
                return classify_inner_script(script, depth, CommandPlatform::Cmd);
            }
        }
        return {CommandKind::Unknown, "cmd without /c"};
    }
    if (in(base, {"powershell", "pwsh"})) {
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            const std::string opt = lower(tokens[i]);
            if (in(opt, {"-encodedcommand", "-enc", "-e", "-ec", "-encoded"})) {
                return {CommandKind::Dangerous, "PowerShell encoded command"};
            }
        }
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            const std::string opt = lower(tokens[i]);
            if (in(opt, {"-command", "-c", "-com", "-comm"})) {
                if (i + 1 >= tokens.size()) return {CommandKind::Unknown, "powershell -Command without script"};
                const std::string script = i + 2 == tokens.size() ? tokens[i + 1] : join_tokens(tokens, i + 1);
                return classify_inner_script(script, depth, CommandPlatform::PowerShell);
            }
            if (in(opt, {"-file", "-f"})) return {CommandKind::Unknown, "powershell -File"};
        }
        return {CommandKind::Unknown, "powershell without -Command"};
    }
    if (base == "env") {
        std::size_t i = 1;
        while (i < tokens.size()) {
            const std::string& t = tokens[i];
            if (t.find('=') != std::string::npos && t[0] != '-') { ++i; continue; }
            if (t == "-i" || t == "--ignore-environment") { ++i; continue; }
            if (t == "-u" || t == "--unset") { i += 2; continue; }
            break;
        }
        if (i >= tokens.size()) return {CommandKind::KnownSafe, "env listing"};
        std::vector<std::string> rest(tokens.begin() + static_cast<std::ptrdiff_t>(i), tokens.end());
        auto result = classify_segment(rest, depth + 1, platform);
        result.nested_segments.push_back({rest});
        if (result.kind == CommandKind::KnownSafe && i > 1) result.kind = CommandKind::Unknown;
        return result;
    }
    if (in(base, {"nice", "time", "command", "builtin", "exec"})) {
        std::size_t i = 1;
        if (base == "nice" && i < tokens.size() && tokens[i] == "-n") i += 2;
        if (i >= tokens.size()) return {CommandKind::Unknown, "wrapper without command"};
        std::vector<std::string> rest(tokens.begin() + static_cast<std::ptrdiff_t>(i), tokens.end());
        auto result = classify_segment(rest, depth + 1, platform);
        result.nested_segments.push_back({rest});
        return result;
    }

    if (segment_is_safe(tokens)) return {CommandKind::KnownSafe, "known read-only command"};
    return {CommandKind::Unknown, "not in the known-safe list: " + base};
}

CommandClassification classify_impl(const std::string& command, int depth, CommandPlatform platform) {
    CommandClassification out;
    if (depth > kMaxWrapperDepth) { out.reason = "wrapper nesting too deep"; return out; }
    Scanned scanned = scan(command, platform);
    std::vector<RawSegment> raw = split_segments(scanned.words);

    for (const auto& seg : raw) {
        CommandSegment s;
        s.tokens = seg.tokens;
        out.segments.push_back(std::move(s));
    }

    const ScanFacts& f = scanned.facts;
    bool plain = !f.malformed && !f.has_dollar && !f.has_backtick && !f.has_percent_var && !f.has_unquoted_glob &&
                 !f.has_grouping && !f.has_redirect && !f.has_background && !f.has_newline;
    for (const auto& seg : raw) {
        if (seg.tokens.empty()) plain = false;
        else if (is_control_keyword(seg.tokens[0])) plain = false;
    }
    out.split_safely = plain;

    // 1) 危险:对每段(含解包)与整体结构判定,任一命中即返回。
    for (std::size_t i = 0; i < raw.size(); ++i) {
        for (const auto& target : raw[i].redirect_targets) {
            if (redirect_target_is_device(target)) {
                out.kind = CommandKind::Dangerous;
                out.reason = "writes to a raw block device";
                return out;
            }
        }
        if (raw[i].tokens.empty()) continue;
        const std::string base = command_basename(raw[i].tokens[0]);
        // 下载即执行:downloader | interpreter。
        if (is_downloader(base)) {
            for (std::size_t j = i + 1; j < raw.size(); ++j) {
                if (raw[j].leading_operator != "|" && raw[j].leading_operator != "|&") break;
                if (!raw[j].tokens.empty() && is_interpreter(command_basename(raw[j].tokens[0]))) {
                    out.kind = CommandKind::Dangerous;
                    out.reason = "pipes a download into an interpreter";
                    return out;
                }
            }
        }
    }

    std::vector<SegmentVerdict> verdicts;
    verdicts.reserve(raw.size());
    for (const auto& seg : raw) {
        verdicts.push_back(classify_segment(seg.tokens, depth, platform));
        const auto& nested = verdicts.back().nested_segments;
        out.nested_segments.insert(out.nested_segments.end(), nested.begin(), nested.end());
        if (!verdicts.back().split_safely) plain = false;
    }
    out.split_safely = plain;
    for (const auto& v : verdicts) {
        if (v.kind == CommandKind::Dangerous) {
            out.kind = CommandKind::Dangerous;
            out.reason = v.reason;
            return out;
        }
    }

    // 2) 非普通脚本不可能是 KnownSafe。
    if (!plain) {
        out.kind = CommandKind::Unknown;
        if (f.has_redirect) out.reason = "script contains a redirection";
        else if (f.has_dollar || f.has_backtick || f.has_percent_var) out.reason = "script contains variable or command substitution";
        else if (f.has_unquoted_glob) out.reason = "script contains an unquoted wildcard";
        else if (f.has_grouping) out.reason = "script contains grouping or a script block";
        else if (f.has_background) out.reason = "script runs a background job";
        else if (f.has_newline) out.reason = "script spans multiple lines";
        else out.reason = "script contains control flow";
        return out;
    }

    // 3) 每段都安全、且没有碰敏感路径,才是 KnownSafe。
    for (std::size_t i = 0; i < verdicts.size(); ++i) {
        if (verdicts[i].kind != CommandKind::KnownSafe) {
            out.kind = CommandKind::Unknown;
            out.reason = verdicts[i].reason;
            return out;
        }
        if (segment_references_sensitive_path(out.segments[i])) {
            out.kind = CommandKind::Unknown;
            out.reason = "references a sensitive path";
            return out;
        }
    }
    out.kind = CommandKind::KnownSafe;
    out.reason = "known read-only command";
    return out;
}

} // namespace

const char* command_kind_name(CommandKind kind) {
    switch (kind) {
        case CommandKind::KnownSafe: return "known_safe";
        case CommandKind::Dangerous: return "dangerous";
        case CommandKind::Unknown:   return "unknown";
    }
    return "unknown";
}

CommandPlatform host_command_platform() {
#ifdef _WIN32
    return CommandPlatform::Cmd;
#else
    return CommandPlatform::Posix;
#endif
}

std::vector<ShellWord> tokenize_shell_words(const std::string& command, CommandPlatform platform) {
    return scan(command, platform).words;
}

std::string command_basename(const std::string& token) {
    std::string t = token;
    const std::size_t slash = t.find_last_of("/\\");
    if (slash != std::string::npos) t = t.substr(slash + 1);
    t = lower(t);
    for (const char* ext : {".exe", ".cmd", ".bat", ".com"}) {
        const std::string e = ext;
        if (t.size() > e.size() && t.compare(t.size() - e.size(), e.size(), e) == 0) {
            t.erase(t.size() - e.size());
            break;
        }
    }
    return t;
}

std::string always_allow_prefix_for_segment(const CommandSegment& segment) {
    const auto tokens = always_allow_prefix_tokens_for_segment(segment);
    std::string out;
    for (const auto& token : tokens) {
        if (!out.empty()) out += ' ';
        out += token;
    }
    return out;
}

std::vector<std::string> always_allow_prefix_tokens_for_segment(const CommandSegment& segment) {
    if (segment.tokens.empty()) return {};
    const std::string base = command_basename(segment.tokens[0]);
    if (base.empty()) return {};
    if (in(base, {"bash", "sh", "zsh", "dash", "ksh", "cmd", "powershell", "pwsh",
                  "env", "sudo", "su", "doas", "pkexec", "runas", "nice", "time", "exec",
                  "command", "builtin", "eval", "source", ".", "xargs", "find", "awk", "gawk",
                  "busybox", "python", "python2", "python3", "node", "perl", "ruby", "php",
                  "lua", "rscript", "npx", "tsx", "ts-node", "osascript", "wscript", "cscript",
                  "start", "start-process", "saps", "mshta"})) return {};
    if (in(base, {"npm", "pnpm", "yarn", "bun", "deno"}) && segment.tokens.size() > 1 &&
        in(segment.tokens[1], {"exec", "dlx", "run", "eval", "x"})) return {};
    static const char* kMultiLevel[] = {
        "git", "npm", "pnpm", "yarn", "cargo", "dotnet", "pip", "pip3", "python", "python3",
        "node", "go", "make", "docker", "kubectl", "gh", "az", "aws", "npx", "pytest", "ctest",
        "cmake", "bun", "deno", "terraform", "gradle", "mvn",
    };
    for (const char* name : kMultiLevel) {
        if (base == name && (segment.tokens.size() < 2 || segment.tokens[1].empty() ||
                            segment.tokens[1][0] == '-')) return {};
        if (base == name && segment.tokens.size() >= 2 && !segment.tokens[1].empty() &&
            segment.tokens[1][0] != '-') {
            return {segment.tokens[0], segment.tokens[1]};
        }
    }
    return {segment.tokens[0]};
}

bool segment_references_sensitive_path(const CommandSegment& segment) {
    for (const auto& token : segment.tokens) {
        std::string norm = lower(token);
        std::replace(norm.begin(), norm.end(), '\\', '/');
        // 逐段看目录名。
        std::size_t start = 0;
        while (start <= norm.size()) {
            std::size_t end = norm.find('/', start);
            if (end == std::string::npos) end = norm.size();
            const std::string part = norm.substr(start, end - start);
            if (!part.empty()) {
                for (const auto& dir : PathValidator::dangerous_directories()) {
                    if (part == dir) return true;
                }
                for (const auto& df : PathValidator::dangerous_files()) {
                    if (df[0] == '*') {
                        const std::string ext = df.substr(1);
                        if (part.size() > ext.size() &&
                            part.compare(part.size() - ext.size(), ext.size(), ext) == 0) {
                            return true;
                        }
                    } else if (part == df) {
                        return true;
                    }
                }
            }
            if (end == norm.size()) break;
            start = end + 1;
        }
    }
    return false;
}

CommandClassification classify_command(const std::string& command, CommandPlatform platform) {
    return classify_impl(command, 0, platform);
}

} // namespace acecode::sandbox
