#include <gtest/gtest.h>

#include "config/config.hpp"
#include "tool/image_generate/image_generation_client.hpp"
#include "tool/image_generate/image_generation_policy.hpp"
#include "tool/question_policy.hpp"
#include "utils/encoding.hpp"

using namespace acecode;
using namespace acecode::image_generation;

namespace {

AppConfig config_with_inline_endpoint() {
    AppConfig cfg;
    cfg.image_generation.enabled = true;
    cfg.image_generation.source = "inline";
    cfg.image_generation.base_url = "https://example.invalid/v1";
    cfg.image_generation.api_key = "sk-test";
    return cfg;
}

// 模拟一次用户选择的 AskUserQuestion 响应。
nlohmann::json answer_with(const std::string& selected) {
    return nlohmann::json{
        {"cancelled", false},
        {"timed_out", false},
        {"answers", nlohmann::json::array({
            nlohmann::json{
                {"question_id", kCostQuestionId},
                {"selected", nlohmann::json::array({selected})},
            },
        })},
    };
}

} // namespace

// ── 端点解析 ────────────────────────────────────────────────────────

TEST(ImageGenerationEndpoint, InlineSourceUsesOwnCredentials) {
    // 触发场景:用户在设置里选了「单独配置」并填了 base_url + api_key。
    // 期望行为:直接用本段字段,不去碰 saved_models。
    AppConfig cfg = config_with_inline_endpoint();

    const ResolvedEndpoint endpoint = resolve_endpoint(cfg);

    EXPECT_TRUE(endpoint.ok);
    EXPECT_EQ(endpoint.base_url, "https://example.invalid/v1");
    EXPECT_EQ(endpoint.api_key, "sk-test");
}

TEST(ImageGenerationEndpoint, SavedModelSourceBorrowsCredentials) {
    // 触发场景:同一个网关既提供聊天模型也提供图像模型,用户选了「复用已保存
    // 的模型连接」。期望行为:借用那条 saved_models 的 base_url + api_key,
    // 用户不需要把同一个 key 抄第二遍。
    AppConfig cfg;
    cfg.image_generation.enabled = true;
    cfg.image_generation.source = "saved_model";
    cfg.image_generation.saved_model_name = "gateway";

    ModelProfile profile;
    profile.name = "gateway";
    profile.provider = "openai";
    profile.base_url = "https://gateway.invalid/v1";
    profile.api_key = "sk-shared";
    profile.model = "aurora";
    cfg.saved_models.push_back(profile);

    const ResolvedEndpoint endpoint = resolve_endpoint(cfg);

    EXPECT_TRUE(endpoint.ok);
    EXPECT_EQ(endpoint.base_url, "https://gateway.invalid/v1");
    EXPECT_EQ(endpoint.api_key, "sk-shared");
}

TEST(ImageGenerationEndpoint, MissingSavedModelIsDiagnosableFailure) {
    // 触发场景:引用的 saved_models 条目被改名或删掉了。
    // 期望行为:解析失败并说明原因(调用方据此跳过注册 + 记一条启动日志),
    // 而不是注册一个必然 401 的工具。
    AppConfig cfg;
    cfg.image_generation.enabled = true;
    cfg.image_generation.source = "saved_model";
    cfg.image_generation.saved_model_name = "gone";

    const ResolvedEndpoint endpoint = resolve_endpoint(cfg);

    EXPECT_FALSE(endpoint.ok);
    EXPECT_NE(endpoint.reason.find("gone"), std::string::npos);
}

TEST(ImageGenerationEndpoint, DisabledOrKeylessConfigIsNotUsable) {
    // 期望行为:关掉开关、或没有 key,都解析不出端点。两者都不该注册工具。
    AppConfig disabled = config_with_inline_endpoint();
    disabled.image_generation.enabled = false;
    EXPECT_FALSE(resolve_endpoint(disabled).ok);

    AppConfig keyless = config_with_inline_endpoint();
    keyless.image_generation.api_key.clear();
    EXPECT_FALSE(resolve_endpoint(keyless).ok);
}

TEST(ImageGenerationEndpoint, TrailingSlashIsStripped) {
    // 回归测试:base_url 带尾斜杠时曾拼出 ".../v1//images/generations"。
    // 归一化放在解析这一处做,调用方就不用各自小心。
    AppConfig cfg = config_with_inline_endpoint();
    cfg.image_generation.base_url = "https://example.invalid/v1/";

    EXPECT_EQ(resolve_endpoint(cfg).base_url, "https://example.invalid/v1");
}

