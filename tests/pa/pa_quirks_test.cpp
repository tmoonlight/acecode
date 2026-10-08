#include <gtest/gtest.h>

#include "agent/compaction/compact.hpp"
#include "provider/retry_policy.hpp"
#include "pa/pa_quirks.hpp"

#include <string>

namespace {

// set_enabled 改的是进程级开关,测试之间必须还原,否则关掉开关的那个用例会
// 让后面所有用例都跑在「适配已禁用」的状态下。
class PaEnabledGuard {
public:
    explicit PaEnabledGuard(bool value) : previous_(acecode::pa::enabled()) {
        acecode::pa::set_enabled(value);
    }
    ~PaEnabledGuard() { acecode::pa::set_enabled(previous_); }

private:
    bool previous_;
};

acecode::ProviderErrorInfo make_pa_overflow_error() {
    // 线上实测报文,一字不改。
    acecode::ProviderErrorInfo info;
    info.kind = acecode::ProviderErrorKind::Http;
    info.status_code = 400;
    info.display_message = "HTTP 400 from openai model aicoder-pro";
    info.raw_body =
        R"({"object":"error","message":"请求上下文过大",)"
        R"("type":"BadRequestError","code":400})";
    info.body_is_json = true;
    return info;
}

// 线上实测报文(2026-10-08 截图),一字不改:限流被网关以 SSE 形式返回,HTTP 429,
// body 带 "data:" 前缀。
constexpr const char* kObservedRateLimitBody =
    "data:{\"object\":\"error\",\"message\":\"您的请求频率已达到限制 (100次/10分钟)。"
    "当前已使用: 100次。\",\"type\":\"BadRequestError\",\"code\":429}";

acecode::ProviderErrorInfo make_pa_rate_limit_error(int status_code = 429) {
    acecode::ProviderErrorInfo info;
    info.kind = acecode::ProviderErrorKind::Http;
    info.status_code = status_code;
    info.display_message =
        "HTTP " + std::to_string(status_code) + " from openai model aicoder-pro";
    info.raw_body = kObservedRateLimitBody;
    info.body_is_json = false;
    return info;
}

} // namespace

// 触发场景:内网网关把「请求频率已达到限制 (100次/10分钟)」以 429 返回。
// 期望行为:判为 RateLimited;通用超限入口返回 false;重试策略放行。
// 回归背景(2026-10-08 反馈):内网做了限速后,这条报文被怀疑当成了上下文超限的
// 中断,会话表现为一直在压缩。把「限流」在 PA 层显式建模,是为了让它永远走不进
// 压缩 / 兜底收缩 / 预算学习这三条路。
TEST(PaQuirks, ObservedRateLimitPayloadIsClassifiedAsRateLimited) {
    const auto info = make_pa_rate_limit_error();
    EXPECT_EQ(acecode::pa::classify(info), acecode::pa::FaultKind::RateLimited);
    EXPECT_TRUE(acecode::pa::is_rate_limited(info.raw_body));
    EXPECT_FALSE(acecode::pa::is_context_overflow(info));
    EXPECT_FALSE(acecode::is_context_overflow_error(info));
    EXPECT_TRUE(acecode::provider_http_error_is_retryable(429, info.raw_body));
    EXPECT_TRUE(acecode::provider_error_is_rate_limited(429, info.raw_body));
}

// 触发场景:同一条限流报文,状态码被网关写成 400 / 451(「错误代码不一定靠谱」)。
// 期望行为:仍按文案判为限流 —— 可重试、不是超限。修复前 4xx 一律终止,一次
// 限流就断掉整个回合;而 451 本来是额度用完的码,这里只认报文不认码。
TEST(PaQuirks, RateLimitTextOverridesUnreliableStatusCode) {
    for (int status : {400, 451, 500}) {
        const auto info = make_pa_rate_limit_error(status);
        EXPECT_EQ(acecode::pa::classify(info), acecode::pa::FaultKind::RateLimited)
            << status;
        EXPECT_FALSE(acecode::is_context_overflow_error(info)) << status;
        EXPECT_TRUE(acecode::provider_http_error_is_retryable(status, info.raw_body))
            << status;
        EXPECT_TRUE(acecode::provider_error_is_rate_limited(status, info.raw_body))
            << status;
    }
    // 硬配额(余额 / 额度用完)仍然终止:那不是等一会儿就能过的事。
    const std::string quota_body =
        R"({"message":"请求过于频繁，且账户余额不足","code":429})";
    EXPECT_FALSE(acecode::provider_http_error_is_retryable(429, quota_body));
    EXPECT_FALSE(acecode::provider_error_is_rate_limited(429, quota_body));
}

