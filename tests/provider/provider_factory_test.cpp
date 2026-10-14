#include <gtest/gtest.h>

#include "config/config.hpp"
#include "provider/provider_factory.hpp"
#include "provider/openai_responses_provider.hpp"

#include <httplib.h>

#include <chrono>
#include <functional>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

using acecode::ModelProfile;
using acecode::create_provider_from_entry;

namespace {

using namespace std::chrono_literals;

struct LocalHttpServer {
    httplib::Server svr;
    int port = 0;
    std::thread th;

    explicit LocalHttpServer(std::function<void(httplib::Server&)> setup) {
        setup(svr);
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 50 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(10ms);
        }
    }

    ~LocalHttpServer() {
        svr.stop();
        if (th.joinable()) th.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

acecode::ChatMessage user_message() {
    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content = "hi";
    return msg;
}

ModelProfile openai_profile(const std::string& base_url) {
    ModelProfile profile;
    profile.name = "gateway";
    profile.provider = "openai";
    profile.base_url = base_url;
    profile.api_key = "sk-test";
    profile.model = "test-model";
    return profile;
}

acecode::ProviderConstructionFingerprint fingerprint_for(
    const ModelProfile& profile,
    const acecode::AppConfig* config = nullptr) {
    auto prepared = acecode::prepare_provider_construction(profile, config);
    if (!prepared.has_value()) {
        throw std::runtime_error("expected valid provider construction plan");
    }
    return prepared->fingerprint();
}

template <typename T, typename = void>
struct IsStreamInsertable : std::false_type {};

template <typename T>
struct IsStreamInsertable<T, std::void_t<decltype(
    std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

} // namespace

TEST(ProviderFactory, EmptyProviderReturnsNull) {
    ModelProfile profile;
    profile.name = "";
    profile.provider = "";
    profile.model = "";

    EXPECT_EQ(create_provider_from_entry(profile), nullptr);
}

TEST(ProviderFactory, OpenAiProfileRequestHeadersReachProvider) {
    std::mutex mu;
    std::string seen_header;

    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard<std::mutex> lk(mu);
                seen_header = req.get_header_value("X-Factory");
            }
            res.set_content(R"({"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}]})",
                            "application/json");
        });
    });

    auto profile = openai_profile(server.base_url());
    profile.request_headers = {{"X-Factory", "profile"}};
    auto provider = create_provider_from_entry(profile);
    ASSERT_TRUE(provider);

    auto response = provider->chat({user_message()}, {});
    EXPECT_EQ(response.content, "ok");

    std::lock_guard<std::mutex> lk(mu);
    EXPECT_EQ(seen_header, "profile");
}

TEST(ProviderFactory, OpenAiProfileInheritsGlobalRequestHeadersWhenEntryOmitsThem) {
    std::mutex mu;
    std::string seen_header;

    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard<std::mutex> lk(mu);
                seen_header = req.get_header_value("X-Global");
            }
            res.set_content(R"({"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}]})",
                            "application/json");
        });
    });

    acecode::AppConfig cfg;
    cfg.openai.request_headers = {{"X-Global", "fallback"}};
    auto profile = openai_profile(server.base_url());
    auto provider = create_provider_from_entry(profile, &cfg);
    ASSERT_TRUE(provider);

    auto response = provider->chat({user_message()}, {});
    EXPECT_EQ(response.content, "ok");

    std::lock_guard<std::mutex> lk(mu);
    EXPECT_EQ(seen_header, "fallback");
}

TEST(ProviderFactory, UnknownProviderReturnsNull) {
    ModelProfile profile;
    profile.name = "unknown";
    profile.provider = "unknown";
    profile.model = "model";

    EXPECT_EQ(create_provider_from_entry(profile), nullptr);
}

