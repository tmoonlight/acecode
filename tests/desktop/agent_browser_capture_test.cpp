#include "desktop/agent_browser_capture.hpp"
#include "desktop/agent_browser_capture_cdp.hpp"

#include <gtest/gtest.h>
#include <limits>
#include <vector>

using namespace acecode::desktop::browser_capture;
using nlohmann::json;

namespace {

void append_word(std::string& bytes, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) bytes += static_cast<char>(value >> shift);
}

void append_chunk(std::string& bytes, const std::string& type, const std::string& data) {
    append_word(bytes, static_cast<std::uint32_t>(data.size()));
    const auto payload = type + data;
    bytes += payload;
    std::uint32_t crc = 0xffffffffu;
    for (unsigned char byte : payload) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    append_word(bytes, crc ^ 0xffffffffu);
}

std::string png(std::uint32_t width = 2, std::uint32_t height = 2000) {
    std::string bytes("\x89PNG\r\n\x1a\n", 8);
    std::string header;
    append_word(header, width);
    append_word(header, height);
    header += std::string("\x08\x06\0\0\0", 5);
    append_chunk(bytes, "IHDR", header);
    std::string pixels(height * (1 + width * 4), '\0');
    for (std::uint32_t row = 0; row < height; ++row) {
        for (std::uint32_t column = 0; column < width; ++column) {
            const auto offset = row * (1 + width * 4) + 1 + column * 4;
            pixels[offset + (row < height / 2 ? 0 : 1)] = '\xff';
            pixels[offset + 3] = '\xff';
        }
    }
    const auto length = static_cast<std::uint16_t>(pixels.size());
    std::string compressed("\x78\x01\x01", 3);
    compressed += static_cast<char>(length);
    compressed += static_cast<char>(length >> 8);
    compressed += static_cast<char>(~length);
    compressed += static_cast<char>((~length) >> 8);
    compressed += pixels;
    std::uint32_t sum = 1, accum = 0;
    for (unsigned char byte : pixels) {
        sum = (sum + byte) % 65521;
        accum = (accum + sum) % 65521;
    }
    append_word(compressed, (accum << 16) | sum);
    append_chunk(bytes, "IDAT", compressed);
    append_chunk(bytes, "IEND", {});
    return bytes;
}

struct Backend {
    Bounds bounds{0, 0, 2, 2000, "document-a"};
    bool current = true;
    int captures = 0;
    std::vector<json> responses;
    Measured measured;
    Captured captured;

    void start(const std::string& expected = {}) {
        std::make_shared<FullPageCapture>("page-a", expected,
            [this](Measured done) { measured = std::move(done); },
            [this](const Bounds& requested, Captured done) {
                EXPECT_TRUE(same_document_bounds(requested, bounds));
                ++captures;
                captured = std::move(done);
            }, [this] { return current; },
            [this](json response) { responses.push_back(std::move(response)); })->start();
    }

    void measure() { auto done = std::move(measured); ASSERT_TRUE(done); done(bounds, {}); }
    void capture() { auto done = std::move(captured); ASSERT_TRUE(done); done(png(), {}); }
    void expect_error(const std::string& error) {
        ASSERT_EQ(responses.size(), 1u);
        EXPECT_FALSE(responses[0]["ok"].get<bool>());
        EXPECT_TRUE(responses[0]["recoverable"].get<bool>());
        EXPECT_EQ(responses[0]["page_id"], "page-a");
        EXPECT_EQ(responses[0]["error"], error);
        EXPECT_FALSE(responses[0].contains("result"));
    }
};

TEST(AgentBrowserCapture, ViewportDefaultAndExplicitFullPageAreDistinct) {
    bool full = true;
    EXPECT_TRUE(request_mode(json::object(), full).empty());
    EXPECT_FALSE(full);
    for (const auto* key : {"fullPage", "captureBeyondViewport"}) {
        EXPECT_TRUE(request_mode({{key, true}}, full).empty());
        EXPECT_TRUE(full);
        EXPECT_TRUE(request_mode({{key, false}}, full).empty());
        EXPECT_FALSE(full);
    }
    EXPECT_TRUE(request_mode({{"format", "jpeg"}, {"quality", 80}}, full).empty());
}

