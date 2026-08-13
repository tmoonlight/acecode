#include <gtest/gtest.h>

#include "web/handlers/grok_auth_handler.hpp"

namespace {

void expect_no_oauth_tokens(const nlohmann::json& value) {
    const std::string wire = value.dump();
    EXPECT_EQ(wire.find("access_token"), std::string::npos);
    EXPECT_EQ(wire.find("refresh_token"), std::string::npos);
    EXPECT_EQ(wire.find("access-secret"), std::string::npos);
    EXPECT_EQ(wire.find("refresh-secret"), std::string::npos);
}

} // namespace

TEST(GrokAuthHandler, StatusContainsNoCredentialOrIdentityFields) {
    const auto status = acecode::web::grok_auth_status_to_json(true);
    EXPECT_EQ(status["provider"], "grok");
    EXPECT_EQ(status["authenticated"], true);
    EXPECT_EQ(status.size(), 2u);
    expect_no_oauth_tokens(status);
}

TEST(GrokAuthHandler, DeviceResponseContainsOnlyPollingContract) {
    acecode::GrokDeviceCodeResponse device;
    device.device_code = "device-secret";
    device.user_code = "ABCD-EFGH";
    device.verification_uri = "https://accounts.x.ai/activate";
    device.verification_uri_complete =
        "https://accounts.x.ai/activate?user_code=ABCD-EFGH";
    device.interval = 7;
    device.expires_in = 600;

    const auto response = acecode::web::grok_device_code_to_json(device, 1000);
    EXPECT_EQ(response["expires_at_unix_ms"], 601000);
    EXPECT_EQ(response["device_code"], "device-secret");
    EXPECT_EQ(response["authenticated"], false);
    expect_no_oauth_tokens(response);
}

TEST(GrokAuthHandler, PollRequestRequiresNonEmptyString) {
    std::string error;
    EXPECT_FALSE(acecode::web::parse_grok_device_poll_request(
        nlohmann::json::object(), error).has_value());
    EXPECT_FALSE(error.empty());

    const auto parsed = acecode::web::parse_grok_device_poll_request(
        nlohmann::json{{"device_code", "device-secret"}}, error);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, "device-secret");
    EXPECT_TRUE(error.empty());
}

TEST(GrokAuthHandler, PollResponseNeverSerializesTokens) {
    acecode::GrokDevicePollResult result;
    result.status = "authorized";
    result.tokens.access_token = "access-secret";
    result.tokens.refresh_token = "refresh-secret";
    result.tokens.email = "private@example.test";
    result.error = "accessToken=error-secret";
    result.message = "upstream abcdefghij.klmnopqrst.uvwxyzABCD";

    const auto response = acecode::web::grok_device_poll_to_json(result, true);
    EXPECT_EQ(response["status"], "authenticated");
    EXPECT_EQ(response["authenticated"], true);
    EXPECT_EQ(response.dump().find("private@example.test"), std::string::npos);
    EXPECT_EQ(response.dump().find("error-secret"), std::string::npos);
    EXPECT_EQ(response.dump().find("abcdefghij.klmnopqrst.uvwxyzABCD"),
              std::string::npos);
    expect_no_oauth_tokens(response);
}
