#pragma once

// TUI 侧的 AskUserQuestion 传输层。
//
// AskUserQuestion 的工具逻辑(参数校验、策略判定、结果拼装、超时/拒绝的
// 各种 ToolResult)本来就只有一份;TUI 与 daemon 唯一不同的是**怎么把问题
// 送到人面前、怎么把答案拿回来**。daemon 那侧早就把这段收进了
// `ToolContext::ask_user_questions` 这个 `json(json)` 口子,TUI 却还留着一个
// 自带 overlay 逻辑的独立工具工厂 —— 纯属历史顺序(TUI 先有,daemon 后加的
// 口子没回头统一),不是设计约束。
//
// 这个文件就是把 TUI 的那段传输也塞进同一个口子。收益不止少一份代码:
//   - 两端注册的是同一个工具工厂,提问行为不会再各自漂移;
//   - **任何**工具都能问用户,而不是只有 AskUserQuestion 工具自己能问
//     (image_generate 的高档位成本确认就需要这个);
//   - ask_user_question_tool.cpp 不再引用 ftxui,那条"链接器要拉 ftxui 才能
//     resolve"的连带依赖跟着消失。
//
// 契约与 `ToolContext::ask_user_questions` 逐字一致:
//   in : [{id, text, header, options:[{label, value, description}], multiSelect}]
//   out: {cancelled: bool, timed_out: bool,
//         answers: [{question_id, selected: [str], custom_text: str}]}

#include <ftxui/component/screen_interactive.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <string>

namespace acecode {

struct TuiState;

namespace tui {

// 阻塞直到用户回答 / 超时 / 中止。
//   timeout_seconds > 0 —— 到期返回 timed_out=true(不代填答案,采纳推荐项
//                          由工具层的 make_timeout_adopted_ask_result 负责,
//                          与 daemon 路径同一处)。
//   timeout_seconds = 0 —— 无限期等待。
//   origin_label        —— 非空时在 overlay 顶部标注提问来源(子代理场景)。
nlohmann::json ask_via_tui_overlay(TuiState& state,
                                   ftxui::ScreenInteractive& screen,
                                   const nlohmann::json& questions_payload,
                                   const std::atomic<bool>* abort_flag,
                                   int timeout_seconds,
                                   const std::string& origin_label);

} // namespace tui
} // namespace acecode