TEST(AgentBrowserCapture, InvalidRequestsCannotSilentlyBecomeViewportCaptures) {
    bool full = false;
    for (const json& params : std::vector<json>{nullptr, json::array(),
        {{"fullPage", "true"}}, {{"captureBeyondViewport", 1}},
        {{"fullPage", true}, {"captureBeyondViewport", false}},
        {{"fullPage", true}, {"clip", json::object()}},
        {{"fullPage", true}, {"quality", 90}},
        {{"fullPage", true}, {"format", "jpeg"}}, {{"documentId", 5}},
        {{"documentId", ""}}, {{"documentId", std::string(257, 'x')}}}) {
        EXPECT_EQ(request_mode(params, full), "capture_invalid_request") << params;
    }
}

TEST(AgentBrowserCapture, BoundsAreCheckedBeforeNativeAllocation) {
    Bounds bounds{0, 0, 1024, 30000, "doc"};
    EXPECT_TRUE(validate_bounds(bounds).empty());
    bounds.height = 32769;
    EXPECT_EQ(validate_bounds(bounds), "capture_resource_limit");
    bounds.height = 32768;
    bounds.width = 1025;
    EXPECT_EQ(validate_bounds(bounds), "capture_resource_limit");
    bounds.width = 0;
    EXPECT_EQ(validate_bounds(bounds), "capture_invalid_bounds");
    bounds.width = std::numeric_limits<double>::infinity();
    EXPECT_EQ(validate_bounds(bounds), "capture_invalid_bounds");
    bounds.width = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(validate_bounds(bounds), "capture_invalid_bounds");
    bounds.width = 1;
    bounds.device_scale = 0;
    EXPECT_EQ(validate_bounds(bounds), "capture_invalid_bounds");
}

TEST(AgentBrowserCapture, RejectsViewportSubstitutionTruncationCorruptionAndResourceOverflow) {
    const Bounds bounds{0, 0, 2, 2000, "doc"};
    EXPECT_TRUE(validate_png(png(), bounds).empty());
    EXPECT_EQ(validate_png(png(2, 600), bounds), "capture_incomplete_image");
    auto bytes = png();
    bytes.resize(bytes.size() - 1);
    EXPECT_EQ(validate_png(bytes, bounds), "capture_invalid_png");
    bytes = png();
    bytes[45] ^= 1;
    EXPECT_EQ(validate_png(bytes, bounds), "capture_invalid_png");
    EXPECT_EQ(validate_png(std::string(kMaxPngBytes + 1, 'x'), bounds), "capture_resource_limit");
}

TEST(AgentBrowserCapture, TallCaptureReturnsOneBoundImageOnlyAfterPostCaptureVerification) {
    Backend backend;
    backend.start("document-a");
    backend.measure();
    EXPECT_EQ(backend.captures, 1);
    backend.capture();
    EXPECT_TRUE(backend.responses.empty());
    backend.measure();
    ASSERT_EQ(backend.responses.size(), 1u);
    const auto& response = backend.responses[0];
    EXPECT_TRUE(response["ok"].get<bool>());
    EXPECT_EQ(response["result"]["documentId"], "document-a");
    EXPECT_EQ(response["result"]["height"], 2000);
    EXPECT_EQ(response["result"]["fullPage"], true);
    EXPECT_EQ(acecode::base64_decode(response["result"]["data"].get<std::string>()), png());
}

TEST(AgentBrowserCapture, ExpectedDocumentMismatchDoesNotCapture) {
    Backend backend;
    backend.start("previous-document");
    backend.measure();
    EXPECT_EQ(backend.captures, 0);
    backend.expect_error("capture_document_changed");
}

TEST(AgentBrowserCapture, NavigationCloseUnshareOrTimeoutInvalidatesEveryAsyncPhase) {
    for (int phase = 0; phase < 4; ++phase) {
        Backend backend;
        if (phase == 0) backend.current = false;
        backend.start();
        if (phase >= 2) backend.measure();
        if (phase >= 3) backend.capture();
        backend.current = false;
        if (phase == 1 || phase == 3) backend.measure();
        if (phase == 2) backend.capture();
        backend.expect_error("capture_document_changed");
    }
}

