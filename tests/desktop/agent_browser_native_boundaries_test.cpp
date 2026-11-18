#include "desktop/agent_browser_capture_cdp.hpp"
#include "desktop/agent_browser_proxy_request.hpp"
#include "desktop/agent_browser_state_json.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <vector>

using namespace acecode::desktop;
using nlohmann::json;

namespace {

struct CdpProbe {
    bool current = true;
    std::string failure;
    std::vector<std::string> methods;
    std::vector<json> parameters;
    std::vector<json> responses;
};

browser_capture::Command command_for(const std::shared_ptr<CdpProbe>& probe) {
    return [probe](const std::string& method, const json& params,
                   browser_capture::CommandResult done) {
        probe->methods.push_back(method);
        probe->parameters.push_back(params);
        if (method == probe->failure) {
            done({}, "capture_native_failed");
        } else if (method == "Page.getFrameTree") {
            done({{"frameTree", {{"frame", {{"id", "frame"}, {"loaderId", "document"}}}}}}, {});
        } else if (method == "Page.getLayoutMetrics") {
            done({{"cssContentSize", {{"x", 3}, {"y", 4}, {"width", 2}, {"height", 2000}}}}, {});
        } else if (method == "Page.createIsolatedWorld") {
            done({{"executionContextId", 7}}, {});
        } else if (method == "Runtime.evaluate") {
            done({{"result", {{"value", 2}}}}, {});
        } else {
            done({{"data", "invalid PNG"}}, {});
        }
    };
}

void start_probe(const std::shared_ptr<CdpProbe>& probe, browser_capture::Command command) {
    browser_capture::capture_cdp("page", json::object(), std::move(command),
        [probe] { return probe->current; },
        [probe](json response) { probe->responses.push_back(std::move(response)); });
}

json full_page_request() {
    return {{"page_id", "page"}, {"method", "Page.captureScreenshot"},
        {"params", {{"fullPage", true}}}};
}

}

TEST(AgentBrowserNativeBoundaries, CdpUsesMeasuredCssClipAndDeviceScale) {
    const auto probe = std::make_shared<CdpProbe>();
    start_probe(probe, command_for(probe));
    EXPECT_EQ(probe->methods, (std::vector<std::string>{"Page.getFrameTree",
        "Page.getLayoutMetrics", "Page.createIsolatedWorld", "Runtime.evaluate",
        "Page.captureScreenshot"}));
    ASSERT_EQ(probe->parameters.size(), 5u);
    EXPECT_EQ(probe->parameters[2], (json{{"frameId", "frame"},
        {"worldName", "acecode.full-page-capture"}}));
    EXPECT_EQ(probe->parameters[3], (json{{"expression", "window.devicePixelRatio"},
        {"contextId", 7}, {"returnByValue", true}}));
    EXPECT_EQ(probe->parameters[4], (json{{"format", "png"}, {"fromSurface", true},
        {"captureBeyondViewport", true}, {"clip", {{"x", 3}, {"y", 4},
            {"width", 2}, {"height", 2000}, {"scale", 0.5}}}}));
    ASSERT_EQ(probe->responses.size(), 1u);
    EXPECT_EQ(probe->responses[0]["error"], "capture_invalid_png");
}

TEST(AgentBrowserNativeBoundaries, CdpMeasurementFailureNeverCaptures) {
    const auto probe = std::make_shared<CdpProbe>();
    probe->failure = "Page.getLayoutMetrics";
    start_probe(probe, command_for(probe));
    EXPECT_EQ(probe->methods.size(), 2u);
    ASSERT_EQ(probe->responses.size(), 1u);
    EXPECT_EQ(probe->responses[0]["error"], "capture_native_failed");
}

TEST(AgentBrowserNativeBoundaries, AnnotationMeasurementRejectsInvalidOrReplacedTargets) {
    for (bool valid : {false, true}) {
        bool current = true;
        int measurements = 0;
        std::string error;
        const auto measure = browser_capture::validated_measure(
            [&measurements](browser_capture::Measured done) {
                ++measurements;
                done({}, {});
            }, [&current] { return current; },
            [&current, valid](std::function<void(bool)> done) {
                if (valid) current = false;
                done(valid);
            });
        measure([&error](browser_capture::Bounds, std::string failure) { error = failure; });
        EXPECT_EQ(measurements, 0);
        EXPECT_EQ(error, "annotation_target_or_document_changed");
    }
}

