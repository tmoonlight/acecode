#pragma once

// 会话级后台任务登记表:后台 shell 命令 + 后台子代理。
//
// - 命令可以一开始就放后台(bash run_in_background),也可以在前台跑到一半被
//   「领养」进来(超时、用户插话、用户手动转后台):进程、管道和已经读到的输出
//   原样交过来,不中断进程。
// - 一个泵线程轮询本会话的全部后台任务:非阻塞读输出写进日志文件
//   (<scratch>/background/<id>.log),检查退出 / 运行上限 / 停止请求;子代理
//   任务由调用方给的 poll 函数判断是否结束。
// - 结束时经 Sink::notify 投一条「后台任务完成」通知(AgentLoop 把它放进邮箱:
//   回合进行中在下一个模型边界插入,空闲时开新回合);开始 / 结束 / 状态变化
//   经 Sink::changed 通知界面。
// - 关闭(会话销毁、daemon 退出)时杀掉还在跑的命令,不再发通知。

#include "shell_process.hpp"
#include "utils/encoding.hpp"
#include "utils/joining_thread.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

enum class BackgroundTaskKind { Shell, Agent };
enum class BackgroundTaskState { Running, Completed, Failed, Stopped };
// 进入后台的原因:决定工具结果与界面怎么说。
enum class BackgroundOrigin { Requested, Timeout, UserMessage, UserRequest };

struct BackgroundTaskInfo {
    std::string id;
    BackgroundTaskKind kind = BackgroundTaskKind::Shell;
    BackgroundTaskState state = BackgroundTaskState::Running;
    BackgroundOrigin origin = BackgroundOrigin::Requested;
    std::string title;
    std::string command;           // 仅命令
    std::string cwd;
    std::string output_file;       // 仅命令
    std::string child_session_id;  // 仅子代理
    std::int64_t started_at_ms = 0;  // Unix 毫秒
    std::int64_t ended_at_ms = 0;
    std::int64_t last_output_at_ms = 0;
    int exit_code = -1;
    std::uint64_t output_bytes = 0;
    bool monitor = false;          // 逐行通知
    std::string stop_reason;       // Stopped 时:user / limit / shutdown
    std::string error;             // 启动失败 / 子代理失败原因
};

const char* background_task_kind_name(BackgroundTaskKind kind);
const char* background_task_state_name(BackgroundTaskState state);
const char* background_origin_name(BackgroundOrigin origin);
nlohmann::json background_task_to_json(const BackgroundTaskInfo& info);

// 投给模型的一条通知。text 是模型看到的正文,metadata 进消息的
// metadata.background_task(界面据此渲染成紧凑的系统行)。
struct BackgroundNotice {
    std::string task_id;
    std::string text;
    nlohmann::json metadata;
    bool trigger_turn = true;
};

struct BackgroundAgentOutcome {
    BackgroundTaskState state = BackgroundTaskState::Completed;
    std::string text;  // 子代理最终答复 / 失败说明
};

// 纯函数,单测覆盖。
std::string format_background_completion(const BackgroundTaskInfo& info,
                                         const std::string& output_tail);
std::string format_background_output_lines(const BackgroundTaskInfo& info,
                                           const std::string& lines,
                                           bool final_batch);
std::string format_background_quiet_checkin(const BackgroundTaskInfo& info,
                                            std::int64_t silent_ms);
std::string format_background_agent_completion(const BackgroundTaskInfo& info,
                                               const BackgroundAgentOutcome& outcome);
// 「…已转到后台」的工具结果说明(给模型)。
std::string format_backgrounded_tool_result(const BackgroundTaskInfo& info,
                                            std::int64_t foreground_timeout_ms,
                                            const std::string& stop_tool_name);
std::string background_duration_text(std::int64_t ms);
// 只保留最后 max_bytes 字节,并从完整的一行开始(UTF-8 安全)。
std::string background_output_tail(const std::string& text, std::size_t max_bytes);

