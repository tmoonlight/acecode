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

// 上游瞬时故障的中文特征串。收录标准比上下文超限更严:必须是报文**自己**
// 表达了「这是暂时的、可以再试」,否则一条真正的客户端错误会被无限重试。
//
// 刻意不收 "LLMRequestError" 这个网关统一前缀 —— 它同时罩着瞬时故障和真正的
// 参数错误,按它重试等于对所有 4xx 都重试。也刻意不收单独的「重试」二字:
// 额度用完的报文写的是「请切换模型后重试」,那是要人换模型,不是让程序再发一次。
const std::vector<std::string>& transient_upstream_needles() {
    static const std::vector<std::string> needles = {
        // 实测:{"code":400,"message":"LLMRequestError: 模型服务异常，请稍候重试，
        //        如持续出现，请联系技术支持"}
        "请稍候重试",
        "请稍后重试",
        "模型服务异常",
        // 实测:{"code":502,"message":"LLMRequestError: 网络波动或模型处理超时，
        //        请稍候重试…"}
        "网络波动",
        "模型处理超时",
        "服务繁忙",
        "系统繁忙",
        "服务暂时不可用",
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
    // 顺序有意:上下文超限优先。它也可能带「请稍候重试」的客套话,但盲目重试
    // 同一个超大请求只会再撞一次墙,必须先走压缩。
    if (contains_any(text, transient_upstream_needles())) {
        return FaultKind::TransientUpstream;
    }
    return FaultKind::None;
}

bool is_transient_upstream(const std::string& error_text) {
    return classify_error_text(error_text) == FaultKind::TransientUpstream;
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
