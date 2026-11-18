#pragma once

#include "utils/base64.hpp"

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace acecode::desktop::browser_capture {

inline constexpr std::uint32_t kMaxDimension = 32768;
inline constexpr std::uint64_t kMaxPixels = 32u * 1024u * 1024u;
inline constexpr std::size_t kMaxPngBytes = 32u * 1024u * 1024u;

struct Bounds {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    std::string document;
    double device_scale = 1;
};

inline std::string request_mode(const nlohmann::json& params, bool& full_page) {
    full_page = false;
    if (!params.is_object()) return "capture_invalid_request";
    for (const auto* key : {"fullPage", "captureBeyondViewport"}) {
        if (params.contains(key) && !params[key].is_boolean()) {
            return "capture_invalid_request";
        }
    }
    if (params.contains("fullPage") && params.contains("captureBeyondViewport") &&
        params["fullPage"] != params["captureBeyondViewport"]) {
        return "capture_invalid_request";
    }
    full_page = params.value("fullPage", params.value("captureBeyondViewport", false));
    if (params.contains("documentId")) {
        if (!params["documentId"].is_string() ||
            params["documentId"].get_ref<const std::string&>().empty() ||
            params["documentId"].get_ref<const std::string&>().size() > 256) {
            return "capture_invalid_request";
        }
    }
    if (full_page && (params.contains("clip") || params.contains("quality") ||
        (params.contains("format") && params["format"] != "png"))) {
        return "capture_invalid_request";
    }
    return {};
}

inline std::string validate_bounds(const Bounds& bounds) {
    if (bounds.document.empty() || !std::isfinite(bounds.x) ||
        !std::isfinite(bounds.y) || !std::isfinite(bounds.width) ||
        !std::isfinite(bounds.height) || bounds.width <= 0 || bounds.height <= 0 ||
        !std::isfinite(bounds.device_scale) || bounds.device_scale <= 0) {
        return "capture_invalid_bounds";
    }
    const double width = std::ceil(bounds.width);
    const double height = std::ceil(bounds.height);
    if (width > kMaxDimension || height > kMaxDimension ||
        width * height > kMaxPixels) return "capture_resource_limit";
    return {};
}

inline bool same_document_bounds(const Bounds& before, const Bounds& after) {
    return before.document == after.document && before.x == after.x &&
        before.y == after.y && before.width == after.width &&
        before.height == after.height && before.device_scale == after.device_scale;
}

inline std::string validate_png(const std::string& bytes, const Bounds& bounds) {
    if (bytes.size() > kMaxPngBytes) return "capture_resource_limit";
    if (bytes.size() < 33 || bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) != 0 ||
        bytes.compare(8, 8, "\0\0\0\x0dIHDR", 8) != 0) {
        return "capture_invalid_png";
    }
    const auto read_dimension = [&bytes](std::size_t offset) {
        std::uint32_t value = 0;
        for (std::size_t index = offset; index < offset + 4; ++index) {
            value = (value << 8) | static_cast<unsigned char>(bytes[index]);
        }
        return value;
    };
    if (read_dimension(16) != std::ceil(bounds.width) ||
        read_dimension(20) != std::ceil(bounds.height)) {
        return "capture_incomplete_image";
    }
    bool has_pixels = false;
    for (std::size_t offset = 8; offset + 12 <= bytes.size();) {
        const auto length = read_dimension(offset);
        if (length > bytes.size() - offset - 12) return "capture_invalid_png";
        std::uint32_t crc = 0xffffffffu;
        for (std::size_t index = offset + 4; index < offset + 8 + length; ++index) {
            crc ^= static_cast<unsigned char>(bytes[index]);
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
            }
        }
        if ((crc ^ 0xffffffffu) != read_dimension(offset + 8 + length)) {
            return "capture_invalid_png";
        }
        if (bytes.compare(offset + 4, 4, "IDAT") == 0 && length) has_pixels = true;
        if (bytes.compare(offset + 4, 4, "IEND") == 0) {
            return has_pixels && length == 0 && offset + 12 == bytes.size()
                ? "" : "capture_invalid_png";
        }
        offset += 12 + length;
    }
    return "capture_invalid_png";
}

using Measured = std::function<void(Bounds, std::string)>;
using Captured = std::function<void(std::string, std::string)>;
using Measure = std::function<void(Measured)>;
using Capture = std::function<void(const Bounds&, Captured)>;
using Current = std::function<bool()>;
using Complete = std::function<void(nlohmann::json)>;
using Validate = std::function<void(std::function<void(bool)>)>;

inline Measure validated_measure(Measure measure, Current current, Validate validate) {
    if (!validate) return measure;
    return [measure, current, validate](Measured done) {
        if (!current()) { done({}, "capture_document_changed"); return; }
        validate([measure, current, done](bool valid) {
            if (!valid || !current()) {
                done({}, "annotation_target_or_document_changed");
                return;
            }
            measure(done);
        });
    };
}

class FullPageCapture : public std::enable_shared_from_this<FullPageCapture> {
public:
    FullPageCapture(std::string page_id, std::string expected_document,
                    Measure measure, Capture capture, Current current,
                    Complete complete)
        : page_id_(std::move(page_id)), expected_document_(std::move(expected_document)),
          measure_(std::move(measure)), capture_(std::move(capture)),
          current_(std::move(current)), complete_(std::move(complete)) {}

    void start() {
        if (!check_current()) return;
        measure_([operation = shared_from_this()](Bounds bounds, std::string error) {
            if (!operation->check_current()) return;
            if (error.empty()) error = validate_bounds(bounds);
            if (error.empty() && !operation->expected_document_.empty() &&
                operation->expected_document_ != bounds.document) {
                error = "capture_document_changed";
            }
            if (!error.empty()) { operation->fail(error); return; }
            operation->bounds_ = bounds;
            operation->capture_(bounds,
                [operation](std::string bytes, std::string capture_error) {
                    if (!operation->check_current()) return;
                    if (capture_error.empty()) {
                        capture_error = validate_png(bytes, operation->bounds_);
                    }
                    if (!capture_error.empty()) { operation->fail(capture_error); return; }
                    operation->verify(std::move(bytes));
                });
        });
    }

private:
    bool check_current() {
        if (finished_) return false;
        if (current_()) return true;
        fail("capture_document_changed");
        return false;
    }

    void fail(const std::string& error) {
        if (finished_) return;
        finished_ = true;
        complete_({{"ok", false}, {"page_id", page_id_},
                   {"error", error}, {"recoverable", true}});
    }

    void verify(std::string bytes) {
        measure_([operation = shared_from_this(), bytes = std::move(bytes)](
                     Bounds after, std::string error) {
            if (!operation->check_current()) return;
            if (error.empty() && !same_document_bounds(operation->bounds_, after)) {
                error = "capture_document_changed";
            }
            if (!error.empty()) { operation->fail(error); return; }
            try {
                nlohmann::json response = {{"ok", true}, {"page_id", operation->page_id_},
                    {"result", {{"data", acecode::base64_encode(bytes)},
                        {"captureBeyondViewport", true}, {"fullPage", true},
                        {"documentId", after.document},
                        {"width", std::ceil(after.width)},
                        {"height", std::ceil(after.height)}}}};
                operation->finished_ = true;
                operation->complete_(std::move(response));
            } catch (const std::bad_alloc&) {
                operation->fail("capture_resource_limit");
            }
        });
    }

    std::string page_id_;
    std::string expected_document_;
    Bounds bounds_;
    Measure measure_;
    Capture capture_;
    Current current_;
    Complete complete_;
    bool finished_ = false;
};

}
