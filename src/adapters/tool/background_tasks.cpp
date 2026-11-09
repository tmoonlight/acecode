#include "background_tasks.hpp"

#include "llm/tool_protocol_names.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>

namespace acecode {

namespace {

constexpr std::size_t kTailKeepBytes = 16 * 1024;
constexpr std::size_t kNotificationTailBytes = 3000;
constexpr std::size_t kMonitorBatchMaxBytes = 4000;
constexpr std::size_t kMaxRetainedFinished = 50;
constexpr std::size_t kMaxReadPerStep = 256 * 1024;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string new_task_id() {
    static std::mutex mu;
    static std::mt19937_64 rng{std::random_device{}()};
    std::lock_guard<std::mutex> lock(mu);
    std::ostringstream out;
    out << "bg_" << std::hex << std::setw(8) << std::setfill('0') << (rng() & 0xffffffffull);
    return out.str();
}

std::string stop_tool_name() {
    return model_tool_name_for_native("stop_background_task");
}

std::string xml_attr(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '"': out += "&quot;"; break;
            case '<': out += "&lt;"; break;
            case '&': out += "&amp;"; break;
            case '\n': out += ' '; break;
            default: out += c;
        }
    }
    return out;
}

// 只保留最后 max_bytes 字节,并且不从 UTF-8 多字节字符中间截断。
std::string utf8_suffix(const std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    std::size_t start = text.size() - max_bytes;
    while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) ++start;
    return text.substr(start);
}

void append_tail(std::string& tail, const std::string& text) {
    tail += text;
    if (tail.size() > kTailKeepBytes * 2) tail = utf8_suffix(tail, kTailKeepBytes);
}

} // namespace

const char* background_task_kind_name(BackgroundTaskKind kind) {
    return kind == BackgroundTaskKind::Agent ? "agent" : "shell";
}

const char* background_task_state_name(BackgroundTaskState state) {
    switch (state) {
        case BackgroundTaskState::Running: return "running";
        case BackgroundTaskState::Completed: return "completed";
        case BackgroundTaskState::Failed: return "failed";
        case BackgroundTaskState::Stopped: return "stopped";
    }
    return "running";
}

const char* background_origin_name(BackgroundOrigin origin) {
    switch (origin) {
        case BackgroundOrigin::Requested: return "requested";
        case BackgroundOrigin::Timeout: return "timeout";
        case BackgroundOrigin::UserMessage: return "user_message";
        case BackgroundOrigin::UserRequest: return "user_request";
    }
    return "requested";
}

nlohmann::json background_task_to_json(const BackgroundTaskInfo& info) {
    nlohmann::json out = {
        {"id", info.id},
        {"kind", background_task_kind_name(info.kind)},
        {"state", background_task_state_name(info.state)},
        {"origin", background_origin_name(info.origin)},
        {"title", info.title},
        {"started_at_ms", info.started_at_ms},
    };
    if (!info.command.empty()) out["command"] = info.command;
    if (!info.cwd.empty()) out["cwd"] = info.cwd;
    if (!info.output_file.empty()) out["output_file"] = info.output_file;
    if (!info.child_session_id.empty()) out["child_session_id"] = info.child_session_id;
    if (info.ended_at_ms) out["ended_at_ms"] = info.ended_at_ms;
    if (info.last_output_at_ms) out["last_output_at_ms"] = info.last_output_at_ms;
    if (info.state != BackgroundTaskState::Running && info.kind == BackgroundTaskKind::Shell) {
        out["exit_code"] = info.exit_code;
    }
    if (info.output_bytes) out["output_bytes"] = info.output_bytes;
    if (info.monitor) out["monitor"] = true;
    if (!info.stop_reason.empty()) out["stop_reason"] = info.stop_reason;
    if (!info.error.empty()) out["error"] = info.error;
    return out;
}