TEST(ProviderFactory, AnthropicProfileRequestHeadersReachProvider) {
    std::mutex mu;
    std::string seen_api_key;
    std::string seen_header;

    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/messages", [&](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard<std::mutex> lk(mu);
                seen_api_key = req.get_header_value("x-api-key");
                seen_header = req.get_header_value("X-Factory");
            }
            res.set_content(
                R"({"id":"msg_1","type":"message","role":"assistant","model":"claude-test","stop_reason":"end_turn","content":[{"type":"text","text":"ok"}]})",
                "application/json");
        });
    });

    ModelProfile profile;
    profile.name = "claude";
    profile.provider = "anthropic";
    profile.base_url = server.base_url();
    profile.api_key = "sk-ant-test";
    profile.model = "claude-test";
    profile.request_headers = {{"X-Factory", "profile"}};

    auto provider = create_provider_from_entry(profile);
    ASSERT_TRUE(provider);
    EXPECT_EQ(provider->name(), "anthropic");

    auto response = provider->chat({user_message()}, {});
    EXPECT_EQ(response.content, "ok");

    std::lock_guard<std::mutex> lk(mu);
    EXPECT_EQ(seen_api_key, "sk-ant-test");
    EXPECT_EQ(seen_header, "profile");
}

// 触发场景:任一实际参与 Provider 构造的连接/请求字段变化;期望私有指纹变化。
// 旧缺陷只刷新 context_window,同名历史会话继续持有旧 URL、密钥和请求参数。
TEST(ProviderFactory, FingerprintCoversEveryEffectiveConstructionField) {
    acecode::AppConfig cfg;
    auto base = openai_profile("https://gateway.example/v1");
    base.stream_timeout_ms = 41000;
    base.request_headers = {{"Authorization", "Bearer old"}};
    base.endpoint_mode = "base_url";
    base.max_output_tokens = 8192;
    acecode::ModelReasoningOptions reasoning;
    reasoning.supported = true;
    reasoning.mandatory = false;
    reasoning.default_enabled = true;
    reasoning.enabled = true;
    reasoning.supported_efforts = {"low", "high"};
    reasoning.default_effort = "low";
    reasoning.effort = "high";
    reasoning.supports_max_tokens = true;
    reasoning.max_tokens = 4096;
    base.reasoning = reasoning;
    const auto expected = fingerprint_for(base, &cfg);

    const std::vector<std::function<void(ModelProfile&)>> mutations = {
        [](ModelProfile& p) { p.provider = "anthropic"; },
        [](ModelProfile& p) { p.base_url = "https://other.example/v1"; },
        [](ModelProfile& p) { p.api_key = "different-secret"; },
        [](ModelProfile& p) { p.model = "other-model"; },
        [](ModelProfile& p) { p.stream_timeout_ms = 42000; },
        [](ModelProfile& p) { p.request_headers["Authorization"] = "Bearer new"; },
        [](ModelProfile& p) { p.endpoint_mode = "full_url"; },
        [](ModelProfile& p) { p.api_protocol = "responses"; },
        [](ModelProfile& p) { p.max_output_tokens = 8193; },
        [](ModelProfile& p) { p.reasoning->supported = false; },
        [](ModelProfile& p) { p.reasoning->mandatory = true; },
        [](ModelProfile& p) { p.reasoning->default_enabled = false; },
        [](ModelProfile& p) { p.reasoning->enabled = false; },
        [](ModelProfile& p) { p.reasoning->supported_efforts.push_back("medium"); },
        [](ModelProfile& p) { p.reasoning->default_effort = "high"; },
        [](ModelProfile& p) { p.reasoning->effort = "low"; },
        [](ModelProfile& p) { p.reasoning->supports_max_tokens = false; },
        [](ModelProfile& p) { p.reasoning->max_tokens = 4097; },
    };
    for (const auto& mutate : mutations) {
        auto changed = base;
        mutate(changed);
        EXPECT_NE(fingerprint_for(changed, &cfg), expected);
    }
}

// 触发场景:profile 省略 timeout/header,全局 OpenAI fallback 变化;期望指纹
// 跟随工厂的最终输入。显式 profile 值存在时,无关全局值不能触发重建。
TEST(ProviderFactory, FingerprintUsesExactConfigFallbacks) {
    auto inherited = openai_profile("https://gateway.example/v1");
    acecode::AppConfig first;
    first.openai.stream_timeout_ms = 31000;
    first.openai.request_headers = {{"X-Route", "one"}};
    acecode::AppConfig second = first;
    second.openai.stream_timeout_ms = 32000;
    EXPECT_NE(fingerprint_for(inherited, &first),
              fingerprint_for(inherited, &second));
    second = first;
    second.openai.request_headers["X-Route"] = "two";
    EXPECT_NE(fingerprint_for(inherited, &first),
              fingerprint_for(inherited, &second));

    auto explicit_values = inherited;
    explicit_values.stream_timeout_ms = 90000;
    explicit_values.request_headers = {{"X-Route", "profile"}};
    EXPECT_EQ(fingerprint_for(explicit_values, &first),
              fingerprint_for(explicit_values, &second));
}