// ── 档位映射 ────────────────────────────────────────────────────────

TEST(ImageGenerationQuality, MapsTiersToConfiguredModelNames) {
    // 期望行为:三档各自映射到配置里的模型名。上游对 size/n 参数不生效,
    // 分辨率只能靠模型名选,所以这层映射是唯一的档位表达方式。
    ImageGenerationConfig cfg;
    cfg.model_standard = "img-1k";
    cfg.model_high = "img-2k";
    cfg.model_ultra = "img-4k";

    EXPECT_EQ(model_for_quality(cfg, Quality::Standard), "img-1k");
    EXPECT_EQ(model_for_quality(cfg, Quality::High), "img-2k");
    EXPECT_EQ(model_for_quality(cfg, Quality::Ultra), "img-4k");
}

TEST(ImageGenerationQuality, EmptyConfiguredNameFallsBackToDefault) {
    // 触发场景:用户把某一档的模型名显式清空了。
    // 期望行为:回退到该档的默认模型名,而不是发出一个 model="" 的请求。
    ImageGenerationConfig cfg;
    cfg.model_ultra.clear();

    EXPECT_FALSE(model_for_quality(cfg, Quality::Ultra).empty());
}

TEST(ImageGenerationQuality, UnknownRequestFallsBackToConfiguredDefault) {
    // 触发场景:模型传了个没定义的档位值(或者干脆没传)。
    // 期望行为:用配置的默认档,不报错 —— 档位是可选参数。
    ImageGenerationConfig cfg;
    cfg.default_quality = "high";

    const QualityDecision unknown =
        decide_quality(cfg, "gigantic", false, QuestionPolicy::Ask, true);
    const QualityDecision omitted =
        decide_quality(cfg, "", false, QuestionPolicy::Ask, true);

    EXPECT_EQ(unknown.requested, Quality::High);
    EXPECT_EQ(omitted.requested, Quality::High);
}

// ── 成本确认与无人值守降级 ──────────────────────────────────────────

TEST(ImageGenerationCost, StandardTierDoesNotAskAnything) {
    // 期望行为:便宜档不打扰用户 —— 每次画图都弹一次确认会让功能没法用。
    ImageGenerationConfig cfg;

    const QualityDecision decision =
        decide_quality(cfg, "standard", false, QuestionPolicy::Ask, true);

    EXPECT_FALSE(decision.needs_confirmation);
    EXPECT_EQ(decision.quality, Quality::Standard);
    EXPECT_EQ(decision.downgrade, DowngradeReason::None);
}

TEST(ImageGenerationCost, HighTiersRequestConfirmationWhenSomeoneCanAnswer) {
    // 期望行为:有人值守时高档位要问,而不是直接放行。
    ImageGenerationConfig cfg;

    for (const char* tier : {"high", "ultra"}) {
        const QualityDecision decision =
            decide_quality(cfg, tier, false, QuestionPolicy::Ask, true);
        EXPECT_TRUE(decision.needs_confirmation) << tier;
        EXPECT_EQ(decision.downgrade, DowngradeReason::None) << tier;
    }
}

TEST(ImageGenerationCost, DowngradeOptionIsAlwaysFirst) {
    // 这条是整个成本控制的支点,不是排版偏好:question_policy=timeout 与
    // active goal 的无人值守分支都以「超时自动采纳第一个选项」收尾。降级排在
    // 首位,这些路径不需要任何特判就会收敛到低成本档;顺序一改,超时即变成
    // 自动确认最贵的档位。
    ImageGenerationConfig cfg;

    const nlohmann::json payload =
        build_cost_confirmation_payload(cfg, Quality::Ultra);

    ASSERT_TRUE(payload.is_array());
    ASSERT_EQ(payload.size(), 1u);
    const auto& options = payload[0]["options"];
    ASSERT_TRUE(options.is_array());
    ASSERT_GE(options.size(), 2u);
    EXPECT_NE(options[0]["description"].get<std::string>().find(
                  model_for_quality(cfg, Quality::Standard)),
              std::string::npos);
}

