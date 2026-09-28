#pragma once

#include "ask_user_question_types.hpp"
#include "tool_executor.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace acecode {

struct TuiState;

inline constexpr int kDefaultAskMaxQuestions = 10;
inline constexpr int kMinAskQuestions = 1;
inline constexpr int kMaxAskQuestions = 50;

// 选项数量上限：可配置，默认 6，合法范围 [4, 8]。
// 与 AskConfig.max_options 的默认值与钳制范围保持一致。
inline constexpr int kDefaultAskMaxOptions = 6;
inline constexpr int kMinAskMaxOptions = 4;
inline constexpr int kMaxAskMaxOptions = 8;

// 解析 + 校验 `AskUserQuestion` 工具的 JSON 参数。成功时返回解析出来的
// question 列表,失败时返回 std::nullopt 并把错误消息写入 `err`
// (以 "questions" / "options" / "unique" / "labels" / "header" 等关键词
// 为索引供上层匹配)。纯函数 —— 不碰 TuiState,供单测直接调用。
std::optional<std::vector<AskQuestion>> validate_ask_user_question_args(
    const std::string& arguments_json, std::string& err);

// 使用指定的跨端题目上限校验单次调用。上限应来自已校验的
// AppConfig::ask.max_questions;保留上面的无参上限版本供独立调用方兼容。
std::optional<std::vector<AskQuestion>> validate_ask_user_question_args(
    const std::string& arguments_json, std::string& err, int max_questions);

// 同时指定题目上限与选项数量上限；选项上限应来自已校验的
// AppConfig::ask.max_options，钳制到 [4,8]。
std::optional<std::vector<AskQuestion>> validate_ask_user_question_args(
    const std::string& arguments_json, std::string& err, int max_questions,
    int max_options);

// 拼接最终的 ToolResult 输出字符串。question_order 保留模型给问题的原始顺序,
// answers 的 value 对于 multi-select 是调用方已经用 ", " 拼好的单一字符串。
// 返回形如 `User has answered your questions: "Q1"="A1", "Q2"="A2"` 的单行。
std::string format_ask_answers(
    const std::vector<std::string>& question_order,
    const std::map<std::string, std::string>& answers);

// Build UI-only metadata for answered AskUserQuestion results. The tool output
// remains the provider-visible text contract; UI surfaces use this structured
// payload to render compact confirmation cards.
nlohmann::json build_ask_user_question_result_metadata(
    const std::vector<std::string>& question_order,
    const std::map<std::string, std::string>& answers,
    const std::set<std::string>* auto_selected_questions = nullptr,
    const std::set<std::string>* multi_select_questions = nullptr);

// Build a compact UI-only Q/A transcript from ask_user_question_result
// metadata. Returns empty for missing or malformed metadata.
std::string format_ask_user_question_result_display(
    const nlohmann::json& metadata);

// 拒绝路径(Esc / agent abort)的固定 ToolResult:success=false,
// output="[Error] User declined to answer questions."
ToolResult make_rejected_ask_result();

// 插话路径(AgentLoop::interject_question):用户没作答,而是在提问挂起期间
// 直接发了一条文本。success=true —— 这不是失败,问题是被用户的新指令
// 取代了;output 告诉模型「用户改为直接输入,内容紧跟在本工具结果之后的
// user 消息里,按那条继续,别原样重问」。文本本身不重复塞进 output:它
// 由同回合 steering 机制作为真正的 user 消息提交(可带附件 / 上下文),
// 顺序由 AgentLoop 保证。metadata 携带
// ask_user_question_result={interjected:true, items:[]} 供转录行标注。
ToolResult make_interjected_ask_result();

// Headless(-p / --print)模式的自动应答 ToolResult:success=true,文案指示
// 模型在 print 模式下自行决策并继续(openspec add-headless-print-mode)。
ToolResult make_headless_ask_result();

// question_policy=deny 的自动应答 ToolResult(add-ask-question-policy)。
// success=true(沿用 goal 无人值守的实证教训:false 会让模型当失败反复
// 重问),文案指示模型选推荐项或最合理假设并继续;metadata 携带
// ask_user_question_auto={mode:"deny", origin} 供转录行标注。origin 传
// ResolvedQuestionPolicy::origin("explicit")。
ToolResult make_policy_denied_ask_result(const char* origin);

// question_policy=timeout 到期的自动采纳 ToolResult:默认每个 question 取第一
// 个选项(工具 description 约定推荐项排第一)作为答案；如果 TUI 已经提供
// 结构化收卷答案，则优先采用该答案。output 前缀注明用户 N 秒未回答、答案
// 是自动采纳而非用户真实意志;metadata 同时携带
// ask_user_question_result 与 ask_user_question_auto={mode:"timeout", seconds}。
ToolResult make_timeout_adopted_ask_result(
    const std::vector<AskQuestion>& questions,
    const std::vector<std::string>& question_order,
    int timeout_seconds,
    const std::map<std::string, std::string>* adopted_answers = nullptr,
    const std::set<std::string>* adopted_auto_selected_questions = nullptr);

// AskUserQuestion 的唯一工厂 —— TUI 与 daemon 共用。execute() 不碰
// TuiState/ScreenInteractive,完全靠 `ToolContext::ask_user_questions` 通道:
//   daemon → WS question_request → 浏览器 modal → question_answer 回流
//   TUI    → src/tui/tui_ask_channel.cpp 的阻塞 overlay
// 两端只有传输不同,工具逻辑只有这一份。ctx.ask_user_questions 为空时
// 直接报错(该会话没接提问通道 = AskUserQuestion 不可用)。
ToolImpl create_ask_user_question_tool_async();
ToolImpl create_ask_user_question_tool_async(int max_questions);
ToolImpl create_ask_user_question_tool_async(int max_questions, int max_options);

} // namespace acecode
