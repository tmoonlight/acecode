#pragma once

// 文本形式工具调用恢复(fix-feedback-0924 第 3 条)。
//
// 部分 OpenAI 兼容模型(实测 dots3-note-prev,Qwen / Hermes 模板亦然)会把工具
// 调用写进正文 —— `<invoke name="Bash">…`、`<dots_function_call>` 外壳、
// Hermes 的 `<tool_call>{json}</tool_call>`、Qwen3-Coder 的
// `<tool_call><function=…>` —— provider 返回 tool_calls=0,AgentLoop 于是
// 把它当纯文本回复静默结束回合。
//
// 本模块是纯逻辑(不接线):
// - TextToolCallStreamFilter:流式增量过滤器。正常正文原样放行;行首、围栏外
//   出现调用开标签时**扣住**,等语法定案后要么释放成正文,要么在 finish() 里
//   转成原生 ToolCall(执行级)或报 Rejected(不执行,走纠正)。
// - 执行级判定五条全部成立才执行:行首、不在代码围栏内、语法完整、块前可见
//   正文只有空白、块后只有空白 / 特殊 token / 孤立外壳闭合标签。块前有正文
//   一律 Rejected(prose_prefix):那可能是在解释 XML 或转述文件 / 网页内容,
//   yolo 与 goal 无人值守下执行没有确认兜底。
// - 可疑级 detect_suspicious_text_tool_call:执行级没认出、但明显在尝试调用
//   工具(行中的 `<invoke name=` 等强特征),只触发纠正,绝不执行。
// - 解析是增量的:扣住期间记下扫描位置,只在新数据里找结束标签,复杂度随输入
//   线性增长(文本形式的 file_write 参数可能有几百 KB,且跑在 cpr 写回调线程上)。

#include "llm/llm_provider.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acecode {

struct TextToolCallRecoveryResult {
    // 需要追加到已输出正文之后的可见文本(流式 finish() 只含尾部残留;
    // recover_text_tool_calls 为完整可见正文)。标记本身永远不在这里。
    std::string visible_text;
    // 仅 Outcome::Recovered 时非空:与原生调用同形,function_name 是请求工具表
    // 里的模型侧名,id 形如 call_text_<sha1 前 24 位>。
    std::vector<ToolCall> tool_calls;
    TextToolCallDiagnostic diagnostic;
    // 语法上解析出的全部调用(不论校验是否通过、块前是否有正文),名字尽量规范
    // 成请求表里的名字。**只**用于与原生调用做回显比对,绝不执行。
    std::vector<ToolCall> candidate_calls;
};

class TextToolCallStreamFilter {
public:
    // tools = 本次请求的工具表(模型侧名 + 参数 schema)。为空时只做结构判定,
    // 认出的调用一律 Rejected(provider 只在请求带工具时才构造过滤器)。
    explicit TextToolCallStreamFilter(const std::vector<ToolDef>& tools);
    ~TextToolCallStreamFilter();
    TextToolCallStreamFilter(TextToolCallStreamFilter&&) noexcept;
    TextToolCallStreamFilter& operator=(TextToolCallStreamFilter&&) noexcept;
    TextToolCallStreamFilter(const TextToolCallStreamFilter&) = delete;
    TextToolCallStreamFilter& operator=(const TextToolCallStreamFilter&) = delete;

    // 喂入一段正文,返回此刻可以安全显示的正文。
    std::string push(std::string_view chunk);
    // 流结束:对扣住的内容定案。之后过滤器回到初始正文状态(可继续 push)。
    TextToolCallRecoveryResult finish();
    // 清空全部状态,并重新生成 id 作用域(provider 重试时用,与 DSML 一致)。
    void reset();

