#include "agent_browser_capture_cdp.hpp"
#include "agent_browser_capture_layout.hpp"

namespace acecode::desktop::browser_capture {

void capture_cdp(std::string page_id, const nlohmann::json& params,
                 Command command, Current current, Complete complete, Validate validate) {
    const Measure measure = [command, current](Measured done) {
        if (!current()) { done({}, "capture_document_changed"); return; }
        command("Page.getFrameTree", nlohmann::json::object(),
            [command, current, done](nlohmann::json tree, std::string error) {
                if (!current()) { done({}, "capture_document_changed"); return; }
                if (!error.empty()) { done({}, error); return; }
                if (!tree.contains("frameTree") || !tree["frameTree"].is_object() ||
                    !tree["frameTree"].contains("frame") ||
                    !tree["frameTree"]["frame"].is_object()) {
                    done({}, "capture_invalid_bounds"); return;
                }
                const auto frame = tree["frameTree"]["frame"];
                if (!frame.contains("loaderId") || !frame["loaderId"].is_string() ||
                    !frame.contains("id") || !frame["id"].is_string()) {
                    done({}, "capture_invalid_bounds"); return;
                }
                const std::string document = frame.value("loaderId", "");
                command("Page.getLayoutMetrics", nlohmann::json::object(),
                    [command, current, frame, document, done](nlohmann::json metrics, std::string metrics_error) {
                        if (!metrics_error.empty()) { done({}, metrics_error); return; }
                        if (!current()) { done({}, "capture_document_changed"); return; }
                        const auto size = metrics.value("cssContentSize", nlohmann::json::object());
                        if (!size.contains("width") || !size["width"].is_number() ||
                            !size.contains("height") || !size["height"].is_number() ||
                            !size.contains("x") || !size["x"].is_number() ||
                            !size.contains("y") || !size["y"].is_number()) {
                            done({}, "capture_invalid_bounds");
                            return;
                        }
                        const Bounds bounds{size["x"].get<double>(),
                            size["y"].get<double>(), size["width"].get<double>(),
                            size["height"].get<double>(), document};
                        command("Page.createIsolatedWorld",
                            {{"frameId", frame["id"]}, {"worldName", "acecode.full-page-capture"}},
                            [command, current, bounds, done](nlohmann::json world, std::string world_error) {
                                if (!world_error.empty()) { done({}, world_error); return; }
                                if (!current()) { done({}, "capture_document_changed"); return; }
                                if (!world.contains("executionContextId") ||
                                    !world["executionContextId"].is_number_integer()) {
                                    done({}, "capture_measure_failed"); return;
                                }
                                command("Runtime.evaluate",
                                    {{"expression", "window.devicePixelRatio"},
                                     {"contextId", world["executionContextId"]}, {"returnByValue", true}},
                                    [bounds, done](nlohmann::json value, std::string value_error) mutable {
                                        if (!value_error.empty()) { done({}, value_error); return; }
                                        if (!value.contains("result") || !value["result"].is_object() ||
                                            !value["result"].contains("value") ||
                                            !value["result"]["value"].is_number()) {
                                            done({}, "capture_measure_failed"); return;
                                        }
                                        auto measured = bounds;
                                        measured.device_scale = value["result"]["value"].get<double>();
                                        done(std::move(measured), {});
                                    });
                            });
                    });
            });
    };
    const Capture capture = [command, current](const Bounds& bounds, Captured done) {
        if (!current()) { done({}, "capture_document_changed"); return; }
        capture_preserving_layout(command, current,
            {{"format", "png"}, {"fromSurface", true}, {"captureBeyondViewport", true},
             {"clip", {{"x", bounds.x}, {"y", bounds.y},
                       {"width", std::ceil(bounds.width)},
                       {"height", std::ceil(bounds.height)}, {"scale", 1 / bounds.device_scale}}}},
            [done](nlohmann::json result, std::string error) {
                if (!error.empty()) { done({}, error); return; }
                const auto encoded = result.find("data");
                if (encoded == result.end() || !encoded->is_string()) {
                    done({}, "capture_invalid_png"); return;
                }
                if (encoded->get_ref<const std::string&>().size() >
                    ((kMaxPngBytes + 2) / 3) * 4) {
                    done({}, "capture_resource_limit"); return;
                }
                const auto bytes = acecode::base64_decode(encoded->get_ref<const std::string&>());
                done(bytes.value_or(""), bytes ? "" : "capture_invalid_png");
            });
    };
    std::make_shared<FullPageCapture>(std::move(page_id), params.value("documentId", ""),
        validated_measure(measure, current, std::move(validate)), capture,
        current, std::move(complete))->start();
}

}
