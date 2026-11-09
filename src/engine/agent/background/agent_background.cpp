// AgentLoop 的后台任务接线:会话级后台任务登记表的创建、完成通知进邮箱、
// 「转后台」请求,以及给界面(WS 事件 + TUI 回调)的状态变化。
#include "agent/agent_loop.hpp"
#include "agent/mailbox/agent_mailbox.hpp"
#include "agent/worker/agent_task_queue.hpp"
#include "llm/message_predicates.hpp"
#include "session/session_client.hpp"
#include "tool/background_tasks.hpp"

#include <utility>

namespace acecode {

void AgentLoop::install_background_tasks() {
    background_tasks_ = std::make_unique<BackgroundTaskRegistry>();
    BackgroundTaskRegistry::Sink sink;
    // 回调在登记表的泵线程上执行;AgentLoop 关闭(lifetime_ 撤销)后静默丢弃。
    sink.notify = [ref = lifetime_.ref(*this)](BackgroundNotice notice) {
        ref.with([notice = std::move(notice)](AgentLoop& loop) mutable {
            loop.deliver_background_notice(std::move(notice));
        });
    };
    sink.changed = [ref = lifetime_.ref(*this)](const BackgroundTaskInfo& info) {
        ref.with([payload = background_task_to_json(info)](AgentLoop& loop) {
            loop.emit_background_task_changed(payload);
        });
    };
    background_tasks_->connect(std::move(sink));
}

bool AgentLoop::request_background(BackgroundOrigin reason) {
    background_reason_.store(static_cast<int>(reason));
    background_generation_.fetch_add(1);
    return busy_.load();
}

void AgentLoop::deliver_background_notice(BackgroundNotice notice) {
    UserInput input;
    input.text = std::move(notice.text);
    input.metadata = nlohmann::json::object();
    input.metadata[kBackgroundTaskMetadataKey] = std::move(notice.metadata);
    // 与跨 agent 信封同一个邮箱:回合进行中在下一个模型边界插入,
    // trigger_turn 时空闲的会话会开一个新回合。
    deliver_inter_agent_message(std::move(input), notice.trigger_turn);
}

bool AgentLoop::has_background_activity() {
    return background_tasks_->running_count() > 0 || has_pending_trigger_mail() || has_pending_work();
}

void AgentLoop::emit_background_task_changed(const nlohmann::json& task) {
    events_.emit(SessionEventKind::BackgroundTask, nlohmann::json{{"task", task}});
    const auto callbacks = callbacks_.snapshot();
    if (callbacks.on_background_task) callbacks.on_background_task(task);
}

} // namespace acecode
