#include "provider/openai_provider.hpp"
#include "provider/openai_responses.hpp"
#include "provider/openai_responses_provider.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using namespace acecode;
using Json = nlohmann::json;

template<class Provider>
class ExposedCacheProvider : public Provider {
public:
    using Provider::Provider;
    using Provider::build_request_body;
    using Provider::remove_rejected_prompt_cache_key;
};

using ChatProvider = ExposedCacheProvider<OpenAiCompatProvider>;
using ResponsesProvider = ExposedCacheProvider<OpenAiResponsesProvider>;

std::vector<ChatMessage> messages() {
    ChatMessage message;
    message.role = "user";
    message.content = "cache test";
    return {message};
}

template<class Provider>
void check_endpoint(const std::string& url, const std::string& mode, bool supported) {
    ProviderRequestOptions options;
    options.endpoint_mode = mode;
    Provider provider(url, "", "aurora", OpenAiConfig::kDefaultStreamTimeoutMs, {}, options);
    ChatRequestOptions call;
    call.prompt_cache_key = "parent-session";
    const auto body = provider.build_request_body(messages(), {}, true, false, &call);
    EXPECT_EQ(body.contains("prompt_cache_key"), supported) << url;
    if (supported) EXPECT_EQ(body["prompt_cache_key"], call.prompt_cache_key) << url;
}

TEST(AceModelPromptCache, CanonicalBaseAndFullUrlsSendSessionKey) {
    for (const auto& base : {"https://ge.bigjuan.xyz/aceapi/v1",
                            "https://GE.BIGJUAN.XYZ:443/aceapi/v1/"}) {
        check_endpoint<ChatProvider>(base, "base_url", true);
        check_endpoint<ResponsesProvider>(base, "base_url", true);
    }
    check_endpoint<ChatProvider>(
        "https://ge.bigjuan.xyz:443/aceapi/v1/chat/completions?test=1", "full_url", true);
    check_endpoint<ResponsesProvider>(
        "https://ge.bigjuan.xyz:443/aceapi/v1/responses?test=1", "full_url", true);
}

TEST(AceModelPromptCache, OtherOriginsAndProtocolPathsKeepLegacyPolicy) {
    for (const auto& base : {"http://ge.bigjuan.xyz/aceapi/v1",
                            "https://ge.bigjuan.xyz:8443/aceapi/v1",
                            "https://ge.bigjuan.xyz.evil.test/aceapi/v1",
                            "https://ge.bigjuan.xyz@evil.test/aceapi/v1",
                            "https://evil.test/ge.bigjuan.xyz/aceapi/v1",
                            "https://ge.bigjuan.xyz/v1",
                            "https://ge.bigjuan.xyz/aceapi/v10",
                            "https://custom.test/aceapi/v1"}) {
        check_endpoint<ChatProvider>(base, "base_url", false);
        check_endpoint<ResponsesProvider>(base, "base_url", false);
    }
    check_endpoint<ChatProvider>(
        "https://ge.bigjuan.xyz/aceapi/v1/responses", "full_url", false);
    check_endpoint<ResponsesProvider>(
        "https://ge.bigjuan.xyz/aceapi/v1/chat/completions", "full_url", false);
    check_endpoint<ResponsesProvider>(
        "https://ge.bigjuan.xyz/aceapi/v1/responses/extra", "full_url", false);
    check_endpoint<ResponsesProvider>(
        "https://api.openai.com/v1/responses", "full_url", true);
    check_endpoint<ChatProvider>(
        "https://api.mistral.ai/v1/chat/completions", "full_url", true);
}

template<class Provider>
void check_session_isolation(bool responses) {
    Provider provider("https://ge.bigjuan.xyz/aceapi/v1", "", "aurora");
    provider.set_prompt_cache_key("legacy-default");
    const auto history = messages();
    for (const auto& session : {"parent-session", "child-session", "parent-session"}) {
        ChatRequestOptions call;
        call.prompt_cache_key = session;
        for (const bool stream : {false, true}) {
            for (const bool compact : {false, true}) {
                auto body = provider.build_request_body(history, {}, stream, compact, &call);
                if (responses) {
                    std::string error;
                    body = build_openai_responses_request(body, &history, &error);
                    ASSERT_TRUE(error.empty()) << error;
                }
                EXPECT_EQ(body["prompt_cache_key"], session);
            }
        }
    }
    EXPECT_EQ(provider.build_request_body(history, {})["prompt_cache_key"], "legacy-default");
    ChatRequestOptions no_session;
    EXPECT_FALSE(provider.build_request_body(history, {}, false, false, &no_session)
                     .contains("prompt_cache_key"));
}

TEST(AceModelPromptCache, ChatCallsKeepParentAndChildKeysIndependent) {
    check_session_isolation<ChatProvider>(false);
}

TEST(AceModelPromptCache, ResponsesWireKeepsParentAndChildKeysIndependent) {
    check_session_isolation<ResponsesProvider>(true);
}

template<class Provider>
void check_rejection_scope() {
    Provider provider("https://ge.bigjuan.xyz/aceapi/v1", "", "aurora");
    provider.set_prompt_cache_key("session");
    auto body = provider.build_request_body(messages(), {});
    const auto original = body;
    EXPECT_FALSE(provider.remove_rejected_prompt_cache_key(body, provider.request_url(), 500,
        R"({"error":{"param":"prompt_cache_key","code":"unsupported_parameter"}})"));
    EXPECT_EQ(body, original);
    EXPECT_TRUE(provider.remove_rejected_prompt_cache_key(body, provider.request_url(), 400,
        R"({"error":{"param":"prompt_cache_key","code":"unsupported_parameter"}})"));
    auto expected = original;
    expected.erase("prompt_cache_key");
    EXPECT_EQ(body, expected);
    EXPECT_FALSE(provider.build_request_body(messages(), {}).contains("prompt_cache_key"));
    provider.set_model("moonlight");
    EXPECT_EQ(provider.build_request_body(messages(), {})["prompt_cache_key"], "session");
    provider.set_model("aurora");
    provider.reconfigure("https://api.openai.com/v1", "");
    EXPECT_EQ(provider.build_request_body(messages(), {})["prompt_cache_key"], "session");
}

TEST(AceModelPromptCache, ChatRejectionIsScopedToEndpointAndModel) {
    check_rejection_scope<ChatProvider>();
}

TEST(AceModelPromptCache, ResponsesRejectionIsScopedToEndpointAndModel) {
    check_rejection_scope<ResponsesProvider>();
}

} // namespace
