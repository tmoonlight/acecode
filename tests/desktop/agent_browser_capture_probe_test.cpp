#include "test_support/desktop/agent_browser_capture_probe_command.hpp"

#include <gtest/gtest.h>

namespace acecode::desktop::capture_probe {
namespace {

struct FakeClient {
    nlohmann::json response;
    std::string native_error;
    std::string method;
    nlohmann::json params;

    nlohmann::json command(const std::string& requested_method,
                           const nlohmann::json& requested_params,
                           std::chrono::milliseconds, std::nullptr_t, std::string& error) {
        method = requested_method;
        params = requested_params;
        error = native_error;
        return response;
    }
};

TEST(AgentBrowserCaptureProbe, NullFullPageFailurePreservesNativeError) {
    FakeClient client{nullptr, "capture_measure_failed"};
    std::string error;
    nlohmann::json result;
    EXPECT_NO_THROW(result = checked_command(client, "Page.captureScreenshot",
        {{"fullPage", true}}, std::chrono::seconds(20), error));
    EXPECT_TRUE(result.is_null());
    EXPECT_EQ(error, "capture_measure_failed");
    EXPECT_EQ(client.method, "Page.captureScreenshot");
    EXPECT_EQ(client.params, nlohmann::json({{"fullPage", true}}));
}

TEST(AgentBrowserCaptureProbe, MalformedCommandResultsFailWithoutThrowing) {
    for (const auto& method : {"Runtime.evaluate", "Page.captureScreenshot"}) {
        for (const auto& response : {nlohmann::json(), nlohmann::json::array(),
                                    nlohmann::json("invalid")}) {
            FakeClient client{response, {}};
            std::string error;
            EXPECT_NO_THROW(checked_command(client, method, nlohmann::json::object(),
                std::chrono::seconds(10), error));
            EXPECT_EQ(error, "native command returned a non-object result");
        }
    }
}

TEST(AgentBrowserCaptureProbe, ObjectCannotMaskNativeFailure) {
    FakeClient client{nlohmann::json::object(), "native transport failed"};
    std::string error;
    EXPECT_TRUE(checked_command(client, "Runtime.evaluate", nlohmann::json::object(),
        std::chrono::seconds(10), error).is_null());
    EXPECT_EQ(error, "native transport failed");
}

TEST(AgentBrowserCaptureProbe, SuccessfulResultIsUnchanged) {
    FakeClient client{{{"result", {{"value", {{"height", 2400}}}}}}, {}};
    std::string error;
    EXPECT_EQ(checked_command(client, "Runtime.evaluate", nlohmann::json::object(),
        std::chrono::seconds(10), error), client.response);
    EXPECT_TRUE(error.empty());
}

}
}
