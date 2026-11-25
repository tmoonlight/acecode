#pragma once

// 自动模式（试验）裁决模型回答的解析(openspec add-auto-review-mode,design D8)。
//
// 裁决模型被要求只输出 `<block>yes|no</block>`,拦下时再附一句 `<reason>…</reason>`。
// 这里只做文本解析,不做任何策略判断:解析不出就是 Invalid,由调用方决定
// 「没有结果」怎么走(退回弹窗 / 拒绝并提示重试),绝不把格式错误记成拦下。

#include <string>

namespace acecode::auto_review {

struct ParsedVerdict {
    enum class Kind {
        Allow,    // <block>no
        Block,    // <block>yes
        Invalid   // 标记缺失,或同一份回答里 yes / no 都出现
    };
    Kind kind = Kind::Invalid;
    // 仅 Block 时有意义:<reason> 里的一句话,已折成单行并截短;没有给理由时为空。
    std::string reason;
};

// 先剔除 <think> / <thinking> 思考块(未闭合的一直剔到末尾),再读标记。
ParsedVerdict parse_verdict(const std::string& text);

} // namespace acecode::auto_review
