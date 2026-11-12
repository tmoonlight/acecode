#include "bash_tool.hpp"
#include "background_tasks.hpp"
#include "shell_output_files.hpp"
#include "shell_process.hpp"
#include "llm/tool_icons.hpp"
#include "llm/tool_protocol_names.hpp"
#include "environment/terminal_runtime.hpp"
#include "utils/logger.hpp"
#include "utils/encoding.hpp"
#include "utils/stream_processing.hpp"
#include "tool/tool_errors.hpp"
#include "utils/utf8_path.hpp"
#include "sandbox/exec_permission.hpp"
#include "sandbox/sandbox_backend.hpp"
#include "sandbox/sandbox_denial.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <deque>
#include <chrono>
#include <filesystem>
#include <atomic>
#include <set>
#include <system_error>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <optional>
#include <thread>

namespace acecode {

static constexpr int DEFAULT_TIMEOUT_MS = 120000;    // 2 minutes
static constexpr size_t MAX_OUTPUT_SIZE = 100 * 1024; // 100KB
// 前置 sleep 达到这个秒数就拒绝执行(见 bash_leading_sleep_seconds)。
static constexpr double LONG_LEADING_SLEEP_SECONDS = 25.0;

static std::string ascii_lower_copy(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

static bool is_root_script_extension(const std::filesystem::path& path) {
    std::string ext = ascii_lower_copy(path_to_utf8(path.extension()));
    return ext == ".py" || ext == ".ps1" || ext == ".js" ||
           ext == ".bat" || ext == ".cmd";
}

static std::set<std::string> list_root_script_files(const std::filesystem::path& root) {
    std::set<std::string> out;
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec) || ec) return out;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        const auto path = entry.path();
        if (is_root_script_extension(path)) {
            out.insert(path_to_utf8(path.filename()));
        }
    }
    return out;
}

static std::vector<std::string> new_root_script_files(
    const std::set<std::string>& before,
    const std::set<std::string>& after) {
    std::vector<std::string> created;
    for (const auto& name : after) {
        if (before.find(name) == before.end()) created.push_back(name);
    }
    return created;
}

static void append_root_script_warning(ToolResult& result,
                                       const std::vector<std::string>& created,
                                       const std::string& scratch_dir) {
    if (created.empty()) return;
    if (!result.output.empty() && result.output.back() != '\n') result.output += "\n";
    result.output += "[Warning] Shell command created script file(s) in the workspace root: ";
    for (size_t i = 0; i < created.size(); ++i) {
        if (i > 0) result.output += ", ";
        result.output += created[i];
    }
    result.output += ". Temporary helper scripts should be written under ACECODE_TMPDIR";
    if (!scratch_dir.empty()) result.output += " (" + scratch_dir + ")";
    result.output += ".";
}

// 把命令生成的文件记进结果元数据,摘要面板的「产物」分组读它
// (shell_output_files.hpp)。不进模型看到的正文。
static void attach_output_files(ToolResult& result, const ShellOutputProbe& probe,
                                const std::string& command, const std::string& output) {
    auto files = collect_shell_output_files(probe, command, output);
    if (!files.empty()) result.metadata["output_files"] = std::move(files);
}

namespace {

std::string trim_copy(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

std::vector<std::string> split_words(const std::string& text) {
    std::vector<std::string> words;
    std::string current;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) words.push_back(current), current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) words.push_back(current);
    return words;
}

std::optional<double> parse_number(const std::string& token, double scale = 1.0) {
    std::string digits = token;
    if (!digits.empty() && (digits.back() == 's' || digits.back() == 'S')) digits.pop_back();
    if (digits.empty()) return std::nullopt;
    char* end = nullptr;
    const double value = std::strtod(digits.c_str(), &end);
    if (!end || *end != '\0' || value < 0) return std::nullopt;
    return value * scale;
}

} // namespace