// 触发场景:上下文超限报文被网关以 429 / 503 返回(同一个「码不可信」的问题的
// 另一面)。
// 期望行为:不重试、判为超限。修复前 429 / 5xx 在状态码白名单里被无上限重试,
// 同一个超大请求每次都被秒拒,回合困在退避里永远进不了压缩 / 兜底链。
TEST(PaQuirks, OverflowTextIsNeverRetriedEvenWithRetryableStatusCode) {
    for (int status : {429, 503}) {
        auto info = make_pa_overflow_error();
        info.status_code = status;
        EXPECT_FALSE(acecode::provider_http_error_is_retryable(status, info.raw_body))
            << status;
        EXPECT_TRUE(acecode::is_context_overflow_error(info)) << status;
        EXPECT_FALSE(acecode::provider_error_is_rate_limited(status, info.raw_body))
            << status;
    }
}

// 触发场景:限流措辞与超限 / 瞬时故障措辞同时出现。
// 期望行为:RateLimited 优先。「频率 / 限流」说的是时间窗口,比「过大」更具体;
// 判成超限的代价是压缩丢历史,判成限流的代价只是多等一会儿。
TEST(PaQuirks, RateLimitWordingWinsOverOverflowAndTransientWording) {
    EXPECT_EQ(acecode::pa::classify_error_text(
                  "请求频率过高，请求上下文过大，请稍候重试"),
              acecode::pa::FaultKind::RateLimited);
    EXPECT_EQ(acecode::pa::classify_error_text("服务繁忙，已触发限流"),
              acecode::pa::FaultKind::RateLimited);
    EXPECT_FALSE(acecode::pa::is_transient_upstream("操作过于频繁，请稍后重试"));
}

// 触发场景:关掉 PA 适配总开关。
// 期望行为:退回纯状态码判定 —— 400 + 限流文案终止,429 + 超限文案重试,证明
// 这层是可摘除的。
TEST(PaQuirks, DisablingAdapterRestoresStatusDrivenRetryDecisions) {
    PaEnabledGuard guard(false);
    const auto rate_limit = make_pa_rate_limit_error(400);
    EXPECT_EQ(acecode::pa::classify(rate_limit), acecode::pa::FaultKind::None);
    EXPECT_FALSE(acecode::provider_http_error_is_retryable(400, rate_limit.raw_body));
    EXPECT_FALSE(acecode::provider_error_is_rate_limited(400, rate_limit.raw_body));
    // 429 本身仍算限流:那是 HTTP 协议语义,不是 PA 适配。
    EXPECT_TRUE(acecode::provider_error_is_rate_limited(429, rate_limit.raw_body));

    const auto overflow = make_pa_overflow_error();
    EXPECT_TRUE(acecode::provider_http_error_is_retryable(429, overflow.raw_body));
}

// 触发场景:内网服务端以中文文案 + 非标准 type 报上下文超限。
// 期望行为:PA 层识别为 ContextOverflow。
// 回归背景:通用判定只认英文 needle 与 OpenAI 标准错误码,这条报文两套都不
// 命中 —— 表现为 agent_loop 的三级恢复链(修剪历史重试 → 精简请求档重试)
// 整个不启动,400 直接抛到界面,用户只能手动 /compact 或重开会话。
TEST(PaQuirks, ObservedChineseOverflowPayloadIsClassified) {
    const auto info = make_pa_overflow_error();
    EXPECT_EQ(acecode::pa::classify(info),
              acecode::pa::FaultKind::ContextOverflow);
    EXPECT_TRUE(acecode::pa::is_context_overflow(info));
}