std::string background_duration_text(std::int64_t ms) {
    const std::int64_t seconds = (std::max<std::int64_t>)(0, (ms + 500) / 1000);
    if (seconds < 60) return std::to_string(seconds) + "s";
    const std::int64_t minutes = seconds / 60;
    if (minutes < 60) {
        const std::int64_t rest = seconds % 60;
        return std::to_string(minutes) + "m" + (rest ? " " + std::to_string(rest) + "s" : "");
    }
    const std::int64_t hours = minutes / 60;
    const std::int64_t rest = minutes % 60;
    return std::to_string(hours) + "h" + (rest ? " " + std::to_string(rest) + "m" : "");
}

std::string background_output_tail(const std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    std::string tail = utf8_suffix(text, max_bytes);
    const auto newline = tail.find('\n');
    if (newline != std::string::npos && newline + 1 < tail.size()) tail = tail.substr(newline + 1);
    return "[... earlier output omitted ...]\n" + tail;
}

std::string format_background_completion(const BackgroundTaskInfo& info,
                                         const std::string& output_tail) {
    std::ostringstream out;
    out << "<background-task id=\"" << xml_attr(info.id) << "\" kind=\"command\" status=\""
        << background_task_state_name(info.state) << "\"";
    if (info.state == BackgroundTaskState::Completed || info.state == BackgroundTaskState::Failed) {
        out << " exit-code=\"" << info.exit_code << "\"";
    }
    out << ">\n";
    out << "Command: " << info.command << "\n";
    const std::int64_t ran = info.ended_at_ms > info.started_at_ms ? info.ended_at_ms - info.started_at_ms : 0;
    if (!info.error.empty()) {
        out << "It failed to run: " << info.error << "\n";
    } else if (info.state == BackgroundTaskState::Stopped) {
        if (info.stop_reason == "limit") {
            out << "It was stopped after reaching its background time limit (" << background_duration_text(ran) << ").\n";
        } else {
            out << "It was stopped at the user's or your request after " << background_duration_text(ran) << ".\n";
        }
    } else {
        out << "It finished after " << background_duration_text(ran) << " with exit code " << info.exit_code << ".\n";
    }
    if (!info.output_file.empty()) out << "Full output: " << info.output_file << "\n";
    if (output_tail.empty()) {
        out << "It produced no output.\n";
    } else {
        out << "Last output:\n" << output_tail;
        if (output_tail.back() != '\n') out << "\n";
    }
    out << "</background-task>";
    return out.str();
}

std::string format_background_output_lines(const BackgroundTaskInfo& info,
                                           const std::string& lines,
                                           bool final_batch) {
    std::ostringstream out;
    out << "<background-task id=\"" << xml_attr(info.id)
        << "\" kind=\"command\" status=\"running\" event=\"output\">\n";
    out << "New output from the background command `" << info.command << "`:\n" << lines;
    if (!lines.empty() && lines.back() != '\n') out << "\n";
    if (final_batch) {
        out << "Further output lines will not be delivered one by one; you will still be notified when it finishes.\n";
    }
    out << "</background-task>";
    return out.str();
}

std::string format_background_quiet_checkin(const BackgroundTaskInfo& info, std::int64_t silent_ms) {
    std::ostringstream out;
    out << "<background-task id=\"" << xml_attr(info.id)
        << "\" kind=\"command\" status=\"running\" event=\"quiet\">\n";
    out << "The background command `" << info.command << "` is still running but has produced no output for "
        << background_duration_text(silent_ms) << ". If it may be waiting for input or stuck, look at "
        << (info.output_file.empty() ? std::string("its output") : info.output_file)
        << " and stop it with " << stop_tool_name()
        << " if it is no longer needed. Otherwise there is nothing to do; you will be notified when it finishes.\n";
    out << "</background-task>";
    return out.str();
}

