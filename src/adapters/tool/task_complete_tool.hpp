#pragma once

#include "tool_executor.hpp"

#include <string>
#include <vector>

namespace acecode {

// `task_complete` 是 agent loop 的显式终止信号工具。
// 模型调用它告知 "这一回合的工作已经做完",AgentLoop 看到成功的 task_complete
// tool call 就退出循环。工具本身零副作用 —— 不碰文件系统 / 网络 / 子进程,
// 返回值仅作为 tool_result 进入历史。
//
// 参数仿 Claude 桌面版的 wrap_up(「下一步」框):
// - summary(必填,`recap` 为别名):两三句话的回合小结,渲染为完成总结;
// - ask + actions:回合结束时轮到用户决定的那一个问题,以及 1–2 个回复按钮
//   (第一个是推荐项),Web 在回合末尾画成可点击的卡片,点按钮即把按钮文字
//   作为用户回复发出;
// - blocked:工作停在这里等用户回答。
// 校验结果写进 ToolResult::metadata["wrap_up"],界面只认这份规范化结果。
ToolImpl create_task_complete_tool();

// 规范化后的一次 task_complete 调用。
struct TaskCompleteCall {
    std::string summary;               // 去掉首尾空白;空 = 缺参
    std::string ask;                   // 只在 box_shown 时保留
    std::vector<std::string> actions;  // 可显示的回复按钮文字,最多 2 个
    bool blocked = false;
    bool box_shown = false;            // 「下一步」框会显示问题 / 按钮
    bool nothing_pending = false;      // 没给 ask / actions / blocked
    std::vector<std::string> advice;   // 写给模型下一次调用的建议
};

TaskCompleteCall parse_task_complete_call(const std::string& arguments_json);

// 没有按钮可点的渠道(IM)用的纯文本:summary 原文,框显示问题时接上问题与按钮文字,
// 用户直接回复文字。没有非空 summary 时返回空串。
std::string task_complete_plain_text(const std::string& arguments_json);

} // namespace acecode
