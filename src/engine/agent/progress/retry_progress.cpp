#include "retry_progress.hpp"
#include "agent/agent_callbacks.hpp"
#include "session/event_dispatcher.hpp"
#include "utils/time.hpp"
#include <sstream>
#include <utility>

namespace acecode::agent::detail {

// 人类可读的字节量:< 1KB 显示原始字节,否则进位到 KB / MB(保留一位小数)。
// 进度文案里直接打印原始字节数(如 "8641 字节")观感上会显得异常地大,统一走这里。
std::string human_bytes(std::size_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " 字节";
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(1);
    double kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 1024.0) oss << kb << " KB";
    else oss << (kb / 1024.0) << " MB";
    return oss.str();
}

std::string format_bytes_detail(std::size_t bytes) {
    return "参数 " + human_bytes(bytes);
}

} // namespace acecode::agent::detail

namespace acecode::agent {

void RetryProgressReporter::standard(const ProviderErrorInfo& info, bool waiting,
                                     bool compaction) {
    RetryProgressText text;
    text.phase = waiting ? "model_retry" : (compaction ? "compacting" : "model_waiting");
    text.label = waiting
        ? (compaction ? "压缩请求暂时不可用，等待重试" : "网络暂时不可用，等待重试")
        : (compaction ? "正在重新发起压缩请求" : "正在重新连接模型");
    text.detail = "第 " + std::to_string(info.retry_attempt) + " 次重试" +
        (waiting ? "将在 " + std::to_string(info.retry_delay_ms) + " ms 后发起"
                 : std::string{});
    emit(info, waiting, std::move(text));
}

void RetryProgressReporter::emit(const ProviderErrorInfo& info, bool waiting,
                                 RetryProgressText text) {
    if (waiting) {
        if (callbacks_.on_model_retry) callbacks_.on_model_retry(info);
    } else if (callbacks_.on_model_retry_resume) {
        callbacks_.on_model_retry_resume();
    }
    const std::int64_t now_ms = utils::now_epoch_ms();
    nlohmann::json payload{
        {"phase", std::move(text.phase)}, {"label", std::move(text.label)},
        {"detail", std::move(text.detail)}, {"started_at_ms", now_ms},
        {"retry_attempt", info.retry_attempt},
        {"retry_delay_ms", waiting ? info.retry_delay_ms : 0},
        {"retry_at_ms", waiting ? now_ms + info.retry_delay_ms : now_ms},
        {"retry_max_attempts", info.retry_max_attempts},
    };
    EventDispatcher::EmitOptions opts;
    opts.buffered = true;
    opts.coalesce_key = "agent_progress";
    events_.emit(SessionEventKind::AgentProgress, std::move(payload), opts);
}

} // namespace acecode::agent