std::string format_background_agent_completion(const BackgroundTaskInfo& info,
                                               const BackgroundAgentOutcome& outcome) {
    std::ostringstream out;
    out << "<background-task id=\"" << xml_attr(info.id) << "\" kind=\"agent\" status=\""
        << background_task_state_name(outcome.state) << "\">\n";
    out << "Subagent session: " << info.child_session_id << "\n";
    if (!info.title.empty()) out << "Task: " << info.title << "\n";
    if (outcome.state == BackgroundTaskState::Completed) {
        out << "Final reply:\n" << (outcome.text.empty() ? std::string("(no final reply)") : outcome.text);
    } else if (outcome.state == BackgroundTaskState::Stopped) {
        out << "It was stopped before finishing.";
        if (!outcome.text.empty()) out << " Partial output:\n" << outcome.text;
    } else {
        out << "It failed before finishing.";
        if (!outcome.text.empty()) out << "\n" << outcome.text;
    }
    out << "\n</background-task>";
    return out.str();
}

std::string format_backgrounded_tool_result(const BackgroundTaskInfo& info,
                                            std::int64_t foreground_timeout_ms,
                                            const std::string& stop_tool) {
    std::ostringstream out;
    if (info.kind == BackgroundTaskKind::Agent) {
        switch (info.origin) {
            case BackgroundOrigin::UserMessage:
                out << "The subagent (session " << info.child_session_id << ") was moved to the background so that "
                       "a message from the user can reach you; it keeps running.";
                break;
            case BackgroundOrigin::UserRequest:
                out << "The user moved the subagent (session " << info.child_session_id << ") to the background; "
                       "it keeps running.";
                break;
            default:
                out << "The subagent is working in the background (session " << info.child_session_id << ").";
                break;
        }
        out << "\nYou will be notified automatically with its final reply when it finishes. Do not sleep, poll or "
               "call wait_subagent just to check on it, and do not redo its work; work on something that does not "
               "depend on it, or end your reply if there is nothing else to do — you will be woken when it finishes. "
               "Never guess or invent its result before the notification arrives.";
        return out.str();
    }
    switch (info.origin) {
        case BackgroundOrigin::Timeout:
            out << "Command did not finish within its " << background_duration_text(foreground_timeout_ms)
                << " timeout and was moved to the background (ID: " << info.id << "); it was not interrupted.";
            break;
        case BackgroundOrigin::UserMessage:
            out << "Command was moved to the background (ID: " << info.id
                << ") so that a message from the user can reach you; it was not interrupted.";
            break;
        case BackgroundOrigin::UserRequest:
            out << "The user moved this command to the background (ID: " << info.id << "); it was not interrupted.";
            break;
        case BackgroundOrigin::Requested:
            out << "Command is running in the background (ID: " << info.id << ").";
            break;
    }
    if (!info.output_file.empty()) out << "\nOutput is being written to: " << info.output_file;
    out << "\nYou will be notified automatically when it finishes, with its exit code and last output. "
           "Do not poll that file or sleep while waiting: keep working on anything that does not depend on it, "
           "or end your reply if there is nothing else to do — you will be woken when it finishes. "
           "Read the file only if you need something it prints before it finishes, such as a server's ready line.";
    if (info.monitor) out << "\nNew output lines will also be delivered to you as they appear.";
    if (info.origin != BackgroundOrigin::Requested) {
        out << "\nIt is still running: do not start a second copy of it.";
    }
    out << "\nIf it is no longer needed, stop it with " << stop_tool << ".";
    return out.str();
}

struct BackgroundTaskRegistry::Task {
    std::mutex mu;
    BackgroundTaskInfo info;
    // 命令
    std::unique_ptr<ShellProcess> process;
    IncrementalTextDecoder decoder;
    std::ofstream log;
    std::string tail;
    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::time_point last_output;
    // 静默提醒:下一次提醒时的静默时长;0 = 还没开始计(有输出时清零重来)。
    std::chrono::milliseconds next_quiet_checkin{0};
    std::chrono::milliseconds quiet_interval{0};
    std::int64_t max_runtime_ms = 0;
    bool stop_requested = false;
    // 逐行通知
    std::string partial_line;
    std::string pending_lines;
    bool batch_open = false;
    std::chrono::steady_clock::time_point batch_started;
    std::size_t batches_sent = 0;
    // 子代理
    std::function<std::optional<BackgroundAgentOutcome>()> poll;
    std::function<void()> stop_agent;
    std::chrono::steady_clock::time_point next_agent_poll;
    bool stop_signalled = false;
    bool consumed = false;
    // 结束通知已生成、还没交给 Sink:这段时间仍算活跃(headless 等待、子代理完成判定)。
    bool delivering = false;
};

