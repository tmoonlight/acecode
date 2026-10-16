// 覆盖模型采样参数(temperature / top_p / top_k / min_p)从 Provider 配置到
// 请求体的最后一段:OpenAiCompatProvider / AnthropicProvider::build_request_body,
// 以及 Responses 协议的请求转换。
//
// 背景:内网 wizard-ai 网关上的 GLM 模型,平台给出的推荐值是 top_p 0.95 /
// top_k 20 / temperature 1 / min_p 0,同网关的自研 agent 会带上这些值;acecode
// 以前一个都不发,只能吃服务端默认。现在采样参数按模型配置,但没配置时绝不能
// 出现在请求体里 —— 否则所有现有用户的请求都会被悄悄改变。

#include <gtest/gtest.h>

#include "provider/anthropic_provider.hpp"
#include "provider/openai_provider.hpp"
#include "provider/openai_responses.hpp"

#include <string>

namespace {

using acecode::AnthropicProvider;
using acecode::ChatMessage;
using acecode::ModelSamplingOptions;
using acecode::OpenAiCompatProvider;
using acecode::ProviderRequestOptions;

// 把 protected 的 build_request_body 暴露给测试,不发起真实 HTTP 请求。
class TestableOpenAiProvider : public OpenAiCompatProvider {
public:
    using OpenAiCompatProvider::OpenAiCompatProvider;
    using OpenAiCompatProvider::build_request_body;
};

ChatMessage user_message(const std::string& text) {
    ChatMessage msg;
    msg.role = "user";
    msg.content = text;
    return msg;
}

ProviderRequestOptions options_with(const ModelSamplingOptions& sampling) {
    ProviderRequestOptions options;
    options.sampling = sampling;
    return options;
}

ModelSamplingOptions glm_recommended() {
    ModelSamplingOptions sampling;
    sampling.temperature = 1.0;
    sampling.top_p = 0.95;
    sampling.top_k = 20;
    sampling.min_p = 0.0;
    return sampling;
}

} // namespace

TEST(ModelSamplingOverrides, MergesFieldsWithoutLosingZeroOrMutatingDefaults) {
    std::string error;
    const auto defaults = glm_recommended();
    const auto overrides = acecode::parse_model_sampling_overrides(
        nlohmann::json{{"temperature", 0}, {"min_p", 0}, {"top_p", nullptr}}, error);
    ASSERT_TRUE(overrides) << error;
    const auto merged = acecode::merge_model_sampling_options(defaults, *overrides);
    EXPECT_EQ(merged.temperature, 0);
    EXPECT_EQ(merged.min_p, 0);
    EXPECT_EQ(merged.top_p, 0.95);
    EXPECT_EQ(merged.top_k, 20);
    EXPECT_EQ(defaults.temperature, 1);
    EXPECT_TRUE(acecode::parse_model_sampling_overrides(nullptr, error)->empty());
}

TEST(ModelSamplingOverrides, RejectsInvalidAndUnknownFields) {
    for (const auto& value : {
        R"({"temperature":"0.5"})", R"({"temperature":2.1})",
        R"({"top_p":0})", R"({"top_k":1.5})", R"({"top_k":2147483648})",
        R"({"top_k":18446744073709551615})", R"({"min_p":-0.1})",
        R"({"temperatur":0.5})", R"([])"}) {
        std::string error;
        EXPECT_FALSE(acecode::parse_model_sampling_overrides(nlohmann::json::parse(value), error)) << value;
        EXPECT_FALSE(error.empty()) << value;
    }
}

// 触发场景:模型没有配置任何采样参数(所有现有用户的默认状态)。
// 期望:Chat Completions 请求体里一个采样字段都没有,与加这个功能之前一致。
TEST(ModelSamplingRequest, OpenAiOmitsSamplingFieldsWhenUnset) {
    TestableOpenAiProvider provider("https://gateway.example/v1", "sk-test",
                                    "aicoder-pro");
    const auto body = provider.build_request_body({user_message("hi")}, {}, true);
    for (const char* key : {"temperature", "top_p", "top_k", "min_p"}) {
        EXPECT_FALSE(body.contains(key)) << key;
    }
}

