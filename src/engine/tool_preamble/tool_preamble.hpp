#pragma once

// 具体进度提示(openspec add-tool-preamble,设置 > 常规 > 工作模式 > 适合日常工作):
// 等待时 loading 行只说「正在做什么」,不带参数(参数留在工具行),代替
// 「正在推理」「正在调用工具 bash」这类笼统或技术化的文案。文案来源按优先级:
//   1. 推理加粗标题:provider 流回的推理摘要里第一对 **加粗**(OpenAI Responses /
//      Codex app-server / Gemini 的摘要都以此开头),只对本模型步有效;
//   2. 工具模板:按本批次的原生工具名拼现在进行时短语(「正在读取 3 个文件并搜索代码」);
//   3. 场景文案:回合开头「正在分析你的请求」,一批工具跑完后按这批工具的类型
//      (「正在分析文件内容」「正在分析命令输出」…),正文开始流出时「正在撰写回复」。
// 不要求模型额外输出任何东西。曾经的三版(先说一句话 / 必填 preamble 参数 /
// <text_preamble> 标签)都靠模型配合,grok 等模型不照做,已全部撤掉;标签扫描器
// 保留,只用来把历史里残留的标签从界面上剥掉。
// 本文件只放纯字符串逻辑(无 IO / 无 provider 依赖),进 acecode_testable 单测。

#include "llm/text_preamble_tags.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace acecode::tool_preamble {

// 文案来源(agent_progress.preamble.source / tool_start.preamble_source /
// metadata.tool_preamble.source)。
inline constexpr const char* kSourceReasoning = "reasoning";
inline constexpr const char* kSourceTemplate = "template";
inline constexpr const char* kSourceContext = "context";
// assistant(tool_calls) 消息 metadata 子键:{"title","source","kind"},只作记录。
inline constexpr const char* kMetadataKey = "tool_preamble";

// 场景文案。
inline constexpr const char* kActivityPrefix = "\xE6\xAD\xA3\xE5\x9C\xA8";  // 正在
inline constexpr const char* kInitialActivityLabel =
    "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x86\xE6\x9E\x90\xE4\xBD\xA0\xE7\x9A\x84\xE8\xAF\xB7\xE6\xB1\x82";  // 正在分析你的请求
inline constexpr const char* kRespondingActivityLabel =
    "\xE6\xAD\xA3\xE5\x9C\xA8\xE6\x92\xB0\xE5\x86\x99\xE5\x9B\x9E\xE5\xA4\x8D";  // 正在撰写回复

// 标题长度上限(Unicode code point):加粗摘要 60。标签正文的上限在 llm/text_preamble_tags.hpp。
inline constexpr std::size_t kReasoningTitleMaxCodePoints = 60;

// Codex TUI extract_first_bold 同款:第一对闭合的 `**…**`,内文 trim 后非空
// 即返回;没有闭合或内文为空则继续往后找;找不到返回空串。
std::string extract_first_bold_span(const std::string& text);

// ---- 工具模板 ----

// 本批次(同一模型步的全部工具调用,原生名)的现在进行时文案,不带任何参数:
// 同类合并计数(「正在读取 3 个文件」),两类用「并」连接,三类及以上取前两类
// 加「等」;MCP 与没列出的工具归入「调用工具」。空列表返回空串。
std::string batch_activity_label(const std::vector<std::string>& native_tool_names);
// 这批工具跑完、模型在想下一步时的文案,按第一类工具定(「正在分析文件内容」)。
std::string after_batch_activity_label(const std::vector<std::string>& native_tool_names);
// 本批次的读写属性:有写类工具即 write;全是读类工具为 read;否则空串。
std::string batch_activity_kind(const std::vector<std::string>& native_tool_names);

// 标签扫描器、标题规整与标签名 / type 常量已下沉到 domain 层的 llm/text_preamble_tags
// (P2-02,R3:tool_preamble 只允许 engine/agent/progress 引用,但 TUI 回放 / session 也要剥标签)。
// 这里的 using 声明让 tool_preamble:: 前缀的既有写法继续成立。
using llm::kTagName;
using llm::kKindRead;
using llm::kKindWrite;
using llm::kTextPreambleMaxCodePoints;
using llm::kTextPreambleMaxBodyBytes;
using llm::TextPreamble;
using llm::TextPreambleScanner;
using llm::normalize_title_line;
using llm::truncate_code_points;
using llm::strip_text_preamble_tags;

} // namespace acecode::tool_preamble