struct BackgroundTaskRegistry::Shared {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::shared_ptr<Task>> tasks;
    Sink sink;
    bool closed = false;
    BackgroundRegistryOptions options;
};

namespace {

using TaskPtr = std::shared_ptr<BackgroundTaskRegistry::Task>;

struct PumpEvent {
    std::optional<BackgroundNotice> notice;
    std::optional<BackgroundTaskInfo> changed;
    TaskPtr delivering;  // 投递完成后清掉这个任务的 delivering 标记
};

} // namespace

// ---- 泵线程上的单步处理(持 task->mu)----

namespace {

void write_text(BackgroundTaskRegistry::Task& task, const std::string& text,
                std::chrono::steady_clock::time_point now) {
    if (text.empty()) return;
    if (task.log.is_open()) task.log << text;
    append_tail(task.tail, text);
    task.info.output_bytes += text.size();
    task.info.last_output_at_ms = now_ms();
    task.last_output = now;
    task.next_quiet_checkin = std::chrono::milliseconds{0};
    if (task.info.monitor) {
        task.partial_line += text;
        const auto last_newline = task.partial_line.rfind('\n');
        if (last_newline != std::string::npos) {
            task.pending_lines += task.partial_line.substr(0, last_newline + 1);
            task.partial_line.erase(0, last_newline + 1);
            if (!task.batch_open) {
                task.batch_open = true;
                task.batch_started = now;
            }
        }
    }
}

void pump_shell_output(BackgroundTaskRegistry::Task& task, std::chrono::steady_clock::time_point now) {
    std::string raw;
    while (raw.size() < kMaxReadPerStep && task.process->read_available(raw) > 0) {
    }
    if (!raw.empty()) write_text(task, clean_shell_output(task.decoder.push(raw.data(), raw.size())), now);
}

BackgroundNotice make_notice(const BackgroundTaskInfo& info, std::string text, const char* event) {
    BackgroundNotice notice;
    notice.task_id = info.id;
    notice.text = std::move(text);
    notice.metadata = background_task_to_json(info);
    notice.metadata["event"] = event;
    return notice;
}

void finish_shell(BackgroundTaskRegistry::Task& task, BackgroundTaskState state,
                  const std::string& stop_reason, bool closed, std::vector<PumpEvent>& events) {
    std::string raw;
    task.process->drain(raw);
    const auto now = std::chrono::steady_clock::now();
    if (!raw.empty()) write_text(task, clean_shell_output(task.decoder.push(raw.data(), raw.size())), now);
    write_text(task, clean_shell_output(task.decoder.flush()), now);
    const std::string launch_error = task.process->launch_error();
    task.info.exit_code = task.process->exit_code();
    task.info.ended_at_ms = now_ms();
    if (!launch_error.empty()) {
        task.info.error = launch_error;
        state = BackgroundTaskState::Failed;
    } else if (state == BackgroundTaskState::Completed && task.info.exit_code != 0) {
        state = BackgroundTaskState::Failed;
    }
    task.info.state = state;
    task.info.stop_reason = stop_reason;
    if (task.log.is_open()) task.log.close();
    task.process.reset();
    PumpEvent event;
    event.changed = task.info;
    if (!closed) {
        event.notice = make_notice(task.info,
            format_background_completion(task.info, background_output_tail(task.tail, kNotificationTailBytes)),
            "finished");
        task.delivering = true;
    }
    events.push_back(std::move(event));
}

void step_shell(BackgroundTaskRegistry::Task& task, const BackgroundRegistryOptions& options,
                bool closed, std::vector<PumpEvent>& events) {
    const auto now = std::chrono::steady_clock::now();
    pump_shell_output(task, now);
    if (task.process->poll_exited()) {
        finish_shell(task, BackgroundTaskState::Completed, {}, closed, events);
        return;
    }
    if (task.stop_requested) {
        task.process->kill_tree(1000, true);
        finish_shell(task, BackgroundTaskState::Stopped, "user", closed, events);
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - task.started).count();
    if (task.max_runtime_ms > 0 && elapsed >= task.max_runtime_ms) {
        task.process->kill_tree(1000, true);
        finish_shell(task, BackgroundTaskState::Stopped, "limit", closed, events);
        return;
    }
    if (closed) return;
    // 逐行通知:攒够一个窗口(或攒得太多)就发一批。
    if (task.info.monitor && task.batch_open &&
        (now - task.batch_started >= options.monitor_batch_window ||
         task.pending_lines.size() >= kMonitorBatchMaxBytes)) {
        task.batch_open = false;
        std::string lines = std::move(task.pending_lines);
        task.pending_lines.clear();
        if (task.batches_sent < options.monitor_max_batches) {
            ++task.batches_sent;
            const bool final_batch = task.batches_sent == options.monitor_max_batches;
            if (final_batch) task.info.monitor = false;
            PumpEvent event;
            event.notice = make_notice(task.info,
                format_background_output_lines(task.info, background_output_tail(lines, kMonitorBatchMaxBytes),
                                               final_batch),
                "output");
            events.push_back(std::move(event));
        }
    }
    // 静默提醒:持续没有输出时提醒一次,之后每次的间隔翻倍(T、再过 2T、再过 4T…)。
    if (options.quiet_checkin_after.count() > 0) {
        if (task.next_quiet_checkin.count() == 0) {
            task.quiet_interval = options.quiet_checkin_after;
            task.next_quiet_checkin = options.quiet_checkin_after;
        }
        const auto silent = std::chrono::duration_cast<std::chrono::milliseconds>(now - task.last_output);
        if (silent >= task.next_quiet_checkin) {
            task.quiet_interval *= 2;
            task.next_quiet_checkin = silent + task.quiet_interval;
            PumpEvent event;
            event.notice = make_notice(task.info, format_background_quiet_checkin(task.info, silent.count()), "quiet");
            events.push_back(std::move(event));
        }
    }
}

void step_agent(BackgroundTaskRegistry::Task& task, const BackgroundRegistryOptions& options,
                bool closed, std::vector<PumpEvent>& events) {
    const auto now = std::chrono::steady_clock::now();
    if (task.stop_requested && !task.stop_signalled) {
        task.stop_signalled = true;
        if (task.stop_agent) task.stop_agent();
    }
    if (now < task.next_agent_poll) return;
    task.next_agent_poll = now + options.agent_poll_interval;
    std::optional<BackgroundAgentOutcome> outcome;
    try {
        if (task.poll) outcome = task.poll();
    } catch (const std::exception& error) {
        outcome = BackgroundAgentOutcome{BackgroundTaskState::Failed, error.what()};
    }
    if (!outcome) return;
    task.info.state = outcome->state;
    task.info.ended_at_ms = now_ms();
    if (outcome->state == BackgroundTaskState::Stopped) task.info.stop_reason = "user";
    if (outcome->state == BackgroundTaskState::Failed) task.info.error = outcome->text;
    PumpEvent event;
    event.changed = task.info;
    if (!closed && !task.consumed) {
        event.notice = make_notice(task.info, format_background_agent_completion(task.info, *outcome), "finished");
        task.delivering = true;
    }
    events.push_back(std::move(event));
}

void prune_finished_locked(std::vector<TaskPtr>& tasks) {
    std::size_t finished = 0;
    for (auto it = tasks.rbegin(); it != tasks.rend(); ++it) {
        std::lock_guard<std::mutex> lock((*it)->mu);
        if ((*it)->info.state != BackgroundTaskState::Running) ++finished;
    }
    if (finished <= kMaxRetainedFinished) return;
    std::size_t to_drop = finished - kMaxRetainedFinished;
    tasks.erase(std::remove_if(tasks.begin(), tasks.end(), [&to_drop](const TaskPtr& task) {
        if (to_drop == 0) return false;
        std::lock_guard<std::mutex> lock(task->mu);
        if (task->info.state == BackgroundTaskState::Running) return false;
        --to_drop;
        return true;
    }), tasks.end());
}

bool any_running(const std::vector<TaskPtr>& tasks) {
    for (const auto& task : tasks) {
        std::lock_guard<std::mutex> lock(task->mu);
        if (task->info.state == BackgroundTaskState::Running) return true;
    }
    return false;
}

void deliver(const BackgroundTaskRegistry::Sink& sink, std::vector<PumpEvent>& events) {
    for (auto& event : events) {
        try {
            if (event.changed && sink.changed) sink.changed(*event.changed);
            if (event.notice && sink.notify) sink.notify(std::move(*event.notice));
        } catch (const std::exception& error) {
            LOG_WARN(std::string("[background] sink failed: ") + error.what());
        }
        if (event.delivering) {
            std::lock_guard<std::mutex> lock(event.delivering->mu);
            event.delivering->delivering = false;
        }
    }
}

} // namespace

