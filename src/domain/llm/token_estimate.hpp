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

// 每张图片部件按多少 token 计入估算。不按 base64 长度算:Anthropic / OpenAI /
// Qwen-VL / GLM-V 这类原生看图的服务按像素切块计费,一张截图通常 1000~1600
// token,而 base64 长度 / 4 会给出 4 万量级 —— 截图多的会话(Computer Use、
// Agent Browser)会被估算撑爆,几张图就触发一次压缩。1600 取的是 Anthropic
// 单图上限(长边缩到 1568 像素)附近,对常见服务偏保守。
// 曾经的口径是只算 content_parts 里附件元数据的那几百字节,等于一张图 ≈ 100
// token(反馈 LINDANDAN069:估算 3.7 万的请求带着一张截图被服务端判超限,
// 估算器完全看不见那张图)。网关把 base64 当文本算的情况由超限恢复链兜底:
// 带内联图片的请求被拒,先把图片降级成文件引用再重发(thread_repair 的
// withhold_inline_images)。
inline constexpr std::size_t kEstimatedImageTokens = 1600;

// 一条消息里图片部件(content_parts 中 type == "image")的个数。
std::size_t count_image_parts(const ChatMessage& msg);
bool has_image_parts(const std::vector<ChatMessage>& messages);

// 一条消息计入估算的字节数(正文、推理、结构化 parts、tool_calls、角色与请求信封,
// 每张图片另按 kEstimatedImageTokens 折成字节)。
std::size_t estimate_message_payload_bytes(const ChatMessage& msg);

int estimate_message_tokens(const std::vector<ChatMessage>& messages);

} // namespace acecode