// 命令开头那一段如果只是在睡觉(sleep N / Start-Sleep N / timeout /t N /
// ping -n N 127.0.0.1),返回睡眠秒数。用来拦住「睡一会儿再看」式的轮询。
std::optional<double> bash_leading_sleep_seconds(const std::string& command) {
    std::string first = command;
    for (const char* separator : {"&&", "||", ";", "|", "\n"}) {
        const auto pos = first.find(separator);
        if (pos != std::string::npos) first = first.substr(0, pos);
    }
    const auto words = split_words(trim_copy(first));
    if (words.empty()) return std::nullopt;
    const std::string head = ascii_lower_copy(words[0]);
    if (head == "sleep" && words.size() == 2) return parse_number(words[1]);
    if (head == "start-sleep") {
        double scale = 1.0;
        for (std::size_t i = 1; i < words.size(); ++i) {
            const std::string word = ascii_lower_copy(words[i]);
            if (word == "-milliseconds" || word == "-m") { scale = 0.001; continue; }
            if (word == "-seconds" || word == "-s") { scale = 1.0; continue; }
            if (auto value = parse_number(words[i], scale)) return value;
        }
        return std::nullopt;
    }
    if (head == "timeout") {
        for (std::size_t i = 1; i + 1 < words.size(); ++i) {
            if (ascii_lower_copy(words[i]) == "/t") return parse_number(words[i + 1]);
        }
        return std::nullopt;
    }
    if (head == "ping") {
        bool loopback = false;
        std::optional<double> count;
        for (std::size_t i = 1; i < words.size(); ++i) {
            const std::string word = ascii_lower_copy(words[i]);
            if (word == "127.0.0.1" || word == "localhost" || word == "::1") loopback = true;
            if ((word == "-n" || word == "-c") && i + 1 < words.size()) count = parse_number(words[i + 1]);
        }
        if (loopback && count && *count > 1) return *count - 1;
    }
    return std::nullopt;
}