namespace {

void pump_loop(const std::shared_ptr<BackgroundTaskRegistry::Shared>& shared, const StopToken& token) {
    while (!token.stop_requested()) {
        std::vector<TaskPtr> running;
        BackgroundTaskRegistry::Sink sink;
        BackgroundRegistryOptions options;
        {
            std::unique_lock<std::mutex> lock(shared->mu);
            if (shared->closed) return;
            for (const auto& task : shared->tasks) {
                std::lock_guard<std::mutex> task_lock(task->mu);
                if (task->info.state == BackgroundTaskState::Running) running.push_back(task);
            }
            if (running.empty()) {
                shared->cv.wait_for(lock, std::chrono::milliseconds(500), [&shared, &token] {
                    return shared->closed || token.stop_requested() || any_running(shared->tasks);
                });
                continue;
            }
            sink = shared->sink;
            options = shared->options;
        }
        std::vector<PumpEvent> events;
        for (const auto& task : running) {
            std::lock_guard<std::mutex> lock(task->mu);
            if (task->info.state != BackgroundTaskState::Running) continue;
            const std::size_t before = events.size();
            try {
                if (task->info.kind == BackgroundTaskKind::Shell) step_shell(*task, options, false, events);
                else step_agent(*task, options, false, events);
            } catch (const std::exception& error) {
                LOG_WARN("[background] task " + task->info.id + " step failed: " + error.what());
            }
            if (task->delivering) {
                for (std::size_t i = before; i < events.size(); ++i) {
                    if (events[i].notice && events[i].changed) events[i].delivering = task;
                }
            }
        }
        if (!events.empty()) {
            {
                std::lock_guard<std::mutex> lock(shared->mu);
                if (shared->closed) return;
                prune_finished_locked(shared->tasks);
            }
            deliver(sink, events);
        }
        token.wait_for(options.poll_interval);
    }
}

std::string ensure_output_file(const std::string& output_dir, const std::string& id, std::ofstream& log) {
    if (output_dir.empty()) return {};
    std::error_code ec;
    std::filesystem::create_directories(path_from_utf8(output_dir), ec);
    const auto path = path_from_utf8(output_dir) / (id + ".log");
    log.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!log.is_open()) {
        LOG_WARN("[background] cannot open output file under " + output_dir);
        return {};
    }
    return path_to_utf8(path);
}

} // namespace

