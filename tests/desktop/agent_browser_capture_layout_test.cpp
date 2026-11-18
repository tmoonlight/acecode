#include "desktop/agent_browser_capture_layout.hpp"
#include "desktop/page_annotation_requests.hpp"

#include <gtest/gtest.h>

#include <vector>
#include <map>

namespace acecode::desktop::browser_capture {
namespace {

using nlohmann::json;

struct LayoutProbe {
    bool current = true;
    std::string pin = ":root{scrollbar-gutter:stable!important}";
    std::string refresh;
    std::string failure;
    std::string stale;
    std::string delay;
    std::string malformed;
    bool applied = true;
    std::vector<std::string> methods;
    std::vector<json> params;
    CommandResult delayed;
    int completions = 0;
    json result;
    std::string error;
};

void run(const std::shared_ptr<LayoutProbe>& probe, RegisterCleanup cleanup = {}) {
    const Command command = [probe](const std::string& method, const json& params, CommandResult done) {
        probe->methods.push_back(method);
        probe->params.push_back(params);
        if (probe->delay == method) { probe->delayed = std::move(done); return; }
        if (probe->stale == method) probe->current = false;
        if (probe->malformed == method) {
            done(nullptr, {});
        } else if (probe->failure == method ||
            (probe->failure == "clear" && method == "CSS.setStyleSheetText" && params["text"] == "")) {
            done({}, "native_failure");
        } else if (method == "Page.getFrameTree") {
            done({{"frameTree", {{"frame", {{"id", "frame"}}}}}}, {});
        } else if (method == "Page.createIsolatedWorld") {
            done({{"executionContextId", 7}}, {});
        } else if (method == "Runtime.evaluate") {
            const auto expression = params.value("expression", "");
            const json value = expression.find("getComputedStyle(document.documentElement)") == 0
                ? json(probe->applied) : json{{"pin", probe->pin}, {"refresh", probe->refresh}};
            done({{"result", {{"value", value}}}}, {});
        } else if (method == "CSS.createStyleSheet") {
            done({{"styleSheetId", "owned-sheet"}}, {});
        } else if (method == "Page.captureScreenshot") {
            done({{"data", "native PNG"}}, {});
        } else {
            done(json::object(), {});
        }
    };
    capture_preserving_layout(command, [probe] { return probe->current; },
        {{"format", "png"}, {"captureBeyondViewport", true},
         {"clip", {{"x", 0}, {"y", 0}, {"width", 845}, {"height", 2400}, {"scale", 1}}}},
        [probe](json result, std::string error) {
            ++probe->completions;
            probe->result = std::move(result);
            probe->error = std::move(error);
        }, std::move(cleanup));
}

void expect_cleared(const std::shared_ptr<LayoutProbe>& probe) {
    ASSERT_FALSE(probe->methods.empty());
    EXPECT_EQ(probe->methods.back(), "CSS.setStyleSheetText");
    EXPECT_EQ(probe->params.back(), (json{{"styleSheetId", "owned-sheet"}, {"text", ""}}));
    EXPECT_EQ(probe->completions, 1);
}

TEST(AgentBrowserCaptureLayout, OwnsDistinctInspectorSheetAndClearsBeforeCompletion) {
    const auto probe = std::make_shared<LayoutProbe>();
    run(probe);
    EXPECT_EQ(probe->methods, (std::vector<std::string>{"Page.getFrameTree",
        "Page.createIsolatedWorld", "Runtime.evaluate", "DOM.enable", "CSS.enable",
        "CSS.createStyleSheet", "CSS.setStyleSheetText", "Runtime.evaluate", "Page.captureScreenshot",
        "CSS.setStyleSheetText"}));
    EXPECT_EQ(probe->params[5], (json{{"frameId", "frame"}, {"force", true}}));
    EXPECT_EQ(probe->params[6]["text"], probe->pin);
    EXPECT_EQ(probe->params[8]["clip"]["width"], 845);
    EXPECT_EQ(probe->params[8]["clip"]["scale"], 1);
    EXPECT_EQ(probe->result, (json{{"data", "native PNG"}}));
    EXPECT_TRUE(probe->error.empty());
    expect_cleared(probe);
}

TEST(AgentBrowserCaptureLayout, NoClassicGutterDoesNotChangeStylesOrEnableDomains) {
    const auto probe = std::make_shared<LayoutProbe>();
    probe->pin.clear();
    run(probe);
    EXPECT_EQ(probe->methods, (std::vector<std::string>{"Page.getFrameTree",
        "Page.createIsolatedWorld", "Runtime.evaluate", "Page.captureScreenshot"}));
    EXPECT_TRUE(probe->error.empty());
    EXPECT_EQ(probe->completions, 1);
}

TEST(AgentBrowserCaptureLayout, ExistingStableGutterRefreshesAfterCaptureWithoutReplacingGutter) {
    const auto probe = std::make_shared<LayoutProbe>();
    probe->pin.clear();
    probe->refresh = "scroll";
    run(probe);
    EXPECT_EQ(probe->methods[9], "Page.captureScreenshot");
    EXPECT_EQ(probe->params[10]["text"], ":root{overflow-y:scroll!important}");
    EXPECT_EQ(probe->methods[11], "Runtime.evaluate");
    EXPECT_EQ(probe->params[11]["contextId"], 7);
    EXPECT_TRUE(probe->error.empty());
    expect_cleared(probe);
}

TEST(AgentBrowserCaptureLayout, CaptureAndPinErrorsPreserveOriginalFailureAndClearSheet) {
    for (const auto* failure : {"Page.captureScreenshot", "CSS.setStyleSheetText"}) {
        const auto probe = std::make_shared<LayoutProbe>();
        probe->failure = failure;
        run(probe);
        EXPECT_EQ(probe->error, "native_failure");
        expect_cleared(probe);
    }
}

TEST(AgentBrowserCaptureLayout, CancellationDuringSheetCreationStillClearsOwnedSheet) {
    const auto probe = std::make_shared<LayoutProbe>();
    probe->stale = "CSS.createStyleSheet";
    run(probe);
    EXPECT_EQ(probe->error, "capture_document_changed");
    EXPECT_EQ(probe->methods.size(), 7u);
    expect_cleared(probe);
}

TEST(AgentBrowserCaptureLayout, DeferredCaptureOwnsStateAndCleansAfterCancellation) {
    const auto probe = std::make_shared<LayoutProbe>();
    probe->delay = "Page.captureScreenshot";
    run(probe);
    EXPECT_EQ(probe->completions, 0);
    probe->current = false;
    auto delayed = std::move(probe->delayed);
    ASSERT_TRUE(delayed);
    delayed({{"data", "late PNG"}}, {});
    EXPECT_EQ(probe->error, "capture_document_changed");
    expect_cleared(probe);
}

TEST(AgentBrowserCaptureLayout, RegistryRetiresOwnedPinWithoutScreenshotCallbackBeforeRetry) {
    for (const bool deadline : {false, true}) {
        AnnotationRequests requests;
        const auto now = AnnotationRequests::Clock::time_point{};
        std::map<std::string, std::string> sheets;
        std::vector<CommandResult> screenshots;
        int completions = 0;
        int pins = 0;
        const Command command = [&sheets, &screenshots, &pins](const std::string& method,
            const json& params, CommandResult done) {
            if (method == "Page.getFrameTree") {
                done({{"frameTree", {{"frame", {{"id", "frame"}}}}}}, {});
            } else if (method == "Page.createIsolatedWorld") {
                done({{"executionContextId", 7}}, {});
            } else if (method == "Runtime.evaluate") {
                const bool verification = params.at("expression").get<std::string>().find(
                    "getComputedStyle(document.documentElement)") == 0;
                const bool pinned = std::any_of(sheets.begin(), sheets.end(), [](const auto& sheet) {
                    return !sheet.second.empty();
                });
                done({{"result", {{"value", verification ? json(pinned) : json{
                    {"pin", pinned ? "" : ":root{scrollbar-gutter:stable!important}"},
                    {"refresh", pinned ? "scroll" : ""}}}}}}, {});
            } else if (method == "CSS.createStyleSheet") {
                const auto id = "owned-" + std::to_string(sheets.size());
                sheets[id] = "";
                done({{"styleSheetId", id}}, {});
            } else if (method == "CSS.setStyleSheetText") {
                sheets.at(params.at("styleSheetId").get<std::string>()) = params.at("text");
                if (params.at("text") == ":root{scrollbar-gutter:stable!important}") ++pins;
                done(json::object(), {});
            } else if (method == "Page.captureScreenshot") {
                screenshots.push_back(std::move(done));
            } else done(json::object(), {});
        };
        const auto begin = [&requests, now, &completions, &command] {
            const auto pending = requests.add([&completions](json response) {
                ++completions;
                EXPECT_FALSE(response.value("ok", true));
            }, now);
            capture_preserving_layout(command, [pending] { return !pending->completed(); },
                json::object(), [pending](json, std::string error) {
                    pending->finish({{"ok", error.empty()}});
                }, requests.cleanup(pending));
            return pending;
        };
        const auto first = begin();
        ASSERT_EQ(screenshots.size(), 1u);
        ASSERT_FALSE(sheets.at("owned-0").empty());
        if (deadline) requests.expire(now + std::chrono::seconds(16));
        else requests.cancel("Annotation navigation started");
        EXPECT_TRUE(first->completed());
        EXPECT_EQ(completions, 1);
        EXPECT_TRUE(sheets.at("owned-0").empty());
        const auto retry = begin();
        ASSERT_EQ(screenshots.size(), 2u);
        EXPECT_EQ(pins, 2);
        EXPECT_FALSE(sheets.at("owned-1").empty());
        screenshots.front()({{"data", "late old image"}}, {});
        EXPECT_EQ(completions, 1);
        EXPECT_FALSE(retry->completed());
        EXPECT_FALSE(sheets.at("owned-1").empty());
        requests.stop();
        EXPECT_EQ(completions, 2);
        EXPECT_TRUE(sheets.at("owned-1").empty());
        screenshots.back()({{"data", "late retry image"}}, {});
        EXPECT_EQ(completions, 2);
    }
}

TEST(AgentBrowserCaptureLayout, RegistryCancellationRetiresLateCreatedSheetWithoutAdvancingOldCapture) {
    AnnotationRequests requests;
    int completions = 0;
    const auto pending = requests.add([&completions](json) { ++completions; });
    const auto probe = std::make_shared<LayoutProbe>();
    probe->delay = "CSS.createStyleSheet";
    run(probe, requests.cleanup(pending));
    auto late = std::move(probe->delayed);
    ASSERT_TRUE(late);
    requests.cancel("Replaced document");
    EXPECT_EQ(completions, 1);
    EXPECT_EQ(probe->completions, 0);
    late({{"styleSheetId", "retired-sheet"}}, {});
    EXPECT_EQ(probe->params.back(), (json{{"styleSheetId", "retired-sheet"}, {"text", ""}}));
    EXPECT_EQ(probe->methods.back(), "CSS.setStyleSheetText");
    EXPECT_EQ(completions, 1);
    EXPECT_EQ(probe->completions, 0);
    EXPECT_EQ(std::count(probe->methods.begin(), probe->methods.end(), "CSS.setStyleSheetText"), 1);
    EXPECT_EQ(std::count(probe->methods.begin(), probe->methods.end(), "Page.captureScreenshot"), 0);
}

TEST(AgentBrowserCaptureLayout, RegistryDeadlineDuringRefreshClearsWithoutWaitingOrLateRootEvaluation) {
    AnnotationRequests requests;
    const auto now = AnnotationRequests::Clock::time_point{};
    int completions = 0;
    const auto pending = requests.add([&completions](json) { ++completions; }, now);
    const auto probe = std::make_shared<LayoutProbe>();
    probe->pin.clear();
    probe->refresh = "scroll";
    probe->delay = "Page.captureScreenshot";
    run(probe, requests.cleanup(pending));
    auto screenshot = std::move(probe->delayed);
    ASSERT_TRUE(screenshot);
    probe->delay = "CSS.setStyleSheetText";
    screenshot({{"data", "native PNG"}}, {});
    auto refresh = std::move(probe->delayed);
    ASSERT_TRUE(refresh);
    EXPECT_NE(probe->params.back()["text"], "");
    requests.expire(now + std::chrono::seconds(16));
    EXPECT_EQ(completions, 1);
    EXPECT_EQ(probe->params.back(), (json{{"styleSheetId", "owned-sheet"}, {"text", ""}}));
    const auto commands = probe->methods.size();
    refresh(json::object(), {});
    screenshot({{"data", "duplicate PNG"}}, {});
    EXPECT_EQ(probe->methods.size(), commands);
    EXPECT_EQ(probe->completions, 0);
    EXPECT_EQ(completions, 1);
}

TEST(AgentBrowserCaptureLayout, CleanupFailureNeverReportsCaptureSuccess) {
    const auto probe = std::make_shared<LayoutProbe>();
    probe->failure = "clear";
    run(probe);
    EXPECT_EQ(probe->error, "native_failure");
    expect_cleared(probe);
}

TEST(AgentBrowserCaptureLayout, EarlyNativeErrorNeverCapturesOrTouchesStyles) {
    const auto probe = std::make_shared<LayoutProbe>();
    probe->failure = "Page.createIsolatedWorld";
    run(probe);
    EXPECT_EQ(probe->methods.size(), 2u);
    EXPECT_EQ(probe->completions, 1);
    EXPECT_EQ(probe->error, "native_failure");
}

TEST(AgentBrowserCaptureLayout, UnoverridableAuthorStyleFailsBeforeScreenshotAndClearsSheet) {
    for (bool stable : {false, true}) {
        const auto probe = std::make_shared<LayoutProbe>();
        probe->applied = false;
        if (stable) { probe->pin.clear(); probe->refresh = "scroll"; }
        run(probe);
        EXPECT_EQ(probe->error, "capture_layout_unsupported");
        for (const auto& method : probe->methods) EXPECT_NE(method, "Page.captureScreenshot");
        expect_cleared(probe);
    }
}

TEST(AgentBrowserCaptureLayout, MalformedPlanOrSheetNeverDispatchesScreenshot) {
    for (const auto* method : {"Page.getFrameTree", "Page.createIsolatedWorld",
                              "Runtime.evaluate", "CSS.createStyleSheet"}) {
        const auto probe = std::make_shared<LayoutProbe>();
        probe->malformed = method;
        run(probe);
        EXPECT_EQ(probe->error, "capture_layout_failed");
        EXPECT_EQ(probe->completions, 1);
        for (const auto& sent : probe->methods) EXPECT_NE(sent, "Page.captureScreenshot");
    }
}

TEST(AgentBrowserCaptureLayout, StableGutterCancellationStillRefreshesAndClears) {
    const auto probe = std::make_shared<LayoutProbe>();
    probe->pin.clear();
    probe->refresh = "auto";
    probe->stale = "Page.captureScreenshot";
    run(probe);
    EXPECT_EQ(probe->params[10]["text"], ":root{overflow-y:auto!important}");
    EXPECT_EQ(probe->methods[11], "Runtime.evaluate");
    EXPECT_EQ(probe->error, "capture_document_changed");
    expect_cleared(probe);
}

TEST(AgentBrowserCaptureLayout, DeferredRestoreFailureStillClearsAndRetainsCaptureError) {
    for (const auto* failure : {"Runtime.evaluate", "CSS.setStyleSheetText"}) {
        for (bool capture_error : {false, true}) {
            const auto probe = std::make_shared<LayoutProbe>();
            probe->pin.clear();
            probe->refresh = "scroll";
            probe->delay = "Page.captureScreenshot";
            run(probe);
            auto delayed = std::move(probe->delayed);
            ASSERT_TRUE(delayed);
            probe->failure = failure;
            delayed({{"data", "PNG"}}, capture_error ? "screenshot_failure" : "");
            EXPECT_EQ(probe->error, capture_error ? "screenshot_failure" : "native_failure");
            expect_cleared(probe);
        }
    }
}

}
}
