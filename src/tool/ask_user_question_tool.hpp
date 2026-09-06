#pragma once

#include "tool_executor.hpp"

#include <ftxui/component/screen_interactive.hpp>
#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

struct TuiState;

// 单个选项:label 是模型给出的显示文本,description 是可选的解释文本。
// preview 字段上游 claudecodehaha 支持,但 ACECode 的 TUI 当前不渲染 preview,
// 仅作为 schema 兼容层允许传入(parse 时忽略内容)。
struct AskOption {
    std::string label;
    std::string description;
};

struct AskQuestion {
    std::string question;     // 问题文本 (必须以 `?` 结尾 —— 但 schema 不强制,交给模型)
    std::string header;       // 12 字符以内的 chip 标签
    std::vector<AskOption> options;  // 2–4 个显式选项;"Other..." 由 TUI 自动追加
    bool multi_select = false;
};

// 解析 + 校验 `AskUserQuestion` 工具的 JSON 参数。成功时返回解析出来的
// question 列表,失败时返回 std::nullopt 并把错误消息写入 `err`
// (以 "questions" / "options" / "unique" / "labels" / "header" 等关键词
// 为索引供上层匹配)。纯函数 —— 不碰 TuiState,供单测直接调用。
std::optional<std::vector<AskQuestion>> validate_ask_user_question_args(
    const std::string& arguments_json, std::string& err);

// 拼接最终的 ToolResult 输出字符串。question_order 保留模型给问题的原始顺序,
// answers 的 value 对于 multi-select 是调用方已经用 ", " 拼好的单一字符串。
// 返回形如 `User has answered your questions: "Q1"="A1", "Q2"="A2"` 的单行。
std::string format_ask_answers(
    const std::vector<std::string>& question_order,
    const std::map<std::string, std::string>& answers);

// AskUserQuestion 双入口(ask-user-question-dual-entry)每题答案串的最终拼装
// 纯函数:selected label 列表 + 「我要补充」文本 + 「以上都不是」文本 →
// 模型看到的该题答案串。exclusive_text 非空 = 独占生效(selected 与
// supplement_text 作废,防御性兜底);产物示例:
//   仅预设            -> "A, B"
//   预设 + 补充       -> "A, B; 补充: x"
//   仅补充(独立)      -> "补充: x"
//   以上都不是        -> "以上都不是: x"
// 标记词中文为单点常量(改英文只需改本函数);分隔符 ASCII。TUI commit 与
// daemon 解析两处都调用它,是两条路径的答案串构造唯一入口。
std::string format_single_answer(
    const std::vector<std::string>& selected_labels,
    const std::string& supplement_text,
    const std::string& exclusive_text);

// Build UI-only metadata for answered AskUserQuestion results. The tool output
// remains the provider-visible text contract; UI surfaces use this structured
// payload to render compact confirmation cards.
nlohmann::json build_ask_user_question_result_metadata(
    const std::vector<std::string>& question_order,
    const std::map<std::string, std::string>& answers);

// Build a compact UI-only Q/A transcript from ask_user_question_result
// metadata. Returns empty for missing or malformed metadata.
std::string format_ask_user_question_result_display(
    const nlohmann::json& metadata);

// 拒绝路径(Esc / agent abort)的固定 ToolResult:success=false,
// output="[Error] User declined to answer questions."
ToolResult make_rejected_ask_result();

// Headless(-p / --print)模式的自动应答 ToolResult:success=true,文案指示
// 模型在 print 模式下自行决策并继续(openspec add-headless-print-mode)。
ToolResult make_headless_ask_result();

// question_policy=deny 的自动应答 ToolResult(add-ask-question-policy)。
// success=true(沿用 goal 无人值守的实证教训:false 会让模型当失败反复
// 重问),文案指示模型选推荐项或最合理假设并继续;metadata 携带
// ask_user_question_auto={mode:"deny", origin} 供转录行标注。origin 传
// ResolvedQuestionPolicy::origin("explicit")。
ToolResult make_policy_denied_ask_result(const char* origin);

// question_policy=timeout 到期的自动采纳 ToolResult:每个 question 取第一
// 个选项(工具 description 约定推荐项排第一)作为答案,output 前缀注明
// 用户 N 秒未回答、答案是自动采纳而非用户真实意志;metadata 同时携带
// ask_user_question_result(正常答案结构)与 ask_user_question_auto=
// {mode:"timeout", seconds}。TUI 与 daemon 两路共用。
ToolResult make_timeout_adopted_ask_result(
    const std::vector<AskQuestion>& questions,
    const std::vector<std::string>& question_order,
    int timeout_seconds);

// AskUserQuestion 的唯一工厂 —— TUI 与 daemon 共用。execute() 不碰
// TuiState/ScreenInteractive,完全靠 `ToolContext::ask_user_questions` 通道:
//   daemon → WS question_request → 浏览器 modal → question_answer 回流
//   TUI    → src/tui/tui_ask_channel.cpp 的阻塞 overlay
// 两端只有传输不同,工具逻辑只有这一份。ctx.ask_user_questions 为空时
// 直接报错(该会话没接提问通道 = AskUserQuestion 不可用)。
ToolImpl create_ask_user_question_tool_async();

} // namespace acecode