BackgroundTaskRegistry::BackgroundTaskRegistry(BackgroundRegistryOptions options)
    : shared_(std::make_shared<Shared>()) {
    shared_->options = options;
}

BackgroundTaskRegistry::~BackgroundTaskRegistry() {
    shutdown();
}

void BackgroundTaskRegistry::connect(Sink sink) {
    std::lock_guard<std::mutex> lock(shared_->mu);
    shared_->sink = std::move(sink);
}

void BackgroundTaskRegistry::ensure_pump_locked() {
    if (pump_.joinable()) return;
    pump_ = acecode::JoiningThread([shared = shared_](StopToken token) { pump_loop(shared, token); });
}

std::string BackgroundTaskRegistry::start_shell(ShellStart start, ShellLaunchFailure& failure) {
    auto process = ShellProcess::launch(start.spec, failure);
    if (!process) return {};
    auto task = std::make_shared<Task>();
    task->info.id = new_task_id();
    task->info.kind = BackgroundTaskKind::Shell;
    task->info.origin = start.origin;
    task->info.command = start.spec.command;
    task->info.title = start.title.empty() ? start.spec.command : start.title;
    task->info.cwd = start.spec.cwd;
    task->info.started_at_ms = now_ms();
    task->info.monitor = start.monitor;
    task->info.output_file = ensure_output_file(start.output_dir, task->info.id, task->log);
    task->started = std::chrono::steady_clock::now();
    task->last_output = task->started;
    task->max_runtime_ms = start.max_runtime_ms;
    task->process = std::move(process);
    const BackgroundTaskInfo info = task->info;
    Sink sink;
    {
        std::lock_guard<std::mutex> lock(shared_->mu);
        if (shared_->closed) {
            task->process->kill_tree();
            failure.message = "Background tasks are shutting down.";
            return {};
        }
        shared_->tasks.push_back(task);
        sink = shared_->sink;
        ensure_pump_locked();
    }
    shared_->cv.notify_all();
    if (sink.changed) sink.changed(info);
    return info.id;
}

