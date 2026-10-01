#include "side_chat.hpp"

#include "agent/detail/agent_payloads.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "tool/tool_executor.hpp"
#include "utils/encoding.hpp"

#include <algorithm>
#include <cctype>

namespace acecode {
namespace {

std::string trim_side_text(const std::string& text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    return first < last ? std::string(first, last) : std::string{};
}

std::string join_names(const std::vector<std::string>& names) {
    std::string joined;
    for (const auto& name : names) {
        if (!joined.empty()) joined += ", ";
        joined += name;
    }
    return joined;
}

// 主会话的系统提示列着 bash / 写文件等全部工具,侧边对话只放出只读子集。不把
// 「哪些能用、其它都不能用」说死,模型会照着主会话历史去调 bash —— 没有工具表时
// 它就把调用写成尖括号正文,这正是这里要修的现象。
std::string side_chat_instruction(const std::vector<std::string>& tool_names) {
    std::string text =
        "The following messages are a separate, read-only side conversation "
        "about the main conversation above. Answer the latest side question "
        "using both contexts. Do not continue the main task or claim that you "
        "changed files or session state.";
    if (tool_names.empty()) {
        return text + " Do not call tools. Answer directly.";
    }
    return text +
        " In this side conversation you may call only these read-only tools: " +
        join_names(tool_names) +
        ". Every other tool mentioned above (shell commands, file edits, "
        "sub-agents and so on) is unavailable here. Use the read-only tools "
        "only when the answer needs information that is not already in the "
        "context, call them through the native tool-calling interface, never "
        "write tool calls as text, and finish with a direct answer.";
}

std::string text_tool_call_correction(const TextToolCallDiagnostic& diagnostic,
                                      const std::vector<std::string>& tool_names) {
    if (tool_names.empty()) {
        return "[SYSTEM NOTE] Your previous reply wrote a tool call as plain text. "
               "Tools are unavailable in this side conversation and nothing was "
               "executed. Answer the side question directly in plain text.";
    }
    return build_text_tool_call_correction_prompt(diagnostic, tool_names) +
        "\nOnly these read-only tools exist in this side conversation: " +
        join_names(tool_names) +
        ". Shell commands and file edits are not available; use a read-only "
        "tool or answer from the context you already have.";
}

// 新一步的正文与上一步之间空一行,让各步拼成的回答在 Markdown 里分段。
std::string step_separator(const std::string& answer) {
    if (answer.empty()) return {};
    if (answer.size() >= 2 && answer.compare(answer.size() - 2, 2, "\n\n") == 0) return {};
    return answer.back() == '\n' ? "\n" : "\n\n";
}

// 界面上的工具行只显示一个简短参数(路径 / 搜索模式),不显示原始 JSON。
std::string side_chat_tool_target(const std::string& arguments_json) {
    try {
        const auto args = nlohmann::json::parse(arguments_json);
        if (!args.is_object()) return {};
        for (const char* key : {"file_path", "pattern", "query", "filePath", "path"}) {
            const auto it = args.find(key);
            if (it != args.end() && it->is_string() && !it->get<std::string>().empty()) {
                return truncate_utf8_prefix(it->get<std::string>(), 160);
            }
        }
    } catch (...) {}
    return {};
}

struct SideChatStep {
    std::string text;
    std::string reasoning;
    nlohmann::json content_parts = nlohmann::json::array();
    std::vector<ToolCall> calls;
    TextToolCallDiagnostic text_diagnostic;
    bool completed = false;
};

ToolResult run_side_chat_tool(const SideChatToolset& tools, const ToolCall& call,
                              const std::atomic<bool>* abort_flag) {
    ToolResult result;
    try {
        result = tools.execute(call, abort_flag);
    } catch (const std::exception& error) {
        result = ToolResult{std::string("[Error] Tool execution failed: ") + error.what(), false};
    } catch (...) {
        result = ToolResult{"[Error] Tool execution failed", false};
    }
    if (result.output.size() > kMaxSideChatToolOutputBytes) {
        result.output = truncate_utf8_prefix(
            result.output, kMaxSideChatToolOutputBytes,
            "\n[side chat: output truncated; narrow the request to read the rest]");
    }
    return result;
}

} // namespace

void SideChatCancellation::cancel() {
    aborted.store(true);
    std::shared_ptr<LlmProvider> provider;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        provider = provider_.lock();
    }
    if (provider) provider->notify_cancelled_request();
}

void SideChatCancellation::bind_provider(const std::shared_ptr<LlmProvider>& provider) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        provider_ = provider;
    }
    if (aborted.load() && provider) provider->notify_cancelled_request();
}