struct BackgroundRegistryOptions {
    // 泵线程两次轮询的间隔。
    std::chrono::milliseconds poll_interval{25};
    // 子代理状态的检查间隔(需要查会话表,不必太勤)。
    std::chrono::milliseconds agent_poll_interval{250};
    // 后台命令持续这么久没有任何输出时提醒一次模型(之后间隔翻倍);0 = 不提醒。
    std::chrono::milliseconds quiet_checkin_after{std::chrono::minutes(20)};
    // 逐行通知:同一批最多攒这么久再发,一个任务最多发这么多批。
    std::chrono::milliseconds monitor_batch_window{std::chrono::seconds(1)};
    std::size_t monitor_max_batches = 60;
};

class BackgroundTaskRegistry {
public:
    struct Sink {
        std::function<void(BackgroundNotice)> notify;
        std::function<void(const BackgroundTaskInfo&)> changed;
    };

    struct ShellStart {
        ShellLaunchSpec spec;
        std::string title;
        std::string output_dir;
        BackgroundOrigin origin = BackgroundOrigin::Requested;
        std::int64_t max_runtime_ms = 0;  // 0 = 不限
        bool monitor = false;
    };

    struct ShellAdoption {
        std::unique_ptr<ShellProcess> process;
        IncrementalTextDecoder decoder;
        std::string captured_text;  // 前台已经读到并清洗过的输出
        std::string command;
        std::string cwd;
        std::string output_dir;
        std::string title;
        BackgroundOrigin origin = BackgroundOrigin::Timeout;
        std::int64_t started_at_ms = 0;
        std::int64_t max_runtime_ms = 0;
    };

    struct AgentWatch {
        std::string child_session_id;
        std::string title;
        BackgroundOrigin origin = BackgroundOrigin::Requested;
        // 不阻塞:子代理还在跑返回 nullopt,结束时返回结果。
        std::function<std::optional<BackgroundAgentOutcome>()> poll;
        // 停止请求(中止子会话的当前回合)。
        std::function<void()> stop;
        // 子代理自己的后台任务数等「还没真正做完」的额外信号由 poll 自行判断。
    };

    explicit BackgroundTaskRegistry(BackgroundRegistryOptions options = {});
    ~BackgroundTaskRegistry();
    BackgroundTaskRegistry(const BackgroundTaskRegistry&) = delete;
    BackgroundTaskRegistry& operator=(const BackgroundTaskRegistry&) = delete;

    // 由会话的 AgentLoop 在构造后接上;回调在泵线程上执行,不得反过来同步等待登记表。
    void connect(Sink sink);

    // 返回任务 id;失败返回空并写 failure。
    std::string start_shell(ShellStart start, ShellLaunchFailure& failure);
    std::string adopt_shell(ShellAdoption adoption);
    // 子代理任务的 id 就是子会话 id(界面据此与子代理列表合并)。
    std::string watch_agent(AgentWatch watch);

    // 停止一个运行中的任务;已结束 / 不存在返回 false 并写 error。
    bool stop(const std::string& id, std::string& error);
    // wait_subagent 已经同步拿到了结果:之后这个子代理结束时不再发通知。
    void consume_agent_result(const std::string& child_session_id);

    std::vector<BackgroundTaskInfo> list() const;
    std::optional<BackgroundTaskInfo> find(const std::string& id) const;
    std::size_t running_count() const;
    // 后台命令输出日志的末尾(界面查看用)。
    std::string read_output_tail(const std::string& id, std::size_t max_bytes) const;

    // 杀掉所有还在跑的命令并停止泵线程;之后不再发任何通知。可重复调用。
    void shutdown();

    // 实现细节(只在 background_tasks.cpp 里定义);公开声明是为了让那里的
    // 文件内辅助函数能直接操作,外部无法使用不完整类型。
    struct Task;
    struct Shared;

private:
    void ensure_pump_locked();

    std::shared_ptr<Shared> shared_;
    acecode::JoiningThread pump_;
};

} // namespace acecode