// 触发场景:同一条报文流到通用判定入口。
// 期望行为:通用入口经 PA 兜底后同样判定为溢出 —— 这才是恢复链真正读的那个
// 函数,PA 层自己认得出但没接上等于没修。
TEST(PaQuirks, GenericEntryPointRecognizesPayloadThroughPaFallback) {
    EXPECT_TRUE(acecode::is_context_overflow_error(make_pa_overflow_error()));
}

// 触发场景:关掉 PA 适配(验证「上游修好之后能不能拆」)。
// 期望行为:退回适配前的行为,该报文重新变成认不出的普通 400。
TEST(PaQuirks, DisablingAdapterRestoresPreAdapterBehaviour) {
    PaEnabledGuard guard(false);
    const auto info = make_pa_overflow_error();
    EXPECT_EQ(acecode::pa::classify(info), acecode::pa::FaultKind::None);
    EXPECT_FALSE(acecode::is_context_overflow_error(info));
}

// 触发场景:状态码被中间层改写成 500 / 或干脆不是 HTTP 错误。
// 期望行为:仍然按文案判定。PA 层刻意不做状态码前提 —— 这套服务端的上下文
// 核算本身就是不可信的那一环,再叠一层状态码假设只会多一条漏判路径。
TEST(PaQuirks, ClassificationDoesNotDependOnStatusCode) {
    acecode::ProviderErrorInfo wrapped;
    wrapped.kind = acecode::ProviderErrorKind::Http;
    wrapped.status_code = 500;
    wrapped.display_message = "上游返回:请求上下文过大";
    EXPECT_TRUE(acecode::pa::is_context_overflow(wrapped));
}

// 触发场景:泛化的服务端错误措辞。
// 期望行为:一律不判为溢出。收录标准是「专指这次请求太大」,误判的代价是白跑
// 一次压缩重试并丢掉历史,比漏判贵得多,所以 needle 集宁可窄。
TEST(PaQuirks, GenericChineseErrorsAreNotMisclassified) {
    // 注意「请求失败,请稍后重试」不在此列 —— 它自己写着可以再试,现在归
    // TransientUpstream(见 TransientUpstreamFaultInFourHundredIsRetryable)。
    // 「模型繁忙」是状态描述而非重试指示,仍按 None 处理,交给状态码去判。
    const char* benign[] = {
        "参数错误",
        "模型繁忙",
        "鉴权失败",
        "上下文格式不正确",
        "文件过大",
    };
    for (const char* text : benign) {
        acecode::ProviderErrorInfo info;
        info.kind = acecode::ProviderErrorKind::Http;
        info.status_code = 400;
        info.display_message = text;
        EXPECT_EQ(acecode::pa::classify(info), acecode::pa::FaultKind::None)
            << "泛化措辞被误判成了已知故障:" << text;
    }
}

// 触发场景:用户点了停止。
// 期望行为:不参与适配。用户取消不是服务端故障,把它当溢出会触发一次毫无意义
// 的历史修剪。
TEST(PaQuirks, UserCancellationIsNeverAFault) {
    acecode::ProviderErrorInfo cancelled;
    cancelled.kind = acecode::ProviderErrorKind::UserCancelled;
    cancelled.display_message = "已取消:请求上下文过大";
    EXPECT_EQ(acecode::pa::classify(cancelled), acecode::pa::FaultKind::None);
}

// 触发场景:无错误的空结构 / 空文本。
// 期望行为:安全返回 None,不越界不误判。
TEST(PaQuirks, EmptyInputsAreSafe) {
    EXPECT_EQ(acecode::pa::classify(acecode::ProviderErrorInfo{}),
              acecode::pa::FaultKind::None);
    EXPECT_EQ(acecode::pa::classify_error_text(""),
              acecode::pa::FaultKind::None);
}