TEST(AgentBrowserNativeBoundaries, AsyncProxyCompletionIsExactlyOnceAndOutsideLock) {
    int completions = 0;
    PendingProxyCall* borrowed = nullptr;
    PendingProxyCall pending([&completions, &borrowed](json response) {
        ++completions;
        EXPECT_TRUE(borrowed->completed());
        EXPECT_TRUE(response.value("ok", false));
    });
    borrowed = &pending;
    pending.finish({{"ok", true}});
    pending.finish({{"ok", false}});
    EXPECT_EQ(completions, 1);
}

TEST(AgentBrowserNativeBoundaries, CdpDelayedMeasurementRejectsReplacedPage) {
    const auto probe = std::make_shared<CdpProbe>();
    const auto delayed = std::make_shared<browser_capture::CommandResult>();
    start_probe(probe, [delayed](const std::string&, const json&,
                                browser_capture::CommandResult done) {
        *delayed = std::move(done);
    });
    EXPECT_TRUE(probe->responses.empty());
    probe->current = false;
    auto done = std::move(*delayed);
    ASSERT_TRUE(done);
    done({{"frameTree", {{"frame", {{"id", "frame"}, {"loaderId", "document"}}}}}}, {});
    ASSERT_EQ(probe->responses.size(), 1u);
    EXPECT_EQ(probe->responses[0]["error"], "capture_document_changed");
}

TEST(AgentBrowserNativeBoundaries, FullPageTimeoutOrStopCompletesAndRejectsLateSuccess) {
    for (bool stopping : {false, true}) {
        PendingProxyCall pending;
        const std::atomic<bool> stopped{stopping};
        const auto expired = std::chrono::steady_clock::now();
        const auto response = pending.wait(full_page_request(), stopped, expired, "native timeout");
        EXPECT_TRUE(pending.completed());
        EXPECT_EQ(response, (json{{"ok", false}, {"page_id", "page"},
            {"error", stopping ? "capture_stopped" : "capture_timeout"}, {"recoverable", true}}));
        pending.finish({{"ok", true}});
        EXPECT_EQ(pending.wait(full_page_request(), stopped, expired, "native timeout"), response);
    }
}

TEST(AgentBrowserNativeBoundaries, ViewportTimeoutPreservesNativeErrorAndLateCompletion) {
    PendingProxyCall pending;
    const std::atomic<bool> stopping{false};
    auto request = full_page_request();
    request["params"] = json::object();
    const auto expired = std::chrono::steady_clock::now();
    EXPECT_EQ(pending.wait(request, stopping, expired, "native timeout")["error"], "native timeout");
    EXPECT_FALSE(pending.completed());
    pending.finish({{"ok", true}});
    pending.finish({{"ok", false}});
    EXPECT_EQ(pending.wait(request, stopping, expired, "native timeout"), (json{{"ok", true}}));
}

TEST(AgentBrowserNativeBoundaries, ProxyValidationPreservesAuthenticationAndPlatformErrors) {
    EXPECT_EQ(browser_proxy_request_error(nullptr, "secret", "invalid method"),
        "proxy request must be an object");
    EXPECT_EQ(browser_proxy_request_error(json::object(), "secret", "invalid method"),
        "Agent Browser proxy authentication failed");
    json request{{"auth_token", "secret"}, {"method", ""}};
    EXPECT_EQ(browser_proxy_request_error(request, "secret", "invalid method"), "invalid method");
    request["operation"] = "create_page";
    EXPECT_TRUE(browser_proxy_request_error(request, "secret", "invalid method").empty());
    request["operation"] = "unknown";
    EXPECT_EQ(browser_proxy_request_error(request, "secret", "invalid method"),
        "Agent Browser proxy operation is invalid");
}

TEST(AgentBrowserNativeBoundaries, SerializationPreservesOptionalPayloadSemantics) {
    AgentBrowserState state;
    state.page_id = "page";
    const auto empty = agent_browser_state_json(state);
    EXPECT_EQ(empty.size(), 21u);
    EXPECT_EQ(empty["page_id"], "page");
    EXPECT_TRUE(empty["owner"].is_null());
    EXPECT_FALSE(empty.contains("selected_element"));
    EXPECT_FALSE(empty.contains("annotations"));
    state.selected_element_json = "[1]";
    state.page_annotations_json = R"({"pending":2})";
    auto result = agent_browser_state_json(state);
    EXPECT_EQ(result["selected_element"], json::array({1}));
    EXPECT_EQ(result["annotations"], (json{{"pending", 2}}));
    state.selected_element_json = "invalid";
    state.page_annotations_json = "[1]";
    result = agent_browser_state_json(state);
    EXPECT_FALSE(result.contains("selected_element"));
    EXPECT_FALSE(result.contains("annotations"));
}