// 触发场景:按平台推荐值配置了四个参数(wizard-ai 的 GLM 场景)。
// 期望:四个字段原样出现在顶层;min_p = 0 也要发出去(0 是有效值,不是"未设置")。
TEST(ModelSamplingRequest, OpenAiSendsEveryConfiguredField) {
    TestableOpenAiProvider provider("https://gateway.example/v1", "sk-test",
                                    "aicoder-pro", 120000, {},
                                    options_with(glm_recommended()));
    const auto body = provider.build_request_body({user_message("hi")}, {}, true);
    ASSERT_TRUE(body.contains("temperature"));
    EXPECT_DOUBLE_EQ(body["temperature"].get<double>(), 1.0);
    EXPECT_DOUBLE_EQ(body["top_p"].get<double>(), 0.95);
    EXPECT_EQ(body["top_k"].get<int>(), 20);
    ASSERT_TRUE(body.contains("min_p"));
    EXPECT_DOUBLE_EQ(body["min_p"].get<double>(), 0.0);
}

// 触发场景:只配置了 temperature。
// 期望:只发 temperature,其余三个保持服务端默认(不能补成 0 或别的默认值)。
TEST(ModelSamplingRequest, OpenAiSendsOnlyTheFieldsThatWereSet) {
    ModelSamplingOptions sampling;
    sampling.temperature = 0.3;
    TestableOpenAiProvider provider("https://gateway.example/v1", "sk-test",
                                    "aicoder-pro", 120000, {},
                                    options_with(sampling));
    const auto body = provider.build_request_body({user_message("hi")}, {}, false);
    EXPECT_DOUBLE_EQ(body["temperature"].get<double>(), 0.3);
    EXPECT_FALSE(body.contains("top_p"));
    EXPECT_FALSE(body.contains("top_k"));
    EXPECT_FALSE(body.contains("min_p"));
}

// 触发场景:Responses 协议的模型配置了 temperature / top_p。
// 期望:转换成 Responses 请求后两个字段仍在(配置校验已保证 Responses 不会带 top_k / min_p)。
TEST(ModelSamplingRequest, ResponsesConversionKeepsTemperatureAndTopP) {
    ModelSamplingOptions sampling;
    sampling.temperature = 0.7;
    sampling.top_p = 0.9;
    ProviderRequestOptions options = options_with(sampling);
    options.api_protocol = "responses";
    TestableOpenAiProvider provider("https://gateway.example/v1", "sk-test",
                                    "gpt-test", 120000, {}, options);
    const auto chat_body = provider.build_request_body({user_message("hi")}, {}, true);
    std::string error;
    const auto request = acecode::build_openai_responses_request(chat_body, nullptr, &error);
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_DOUBLE_EQ(request["temperature"].get<double>(), 0.7);
    EXPECT_DOUBLE_EQ(request["top_p"].get<double>(), 0.9);
}

// 触发场景:Anthropic 模型没有配置采样参数,以及配置了 temperature / top_p / top_k。
// 期望:未配置时不发任何采样字段;配置后三个字段出现在 Messages API 请求体顶层。
TEST(ModelSamplingRequest, AnthropicSendsConfiguredFieldsOnly) {
    AnthropicProvider plain("https://api.anthropic.example", "sk-test", "claude-test");
    const auto plain_body = plain.build_request_body({user_message("hi")}, {}, false);
    for (const char* key : {"temperature", "top_p", "top_k", "min_p"}) {
        EXPECT_FALSE(plain_body.contains(key)) << key;
    }

    ModelSamplingOptions sampling;
    sampling.temperature = 0.5;
    sampling.top_p = 0.9;
    sampling.top_k = 40;
    AnthropicProvider tuned("https://api.anthropic.example", "sk-test", "claude-test",
                            120000, {}, options_with(sampling));
    const auto body = tuned.build_request_body({user_message("hi")}, {}, false);
    EXPECT_DOUBLE_EQ(body["temperature"].get<double>(), 0.5);
    EXPECT_DOUBLE_EQ(body["top_p"].get<double>(), 0.9);
    EXPECT_EQ(body["top_k"].get<int>(), 40);
    EXPECT_FALSE(body.contains("min_p"));
}