static ToolResult execute_bash(const std::string& arguments_json, const ToolContext& ctx) {
    std::string command;
    int timeout_ms = DEFAULT_TIMEOUT_MS;
    bool timeout_given = false;
    std::string cwd;
    std::vector<std::string> stdin_inputs;
    bool run_in_background = false;
    bool monitor = false;

    try {
        auto args = nlohmann::json::parse(arguments_json);
        const auto error = sandbox::validate_escalation_arguments(args);
        if (!error.empty()) return ToolResult{"[Error] " + error, false};
        command = args.value("command", "");
        if (args.contains("timeout_ms") && args["timeout_ms"].is_number()) {
            timeout_ms = args["timeout_ms"].get<int>();
            timeout_given = true;
        }
        cwd = args.value("cwd", "");
        run_in_background = args.value("run_in_background", false);
        monitor = args.value("monitor", false);
        if (args.contains("stdin_inputs") && args["stdin_inputs"].is_array()) {
            for (const auto& el : args["stdin_inputs"]) {
                if (el.is_string()) stdin_inputs.push_back(el.get<std::string>());
            }
        }
    } catch (...) {
        return ToolResult{"[Error] Failed to parse tool arguments.", false};
    }

    if (command.empty()) {
        return ToolResult{"[Error] No command provided.", false};
    }
    const bool background = run_in_background || monitor;
    if (!background && timeout_ms <= 0) timeout_ms = DEFAULT_TIMEOUT_MS;
    if (background && !ctx.background_tasks) {
        return ToolResult{"[Error] run_in_background is not available here. Run the command in the "
                          "foreground with a timeout_ms long enough for it.", false};
    }
    if (!background && ctx.background_tasks) {
        if (const auto seconds = bash_leading_sleep_seconds(command);
            seconds && *seconds >= LONG_LEADING_SLEEP_SECONDS) {
            return ToolResult{
                "[Blocked] The command starts with a long sleep (" + background_duration_text(
                    static_cast<std::int64_t>(*seconds * 1000)) +
                "). Do not wait by sleeping. To wait for a command you started, run it with "
                "run_in_background=true and you will be notified when it finishes. To wait for a "
                "condition, run a loop such as `until <check>; do sleep 2; done` with "
                "run_in_background=true; you are notified when the loop exits. Do not chain shorter "
                "sleeps to get around this.", false};
        }
    }

    if (cwd.empty() && !ctx.cwd.empty()) {
        cwd = ctx.cwd;
    }

    const bool sandboxed = ctx.exec_sandbox && ctx.exec_sandbox->policy.mode != sandbox::SandboxMode::FullAccess;
    auto sandbox_failure = [&](const std::string& message) {
        ToolResult result{"[Sandbox unavailable] " + message + ". The command was not executed.", false};
        result.metadata["sandbox_unavailable"] = true;
        // 单行原因给 AgentLoop 记进 /sandbox 状态与 system prompt;正文里的
        // 引导句只给模型看。
        result.metadata["sandbox_unavailable_reason"] = message;
        result.output += "\nRequest with_escalated_permissions=true with a non-empty justification if full access is needed.";
        return result;
    };
    if (sandboxed && ctx.exec_sandbox->backend == sandbox::BackendKind::None) {
        return sandbox_failure("No sandbox backend selected");
    }
    std::vector<std::pair<std::string, std::string>> child_environment;
    if (!ctx.scratch_dir.empty()) child_environment.emplace_back("ACECODE_TMPDIR", ctx.scratch_dir);
    if (sandboxed) {
        const auto additions = sandbox::sandbox_environment(ctx.exec_sandbox->backend,
            ctx.exec_sandbox->policy, ctx.exec_sandbox->network_enforced,
            ctx.exec_sandbox->denybin_dir);
        child_environment.insert(child_environment.end(), additions.begin(), additions.end());
    }

    auto t_start = std::chrono::steady_clock::now();
    auto make_summary = [&](const std::string& cmd, long long duration_ms,
                            size_t total_bytes_out, int exit_code,
                            bool is_success, bool was_truncated,
                            bool was_aborted, bool was_timed_out) {
        ToolSummary s;
        s.verb = "Ran";
        s.object = truncate_utf8_prefix(cmd, 60);
        s.metrics.emplace_back("time", format_duration_compact(duration_ms));
        s.metrics.emplace_back("bytes", format_bytes_compact(total_bytes_out));
        if (sandboxed) s.metrics.emplace_back("sandbox", sandbox::sandbox_mode_name(ctx.exec_sandbox->policy.mode));
        if (!is_success && exit_code != 0) {
            s.metrics.emplace_back("exit", std::to_string(exit_code));
        }
        if (was_truncated) s.metrics.emplace_back("truncated", "true");
        if (was_aborted) s.metrics.emplace_back("aborted", "true");
        if (was_timed_out) s.metrics.emplace_back("timeout", "true");
        s.icon = tool_icon("bash");
        return s;
    };

    const std::string scratch_dir = ctx.scratch_dir;
    if (scratch_dir.empty() &&
        ToolContext::references_scratch_path_alias(command)) {
        ToolResult r{
            ToolErrors::invalid_parameter(
                "command",
                "references ACECODE_TMPDIR, but no session scratch directory is available. "
                "Retry in an active session or use an explicit path."),
            false};
        r.summary = make_summary(command, 0, 0, -1, false,
                                 false, false, false);
        return r;
    }

    LOG_INFO("bash: cmd=" + log_truncate(command, 200) + " cwd=" + cwd +
             " timeout=" + std::to_string(timeout_ms) +
             " background=" + (background ? std::string("true") : std::string("false")) +
             " stdin_inputs=" + std::to_string(stdin_inputs.size()));

#ifdef _WIN32
    std::filesystem::path cwd_path = cwd.empty()
        ? std::filesystem::path{}
        : std::filesystem::path(utf8_to_wide(cwd));
#else
    std::filesystem::path cwd_path = cwd;
#endif

    if (!cwd.empty() && !std::filesystem::is_directory(cwd_path)) {
        ToolResult r{"[Error] Working directory does not exist: " + cwd, false};
        r.summary = make_summary(command, 0, 0, -1, false, false, false, false);
        return r;
    }

    std::error_code cwd_ec;
    const std::filesystem::path root_for_script_warning = cwd_path.empty()
        ? std::filesystem::current_path(cwd_ec)
        : cwd_path;
    const auto root_scripts_before = list_root_script_files(root_for_script_warning);

    if (!scratch_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path_from_utf8(scratch_dir), ec);
        if (ec) {
            ToolResult r{"[Error] Failed to create ACECODE_TMPDIR: " + scratch_dir +
                         " (" + ec.message() + ")", false};
            r.summary = make_summary(command, 0, 0, -1, false, false, false, false);
            return r;
        }
    }

    const auto terminal_snapshot = acecode::environment::terminal().last();
    if (terminal_snapshot && !terminal_snapshot->resolved.usable) {
        return ToolResult{"[Error] No usable terminal. Check Settings > Configuration: " +
                          terminal_snapshot->resolved.fallback_reason, false};
    }

    ShellLaunchSpec spec;
    spec.command = command;
    spec.cwd = cwd;
    spec.environment = child_environment;
    spec.stdin_inputs = stdin_inputs;
    if (ctx.exec_sandbox) spec.sandbox = *ctx.exec_sandbox;
    const std::string background_dir = scratch_dir.empty()
        ? std::string{}
        : path_to_utf8(path_from_utf8(scratch_dir) / "background");
    const std::string stop_tool = model_tool_name_for_native("stop_background_task");

    auto backgrounded_result = [&](const std::string& task_id, const std::string& output_so_far,
                                   std::int64_t foreground_timeout_ms) {
        const auto info = ctx.background_tasks->find(task_id);
        ToolResult r;
        r.success = true;
        r.output = output_so_far;
        if (info) {
            if (!r.output.empty() && r.output.back() != '\n') r.output += "\n";
            if (!r.output.empty()) r.output += "\n";
            r.output += format_backgrounded_tool_result(*info, foreground_timeout_ms, stop_tool);
            r.metadata["background_task_id"] = info->id;
            r.metadata["background_origin"] = background_origin_name(info->origin);
            if (!info->output_file.empty()) r.metadata["background_output_file"] = info->output_file;
        }
        ToolSummary s = make_summary(command,
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_start).count(),
            output_so_far.size(), 0, true, false, false, false);
        s.metrics.emplace_back("background", "true");
        r.summary = s;
        return r;
    };

    if (background) {
        BackgroundTaskRegistry::ShellStart start;
        start.spec = spec;
        start.title = command;
        start.output_dir = background_dir;
        start.origin = BackgroundOrigin::Requested;
        start.max_runtime_ms = timeout_given ? (std::max)(0, timeout_ms) : 0;
        start.monitor = monitor;
        ShellLaunchFailure failure;
        const std::string task_id = ctx.background_tasks->start_shell(std::move(start), failure);
        if (task_id.empty()) {
            if (failure.sandbox_unavailable) return sandbox_failure(failure.message);
            ToolResult r{"[Error] " + (failure.message.empty() ? std::string("Failed to start the command.") : failure.message), false};
            r.summary = make_summary(command, 0, 0, -1, false, false, false, false);
            return r;
        }
        return backgrounded_result(task_id, {}, 0);
    }

    // Shared streaming state.
    std::string full_output;
    std::string current_line;
    std::deque<std::string> tail_lines;
    int total_lines = 0;

    // Incremental decoder: turns raw subprocess bytes into valid UTF-8, holding
    // back any trailing partial character across chunk boundaries. On Windows it
    // falls back to the console codepage (e.g. GBK/CP936) so legacy cmd.exe
    // output decodes correctly instead of corrupting JSON serialization with a
    // stray byte like 0xF7. Output here is always safe to stream and to embed in
    // the tool result JSON.
    IncrementalTextDecoder decoder;

    // Emit already-decoded UTF-8 text into the output/stream pipeline.
    auto emit_text = [&](const std::string& text) {
        std::string clean = clean_shell_output(text);
        if (clean.empty()) return;
        full_output += clean;
        feed_line_state(clean, current_line, tail_lines, total_lines);
        if (ctx.stream) ctx.stream(clean);
    };

    const ShellOutputProbe output_probe =
        begin_shell_output_probe(command, path_to_utf8(root_for_script_warning));

    ShellLaunchFailure launch_failure;
    auto process = ShellProcess::launch(spec, launch_failure);
    if (!process) {
        if (launch_failure.sandbox_unavailable) return sandbox_failure(launch_failure.message);
        return ToolResult{"[Error] " + launch_failure.message, false};
    }

    const std::uint64_t background_generation_at_start =
        ctx.background_generation ? ctx.background_generation->load() : 0;
    bool aborted = false;
    bool timed_out = false;
    std::optional<BackgroundOrigin> move_to_background;
    auto start = std::chrono::steady_clock::now();
    std::string raw;

    while (true) {
        raw.clear();
        if (process->read_available(raw) > 0) emit_text(decoder.push(raw.data(), raw.size()));

        if (process->poll_exited()) {
            // 直接子进程已退出:只读管道里当前还有的字节,不等 EOF(孙进程可能仍持有写端)。
            raw.clear();
            process->drain(raw);
            if (!raw.empty()) emit_text(decoder.push(raw.data(), raw.size()));
            break;
        }

        if (ctx.abort_flag && ctx.abort_flag->load()) {
            process->kill_tree(1000, true);
            raw.clear();
            process->drain(raw);
            if (!raw.empty()) emit_text(decoder.push(raw.data(), raw.size()));
            aborted = true;
            break;
        }

        // 用户插话 / 手动转后台:命令不中断,整体交给后台任务登记表。
        if (ctx.background_tasks && ctx.background_generation &&
            ctx.background_generation->load() != background_generation_at_start) {
            const int reason = ctx.background_reason ? ctx.background_reason->load() : 0;
            move_to_background = reason == static_cast<int>(BackgroundOrigin::UserMessage)
                ? BackgroundOrigin::UserMessage
                : BackgroundOrigin::UserRequest;
            break;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) {
            if (ctx.background_tasks) {
                // 超时不杀:转到后台继续跑,结束时通知模型。
                move_to_background = BackgroundOrigin::Timeout;
                break;
            }
            process->kill_tree(1000, false);
            raw.clear();
            process->drain(raw);
            if (!raw.empty()) emit_text(decoder.push(raw.data(), raw.size()));
            timed_out = true;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (move_to_background) {
        const auto started_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() -
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        BackgroundTaskRegistry::ShellAdoption adoption;
        adoption.process = std::move(process);
        adoption.decoder = std::move(decoder);
        adoption.captured_text = full_output;
        adoption.command = command;
        adoption.cwd = cwd;
        adoption.output_dir = background_dir;
        adoption.title = command;
        adoption.origin = *move_to_background;
        adoption.started_at_ms = started_at_ms;
        const std::string task_id = ctx.background_tasks->adopt_shell(std::move(adoption));
        LOG_INFO("bash: moved to background id=" + task_id + " reason=" +
                 background_origin_name(*move_to_background));
        if (!task_id.empty()) {
            std::string output_so_far = ensure_utf8(full_output);
            if (output_so_far.size() > MAX_OUTPUT_SIZE) {
                output_so_far = "[... earlier output omitted; the full output is in the output file ...]\n" +
                                background_output_tail(output_so_far, MAX_OUTPUT_SIZE / 2);
            }
            return backgrounded_result(task_id, output_so_far,
                *move_to_background == BackgroundOrigin::Timeout ? timeout_ms : 0);
        }
        return ToolResult{"[Error] The command could not be moved to the background and was stopped.", false};
    }

#ifndef _WIN32
    const std::string launch_error = process->launch_error();
    if (!launch_error.empty()) {
        return sandboxed ? sandbox_failure(launch_error) : ToolResult{"[Error] " + launch_error, false};
    }
#endif
    const int exit_code = process->exit_code();
    process.reset();

    // EOF reached: drain any bytes the decoder held back (incomplete trailing
    // character or undecodable tail) so they are not silently dropped.
    emit_text(decoder.flush());

    // Flush any trailing partial line into tail_lines so it's visible even
    // without a trailing newline (full_output already holds it).
    if (!current_line.empty()) {
        tail_lines.push_back(current_line);
        while (tail_lines.size() > 5) tail_lines.pop_front();
        current_line.clear();
    }

    const size_t raw_bytes = full_output.size();

    // AgentLoop 会把大结果交给 tool_result_storage 落盘并生成 preview,所以
    // 模型驱动路径必须保留完整输出;直接单测/嵌入式调用默认仍走旧的 100KB 保护。
    const bool should_truncate_inline = !ctx.preserve_full_output;

    // Head+tail truncation: keep first 40% and last 60% of MAX_OUTPUT_SIZE,
    // joined by a one-line marker reporting the omitted byte count. This
    // preserves early context (e.g. build args, cwd, path) which a pure
    // tail-only policy loses.
    bool was_truncated = false;
    if (should_truncate_inline && full_output.size() > MAX_OUTPUT_SIZE) {
        const size_t head_cap = static_cast<size_t>(MAX_OUTPUT_SIZE * 0.4);
        const size_t tail_cap = MAX_OUTPUT_SIZE - head_cap; // ~60%
        const size_t omitted = full_output.size() - head_cap - tail_cap;
        std::string head = full_output.substr(0, head_cap);
        std::string tail = full_output.substr(full_output.size() - tail_cap);
        // Ensure the marker sits on its own line.
        if (!head.empty() && head.back() != '\n') head += "\n";
        full_output = head + "[... " + std::to_string(omitted) + " bytes omitted ...]\n" + tail;
        was_truncated = true;
    }

    full_output = ensure_utf8(full_output);

    auto t_end = std::chrono::steady_clock::now();
    long long duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        t_end - t_start).count();

    if (aborted) {
        if (full_output.empty() || full_output.back() != '\n') full_output += "\n";
        full_output += "[Aborted]";
        ToolResult r{full_output, false};
        r.summary = make_summary(command, duration_ms, raw_bytes, -1, false,
                                 was_truncated, true, false);
        attach_output_files(r, output_probe, command, full_output);
        append_root_script_warning(r,
            new_root_script_files(root_scripts_before,
                                  list_root_script_files(root_for_script_warning)),
            scratch_dir);
        return r;
    }
    if (timed_out) {
        ToolResult r{full_output + "\n[Error] Command timed out after " +
            std::to_string(timeout_ms / 1000) + " seconds.", false};
        r.summary = make_summary(command, duration_ms, raw_bytes, -1, false,
                                 was_truncated, false, true);
        attach_output_files(r, output_probe, command, full_output);
        append_root_script_warning(r,
            new_root_script_files(root_scripts_before,
                                  list_root_script_files(root_for_script_warning)),
            scratch_dir);
        return r;
    }

    if (full_output.empty()) {
        full_output = "(no output)";
    }

    bool is_ok = (exit_code == 0);
    ToolResult r{full_output, is_ok};
    r.metadata["exit_code"] = static_cast<int>(exit_code);
    if (sandboxed) {
        r.metadata["sandbox"] = sandbox::sandbox_mode_name(ctx.exec_sandbox->policy.mode);
        if (const auto violation = sandbox::classify_sandbox_violation(static_cast<int>(exit_code), full_output)) {
            r.output += sandbox::escalation_hint(ctx.exec_sandbox->policy, ctx.exec_sandbox->network_enforced,
                                                 &*violation, ctx.exec_sandbox->network_best_effort);
            r.metadata["sandbox_denied"] = true;
            r.metadata["sandbox_violation"] = sandbox::violation_to_json(*violation);
            LOG_WARN("[sandbox] violation reason=" + violation->reason +
                     (violation->path.empty() ? std::string{} : " path=" + violation->path));
        }
    }
    r.summary = make_summary(command, duration_ms, raw_bytes,
                             static_cast<int>(exit_code), is_ok,
                             was_truncated, false, false);
    attach_output_files(r, output_probe, command, full_output);
    append_root_script_warning(r,
        new_root_script_files(root_scripts_before,
                              list_root_script_files(root_for_script_warning)),
        scratch_dir);
    return r;
}