std::string validate_side_chat_history(const std::vector<SideChatMessage>& history) {
    if (history.size() > kMaxSideChatHistoryMessages || history.size() % 2 != 0) {
        return "history must contain at most 100 complete user/assistant turns";
    }
    std::size_t bytes = 0;
    for (std::size_t index = 0; index < history.size(); ++index) {
        const auto& message = history[index];
        if (message.role != (index % 2 == 0 ? "user" : "assistant")) {
            return "history must alternate user and assistant messages";
        }
        if (trim_side_text(message.content).empty()) return "history messages must not be empty";
        if (message.content.size() > kMaxSideChatHistoryBytes - bytes) {
            return "history exceeds 256 KiB";
        }
        bytes += message.content.size();
    }
    return {};
}

std::string side_question_tools_note(const SideQuestionResult& result) {
    constexpr std::size_t kListed = 6;
    const auto& used = result.tools_used;
    if (used.empty()) return {};
    std::string note = "(read-only tools: ";
    for (std::size_t index = 0; index < used.size() && index < kListed; ++index) {
        if (index > 0) note += "; ";
        note += used[index];
    }
    if (used.size() > kListed) note += "; +" + std::to_string(used.size() - kListed) + " more";
    return note + ")\n";
}

SideChatResult run_side_chat(
    std::shared_ptr<LlmProvider> provider,
    std::vector<ChatMessage> context,
    const std::string& question,
    const std::vector<SideChatMessage>& history,
    SideChatCancellation& cancellation,
    const SideChatStreamCallback& callback,
    const SideChatToolset& tools,
    const SideChatToolCallback& on_tool) {
    SideChatResult result;
    auto& response = result.response;
    response.question = trim_side_text(question);
    response.error = validate_side_chat_history(history);
    if (response.error.empty() && (response.question.empty() ||
        response.question.size() > kMaxSideQuestionBytes)) {
        response.error = response.question.empty() ? "question required" : "question too long";
    }
    if (!response.error.empty()) {
        response.status = SideQuestionStatus::InvalidQuestion;
        return result;
    }
    if (context.empty()) {
        response.status = SideQuestionStatus::ContextNotReady;
        response.error = "side-question context not ready";
        return result;
    }
    if (!provider) {
        response.status = SideQuestionStatus::ProviderUnavailable;
        response.error = "session provider unavailable";
        return result;
    }
    if (!provider->supports_tool_free_chat()) {
        response.status = SideQuestionStatus::ProviderUnavailable;
        response.error = "current model does not support read-only side chat; select another model";
        result.code = "SIDE_CHAT_PROVIDER_UNSUPPORTED";
        return result;
    }
    cancellation.bind_provider(provider);
    if (cancellation.aborted.load()) {
        response.status = SideQuestionStatus::Ok;
        result.cancelled = true;
        return result;
    }

    const bool tools_enabled = tools.enabled();
    std::vector<std::string> tool_names;
    if (tools_enabled) {
        for (const auto& definition : tools.definitions) tool_names.push_back(definition.name);
    }
    const std::vector<ToolDef> request_tools = tools_enabled ? tools.definitions : std::vector<ToolDef>{};

    ChatMessage instruction;
    instruction.role = "system";
    instruction.content = side_chat_instruction(tool_names);
    context.push_back(std::move(instruction));
    for (const auto& previous : history) {
        ChatMessage message;
        message.role = previous.role;
        message.content = previous.content;
        context.push_back(std::move(message));
    }
    ChatMessage message;
    message.role = "user";
    message.content = response.question;
    context.push_back(std::move(message));

    auto& answer = response.answer;
    bool forbidden_tools = false;
    int tool_rounds = 0;
    int corrections = 0;
    bool tool_limit_reported = false;
    const auto emit_tool = [&](const ToolCall& call, const char* status) {
        if (!on_tool) return;
        SideChatToolEvent event;
        event.call_id = call.id;
        event.name = tools.native_name ? tools.native_name(call.function_name) : call.function_name;
        event.target = side_chat_tool_target(call.function_arguments);
        event.status = status;
        on_tool(event);
    };
    const auto reset_step = [&](std::size_t step_start) {
        answer.resize(step_start);
        if (callback) callback({}, true);
    };
    // Only a step's first text is separated from the previous step's text.
    const auto append_visible = [&](const std::string& text, bool step_begins) {
        if (text.empty()) return;
        const std::string visible = (step_begins ? step_separator(answer) : std::string{}) + text;
        answer += visible;
        if (callback) callback(visible, false);
    };

    try {
        while (true) {
            const std::size_t step_start = answer.size();
            SideChatStep step;
            provider->chat_stream(context, request_tools, [&](const StreamEvent& event) {
                if (cancellation.aborted.load()) return;
                switch (event.type) {
                case StreamEventType::Delta:
                    if (event.content.empty()) break;
                    append_visible(event.content, step.text.empty());
                    step.text += event.content;
                    break;
                case StreamEventType::ReasoningDelta:
                    step.reasoning += event.content;
                    break;
                case StreamEventType::Retry:
                    step = SideChatStep{};
                    response.error.clear();
                    reset_step(step_start);
                    break;
                case StreamEventType::ToolCall:
                case StreamEventType::ToolCallDelta:
                    if (!tools_enabled) {
                        forbidden_tools = true;
                        response.error = "side-question response requested tools";
                        cancellation.cancel();
                    } else if (event.type == StreamEventType::ToolCall) {
                        step.calls.push_back(event.tool_call);
                    }
                    break;
                case StreamEventType::Error:
                    response.error = event.error.empty() ? "side-question provider call failed" : event.error;
                    break;
                case StreamEventType::Done:
                    step.completed = true;
                    step.text_diagnostic = event.text_tool_calls;
                    if (event.content_parts.is_array()) step.content_parts = event.content_parts;
                    if (event.finish_reason == "tool_calls" && !tools_enabled) {
                        forbidden_tools = true;
                        response.error = "side-question response requested tools";
                    } else if (event.finish_reason == "error" && response.error.empty()) {
                        response.error = "side-question provider call failed";
                    }
                    break;
                default:
                    break;
                }
            }, &cancellation.aborted);

            if (cancellation.aborted.load() || forbidden_tools || !response.error.empty()) break;
            if (!step.completed) {
                response.error = "side-question stream ended before completion";
                break;
            }

            if (!step.calls.empty()) {
                if (tool_limit_reported) {
                    response.error = "side-question exceeded the tool-call limit";
                    break;
                }
                const bool over_limit = ++tool_rounds > kMaxSideChatToolRounds;
                ChatResponse assistant;
                assistant.content = step.text;
                assistant.reasoning_content = step.reasoning;
                assistant.content_parts = step.content_parts;
                assistant.tool_calls = step.calls;
                context.push_back(ToolExecutor::format_assistant_tool_calls(assistant));
                for (std::size_t index = 0; index < step.calls.size(); ++index) {
                    const auto& call = step.calls[index];
                    ToolResult tool_result;
                    if (over_limit) {
                        tool_result = ToolResult{
                            "[Error] Side chat tool-call limit reached. Answer now with the "
                            "information already gathered.", false};
                    } else if (index >= kMaxSideChatToolCallsPerRound) {
                        tool_result = ToolResult{
                            "[Error] Too many tool calls in one step; this call was skipped.", false};
                    } else {
                        emit_tool(call, "running");
                        tool_result = run_side_chat_tool(tools, call, &cancellation.aborted);
                        emit_tool(call, tool_result.success ? "success" : "error");
                    }
                    context.push_back(ToolExecutor::format_tool_result(call.id, tool_result));
                    if (cancellation.aborted.load()) break;
                }
                if (cancellation.aborted.load()) break;
                tool_limit_reported = over_limit;
                continue;
            }

            // 模型把工具调用写成了正文(尖括号 / JSON 标记)。OpenAI 兼容 provider
            // 在带工具时会扣住标记并上报 Rejected;其它 provider 或漏网的情况由
            // 可疑级扫描兜底。标记绝不留在回答里:丢掉本步正文,要求模型重来。
            TextToolCallDiagnostic diagnostic = step.text_diagnostic;
            if (diagnostic.outcome != TextToolCallDiagnostic::Outcome::Rejected) {
                diagnostic = TextToolCallDiagnostic{};
                if (auto suspicious = detect_suspicious_text_tool_call(step.text)) {
                    diagnostic = std::move(*suspicious);
                }
            }
            if (diagnostic.outcome != TextToolCallDiagnostic::Outcome::Rejected) break;
            const std::string kept =
                agent::detail::text_tool_call_rejected_persisted_content(step.text, diagnostic);
            reset_step(step_start);
            if (corrections < kMaxSideChatTextToolCallCorrections) {
                ++corrections;
                if (!kept.empty()) {
                    ChatMessage rejected;
                    rejected.role = "assistant";
                    rejected.content = kept;
                    rejected.reasoning_content = step.reasoning;
                    context.push_back(std::move(rejected));
                }
                ChatMessage correction;
                correction.role = "user";
                correction.content = text_tool_call_correction(diagnostic, tool_names);
                context.push_back(std::move(correction));
                continue;
            }
            append_visible(kept, true);
            if (trim_side_text(answer).empty()) {
                response.error = "side chat model kept writing tool calls as plain text";
            }
            break;
        }
    } catch (const std::exception& error) {
        response.error = error.what();
    } catch (...) {
        response.error = "side-question provider call failed";
    }
    result.cancelled = cancellation.aborted.load() && !forbidden_tools;
    if (result.cancelled) {
        response.error.clear();
    } else if (response.error.empty() && trim_side_text(answer).empty()) {
        response.error = "side-question response was empty";
    }
    response.status = response.error.empty() ? SideQuestionStatus::Ok : SideQuestionStatus::Failed;
    return result;
}

} // namespace acecode
