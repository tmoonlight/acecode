#pragma once

// 自动模式（试验）送给裁决模型的对话记录(openspec add-auto-review-mode,design D7)。
//
// 纯函数:输入会话历史与待裁决的调用,输出文本。取舍的原则只有一条 ——
// 裁决模型只能看到「用户亲手说的话」和「agent 实际要做的事」:
//   收:用户亲发的消息、用户对提问工具的回答、助手发起的工具调用(名字 + 参数)、
//       「被用户拒绝 / 被自动模式拦下」两种结局标记、标明不是用户原话的压缩摘要。
//   不收:工具输出正文、助手正文、推理内容、请求上下文记录、后台任务通知、
//         hook 注入内容、工具执行后追加的 user 角色提示。
// 这样网页 / 文件里的注入文字到不了裁决模型,主模型也没法用自己的话说服它。
//
// 「用户亲发」按白名单认定:带未知布尔标记的 user 消息默认不算,以后新增的
// 注入类消息自然落在「不算」一侧。

#include "llm/llm_provider.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace acecode::auto_review {

// 上限按 UTF-8 字节计,截断落在字符边界上。
struct TranscriptLimits {
    std::size_t user_message_bytes = 2000;   // 单条用户消息 / 回答 / 群成员消息
    std::size_t tool_arg_field_bytes = 600;  // 历史调用里单个字符串参数
    std::size_t tool_call_bytes = 1500;      // 历史里单次调用整体
    std::size_t summary_bytes = 2000;        // 压缩摘要、其它 agent 派来的任务
    std::size_t transcript_bytes = 24000;    // 整份记录
    std::size_t action_bytes = 6000;         // 待裁决的调用本身
};

struct ReviewAction {
    std::string tool_name;        // 原生工具名
    std::string arguments_json;   // 参数原文(JSON 字符串)
    std::string tool_call_id;     // 历史里同 id 的调用不重复渲染
    // 一句话说明执行边界,例如 "inside a sandbox limited to the workspace"。空则不写。
    std::string execution;
};

struct TranscriptOptions {
    // IM 群聊代答会话:user 角色的话来自群成员而不是机主,渲染成另一种行,
    // 裁决提示词里写明它不能当作机主的许可。
    bool group_delegate = false;
    TranscriptLimits limits;
};

// 这条消息是不是用户亲手发的(可以作为用户意图的证据)。
bool is_user_authored_message(const ChatMessage& message);

// 最后一条用户亲发消息的标识(uuid,缺失时退回它在历史里的序号);没有则为空。
// 拦截计数的锁定用它判断「用户回来了没有」。
std::string last_user_authored_id(const std::vector<ChatMessage>& history);

// `<transcript>` 里的正文:一行一条,续行缩进。超出预算时先丢最早的工具调用,
// 用户消息最后才丢。
std::string render_transcript(const std::vector<ChatMessage>& history,
                              const ReviewAction& action,
                              const TranscriptOptions& options = {});

// `<action>` 里的正文:待裁决的调用,始终完整出现(只受自身上限约束)。
std::string render_action(const ReviewAction& action, const TranscriptLimits& limits = {});

} // namespace acecode::auto_review
