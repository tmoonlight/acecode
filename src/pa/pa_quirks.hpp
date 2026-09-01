#pragma once

// PA 内网模型服务的故障签名适配。目录定位与收录标准见 src/pa/README.md。
//
// 这里只回答一个问题:一条上游错误报文,在通用协议判定失效的情况下,实际
// 对应 ACECode 的哪一类已知故障。判定纯按报文特征做,不看服务地址也不看
// 模型名 —— 同一个真实模型在 saved_models 里常有多个别名,按名字匹配必漏。

#include "../provider/llm_provider.hpp"
#include "pa_adapter.hpp"

#include <string>

namespace acecode::pa {

// 归一化之后的故障类别。每新增一项都要在 README 里写清楚观测报文。
enum class FaultKind {
    // 不是已知的 PA 客制化故障,调用方应继续走通用判定。
    None,
    // 请求超出服务端实际能接受的上下文规模,应当压缩历史后重试。
    ContextOverflow,
};

// 纯文本判定。text 可以是 display_message、raw_body、pretty_json 的任意
// 拼接 —— 调用方不需要预先知道故障文案落在哪个字段里。
FaultKind classify_error_text(const std::string& text);

// 结构化判定。会把 ProviderErrorInfo 里所有可能携带文案的字段合起来看。
FaultKind classify(const ProviderErrorInfo& info);

// classify() == ContextOverflow 的便捷形式,供 compact.cpp 的通用判定兜底。
bool is_context_overflow(const ProviderErrorInfo& info);

} // namespace acecode::pa
