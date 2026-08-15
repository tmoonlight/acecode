#include "grep_tool.hpp"

#include "../hooks/hook_runner.hpp"
#include "../worktree/worktree_manager.hpp"
#include "utils/encoding.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

namespace acecode {
namespace {

constexpr std::size_t DEFAULT_HEAD_LIMIT = 200;
constexpr std::size_t MAX_HEAD_LIMIT = 2000;
constexpr std::size_t MAX_VISIBLE_LINE_CHARS = 1000;
constexpr std::size_t MAX_TOOL_OUTPUT_BYTES = 40000;
constexpr std::size_t MAX_GIT_STDOUT_BYTES = 5000000;
constexpr std::size_t MAX_GIT_STDERR_BYTES = 1000000;
constexpr int CHECK_IGNORE_TIMEOUT_MS = 5000;
constexpr int GREP_TIMEOUT_MS = 20000;
constexpr int WSL_GREP_TIMEOUT_MS = 60000;

constexpr const char* ABORTED_MSG =
    "[Aborted] Search abandoned because the user aborted the turn.";

struct ParsedMatch {
    std::string line;
    bool partial = false;
};

struct IgnoreProbeResult {
    bool ignored = false;
    bool aborted = false;
};

bool abort_requested(const ToolContext& ctx) {
    return ctx.abort_flag && ctx.abort_flag->load();
}

bool contains_nul(const std::string& value) {
    return value.find('\0') != std::string::npos;
}

std::size_t utf8_char_count(std::string_view value) {
    return static_cast<std::size_t>(std::count_if(
        value.begin(), value.end(), [](unsigned char byte) {
            return (byte & 0xC0) != 0x80;
        }));
}

std::string utf8_prefix_chars(std::string_view value, std::size_t max_chars) {
    if (max_chars == 0) return {};
    std::size_t chars = 0;
    std::size_t bytes = 0;
    while (bytes < value.size()) {
        const unsigned char byte = static_cast<unsigned char>(value[bytes]);
        if ((byte & 0xC0) != 0x80) {
            if (chars == max_chars) break;
            ++chars;
        }
        ++bytes;
    }
    return std::string(value.substr(0, bytes));
}

std::string bounded_visible_line(const std::string& raw_line, bool partial) {
    const std::string line = ensure_utf8(raw_line);
    const std::size_t chars = utf8_char_count(line);
    if (!partial && chars <= MAX_VISIBLE_LINE_CHARS) return line;

    std::string marker;
    if (partial) {
        marker = "... [truncated at subprocess output limit]";
    } else {
        marker = "... [truncated; " + std::to_string(chars) + " chars total]";
    }
    const std::size_t marker_chars = utf8_char_count(marker);
    const std::size_t prefix_chars =
        marker_chars < MAX_VISIBLE_LINE_CHARS
            ? MAX_VISIBLE_LINE_CHARS - marker_chars
            : 0;
    return utf8_prefix_chars(line, prefix_chars) + marker;
}

std::string bounded_excerpt(const std::string& value,
                            std::size_t max_chars = MAX_VISIBLE_LINE_CHARS) {
    const std::string valid = ensure_utf8(value);
    if (utf8_char_count(valid) <= max_chars) return valid;
    const std::string marker = "... [truncated]";
    return utf8_prefix_chars(valid, max_chars - marker.size()) + marker;
}

std::string trim_ascii_whitespace(std::string value) {
    while (!value.empty() &&
           (value.back() == '\r' || value.back() == '\n' ||
            value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == '\r' || value[begin] == '\n' ||
            value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }
    return value.substr(begin);
}

std::string bounded_tool_text(const std::string& value) {
    if (value.size() <= MAX_TOOL_OUTPUT_BYTES) return value;
    return truncate_utf8_prefix(value, MAX_TOOL_OUTPUT_BYTES, "");
}

bool is_wsl() {
#ifdef _WIN32
    return false;
#else
    if (const char* value = std::getenv("WSL_INTEROP"); value && *value) {
        return true;
    }
    if (const char* value = std::getenv("WSL_DISTRO_NAME"); value && *value) {
        return true;
    }
    std::ifstream release("/proc/sys/kernel/osrelease");
    std::string text;
    std::getline(release, text);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text.find("microsoft") != std::string::npos;
#endif
}

std::filesystem::path resolve_search_path(const std::string& search_path,
                                          const ToolContext& ctx) {
    namespace fs = std::filesystem;
    const fs::path cwd = path_from_utf8(
        ctx.cwd.empty() ? current_path_utf8() : ctx.cwd);
    fs::path resolved = search_path.empty() ? cwd : path_from_utf8(search_path);
    if (resolved.is_relative()) resolved = cwd / resolved;
    std::error_code ec;
    fs::path absolute = fs::absolute(resolved, ec);
    return (ec ? resolved : absolute).lexically_normal();
}

std::string normalized_git_path(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    while (value.rfind("./", 0) == 0) value.erase(0, 2);
    return value;
}

std::string include_pathspec(const std::string& include_pattern) {
    std::string pattern = normalized_git_path(include_pattern);
    while (!pattern.empty() && pattern.front() == '/') pattern.erase(pattern.begin());
    if (pattern.find('/') == std::string::npos) pattern = "**/" + pattern;
    return ":(glob)" + pattern;
}

IgnoreProbeResult probe_explicit_ignored_directory(
    const std::filesystem::path& search_root,
    const ToolContext& ctx) {
    namespace fs = std::filesystem;
    IgnoreProbeResult probe;
    const std::string git_root_text =
        worktree::find_git_root(path_to_utf8(search_root));
    if (git_root_text.empty()) return probe;

    const fs::path git_root = path_from_utf8(git_root_text);
    std::error_code ec;
    fs::path relative = fs::relative(search_root, git_root, ec);
    if (ec || relative.empty() || relative == fs::path(".")) return probe;
    const std::string relative_text = path_to_utf8_generic(relative);
    if (relative_text.rfind("../", 0) == 0 || relative_text == "..") return probe;

    HookCommandSpec command;
    command.command = "git";
    command.args = {
        "--no-pager", "check-ignore", "--no-index", "-q", "--",
        relative_text,
    };
    HookProcessOptions options;
    options.timeout_ms = CHECK_IGNORE_TIMEOUT_MS;
    options.abort_flag = ctx.abort_flag;
    options.max_stdout_bytes = 4096;
    options.max_stderr_bytes = 4096;
    options.terminate_process_tree = true;
    options.append_output_truncation_notice = false;
    HookProcessResult result = run_hook_process(
        command, "", path_to_utf8(git_root), options);
    probe.aborted = result.aborted || abort_requested(ctx);
    probe.ignored = result.started && !result.timed_out && result.exit_code == 0;
    return probe;
}

HookCommandSpec make_git_grep_command(
    const std::string& pattern,
    const std::string& include_pattern,
    const std::filesystem::path& search_root,
    bool path_is_file,
    bool explicit_directory_is_ignored) {
    HookCommandSpec command;
    command.command = "git";
    command.args = {"--no-pager", "grep", "--no-index"};
    if (path_is_file || explicit_directory_is_ignored) {
        command.args.push_back("--no-exclude-standard");
    } else {
        command.args.push_back("--exclude-standard");
    }
    const std::vector<std::string> fixed_args = {
        "--no-color", "--no-ext-grep", "--no-textconv",
        "-n", "-z", "-I", "-i", "-E", "-e", pattern, "--",
    };
    command.args.insert(command.args.end(), fixed_args.begin(), fixed_args.end());

    if (path_is_file) {
        command.args.push_back(":(literal)" + path_to_utf8_generic(search_root.filename()));
    } else if (!include_pattern.empty()) {
        command.args.push_back(include_pathspec(include_pattern));
    } else {
        command.args.push_back(".");
    }
    return command;
}

std::vector<ParsedMatch> parse_git_grep_output(const std::string& raw,
                                               bool raw_was_truncated,
                                               bool& incomplete_record) {
    std::vector<ParsedMatch> matches;
    std::size_t cursor = 0;
    while (cursor < raw.size()) {
        const std::size_t path_end = raw.find('\0', cursor);
        if (path_end == std::string::npos) {
            incomplete_record = raw_was_truncated;
            break;
        }
        const std::size_t line_end = raw.find('\0', path_end + 1);
        if (line_end == std::string::npos) {
            incomplete_record = raw_was_truncated;
            break;
        }
        std::size_t content_end = raw.find('\n', line_end + 1);
        const bool partial = content_end == std::string::npos;
        if (partial) {
            if (!raw_was_truncated) break;
            content_end = raw.size();
            incomplete_record = true;
        }

        std::string path = normalized_git_path(raw.substr(cursor, path_end - cursor));
        std::string line_number = raw.substr(path_end + 1, line_end - path_end - 1);
        const bool valid_line_number = !line_number.empty() &&
            std::all_of(line_number.begin(), line_number.end(),
                        [](unsigned char ch) { return ch >= '0' && ch <= '9'; });
        if (!valid_line_number) {
            incomplete_record = true;
            break;
        }
        std::string content = raw.substr(line_end + 1, content_end - line_end - 1);
        if (!content.empty() && content.back() == '\r') content.pop_back();
        ParsedMatch match;
        match.partial = partial;
        match.line = bounded_visible_line(
            path + ":" + line_number + ":" + content, partial);
        matches.push_back(std::move(match));
        if (partial) break;
        cursor = content_end + 1;
    }
    return matches;
}

std::string format_matches(std::vector<ParsedMatch> matches,
                           std::size_t head_limit,
                           bool subprocess_truncated,
                           bool incomplete_record) {
    const std::size_t parsed_count = matches.size();
    bool truncated = subprocess_truncated || incomplete_record ||
                     matches.size() > head_limit;
    if (matches.size() > head_limit) matches.resize(head_limit);

    const std::size_t known_count = parsed_count == 0 && subprocess_truncated
                                        ? 1
                                        : parsed_count;
    std::string output;
    if (truncated) {
        output = "Found at least " + std::to_string(known_count) +
                 (known_count == 1 ? " matching line:\n" : " matching lines:\n");
    } else {
        output = "Found " + std::to_string(known_count) +
                 (known_count == 1 ? " matching line:\n" : " matching lines:\n");
    }

    const std::string notice =
        "\n[Results truncated by the match, formatted-output, or subprocess-output "
        "budget. Narrow path, pattern, include_pattern, or head_limit.]";
    const std::size_t content_budget = MAX_TOOL_OUTPUT_BYTES - notice.size();
    for (const auto& match : matches) {
        const std::size_t required = match.line.size() + 1;
        if (output.size() + required > content_budget) {
            truncated = true;
            break;
        }
        output += match.line;
        output.push_back('\n');
    }
    if (truncated) output += notice;
    return bounded_tool_text(output);
}

bool looks_like_invalid_regex(const std::string& stderr_text) {
    std::string lower = stderr_text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lower.find("invalid regular expression") != std::string::npos ||
           lower.find("invalid regexp") != std::string::npos ||
           lower.find("unmatched") != std::string::npos ||
           lower.find("regex") != std::string::npos;
}

ToolResult execute_grep(const std::string& arguments_json, const ToolContext& ctx) {
    std::string pattern;
    std::string include_pattern;
    std::string search_path;
    std::size_t head_limit = DEFAULT_HEAD_LIMIT;

    try {
        const auto args = nlohmann::json::parse(arguments_json);
        pattern = args.value("pattern", "");
        include_pattern = args.value("include_pattern", "");
        search_path = args.value("path", "");
        if (args.contains("head_limit")) {
            if (!args["head_limit"].is_number_integer() &&
                !args["head_limit"].is_number_unsigned()) {
                return ToolResult{"[Error] head_limit must be a positive integer.", false};
            }
            const long long requested = args["head_limit"].get<long long>();
            if (requested <= 0) {
                return ToolResult{"[Error] head_limit must be a positive integer.", false};
            }
            head_limit = std::min<std::size_t>(
                static_cast<std::size_t>(requested), MAX_HEAD_LIMIT);
        }
    } catch (...) {
        return ToolResult{"[Error] Failed to parse tool arguments.", false};
    }

    if (pattern.empty()) return ToolResult{"[Error] No pattern provided.", false};
    if (contains_nul(pattern) || contains_nul(include_pattern) ||
        contains_nul(search_path)) {
        return ToolResult{"[Error] Tool arguments cannot contain NUL bytes.", false};
    }
    if (abort_requested(ctx)) return ToolResult{ABORTED_MSG, false};

    const int wall_timeout_ms = is_wsl() ? WSL_GREP_TIMEOUT_MS : GREP_TIMEOUT_MS;
    const auto search_started = std::chrono::steady_clock::now();

    const std::filesystem::path search_root = resolve_search_path(search_path, ctx);
    std::error_code path_ec;
    const bool path_is_directory =
        std::filesystem::is_directory(search_root, path_ec);
    path_ec.clear();
    const bool path_is_file =
        std::filesystem::is_regular_file(search_root, path_ec);
    if (!path_is_directory && !path_is_file) {
        return ToolResult{
            "[Error] Path is not a file or directory: " +
                bounded_excerpt(path_to_utf8(search_root)),
            false};
    }

    bool explicit_directory_is_ignored = false;
    if (path_is_directory) {
        const IgnoreProbeResult probe =
            probe_explicit_ignored_directory(search_root, ctx);
        if (probe.aborted) return ToolResult{ABORTED_MSG, false};
        explicit_directory_is_ignored = probe.ignored;
    }

    const HookCommandSpec command = make_git_grep_command(
        pattern, include_pattern, search_root, path_is_file,
        explicit_directory_is_ignored);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - search_started).count();
    if (elapsed_ms >= wall_timeout_ms) {
        return ToolResult{
            "[Error] git grep exceeded the " +
                std::to_string(wall_timeout_ms / 1000) +
                " second search timeout. Narrow the path or pattern.",
            false};
    }
    HookProcessOptions options;
    options.timeout_ms = wall_timeout_ms - static_cast<int>(elapsed_ms);
    options.abort_flag = ctx.abort_flag;
    options.max_stdout_bytes = MAX_GIT_STDOUT_BYTES;
    options.max_stderr_bytes = MAX_GIT_STDERR_BYTES;
    options.max_stdout_lines = head_limit + 1;
    options.terminate_on_stdout_limit = true;
    options.terminate_process_tree = true;
    options.append_output_truncation_notice = false;
    const std::string process_cwd = path_to_utf8(
        path_is_file ? search_root.parent_path() : search_root);
    HookProcessResult result = run_hook_process(command, "", process_cwd, options);

    if (result.aborted || abort_requested(ctx)) {
        return ToolResult{ABORTED_MSG, false};
    }
    if (result.timed_out) {
        const int timeout_seconds = wall_timeout_ms / 1000;
        return ToolResult{
            "[Error] git grep exceeded the " + std::to_string(timeout_seconds) +
                " second search timeout. Narrow the path or pattern.",
            false};
    }
    if (!result.started || result.exit_code == 127) {
        const std::string detail = result.error.empty()
            ? "git was not found on PATH"
            : bounded_excerpt(result.error);
        return ToolResult{
            "[Error] Git executable was not found or could not be started: " + detail,
            false};
    }

    const bool subprocess_truncated = result.output_limit_reached ||
                                      result.stdout_truncated;
    if (!subprocess_truncated && result.exit_code == 1 &&
        result.stdout_text.empty()) {
        return ToolResult{
            "No matches found for pattern: " + bounded_excerpt(pattern, 500), true};
    }
    if (!subprocess_truncated && result.exit_code != 0) {
        std::string detail = trim_ascii_whitespace(result.stderr_text);
        if (detail.empty()) detail = "git exited with code " +
                                     std::to_string(result.exit_code);
        detail = bounded_excerpt(detail);
        if (looks_like_invalid_regex(result.stderr_text)) {
            return ToolResult{"[Error] Invalid Git extended regex: " + detail, false};
        }
        return ToolResult{"[Error] git grep failed: " + detail, false};
    }

    bool incomplete_record = false;
    std::vector<ParsedMatch> matches = parse_git_grep_output(
        result.stdout_text, subprocess_truncated, incomplete_record);
    if (matches.empty() && !subprocess_truncated) {
        return ToolResult{
            "No matches found for pattern: " + bounded_excerpt(pattern, 500), true};
    }
    return ToolResult{
        format_matches(std::move(matches), head_limit,
                       subprocess_truncated, incomplete_record),
        true};
}

} // namespace

ToolImpl create_grep_tool() {
    ToolDef def;
    def.name = "grep";
    def.description =
        "Search file contents with Git extended regular expressions (ERE), "
        "case-insensitively. Requires git on PATH. Directory searches recurse "
        "and honor standard Git ignore rules; an explicitly selected ignored "
        "directory or file remains searchable. Large files are searchable, "
        "while match count, line length, subprocess output, wall time, and final "
        "tool output are bounded.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"pattern", {
                {"type", "string"},
                {"description", "Git extended regular expression (ERE), matched case-insensitively"}
            }},
            {"include_pattern", {
                {"type", "string"},
                {"description", "Git glob for files below a directory path (for example *.cpp or src/**/*.hpp). Optional."}
            }},
            {"path", {
                {"type", "string"},
                {"description", "File or directory to search (default: session CWD). Relative paths resolve from the session CWD. Optional."}
            }},
            {"head_limit", {
                {"type", "integer"},
                {"minimum", 1},
                {"maximum", static_cast<int>(MAX_HEAD_LIMIT)},
                {"description", "Maximum matching lines to return (default 200; hard cap 2000). Optional."}
            }}
        }},
        {"required", nlohmann::json::array({"pattern"})}
    });
    return ToolImpl{def, execute_grep, /*is_read_only=*/true};
}

} // namespace acecode