    bool capturing() const;
    std::size_t held_bytes() const;
    // 扣住内容的 UTF-8 安全摘录(中断时写日志用)。
    std::string held_excerpt(std::size_t max_bytes = 300) const;
    // 扣住期间解析器累计扫描的字节数(线性复杂度测试用)。
    std::size_t debug_bytes_scanned() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 非流式:等价于 push(text) + finish(),visible_text 为完整可见正文。
TextToolCallRecoveryResult recover_text_tool_calls(
    std::string_view text,
    const std::vector<ToolDef>& tools);

// 可疑级:执行级没认出、但明显是在尝试调用工具。在围栏与行内代码之外,
// 行首出现 `<function_calls>` / `<dots_function_call>` / `<tool_call>` /
// `<invoke` + 空白 + `name=`,或任意位置出现 `<invoke\s+name=`、
// `<parameter\s+name=`、`<parameter=`、`<function=` 即命中:返回
// Rejected(reason=malformed),visible_cut = 命中处所在行的行首偏移。
// 调用方只在「没有原生调用、执行级结果为 None」时才调用它。
std::optional<TextToolCallDiagnostic> detect_suspicious_text_tool_call(
    std::string_view visible_text);

// 压缩摘要校验:任意位置(不要求在末尾)出现上面的调用标记即算污染;
// DSML 标记也算。围栏与行内代码里的不算。
bool text_contains_tool_call_markup(std::string_view text);

// 混合形态:文本调用与原生调用逐个比对(名字按请求工具表规范化,参数 parse 成
// JSON 比较),完全一致的算回显,从 text_calls 里剔除;每个原生调用最多抵消一个。
void drop_echoes_of_native_calls(std::vector<ToolCall>& text_calls,
                                 const std::vector<ToolCall>& native,
                                 const std::vector<ToolDef>& tools);

// "bash(command)" 形式:工具名 + 参数键名,不带参数值(避免把大段内容再喂给模型)。
std::string describe_unexecuted_text_tool_call(const ToolCall& call);

// 混合形态的定案:text 是执行级结果(Recovered / Rejected 且解析出了调用),
// native 是同一回复里的原生调用。回显全部剔除后没有剩余 → Outcome::None;
// 否则 → IgnoredWithNative(attempted_tools / unexecuted_detail 为剩余调用)。
TextToolCallDiagnostic diagnose_text_tool_calls_with_native(
    const TextToolCallRecoveryResult& text,
    const std::vector<ToolCall>& native,
    const std::vector<ToolDef>& tools);

// 纠正提示(英文,追加在历史末尾的隐藏 user 消息)。model_tool_names 是本次
// 请求实际发给模型的模型侧工具名。文案刻意不出现 `<invoke>` 等字面标签,
// 以免给模型提供可模仿的样本。
std::string build_text_tool_call_correction_prompt(
    const TextToolCallDiagnostic& diagnostic,
    const std::vector<std::string>& model_tool_names);

// 混合形态的说明(只有存在不一致的文本调用时才生成;否则返回空串)。
std::string build_text_tool_call_ignored_note(
    const TextToolCallDiagnostic& diagnostic);

// 发给模型的历史里确定性清洗旧的文本工具调用(fix-feedback-0924 第 3 条)。
// 修复上线前落盘的会话里留着模型把调用写成正文的 assistant 消息、以及内容被
// 调用文本污染的压缩摘要;只要它们还在历史里,模型就会照着继续写文本调用
// (yubo2 现场「越模仿越多、换模型也照样模仿」)。规则:
//   - role=assistant、没有 tool_calls、去掉调用块后只剩空白(块前只有空白,
//     语法完整或写到一半被截断都算)→ content 换成 kTextToolCallHistoryPlaceholder;
//   - 压缩摘要(is_compact_summary,或 content 以 summary_prefix + "\n" 开头):
//     摘要正文末尾的调用块换成同一句话,块前的正文保留;正文只剩调用块时
//     再补 "(summary unavailable)";
//   - user 消息一律不动(用户粘贴的标记属于数据);已有 tool_calls 的消息不动;
//     块前有正文的非摘要消息不动(那是被拒纠正路径已处理过的,或是解释性内容)。
// 结果只由消息内容决定、逐字节稳定、幂等:受影响的老会话升级后只丢一次
// prompt cache,之后前缀照常稳定。
inline constexpr const char* kTextToolCallHistoryPlaceholder =
    "(A tool call was written here as plain text and was not executed.)";
void sanitize_text_tool_call_history(std::vector<ChatMessage>& history,
                                     const std::string& summary_prefix);

} // namespace acecode