std::string BackgroundTaskRegistry::adopt_shell(ShellAdoption adoption) {
    auto task = std::make_shared<Task>();
    task->info.id = new_task_id();
    task->info.kind = BackgroundTaskKind::Shell;
    task->info.origin = adoption.origin;
    task->info.command = adoption.command;
    task->info.title = adoption.title.empty() ? adoption.command : adoption.title;
    task->info.cwd = adoption.cwd;
    task->info.started_at_ms = adoption.started_at_ms ? adoption.started_at_ms : now_ms();
    task->info.output_file = ensure_output_file(adoption.output_dir, task->info.id, task->log);
    task->started = std::chrono::steady_clock::now() -
        std::chrono::milliseconds(std::max<std::int64_t>(0, now_ms() - task->info.started_at_ms));
    task->last_output = std::chrono::steady_clock::now();
    task->max_runtime_ms = adoption.max_runtime_ms;
    task->decoder = std::move(adoption.decoder);
    task->process = std::move(adoption.process);
    if (!adoption.captured_text.empty()) {
        if (task->log.is_open()) task->log << adoption.captured_text;
        append_tail(task->tail, adoption.captured_text);
        task->info.output_bytes = adoption.captured_text.size();
        task->info.last_output_at_ms = now_ms();
    }
    const BackgroundTaskInfo info = task->info;
    Sink sink;
    {
        std::lock_guard<std::mutex> lock(shared_->mu);
        if (shared_->closed) {
            if (task->process) task->process->kill_tree();
            return {};
        }
        shared_->tasks.push_back(task);
        sink = shared_->sink;
        ensure_pump_locked();
    }
    shared_->cv.notify_all();
    if (sink.changed) sink.changed(info);
    return info.id;
}

std::string BackgroundTaskRegistry::watch_agent(AgentWatch watch) {
    if (watch.child_session_id.empty()) return {};
    auto task = std::make_shared<Task>();
    task->info.id = watch.child_session_id;
    task->info.kind = BackgroundTaskKind::Agent;
    task->info.origin = watch.origin;
    task->info.title = watch.title;
    task->info.child_session_id = watch.child_session_id;
    task->info.started_at_ms = now_ms();
    task->started = std::chrono::steady_clock::now();
    task->last_output = task->started;
    task->poll = std::move(watch.poll);
    task->stop_agent = std::move(watch.stop);
    const BackgroundTaskInfo info = task->info;
    Sink sink;
    {
        std::lock_guard<std::mutex> lock(shared_->mu);
        if (shared_->closed) return {};
        // 同一个子会话再次被放到后台(例如 follow-up 之后)时替换旧的观察项。
        shared_->tasks.erase(std::remove_if(shared_->tasks.begin(), shared_->tasks.end(),
            [&info](const TaskPtr& existing) {
                std::lock_guard<std::mutex> task_lock(existing->mu);
                return existing->info.kind == BackgroundTaskKind::Agent &&
                       existing->info.child_session_id == info.child_session_id;
            }), shared_->tasks.end());
        shared_->tasks.push_back(task);
        sink = shared_->sink;
        ensure_pump_locked();
    }
    shared_->cv.notify_all();
    if (sink.changed) sink.changed(info);
    return info.id;
}