// 触发场景:OpenAI-compatible reasoning 目标从普通协议切到 OpenRouter;
// 期望即使其它 reasoning 字段相同也重建,避免历史会话沿用旧 wire shape。
TEST(ProviderFactory, FingerprintCoversDerivedReasoningWireProtocol) {
    auto profile = openai_profile("https://gateway.example/v1");
    acecode::ModelReasoningOptions reasoning;
    reasoning.supported = true;
    reasoning.enabled = true;
    profile.reasoning = reasoning;
    profile.models_dev_provider_id = "other";
    const auto ordinary = fingerprint_for(profile);
    profile.models_dev_provider_id = "OpenRouter";
    EXPECT_NE(fingerprint_for(profile), ordinary);
}

TEST(ProviderFactory, ResponsesProtocolSelectsAdapterAndLegacyRemainsChat) {
    auto profile = openai_profile("https://api.example/v1/");
    const auto legacy_fingerprint = fingerprint_for(profile);
    auto legacy = create_provider_from_entry(profile);
    ASSERT_TRUE(legacy);
    EXPECT_FALSE(std::dynamic_pointer_cast<acecode::OpenAiResponsesProvider>(legacy));

    profile.api_protocol = "chat_completions";
    EXPECT_EQ(fingerprint_for(profile), legacy_fingerprint);
    profile.api_protocol = "responses";
    EXPECT_NE(fingerprint_for(profile), legacy_fingerprint);
    auto provider = create_provider_from_entry(profile);
    auto responses = std::dynamic_pointer_cast<acecode::OpenAiResponsesProvider>(provider);
    ASSERT_TRUE(responses);
    EXPECT_EQ(responses->name(), "openai");
    EXPECT_EQ(responses->request_url(), "https://api.example/v1/responses");

    profile.endpoint_mode = "full_url";
    profile.base_url = "https://api.example/custom/responses?version=1";
    responses = std::dynamic_pointer_cast<acecode::OpenAiResponsesProvider>(
        create_provider_from_entry(profile));
    ASSERT_TRUE(responses);
    EXPECT_EQ(responses->request_url(), profile.base_url);
}

TEST(ProviderFactory, RejectsInvalidOrNonOpenAiProtocolEvenWithoutConfigValidation) {
    auto profile = openai_profile("https://api.example/v1");
    profile.api_protocol = "unknown";
    EXPECT_FALSE(acecode::prepare_provider_construction(profile).has_value());
    profile.api_protocol = "responses";
    for (const auto* provider : {"anthropic", "copilot", "grok", "codex"}) {
        profile.provider = provider;
        EXPECT_FALSE(acecode::prepare_provider_construction(profile).has_value());
    }
}

// 触发场景:仅重命名或修改 context_window;期望 Provider 指纹保持相同。
// 这些是状态/元数据字段,旧实现若混入比较会做无意义的连接重建。
TEST(ProviderFactory, RenameAndContextOnlyChangesAreFingerprintEquivalent) {
    auto profile = openai_profile("https://gateway.example/v1");
    acecode::AppConfig cfg;
    const auto original = fingerprint_for(profile, &cfg);
    profile.name = "renamed";
    profile.context_window = 64000;
    EXPECT_EQ(fingerprint_for(profile, &cfg), original);
}

