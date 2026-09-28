#include "tool_call_message.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/hook_bridge/agent_hook_bridge.hpp"
#include "agent/event_payload/message_payload.hpp"
#include "llm/text_preamble_tags.hpp"
#include "tool/tool_executor.hpp"
#include "session/event_dispatcher.hpp"
#include "session/session_manager.hpp"

namespace acecode::agent {
ToolPreambleTitle ToolCallMessage::record(const ChatResponse& accumulated,
    const std::shared_ptr<LlmProvider>& provider_snapshot, ToolPreambleTitle& pending_preamble) {
    // Record the assistant message with tool_calls in the history
    auto tc_msg = ToolExecutor::format_assistant_tool_calls(accumulated);
    // 文本工具调用恢复成功:消息本体与原生调用字节级同形(不会发给模型的
    // metadata 只多一个诊断字段),让后续历史里出现原生调用可供模仿。
    if (accumulated.text_tool_calls.outcome ==
        TextToolCallDiagnostic::Outcome::Recovered) {
        if (!tc_msg.metadata.is_object()) tc_msg.metadata = nlohmann::json::object();
        tc_msg.metadata["text_tool_call_recovery"] = {
            {"format", accumulated.text_tool_calls.format},
            {"count", accumulated.text_tool_calls.recovered_count > 0
                          ? accumulated.text_tool_calls.recovered_count
                          : static_cast<int>(accumulated.tool_calls.size())},
        };
    }
    // 工具前言(add-tool-preamble):本批次沿用的阶段前言挂在这条
    // assistant(tool_calls) 消息的 metadata 上落盘(只为记录,不还原任何显示行),
    // 实时界面经每个调用的 tool_start.preamble 拿到它。
    const ToolPreambleTitle step_preamble = std::move(pending_preamble);
    pending_preamble = {};
    nlohmann::json preamble_metadata;
    if (!step_preamble.title.empty()) {
        preamble_metadata = {
            {"source", step_preamble.source},
            {"title", step_preamble.title},
            {"kind", step_preamble.kind},
        };
        if (!tc_msg.metadata.is_object()) tc_msg.metadata = nlohmann::json::object();
        tc_msg.metadata[agent::kToolPreambleMetadataKey] = preamble_metadata;
    }
    // 单个调用的前言 = 本批次的阶段前言。
    history_.append(tc_msg);
    if (session_manager_) session_manager_->on_message(tc_msg);
    hooks_.assistant_completed(hook_manager_, session_manager_, tc_msg, provider_snapshot);

    // Web: 工具调用回合的 assistant 文本此前只通过 token 流下发,没有一条权威的
    // Message 帧。文本-only 回合靠末尾那条 assistant Message 事件整体替换流式草稿
    // 来兜底(见 run_agent 的 text-only 分支),工具回合缺这一步 —— 一旦流式 token
    // 在传输/竞态中丢了一段,前端草稿就停在半截,且因为没有权威帧,生成结束也无法
    // 自愈(磁盘已落全量,所以切会话重载才恢复)。这里补发一条 assistant 文本的
    // Message 事件让前端用完整文本整体替换草稿。仅走 web 的 events_,不经
    // dispatch_message 的 on_message 回调,避免改变 TUI 的流式渲染行为。
    // 工具前言:正文里的 <text_preamble> 标签不进界面(id 仍按落盘原文算,与
    // GET /messages 重读一致);整段都是标签时不发这条帧。
    const std::string visible_content =
        llm::strip_text_preamble_tags(accumulated.content);
    const bool has_content_parts =
        accumulated.content_parts.is_array() && !accumulated.content_parts.empty();
    if (!visible_content.empty() || has_content_parts) {
        ChatMessage id_basis;
        id_basis.role = "assistant";
        id_basis.content = accumulated.content;
        nlohmann::json assistant_event = {
            {"role", "assistant"},
            {"content", visible_content},
            {"is_tool", false},
            {"id", web::compute_message_id(id_basis)},
        };
        if (has_content_parts) {
            assistant_event["content_parts"] = accumulated.content_parts;
        }
        if (preamble_metadata.is_object()) {
            assistant_event["metadata"] = {
                {agent::kToolPreambleMetadataKey, preamble_metadata}};
        }
        events_.emit(SessionEventKind::Message, std::move(assistant_event));
    }

    return step_preamble;
}
} // namespace acecode::agent
