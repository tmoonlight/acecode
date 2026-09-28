#include "agent_progress_emitter.hpp"
#include "agent/detail/agent_payloads.hpp"
#include "utils/time.hpp"

namespace acecode::agent {

using utils::now_epoch_ms;
using detail::build_agent_progress_payload;

void AgentProgressEmitter::emit(
    const std::string& phase, const std::string& label, const std::string& detail,
    const std::string& tool, const std::string& tool_call_id, int tool_index, bool force) {
    const auto now = clock_ ? clock_() : std::chrono::steady_clock::now();
    const std::string key = phase + "\0" + tool + "\0" + tool_call_id + "\0" + std::to_string(tool_index);
    // 具体进度提示(add-tool-preamble,「适合日常工作」):开启时 loading 只说正在
    // 做什么、不带参数 —— 等待 / 推理 / 准备调用 / 执行 / 撰写回复这几类 phase 的
    // 文案在这里统一换成 推理加粗标题 > 工具模板 > 场景文案,detail(命令预览、
    // 字节数、片段计数)一律清空;权限 / 提问 / 压缩 / 重试这些必须被看见的状态
    // 不换。批次之间的 model_waiting 也换(「正在分析文件内容」),否则 Web 实时行
    // 会在具体文案与「正在等待模型响应」之间闪动。
    const ToolPreambleTitle preamble = activity_.for_phase(phase);
    const bool concrete_label = !preamble.title.empty();
    const std::string effective_label = concrete_label ? preamble.title : label;
    const std::string effective_detail = concrete_label ? std::string{} : detail;
    if (concrete_label) activity_.announce(preamble.title);
    nlohmann::json payload;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (key != active_key_) {
            active_key_ = key;
            started_at_ms_ = now_epoch_ms();
            force = true;
        }
        if (!force && last_emit_at_.time_since_epoch().count() != 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_emit_at_);
            if (elapsed < std::chrono::milliseconds(750)) return;
        }
        last_emit_at_ = now;
        payload = build_agent_progress_payload(
            phase, effective_label, effective_detail, tool, tool_call_id, tool_index,
            started_at_ms_);
    }
    // 具体文案随进度帧透传,界面据此区分它与普通阶段文案;kind 给以后的
    // 读 / 写效果留位。
    if (concrete_label) {
        payload["preamble"] = {
            {"title", preamble.title},
            {"source", preamble.source},
            {"kind", preamble.kind},
        };
    }
    EventDispatcher::EmitOptions opts;
    opts.buffered = true;
    opts.coalesce_key = "agent_progress";
    events_.emit(SessionEventKind::AgentProgress, std::move(payload), opts);
}
} // namespace acecode::agent