TEST(ImageGenerationCost, HeadlessDowngradesWithoutAsking) {
    // 回归测试:headless(-p)下 AskUserQuestion 会被自动应答成「自行决策并
    // 继续」,等于把花钱的决定权交回模型 —— 而这正是最无人监督的场景。
    // 期望行为:根本不问,直接钉到 standard。
    ImageGenerationConfig cfg;

    const QualityDecision decision =
        decide_quality(cfg, "ultra", /*headless_active=*/true,
                       QuestionPolicy::Ask, /*has_question_channel=*/true);

    EXPECT_FALSE(decision.needs_confirmation);
    EXPECT_EQ(decision.quality, Quality::Standard);
    EXPECT_EQ(decision.requested, Quality::Ultra);
    EXPECT_EQ(decision.downgrade, DowngradeReason::Unattended);
}

TEST(ImageGenerationCost, DenyPolicyDowngradesWithoutAsking) {
    // 同上:question_policy=deny 也是自动应答路径。
    ImageGenerationConfig cfg;

    const QualityDecision decision =
        decide_quality(cfg, "high", false, QuestionPolicy::Deny, true);

    EXPECT_FALSE(decision.needs_confirmation);
    EXPECT_EQ(decision.quality, Quality::Standard);
    EXPECT_EQ(decision.downgrade, DowngradeReason::Unattended);
}

TEST(ImageGenerationCost, TimeoutPolicyStillAsks) {
    // timeout 策略会真的弹出组件,用户在场就能回答 —— 所以它**不**算无人值守。
    // 无人回答的情况由「降级项排第一」兜底,不需要在这里提前降级。
    ImageGenerationConfig cfg;

    const QualityDecision decision =
        decide_quality(cfg, "ultra", false, QuestionPolicy::Timeout, true);

    EXPECT_TRUE(decision.needs_confirmation);
}

TEST(ImageGenerationCost, MissingQuestionChannelDowngrades) {
    // 触发场景:当前 runtime 没有接提问通道。
    // 期望行为:降级而不是放行 —— 问不了就等于没人确认。
    ImageGenerationConfig cfg;

    const QualityDecision decision =
        decide_quality(cfg, "ultra", false, QuestionPolicy::Ask,
                       /*has_question_channel=*/false);

    EXPECT_FALSE(decision.needs_confirmation);
    EXPECT_EQ(decision.quality, Quality::Standard);
    EXPECT_EQ(decision.downgrade, DowngradeReason::QuestionUnavailable);
}

TEST(ImageGenerationCost, ConfirmationFailsSafeToCheaperTier) {
    // 期望行为:只有用户明确选了「继续用高档位」才保留;取消、超时、结构异常
    // 一律按降级处理。成本确认永远 fail 到便宜的一侧。
    ImageGenerationConfig cfg;

    EXPECT_TRUE(confirmation_kept_high_quality(
        answer_with("继续用超高分辨率"), cfg, Quality::Ultra));
    EXPECT_FALSE(confirmation_kept_high_quality(
        answer_with("改用标准分辨率"), cfg, Quality::Ultra));
    EXPECT_FALSE(confirmation_kept_high_quality(
        nlohmann::json{{"cancelled", true}}, cfg, Quality::Ultra));
    EXPECT_FALSE(confirmation_kept_high_quality(
        nlohmann::json{{"timed_out", true}}, cfg, Quality::Ultra));
    EXPECT_FALSE(confirmation_kept_high_quality(
        nlohmann::json::object(), cfg, Quality::Ultra));
}

// ── 响应解析 ────────────────────────────────────────────────────────

TEST(ImageGenerationResponse, ParsesBase64PayloadAndMetadata) {
    // 期望行为:取出 b64_json、尺寸与上游改写后的提示词。
    const std::string body = R"({
        "data": [{
            "b64_json": "aGVsbG8=",
            "width": 1122,
            "height": 1402,
            "revised_prompt": "A small red fox, detailed fur, gentle mist"
        }]
    })";

    const ImageResponse parsed = parse_image_response(200, body, "");

    EXPECT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.b64_data, "aGVsbG8=");
    EXPECT_EQ(parsed.width, 1122);
    EXPECT_EQ(parsed.height, 1402);
    EXPECT_NE(parsed.revised_prompt.find("gentle mist"), std::string::npos);
}

TEST(ImageGenerationResponse, AcceptsDataUrlFromUrlField) {
    // 触发场景:请求带 response_format=url。实测上游回的不是远端链接,而是
    // data URL,所以这条分支也要能吃下来,否则「省流」写法直接失败。
    const std::string body =
        R"({"data":[{"url":"data:image/png;base64,aGVsbG8="}]})";

    const ImageResponse parsed = parse_image_response(200, body, "");

    EXPECT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.b64_data, "aGVsbG8=");
    EXPECT_EQ(parsed.mime_type, "image/png");
}

