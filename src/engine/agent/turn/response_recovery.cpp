#include "response_recovery.hpp"
#include "session/session_rewind.hpp"
#include "agent/detail/agent_payloads.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/goal/goal_runtime.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "session/session_manager.hpp"
#include "session/session_serializer.hpp"
#include "session/system_notice.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <string_view>

namespace acecode::agent {
namespace {
// A deliberately narrow placeholder alphabet. Do not reject useful numeric,
// mathematical, emoji, or other symbolic answers by requiring letters.
bool is_placeholder_only(std::string_view text) {
    constexpr std::string_view separators[] = {
        "\xE2\x80\xA6", // ellipsis
        "\xE2\x80\x93", "\xE2\x80\x94", // en/em dash
        "\xE2\x94\x80", "\xE2\x8E\xAF", // horizontal separators
    };
    std::size_t marks = 0;
    bool ellipsis = false;
    while (!text.empty()) {
        if (std::string_view(" \t\r\n").find(text.front()) != std::string_view::npos) {
            text.remove_prefix(1);
            continue;
        }
        if (text.front() == '.' || text.front() == '-' || text.front() == '_') {
            ++marks;
            text.remove_prefix(1);
            continue;
        }
        bool matched = false;
        for (const auto separator : separators) {
            if (text.substr(0, separator.size()) == separator) {
                ellipsis = ellipsis || separator == separators[0];
                ++marks;
                text.remove_prefix(separator.size());
                matched = true;
                break;
            }
        }
        if (!matched) return false;
    }
    return marks >= 3 || ellipsis;
}
} // namespace
using detail::text_tool_call_rejected_persisted_content;
using detail::text_tool_call_diagnostic_to_json;

ResponseRecoveryResult ResponseRecovery::resolve(
    const ChatResponse& response, ResponseRecoveryState& state,
    const std::vector<std::string>& model_tool_names) {
    constexpr int kMaxEmptyResponseRetries = 2;
    constexpr int kMaxTextToolCallCorrections = 2;
    constexpr int kMaxDsmlToolCallCorrections = 1;
    // 文本工具调用被拒:必须放在 response_is_blank 判断之前 —— provider
    // 藏起标记后内容往往只剩空白,会被误判成空回复(错误的纠正文案)。
    const TextToolCallDiagnostic& text_diag =
        response.text_tool_calls;
    if (text_diag.outcome == TextToolCallDiagnostic::Outcome::Rejected) {
        TextToolCallDiagnostic diag = text_diag;
        if (diag.reason == "truncated" &&
            response.finish_reason == "length") {
            diag.reason = "truncated_by_length";
        }
        const int limit = diag.format == "dsml"
            ? kMaxDsmlToolCallCorrections
            : kMaxTextToolCallCorrections;

        // 被拒的 assistant 消息去掉标记后落盘(原始标记只进 metadata /
        // 日志 / trajectory);只剩空白时清成空串。
        ChatMessage rejected_msg;
        rejected_msg.role = "assistant";
        rejected_msg.content = text_tool_call_rejected_persisted_content(
            response.content, diag);
        rejected_msg.reasoning_content =
            response.reasoning_content;
        rejected_msg.metadata = nlohmann::json{
            {"text_tool_call_rejected", text_tool_call_diagnostic_to_json(diag)},
        };
        history_.append(rejected_msg);
        if (session_manager_) session_manager_->on_message(rejected_msg);
        if (!rejected_msg.content.empty()) {
            // 定稿消息替换流式草稿:可疑级已经流出的标记在界面上随之消失。
            transcript_.dispatch_message("assistant", rejected_msg.content, false, nlohmann::json::object(), nlohmann::json::array());
        }

        if (state.text_tool_call_corrections < limit) {
            ++state.text_tool_call_corrections;
            LOG_WARN("Text-form tool call rejected (format=" + diag.format +
                     " reason=" + diag.reason + "); correction " +
                     std::to_string(state.text_tool_call_corrections) + "/" +
                     std::to_string(limit) + ": " + diag.error +
                     " excerpt=" + log_truncate(diag.raw_excerpt, 300));

            // 与空回复重试同款注入:role=user + hidden_goal_context,
            // 进 API、持久化,TUI / Web 不显示。工具名取本次请求实际
            // 发给模型的模型侧名。
            ChatMessage correction;
            correction.role = "user";
            correction.content = build_text_tool_call_correction_prompt(
                diag, model_tool_names);
            correction.metadata = nlohmann::json{
                {"hidden_goal_context", true},
                {"text_tool_call_correction", true},
            };
            ensure_user_message_identity(correction);
            history_.append(correction);
            if (session_manager_) session_manager_->on_message(correction);

            transcript_.emit_transcript_system_message(session_manager_,
                std::string(u8"[文本工具调用] 模型把工具调用写成了正文文本,未执行(") +
                    diag.error + u8"),已要求其改用原生工具调用重发 " +
                    std::to_string(state.text_tool_call_corrections) + "/" +
                    std::to_string(limit) + u8"…",
                make_system_notice_metadata("response_text_tool_call_retry",
                    {{"attempt", state.text_tool_call_corrections},
                     {"attempts", limit},
                     {"error", diag.error}}));

            return {HandleErrorResult::Continue, "text_tool_call_retry"};
        }

        LOG_ERROR("Text-form tool call still rejected after " +
                  std::to_string(limit) + " correction(s); ending turn "
                  "with error (format=" + diag.format + " reason=" +
                  diag.reason + "): " + diag.error);
        transcript_.dispatch_message(
            "error",
            std::string(u8"[Error] 模型连续 ") + std::to_string(limit + 1) +
                u8" 次把工具调用写成正文文本,无法执行(最后一次:" +
                diag.error +
                u8")。任务未完成,请重试或换用支持原生工具调用的模型。",
            false, nlohmann::json::object(), nlohmann::json::array());
        goal_.stop_after_error(session_manager_, ProviderErrorInfo{});
        return {HandleErrorResult::Break, "error"};
    }

    const bool has_content_parts =
        response.content_parts.is_array() &&
        std::any_of(response.content_parts.begin(), response.content_parts.end(),
            [](const nlohmann::json& part) {
                // Opaque Responses replay state is not a visible answer.
                if (!part.is_object()) return true;
                const auto type = part.find("type");
                return type == part.end() || !type->is_string() ||
                    *type != "openai_responses_item";
            });
    const bool response_is_blank =
        !has_content_parts && !response.has_tool_calls() &&
        (response.content.find_first_not_of(" \t\r\n") == std::string::npos ||
         is_placeholder_only(response.content));
    const bool truncated_by_length =
        response.finish_reason == "length";

    if (response_is_blank) {
        // 「成功但空」的回复是异常,不能当正常 text-only 终止。空 assistant
        // 消息仍然入历史:reasoning 回传能让模型看到自己上一轮的思考直接续
        // 上,同时给事后诊断留证据。不 dispatch 到实时流,避免空气泡。
        ChatMessage empty_msg;
        empty_msg.role = "assistant";
        empty_msg.content = "";
        empty_msg.reasoning_content =
            response.reasoning_content;
        empty_msg.content_parts = response.content_parts;
        history_.append(empty_msg);
        if (session_manager_) session_manager_->on_message(empty_msg);

        if (state.empty_response_retries < kMaxEmptyResponseRetries) {
            ++state.empty_response_retries;
            LOG_WARN("Empty assistant response (no content, no tool_calls); "
                     "retrying " + std::to_string(state.empty_response_retries) +
                     "/" + std::to_string(kMaxEmptyResponseRetries) +
                     " finish_reason=" +
                     response.finish_reason +
                     " reasoning_bytes=" +
                     std::to_string(
                         response.reasoning_content.size()));

            // 与 stop-hook continuation 同款注入机制:role=user +
            // hidden_goal_context,进 API、持久化,但 TUI/Web 不显示。
            ChatMessage nudge;
            nudge.role = "user";
            nudge.content = truncated_by_length
                ? "[SYSTEM NOTE] Your previous reply was cut off by the "
                  "output token limit (finish_reason=length) before any "
                  "answer text or tool call was produced. Keep internal "
                  "reasoning brief this time and continue the task now: "
                  "either call the next tool or reply with your answer "
                  "text directly."
                : "[SYSTEM NOTE] Your previous reply was empty: it "
                  "contained no answer text and no tool calls. Continue "
                  "the task now: either call the next tool or reply with "
                  "your answer text directly.";
            nudge.metadata = nlohmann::json{
                {"hidden_goal_context", true},
                {"empty_response_retry", true},
            };
            ensure_user_message_identity(nudge);
            history_.append(nudge);
            if (session_manager_) session_manager_->on_message(nudge);

            transcript_.emit_transcript_system_message(session_manager_,
                std::string(u8"[空回复] 模型返回了空回复(") +
                (truncated_by_length
                     ? u8"输出被 token 上限截断,思考耗尽了输出预算"
                     : u8"无正文也无工具调用") +
                u8"),自动重试 " +
                std::to_string(state.empty_response_retries) + "/" +
                std::to_string(kMaxEmptyResponseRetries) + u8"…",
                make_system_notice_metadata("response_empty_retry",
                    {{"attempt", state.empty_response_retries}, {"attempts", kMaxEmptyResponseRetries},
                     {"truncated", truncated_by_length}}));

            return {HandleErrorResult::Continue, "empty_response_retry"};
        }

        LOG_ERROR("Empty assistant response persisted after " +
                  std::to_string(kMaxEmptyResponseRetries) +
                  " retries; ending turn with error");
        transcript_.dispatch_message(
            "error",
            std::string(u8"[Error] 模型连续 ") +
                std::to_string(kMaxEmptyResponseRetries + 1) +
                u8" 次返回空回复(无正文也无工具调用" +
                (truncated_by_length
                     ? std::string(u8",输出被 token 上限截断")
                     : std::string{}) +
                u8")。任务未完成,请重试或换用其它模型。",
            false, nlohmann::json::object(), nlohmann::json::array());
        goal_.stop_after_error(session_manager_, ProviderErrorInfo{});
        return {HandleErrorResult::Break, "error"};
    }

    return {};
}
} // namespace acecode::agent
