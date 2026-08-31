#include "pa_quirks.hpp"

#include <string>
#include <vector>

namespace acecode::pa {
namespace {

// 上下文超限的中文特征串。收录标准:必须专指「这次请求太大/太长」这件事。
// 任何请求都可能出现的泛化措辞(例如「参数错误」「请求失败」)一律不收 ——
// 误判的代价是白跑一次压缩重试,而漏判只是回到现状,所以这里宁可窄。
//
// 刻意不做大小写归一:这些全是中文,ASCII 归一对它们没有意义。英文文案由
// compact.cpp 的通用 needle 集负责,两边不重复。
const std::vector<std::string>& context_overflow_needles() {
    static const std::vector<std::string> needles = {
        // 实测报文:{"object":"error","message":"请求上下文过大",
        //           "type":"BadRequestError","code":400}
        "上下文过大",
        "上下文超限",
        "上下文超长",
        "上下文长度超",
        "上下文已满",
        "超出最大上下文",
        "超过最大上下文",
        "输入长度超",
        "输入内容过长",
        "提示词过长",
        "请求体过大",
        "token 数超",
        "token数超",
    };
    return needles;
}

bool contains_any(const std::string& text,
                  const std::vector<std::string>& needles) {
    for (const auto& needle : needles) {
        if (text.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

FaultKind classify_error_text(const std::string& text) {
    if (!enabled()) return FaultKind::None;
    if (text.empty()) return FaultKind::None;
    if (contains_any(text, context_overflow_needles())) {
        return FaultKind::ContextOverflow;
    }
    return FaultKind::None;
}

FaultKind classify(const ProviderErrorInfo& info) {
    if (!enabled()) return FaultKind::None;
    if (!info.has_error()) return FaultKind::None;
    // 用户主动取消不是服务端故障,不参与适配。
    if (info.kind == ProviderErrorKind::UserCancelled) return FaultKind::None;

    // 故意不按 status_code 过滤。通用判定限定 400/413/422 是因为它信任上游
    // 会按协议给码;而这套服务端的上下文核算本身就是不可信的那一环,再叠一层
    // 状态码前提只会多一条漏判路径。中文特征串已经足够专指,不需要状态码兜底。
    std::string haystack;
    haystack.reserve(info.display_message.size() + info.raw_body.size() +
                     info.pretty_json.size() + 2);
    haystack.append(info.display_message);
    haystack.push_back('\n');
    haystack.append(info.raw_body);
    haystack.push_back('\n');
    haystack.append(info.pretty_json);
    return classify_error_text(haystack);
}

bool is_context_overflow(const ProviderErrorInfo& info) {
    return classify(info) == FaultKind::ContextOverflow;
}

} // namespace acecode::pa