// 触发场景:通用英文报文继续流经加了 PA 兜底的入口。
// 期望行为:原有判定一条不变 —— 适配是纯增量,不能改动非 PA 环境的行为。
TEST(PaQuirks, GenericEnglishClassificationIsUnchanged) {
    acecode::ProviderErrorInfo standard;
    standard.kind = acecode::ProviderErrorKind::Http;
    standard.status_code = 400;
    standard.raw_body = R"({"error":{"code":"context_length_exceeded"}})";
    EXPECT_TRUE(acecode::is_context_overflow_error(standard));

    // 上传体积超限不是上下文超限,加了 PA 兜底之后依然不能变成 true。
    acecode::ProviderErrorInfo payload;
    payload.kind = acecode::ProviderErrorKind::Http;
    payload.status_code = 413;
    payload.raw_body =
        R"({"error":{"code":"payload_too_large","message":"upload too large"}})";
    EXPECT_FALSE(acecode::is_context_overflow_error(payload));
}

// 触发场景:网关把一个明确写着「请稍候重试」的瞬时故障塞进 HTTP 400 返回。
// 期望行为:判为 TransientUpstream,并让通用重试策略放行。
// 回归背景:retry_policy 对 4xx 一律 return false(注释写着「客户端错误即使
// 提到 overload 也是终止性的」)。这条规则对守规矩的上游是对的,但这套网关把
// 瞬时故障也编成 400,于是一次本该自动恢复的抽风变成了永久失败。
TEST(PaQuirks, TransientUpstreamFaultInFourHundredIsRetryable) {
    const std::string body =
        R"({"code":400,"message":"LLMRequestError: 模型服务异常，请稍候重试，)"
        R"(如持续出现，请联系技术支持","object":"error","type":"BadRequestError"})";
    EXPECT_EQ(acecode::pa::classify_error_text(body),
              acecode::pa::FaultKind::TransientUpstream);
    EXPECT_TRUE(acecode::pa::is_transient_upstream(body));
    EXPECT_TRUE(acecode::provider_http_error_is_retryable(400, body));

    const std::string gateway_502 =
        R"({"code":502,"message":"LLMRequestError: 网络波动或模型处理超时，请稍候重试"})";
    EXPECT_TRUE(acecode::pa::is_transient_upstream(gateway_502));
}

// 触发场景:额度用完(HTTP 451),报文里也带「重试」二字。
// 期望行为:**不**判为瞬时故障,保持终止。
// 「请切换模型后重试」是要人换模型,不是让程序再发一次 —— 这里重试只会把剩下
// 的配额也烧掉,而且永远不会成功。
TEST(PaQuirks, QuotaExhaustedIsNotTreatedAsTransient) {
    const std::string body =
        R"({"object":"error","message":"当前Agent使用的模型「x」今日额度已用完，)"
        R"(请切换模型后重试","type":"BadRequestError","code":451})";
    EXPECT_EQ(acecode::pa::classify_error_text(body),
              acecode::pa::FaultKind::None);
    EXPECT_FALSE(acecode::provider_http_error_is_retryable(451, body));
}

// 触发场景:上下文超限的报文里也出现了瞬时故障的措辞。
// 期望行为:ContextOverflow 优先。盲目重试同一个超大请求只会再撞一次墙,
// 必须先走压缩;分类顺序在 classify_error_text 里是有意为之的。
TEST(PaQuirks, ContextOverflowWinsOverTransientWording) {
    const std::string body =
        R"({"message":"请求上下文过大，请稍候重试","code":400})";
    EXPECT_EQ(acecode::pa::classify_error_text(body),
              acecode::pa::FaultKind::ContextOverflow);
    EXPECT_FALSE(acecode::pa::is_transient_upstream(body));
}

// 触发场景:关掉 PA 适配总开关后的瞬时故障报文。
// 期望行为:退回通用行为 —— 4xx 仍然终止,证明这层是可摘除的。
TEST(PaQuirks, DisablingAdapterAlsoDisablesTransientRetry) {
    PaEnabledGuard guard(false);
    const std::string body =
        R"({"code":400,"message":"模型服务异常，请稍候重试"})";
    EXPECT_EQ(acecode::pa::classify_error_text(body),
              acecode::pa::FaultKind::None);
    EXPECT_FALSE(acecode::provider_http_error_is_retryable(400, body));
}