bool BackgroundTaskRegistry::stop(const std::string& id, std::string& error) {
    TaskPtr target;
    {
        std::lock_guard<std::mutex> lock(shared_->mu);
        for (const auto& task : shared_->tasks) {
            std::lock_guard<std::mutex> task_lock(task->mu);
            if (task->info.id == id) { target = task; break; }
        }
    }
    if (!target) {
        error = "unknown background task: " + id;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(target->mu);
        if (target->info.state != BackgroundTaskState::Running) {
            error = "background task " + id + " is not running";
            return false;
        }
        target->stop_requested = true;
    }
    shared_->cv.notify_all();
    return true;
}

void BackgroundTaskRegistry::consume_agent_result(const std::string& child_session_id) {
    std::lock_guard<std::mutex> lock(shared_->mu);
    for (const auto& task : shared_->tasks) {
        std::lock_guard<std::mutex> task_lock(task->mu);
        if (task->info.kind == BackgroundTaskKind::Agent && task->info.child_session_id == child_session_id) {
            task->consumed = true;
        }
    }
}

std::vector<BackgroundTaskInfo> BackgroundTaskRegistry::list() const {
    std::vector<BackgroundTaskInfo> out;
    std::lock_guard<std::mutex> lock(shared_->mu);
    out.reserve(shared_->tasks.size());
    for (const auto& task : shared_->tasks) {
        std::lock_guard<std::mutex> task_lock(task->mu);
        out.push_back(task->info);
    }
    return out;
}

std::optional<BackgroundTaskInfo> BackgroundTaskRegistry::find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(shared_->mu);
    for (const auto& task : shared_->tasks) {
        std::lock_guard<std::mutex> task_lock(task->mu);
        if (task->info.id == id) return task->info;
    }
    return std::nullopt;
}

std::size_t BackgroundTaskRegistry::running_count() const {
    std::size_t count = 0;
    std::lock_guard<std::mutex> lock(shared_->mu);
    for (const auto& task : shared_->tasks) {
        std::lock_guard<std::mutex> task_lock(task->mu);
        if (task->info.state == BackgroundTaskState::Running || task->delivering) ++count;
    }
    return count;
}

std::string BackgroundTaskRegistry::read_output_tail(const std::string& id, std::size_t max_bytes) const {
    std::lock_guard<std::mutex> lock(shared_->mu);
    for (const auto& task : shared_->tasks) {
        std::lock_guard<std::mutex> task_lock(task->mu);
        if (task->info.id == id) return background_output_tail(task->tail, max_bytes);
    }
    return {};
}

void BackgroundTaskRegistry::shutdown() {
    std::vector<TaskPtr> tasks;
    {
        std::lock_guard<std::mutex> lock(shared_->mu);
        if (shared_->closed && !pump_.joinable()) return;
        shared_->closed = true;
        tasks = shared_->tasks;
    }
    shared_->cv.notify_all();
    pump_.request_stop();
    pump_.join();
    pump_ = acecode::JoiningThread();
    std::vector<PumpEvent> ignored;
    for (const auto& task : tasks) {
        std::lock_guard<std::mutex> lock(task->mu);
        if (task->info.state != BackgroundTaskState::Running) continue;
        if (task->info.kind == BackgroundTaskKind::Shell && task->process) {
            task->process->kill_tree(1000, true);
            finish_shell(*task, BackgroundTaskState::Stopped, "shutdown", true, ignored);
        } else {
            task->info.state = BackgroundTaskState::Stopped;
            task->info.stop_reason = "shutdown";
            task->info.ended_at_ms = now_ms();
        }
    }
}

} // namespace acecode
