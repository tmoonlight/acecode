#pragma once

// /history 回看(纯逻辑):把会话最近几轮的持久化消息排成 IM 文本,最新的一轮在前,
// 每轮内部按时间顺序(我 → ACECode)。只放文字:工具调用只计次数,工具结果、隐藏的
// 目标上下文、压缩摘要、蜂群内部消息都不显示。结果经分页交给 /more 翻看。

#include "channels/core/paging.hpp"
#include "llm/llm_provider.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace acecode::channels::core {

constexpr std::size_t kDefaultHistoryTurns = 1;
constexpr std::size_t kMaxHistoryTurns = 20;
// 整段回看的字节上限(约一百页);超过时更早的轮次只给出省略说明。
constexpr std::size_t kMaxHistoryBytes = 192 * 1024;

// 这条消息是否开始一轮新对话(用户本人发的、可见的消息)。
bool starts_history_turn(const ChatMessage& message);

// messages 按时间顺序(旧 → 新),可以从某一轮中间开始;第一条轮首之前的消息丢弃。
// busy = 会话正在执行,最新一轮标为执行中。
PagedOutput render_recent_turns(const std::vector<ChatMessage>& messages, std::size_t turns,
                                int utc_offset_minutes, bool busy);

// UTC 的 ISO 8601("2026-10-26T07:58:04Z")显示成本地 "10-26 15:58";格式不符时原样返回。
std::string short_local_time(const std::string& iso, int offset_minutes);

} // namespace acecode::channels::core