// 触发场景:当前非视觉模型自身 vision gate 或全局可用视觉模型发生变化;
// 期望两项派生路由输入都进入指纹。旧 Provider 会缓存附件 fallback 文案。
TEST(ProviderFactory, FingerprintCoversBothVisionRoutingInputs) {
    acecode::AppConfig cfg;
    auto selected = openai_profile("https://gateway.example/v1");
    cfg.saved_models = {selected};
    const auto no_vision = fingerprint_for(selected, &cfg);

    auto selected_with_vision = selected;
    selected_with_vision.capabilities = {"vision"};
    acecode::AppConfig selected_cfg = cfg;
    selected_cfg.saved_models = {selected_with_vision};
    EXPECT_NE(fingerprint_for(selected_with_vision, &selected_cfg), no_vision);

    auto another = selected;
    another.name = "vision-peer";
    another.model = "vision-model";
    another.capabilities = {"vision"};
    acecode::AppConfig global_vision = cfg;
    global_vision.saved_models.push_back(another);
    EXPECT_NE(fingerprint_for(selected, &global_vision), no_vision);
}

// 触发场景:凭据和 Authorization header 进入构造计划;期望公开类型只能比较,
// 不能转换为字符串或写入 stream。旧式 raw snapshot 容易被日志/JSON 泄露。
TEST(ProviderFactory, FingerprintPublicSurfaceCannotExposeRawSecrets) {
    static_assert(!std::is_constructible_v<
        std::string, acecode::ProviderConstructionFingerprint>);
    static_assert(!IsStreamInsertable<
        acecode::ProviderConstructionFingerprint>::value);

    auto profile = openai_profile("https://gateway.example/v1");
    profile.api_key = "private-api-key-sentinel";
    profile.request_headers = {
        {"Authorization", "Bearer private-header-sentinel"},
    };
    auto prepared = acecode::prepare_provider_construction(profile);
    ASSERT_TRUE(prepared.has_value());
    const auto built = prepared->construct();
    ASSERT_TRUE(built.provider);
    EXPECT_EQ(built.provider->model(), "test-model");
}

// 触发场景:在设置里改了某个采样参数(包括从"未设置"改成 0、或只改小数点后
// 很远的一位),运行中的会话要靠指纹变化才会重建 Provider。
// 期望:每个字段的增删改都改变指纹;0.95 与 0.9500000001 也必须区分(按位编码,
// 不能先格式化成文本);采样参数原样进入 Provider 的请求选项。
TEST(ProviderFactory, FingerprintCoversEverySamplingField) {
    auto base = openai_profile("https://gateway.example/v1");
    base.sampling.temperature = 1.0;
    base.sampling.top_p = 0.95;
    base.sampling.top_k = 20;
    base.sampling.min_p = 0.0;
    const auto expected = fingerprint_for(base);

    const std::vector<std::function<void(ModelProfile&)>> mutations = {
        [](ModelProfile& p) { p.sampling.temperature = 0.99; },
        [](ModelProfile& p) { p.sampling.temperature.reset(); },
        [](ModelProfile& p) { p.sampling.top_p = 0.9500000001; },
        [](ModelProfile& p) { p.sampling.top_p.reset(); },
        [](ModelProfile& p) { p.sampling.top_k = 21; },
        [](ModelProfile& p) { p.sampling.top_k.reset(); },
        [](ModelProfile& p) { p.sampling.min_p = 0.01; },
        [](ModelProfile& p) { p.sampling.min_p.reset(); },
    };
    for (const auto& mutate : mutations) {
        auto changed = base;
        mutate(changed);
        EXPECT_NE(fingerprint_for(changed), expected);
    }
    EXPECT_EQ(fingerprint_for(base), expected);

    const auto provider = create_provider_from_entry(base);
    const auto compat = std::dynamic_pointer_cast<acecode::OpenAiCompatProvider>(provider);
    ASSERT_TRUE(compat);
    EXPECT_EQ(compat->request_options().sampling.top_k, 20);
    EXPECT_EQ(compat->request_options().sampling.min_p, 0.0);
}

// 触发场景:绕过配置校验(例如旧数据或直接构造)给托管 provider 塞了采样参数。
// 期望:工厂拒绝构造,与托管 provider 的其它自定义项一致,不会带着用户以为
// 生效、其实被托管协议忽略的参数运行。
TEST(ProviderFactory, ManagedProvidersRefuseSampling) {
    for (const auto* managed : {"copilot", "grok"}) {
        ModelProfile profile;
        profile.name = managed;
        profile.provider = managed;
        profile.model = "model-id";
        profile.sampling.temperature = 0.5;
        EXPECT_FALSE(acecode::prepare_provider_construction(profile).has_value())
            << managed;
    }
}
