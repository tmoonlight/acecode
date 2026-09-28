#pragma once

// Codex 同款的 token 估算(P2-02 自 commands/compact.hpp 拆出):ceil(bytes / 4),不假装有
// provider 专属分词器。thread_repair / system_prompt / context_usage_breakdown 只依赖这里,
// 不再依赖整个压缩模块。

#include "llm/llm_provider.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace acecode {

// Codex uses an intentionally simple UTF-8 byte estimate: ceil(bytes / 4).
std::size_t approx_token_count(const std::string& text);

// Codex's token truncation keeps the beginning and end around a marker.
std::string truncate_text_to_token_budget(const std::string& text,
                                          std::size_t max_tokens);

// 一条消息计入估算的字节数(正文、推理、结构化 parts、tool_calls、角色与请求信封)。
std::size_t estimate_message_payload_bytes(const ChatMessage& msg);

int estimate_message_tokens(const std::vector<ChatMessage>& messages);

} // namespace acecode