TEST(ImageGenerationResponse, RemoteUrlIsRejectedWithReason) {
    // 期望行为:真的远端链接当前不支持(下载再落盘是另一条路),要说清楚
    // 而不是静默产出一个空附件。
    const std::string body =
        R"({"data":[{"url":"https://cdn.invalid/a.png"}]})";

    const ImageResponse parsed = parse_image_response(200, body, "");

    EXPECT_FALSE(parsed.ok);
    EXPECT_NE(parsed.error.find("remote image URL"), std::string::npos);
}

TEST(ImageGenerationResponse, EmptyDataIsFailure) {
    // 期望行为:2xx 但没有图片数据也算失败,不能返回一个成功的空结果。
    EXPECT_FALSE(parse_image_response(200, R"({"data":[]})", "").ok);
    EXPECT_FALSE(parse_image_response(200, R"({"ok":true})", "").ok);
}

TEST(ImageGenerationResponse, QuotaFailuresAreDistinguishable) {
    // 期望行为:额度/限流类失败要能和网络失败区分开 —— 否则用户会去排查代理
    // 而不是去看余额。
    const ImageResponse rate_limited = parse_image_response(
        429, R"({"error":{"message":"Rate limit exceeded"}})", "");
    EXPECT_FALSE(rate_limited.ok);
    EXPECT_TRUE(rate_limited.quota_error);

    const ImageResponse server_error =
        parse_image_response(500, R"({"error":{"message":"boom"}})", "");
    EXPECT_FALSE(server_error.ok);
    EXPECT_FALSE(server_error.quota_error);
}

TEST(ImageGenerationResponse, TransportFailureKeepsOriginalMessage) {
    // 触发场景:连接层就失败了(status 为 0),比如代理不通。
    // 期望行为:把 cpr 的错误原文带出来,别用一句泛化文案盖掉线索。
    const ImageResponse parsed =
        parse_image_response(0, "", "Could not resolve host");

    EXPECT_FALSE(parsed.ok);
    EXPECT_EQ(parsed.error, "Could not resolve host");
}

TEST(ImageGenerationResponse, SuccessStatusWithEmbeddedErrorIsFailure) {
    // 触发场景:部分网关在 2xx 里塞 error 对象。
    // 期望行为:按失败处理,不要把错误当成图片数据缺失来报。
    const ImageResponse parsed = parse_image_response(
        200, R"({"error":{"message":"insufficient credits"}})", "");

    EXPECT_FALSE(parsed.ok);
    EXPECT_TRUE(parsed.quota_error);
    EXPECT_NE(parsed.error.find("insufficient"), std::string::npos);
}

// ── 改写提示词截断 ──────────────────────────────────

// 判定一段字节是否为完整合法的 UTF-8(不允许尾部有截断的多字节序列)。
bool is_complete_utf8(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char b = static_cast<unsigned char>(text[i]);
        size_t len = 0;
        if (b < 0x80) len = 1;
        else if ((b & 0xE0) == 0xC0) len = 2;
        else if ((b & 0xF0) == 0xE0) len = 3;
        else if ((b & 0xF8) == 0xF0) len = 4;
        else return false;  // 孤立的续位或非法首字节
        if (i + len > text.size()) return false;  // 尾部序列被截断
        for (size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
        }
        i += len;
    }
    return true;
}

TEST(ImageGenerationRevisedPrompt, TruncationKeepsUtf8Boundaries) {
    // 上游会大幅扩写提示词,模型可见文本只放截断版。中文提示词
    // 每字 3 字节,按字节硬切会切出半个字 —— 那段碎码会直接进模型
    // 上下文与 session JSONL。期望行为:任何截断长度下结果都是完整 UTF-8。
    const std::string wide = "一二三四五六七八九十";
    ASSERT_EQ(wide.size(), 30u) << "源文件必须以 UTF-8 编译(10 个汉字 × 3 字节)";
    ASSERT_TRUE(is_complete_utf8(wide));

    for (size_t limit = 1; limit <= 40; ++limit) {
        const std::string cut = truncate_utf8_prefix(wide, limit);
        EXPECT_TRUE(is_complete_utf8(cut))
            << "limit=" << limit << " produced a split multi-byte character";
        EXPECT_LE(cut.size(), limit) << "limit=" << limit;
    }
}
