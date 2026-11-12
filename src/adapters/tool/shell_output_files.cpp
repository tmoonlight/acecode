#include "shell_output_files.hpp"

#include "ignore_utils.hpp"
#include "sandbox/command_classifier.hpp"
#include "session/session_outputs.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <system_error>
#include <unordered_set>

namespace acecode {

namespace {

namespace fs = std::filesystem;

// 文件系统时间戳有粗有细(FAT 2 秒),开始时刻往前放宽这么多。
constexpr std::int64_t kClockSlackMs = 2000;
constexpr std::size_t kOutputTailBytes = 32 * 1024;
constexpr std::size_t kMaxTextMentions = 40;
constexpr std::size_t kAbsoluteStartWindow = 300;

// 构建 / 包管理 / 版本控制 / 解包:会一次改动一批文件,它们动过的文档不是产物。
const std::unordered_set<std::string>& bulk_tools() {
    static const std::unordered_set<std::string> names = {
        "git", "svn", "hg", "npm", "pnpm", "yarn", "bun", "pip", "pip3", "uv", "conda",
        "poetry", "cargo", "cmake", "ctest", "ninja", "make", "nmake", "msbuild", "dotnet",
        "gradle", "gradlew", "mvn", "go", "docker", "apt", "apt-get", "brew", "winget",
        "choco", "tar", "unzip", "7z",
    };
    return names;
}

// 这些工具的 `run` 子命令是在跑用户自己的程序(uv run report.py),照常找。
const std::unordered_set<std::string>& runner_tools() {
    static const std::unordered_set<std::string> names = {
        "uv", "conda", "poetry", "cargo", "dotnet", "go",
    };
    return names;
}

// 不会生成文件的铺垫命令(cd x && python gen.py 里的 cd)。
const std::unordered_set<std::string>& inert_commands() {
    static const std::unordered_set<std::string> names = {
        "cd", "pushd", "popd", "echo", "set", "export", "pwd", "ls", "dir", "cat", "type",
        "sleep", "true", "exit", "chcp", "set-location", "write-host", "write-output",
    };
    return names;
}

// 整个跳过的目录(点开头的另算):依赖与缓存,不会是产物的落点。
bool is_skipped_directory(const std::string& name) {
    static const std::unordered_set<std::string> names = {
        "node_modules", "__pycache__", "venv", "site-packages", "bower_components",
    };
    return names.count(name) > 0;
}

bool is_ascii_alnum(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_ascii_alpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

std::string ascii_lower_copy(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

bool is_env_assignment(const std::string& token) {
    const std::size_t eq = token.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    if (!is_ascii_alpha(static_cast<unsigned char>(token[0])) && token[0] != '_') return false;
    for (std::size_t i = 1; i < eq; ++i) {
        const auto c = static_cast<unsigned char>(token[i]);
        if (!is_ascii_alnum(c) && c != '_') return false;
    }
    return true;
}

bool segment_may_produce(const sandbox::CommandSegment& segment) {
    std::size_t first = 0;
    while (first < segment.tokens.size() && is_env_assignment(segment.tokens[first])) ++first;
    if (first >= segment.tokens.size()) return false;
    const std::string name = sandbox::command_basename(segment.tokens[first]);
    if (name.empty() || inert_commands().count(name) > 0) return false;
    if (bulk_tools().count(name) > 0) {
        return runner_tools().count(name) > 0 && first + 1 < segment.tokens.size() &&
               ascii_lower_copy(segment.tokens[first + 1]) == "run";
    }
    return true;
}

// 能拿去查磁盘的「提到的路径」:是文档类扩展名,不是网址,不是 UNC(对一个
// 不存在的主机做 stat 会卡在网络超时上),不靠 shell 展开(~、$VAR、%VAR%)。
bool is_usable_mention(std::string_view mention) {
    if (mention.empty() || mention.size() > 1024) return false;
    if (mention.find("://") != std::string_view::npos) return false;
    if (mention.find_first_of("\n\r$%") != std::string_view::npos) return false;
    if (mention[0] == '~') return false;
    if (mention.size() >= 2 && (mention[0] == '/' || mention[0] == '\\') &&
        (mention[1] == '/' || mention[1] == '\\')) return false;
    return is_deliverable_file_name(mention);
}

void push_unique(std::vector<std::string>& out, std::string value) {
    if (std::find(out.begin(), out.end(), value) == out.end()) out.push_back(std::move(value));
}

// 紧凑片段的左边界:空白、引号、括号、重定向与通配符、逗号分号等号。
bool is_hard_stop(unsigned char c) {
    if (c <= 0x20) return true;
    switch (c) {
        case '"': case '\'': case '`': case '<': case '>': case '|': case '*': case '?':
        case '(': case ')': case '[': case ']': case '{': case '}': case ',': case ';':
        case '=':
            return true;
        default:
            return false;
    }
}

// 「从绝对路径起点到扩展名」这一整段里不允许出现的字符(空格允许)。
bool blocks_absolute_span(unsigned char c) {
    switch (c) {
        case '"': case '\'': case '`': case '<': case '>': case '|': case '*': case '?':
            return true;
        default:
            return false;
    }
}

// 中文输出里紧贴路径的全角标点:已保存到:report.xlsx。
bool is_cjk_punctuation(std::string_view code_point) {
    // 前五个是全角的冒号、逗号、左右括号、分号,用字节写,免得被编辑器换成半角。
    static constexpr std::array<std::string_view, 17> kMarks = {
        "\xEF\xBC\x9A", "\xEF\xBC\x8C", "\xEF\xBC\x88", "\xEF\xBC\x89", "\xEF\xBC\x9B",
        "。", "“", "”", "‘", "’", "「", "」", "、", "【", "】", "《", "》",
    };
    return std::find(kMarks.begin(), kMarks.end(), code_point) != kMarks.end();
}

bool is_drive_start(std::string_view text, std::size_t pos) {
    if (pos + 2 >= text.size()) return false;
    if (!is_ascii_alpha(static_cast<unsigned char>(text[pos])) || text[pos + 1] != ':') return false;
    if (text[pos + 2] != '\\' && text[pos + 2] != '/') return false;
    return pos == 0 || !is_ascii_alnum(static_cast<unsigned char>(text[pos - 1]));
}

bool is_posix_root_start(std::string_view text, std::size_t pos) {
    if (text[pos] != '/' || pos + 1 >= text.size()) return false;
    const auto next = static_cast<unsigned char>(text[pos + 1]);
    if (next == '/' || next <= 0x20) return false;
    if (pos == 0) return true;
    const auto before = static_cast<unsigned char>(text[pos - 1]);
    return before <= 0x20 || before == '=' || before == ':';
}

std::int64_t unix_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Unix 毫秒时刻换成文件时钟上的同一时刻(C++17 没有 clock_cast,用两边的「现在」对齐)。
fs::file_time_type file_time_at(std::int64_t unix_ms) {
    const std::int64_t age_ms = std::max<std::int64_t>(0, unix_now_ms() - unix_ms);
    return fs::file_time_type::clock::now() -
           std::chrono::duration_cast<fs::file_time_type::duration>(std::chrono::milliseconds(age_ms));
}

std::string dedupe_key(const std::string& path) {
    std::string key = path;
    std::replace(key.begin(), key.end(), '\\', '/');
#ifdef _WIN32
    key = ascii_lower_copy(std::move(key));
#endif
    return key;
}

std::vector<std::string> scan_recent_deliverables(const std::string& cwd,
                                                  fs::file_time_type threshold,
                                                  const ShellOutputScanLimits& limits) {
    struct Pending {
        fs::path dir;
        std::size_t depth = 0;
        bool shallow = false;   // 构建目录:只看这一层的文件,不往下走
    };
    std::vector<std::string> found;
    if (cwd.empty()) return found;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.max_millis);
    std::deque<Pending> queue;
    queue.push_back({path_from_utf8(cwd), 0, false});
    std::size_t visited = 0;
    bool out_of_budget = false;
    const fs::directory_iterator end;
    // 广度优先:预算用完时,浅层(最可能放产物的地方)已经看过了。
    while (!queue.empty() && !out_of_budget) {
        const Pending current = std::move(queue.front());
        queue.pop_front();
        std::error_code ec;
        fs::directory_iterator it(current.dir, fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != end; it.increment(ec)) {
            if (++visited > limits.max_entries ||
                ((visited & 0xFF) == 0 && std::chrono::steady_clock::now() > deadline)) {
                out_of_budget = true;
                break;
            }
            std::error_code entry_ec;
            // symlink_status:不跟符号链接和目录联接,免得绕圈或扫到工作目录外面。
            const fs::file_status status = it->symlink_status(entry_ec);
            if (entry_ec) continue;
            const std::string name = path_to_utf8(it->path().filename());
            if (fs::is_directory(status)) {
                if (current.shallow || current.depth + 1 > limits.max_depth) continue;
                if (name.empty() || name[0] == '.' || is_skipped_directory(name)) continue;
                queue.push_back({it->path(), current.depth + 1, should_ignore_dir(name)});
                continue;
            }
            if (!fs::is_regular_file(status) || !is_deliverable_file_name(name)) continue;
            const auto modified = it->last_write_time(entry_ec);
            if (entry_ec || modified < threshold) continue;
            found.push_back(path_to_utf8(it->path()));
            // 一条命令动了这么多文档类文件:是构建 / 检出这类批量操作,不当产物。
            if (found.size() > limits.bulk_threshold) return {};
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

} // namespace

bool shell_command_may_produce_files(const std::string& command) {
    if (command.find_first_not_of(" \t\r\n") == std::string::npos) return false;
    const auto classification = sandbox::classify_command(command);
    if (classification.kind == sandbox::CommandKind::KnownSafe) return false;
    // 输出重定向本身就是在写文件(echo ... > notes.md),不管前面是什么命令。
    for (const auto& word : sandbox::tokenize_shell_words(command)) {
        if (word.is_operator && !word.text.empty() && word.text[0] == '>') return true;
    }
    // 拆不出段(整条是脚本块之类)时按「可能生成」处理。
    if (classification.segments.empty()) return true;
    return std::any_of(classification.segments.begin(), classification.segments.end(),
                       segment_may_produce);
}

std::vector<std::string> shell_command_path_mentions(const std::string& command) {
    std::vector<std::string> out;
    for (const auto& word : sandbox::tokenize_shell_words(command)) {
        if (word.is_operator || word.text.empty()) continue;
        // --out=reports/q3.xlsx:路径是等号后面那段。
        const std::size_t eq = word.text.rfind('=');
        const std::string value = eq == std::string::npos ? word.text : word.text.substr(eq + 1);
        if (is_usable_mention(value)) push_unique(out, value);
        if (out.size() >= kMaxTextMentions) break;
    }
    return out;
}

std::vector<std::string> shell_text_path_mentions(std::string_view text) {
    std::vector<std::string> out;
    const std::size_t size = text.size();
    for (std::size_t dot = text.find('.'); dot != std::string_view::npos && out.size() < kMaxTextMentions;
         dot = text.find('.', dot + 1)) {
        // 扩展名:点后面的一串字母数字,后面必须是边界。
        std::size_t end = dot + 1;
        while (end < size && is_ascii_alnum(static_cast<unsigned char>(text[end]))) ++end;
        if (end == dot + 1 || end - dot > 9) continue;
        if (end < size) {
            const auto next = static_cast<unsigned char>(text[end]);
            if (next == '_' || next == '-') continue;
            // a.md.bak:后面还接着别的扩展名。
            if (next == '.' && end + 1 < size && is_ascii_alnum(static_cast<unsigned char>(text[end + 1]))) continue;
        }
        if (!is_deliverable_file_name(std::string("x") + std::string(text.substr(dot, end - dot)))) continue;

        // 向左扩到文件名 / 路径的起点。
        std::size_t start = dot;
        while (start > 0) {
            const auto c = static_cast<unsigned char>(text[start - 1]);
            if (c < 0x80) {
                if (is_hard_stop(c)) break;
                if (c == ':') {
                    // 只有盘符后面的冒号属于路径(C:\x);其它冒号是分隔(saved:a.csv)。
                    if (start >= 2 && is_drive_start(text, start - 2)) start -= 2;
                    break;
                }
                --start;
                continue;
            }
            std::size_t lead = start - 1;
            while (lead > 0 && start - lead < 4 &&
                   (static_cast<unsigned char>(text[lead]) & 0xC0) == 0x80) --lead;
            if (is_cjk_punctuation(text.substr(lead, start - lead))) break;
            start = lead;
        }
        if (start == dot) continue;   // 只有扩展名,没有文件名
        const std::string_view tight = text.substr(start, end - start);
        if (is_usable_mention(tight)) push_unique(out, std::string(tight));

        // 路径里带空格时紧凑片段只截到最后一个空格。同一行左边若有绝对路径起点,
        // 再给出从起点到扩展名的整段;对不对由调用方查磁盘决定。
        if (is_drive_start(text, start) || text[start] == '/') continue;
        const std::size_t line_start = text.rfind('\n', dot) == std::string_view::npos
            ? 0 : text.rfind('\n', dot) + 1;
        const std::size_t window_start = std::max(line_start,
            dot > kAbsoluteStartWindow ? dot - kAbsoluteStartWindow : 0);
        for (std::size_t pos = start; pos > window_start; --pos) {
            const std::size_t at = pos - 1;
            if (blocks_absolute_span(static_cast<unsigned char>(text[at]))) break;
            if (is_drive_start(text, at) || is_posix_root_start(text, at)) {
                const std::string_view span = text.substr(at, end - at);
                if (is_usable_mention(span)) push_unique(out, std::string(span));
                break;
            }
        }
    }
    return out;
}

ShellOutputProbe make_shell_output_probe(const std::string& command, const std::string& cwd,
                                         std::int64_t started_ms) {
    ShellOutputProbe probe;
    probe.cwd = cwd;
    probe.started_ms = started_ms;
    probe.enabled = started_ms > 0 && shell_command_may_produce_files(command);
    return probe;
}

ShellOutputProbe begin_shell_output_probe(const std::string& command, const std::string& cwd) {
    return make_shell_output_probe(command, cwd, unix_now_ms());
}

std::vector<std::string> collect_shell_output_files(const ShellOutputProbe& probe,
                                                    const std::string& command,
                                                    std::string_view output,
                                                    const ShellOutputScanLimits& limits) {
    std::vector<std::string> files;
    if (!probe.enabled) return files;
    try {
        const fs::file_time_type threshold = file_time_at(probe.started_ms - kClockSlackMs);
        std::unordered_set<std::string> seen;

        std::vector<std::string> mentions = shell_command_path_mentions(command);
        const std::string_view tail = output.size() > kOutputTailBytes
            ? output.substr(output.size() - kOutputTailBytes) : output;
        for (auto& mention : shell_text_path_mentions(tail)) mentions.push_back(std::move(mention));

        for (const auto& mention : mentions) {
            if (files.size() >= limits.max_mentions) break;
            fs::path path = path_from_utf8(mention);
            if (!path.is_absolute()) {
                if (probe.cwd.empty()) continue;
                path = path_from_utf8(probe.cwd) / path;
            }
            std::error_code ec;
            if (!fs::is_regular_file(path, ec) || ec) continue;
            const auto modified = fs::last_write_time(path, ec);
            if (ec || modified < threshold) continue;   // 提到了但不是这次生成的(只是读它)
            std::string resolved = path_to_utf8(path.lexically_normal());
            if (seen.insert(dedupe_key(resolved)).second) files.push_back(std::move(resolved));
        }

        for (auto& found : scan_recent_deliverables(probe.cwd, threshold, limits)) {
            std::string resolved = path_to_utf8(path_from_utf8(found).lexically_normal());
            if (seen.insert(dedupe_key(resolved)).second) files.push_back(std::move(resolved));
        }
    } catch (const std::exception&) {
        // 尽力而为:路径转换 / 文件系统异常都不该影响命令本身的结果。
        files.clear();
    }
    return files;
}

} // namespace acecode