TEST(AgentBrowserCapture, DocumentLayoutAndScaleChangesRejectCapturedImage) {
    for (int change = 0; change < 3; ++change) {
        Backend backend;
        backend.start(); backend.measure(); backend.capture();
        if (change == 0) backend.bounds.document = "reloaded";
        if (change == 1) backend.bounds.height += 1;
        if (change == 2) backend.bounds.device_scale = 2;
        backend.measure();
        backend.expect_error("capture_document_changed");
    }
}

TEST(AgentBrowserCapture, NativeFailureCompletesOnceAndIndependentRetryCanSucceed) {
    Backend backend;
    backend.start(); backend.measure();
    auto delayed = std::move(backend.captured);
    delayed({}, "capture_native_failed");
    delayed(png(), {});
    backend.expect_error("capture_native_failed");
    backend.responses.clear();
    backend.start(); backend.measure(); backend.capture(); backend.measure();
    ASSERT_EQ(backend.responses.size(), 1u);
    EXPECT_EQ(backend.responses[0]["ok"], true);
}

TEST(AgentBrowserCapture, MeasurementAndResourceFailuresNeverDispatchCapture) {
    Backend backend;
    backend.start();
    auto measured = std::move(backend.measured);
    measured({}, "capture_measure_failed");
    backend.expect_error("capture_measure_failed");
    backend.responses.clear();
    backend.bounds.height = 40000;
    backend.start(); backend.measure();
    backend.expect_error("capture_resource_limit");
    EXPECT_EQ(backend.captures, 0);
}

TEST(AgentBrowserCapture, CdpPipelineRemeasuresBeforeReturningCompletePng) {
    const auto methods = std::make_shared<std::vector<std::string>>();
    const auto responses = std::make_shared<std::vector<json>>();
    capture_cdp("page-a", json::object(),
        [methods](const std::string& method, const json& params, CommandResult done) {
            methods->push_back(method);
            if (method == "Page.getFrameTree") {
                done({{"frameTree", {{"frame", {{"id", "frame"}, {"loaderId", "document"}}}}}}, {});
            } else if (method == "Page.getLayoutMetrics") {
                done({{"cssContentSize", {{"x", 0}, {"y", 0}, {"width", 2}, {"height", 2000}}}}, {});
            } else if (method == "Page.createIsolatedWorld") {
                done({{"executionContextId", 7}}, {});
            } else if (method == "Runtime.evaluate") {
                const json value = params.value("expression", "") == "window.devicePixelRatio"
                    ? json(1) : json{{"pin", ""}, {"refresh", ""}};
                done({{"result", {{"value", value}}}}, {});
            } else {
                done({{"data", acecode::base64_encode(png())}}, {});
            }
        }, [] { return true; },
        [responses](json response) { responses->push_back(std::move(response)); });
    EXPECT_EQ(*methods, (std::vector<std::string>{"Page.getFrameTree", "Page.getLayoutMetrics",
        "Page.createIsolatedWorld", "Runtime.evaluate", "Page.getFrameTree",
        "Page.createIsolatedWorld", "Runtime.evaluate", "Page.captureScreenshot",
        "Page.getFrameTree", "Page.getLayoutMetrics", "Page.createIsolatedWorld", "Runtime.evaluate"}));
    ASSERT_EQ(responses->size(), 1u);
    EXPECT_EQ((*responses)[0]["ok"], true);
    EXPECT_EQ((*responses)[0]["result"]["data"], acecode::base64_encode(png()));
    EXPECT_EQ((*responses)[0]["result"]["captureBeyondViewport"], true);
}

TEST(AgentBrowserCapture, NativeViewportImageAndPostCaptureMeasurementFailureAreNotSuccess) {
    Backend backend;
    backend.start(); backend.measure();
    auto captured = std::move(backend.captured);
    captured(png(2, 600), {});
    backend.expect_error("capture_incomplete_image");
    backend.responses.clear();
    backend.start(); backend.measure(); backend.capture();
    auto measured = std::move(backend.measured);
    measured({}, "capture_measure_failed");
    backend.expect_error("capture_measure_failed");
}

}