ToolImpl create_bash_tool() {
    ToolDef def;
    def.name = "bash";
    def.description = "Execute a shell command and return its output. "
                      "Use this to run commands, check files, install packages, etc. "
                      "For programs that prompt for input (e.g. 'apt install' confirming, "
                      "'npm login' asking for credentials), pass stdin_inputs with the "
                      "answers to pipe into the command's stdin in order. "
                      "Long-running work: set run_in_background=true for dev servers, watchers, and builds "
                      "or tests that may take more than about a minute. The call returns at once with a task "
                      "ID and an output file, and you are notified automatically when the command finishes, "
                      "with its exit code and last output; do not poll for it. A foreground command still "
                      "running at its timeout is moved to the background instead of being killed. Do not wait "
                      "by sleeping: commands that start with a long sleep are rejected. To wait for a condition, "
                      "run a loop such as `until <check>; do sleep 2; done` with run_in_background=true; you "
                      "are notified when it exits. Set monitor=true to also receive new output lines while a "
                      "background command runs (for example to watch a log). Stop a background command with "
                      "stop_background_task. "
                      "Commands may run in a filesystem/network sandbox. If a command is denied by the sandbox, "
                      "prefer the smallest request: sandbox_permissions=\"with_additional_permissions\" plus "
                      "additional_permissions listing only the extra paths (or network) it needs; use "
                      "sandbox_permissions=\"require_escalated\" only when unrestricted access is genuinely "
                      "required. Both need a non-empty justification and are subject to user approval.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"sandbox_permissions", {
                {"type", "string"},
                {"enum", nlohmann::json::array({"use_default", "with_additional_permissions", "require_escalated"})},
                {"description", "use_default (default): run under the current sandbox policy. "
                                "with_additional_permissions: stay sandboxed but also grant additional_permissions "
                                "(user approval required). require_escalated: ask the user to run outside the sandbox."}
            }},
            {"additional_permissions", {
                {"type", "object"},
                {"description", "Extra permissions for sandbox_permissions=with_additional_permissions: "
                                "{\"file_system\": {\"read\": [absolute paths], \"write\": [absolute paths]}, "
                                "\"network\": {\"enabled\": true}}. Paths may start with ~. Denied secret stores "
                                "(e.g. ~/.ssh) cannot be requested."},
                {"properties", {
                    {"file_system", {
                        {"type", "object"},
                        {"properties", {
                            {"read", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                            {"write", {{"type", "array"}, {"items", {{"type", "string"}}}}}
                        }}
                    }},
                    {"network", {
                        {"type", "object"},
                        {"properties", {{"enabled", {{"type", "boolean"}}}}}
                    }}
                }}
            }},
            {"with_escalated_permissions", {
                {"type", "boolean"}, {"description", "Legacy alias for sandbox_permissions=require_escalated (default: false)"}
            }},
            {"justification", {
                {"type", "string"}, {"description", "One sentence explaining why extra or escalated permissions are necessary"}
            }},
            {"prefix_rule", {
                {"type", "array"}, {"items", {{"type", "string"}}},
                {"description", "Optional command prefix tokens (e.g. [\"pnpm\", \"install\"]) the user may choose "
                                "to always allow; it must cover every command segment and cannot be a shell, "
                                "interpreter, rm or sudo."}
            }},
            {"command", {
                {"type", "string"},
                {"description", "The shell command to execute"}
            }},
            {"timeout_ms", {
                {"type", "integer"},
                {"description", "Foreground: milliseconds to wait before the command is moved to the background "
                                "(default: 120000); it is not killed. With run_in_background: the maximum run time, "
                                "after which it is stopped (default: no limit)."}
            }},
            {"run_in_background", {
                {"type", "boolean"},
                {"description", "Run the command in the background and return immediately with a task ID and an "
                                "output file. You are notified automatically when it finishes; do not poll or "
                                "sleep. No '&' needed."}
            }},
            {"monitor", {
                {"type", "boolean"},
                {"description", "Implies run_in_background. New output lines are also delivered to you while the "
                                "command runs (batched), for watching logs or progress."}
            }},
            {"cwd", {
                {"type", "string"},
                {"description", "Working directory for the command (default: agent CWD)"}
            }},
            {"stdin_inputs", {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "Optional: lines to feed to the command's stdin in order, "
                                "each followed by a newline. Use for programs that prompt "
                                "interactively, e.g. [\"y\"] for an 'apt install' confirmation. "
                                "On Windows this parameter is currently accepted but ignored."}
            }}
        }},
        {"required", nlohmann::json::array({"command"})}
    });

    return ToolImpl{def, execute_bash, /*is_read_only=*/false};
}

} // namespace acecode
