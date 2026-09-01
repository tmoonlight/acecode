#include <gtest/gtest.h>

#include "commands/compact.hpp"
#include "pa/pa_quirks.hpp"

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

} // namespace

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
    const char* benign[] = {
        "参数错误",
        "请求失败,请稍后重试",
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
            << "误判为上下文溢出:" << text;
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
