#include "agent_browser_capture_layout.hpp"

namespace acecode::desktop::browser_capture {
namespace {

constexpr const char* kGutterPlan = R"JS((() => {
    const root = document.documentElement;
    if (!root) return null;
    const style = getComputedStyle(root);
    const classic = innerWidth > root.clientWidth;
    const stable = style.scrollbarGutter.startsWith('stable');
    return {
        pin: classic && !stable ? ':root{scrollbar-gutter:stable!important}' : '',
        refresh: classic && stable && root.scrollHeight > root.clientHeight
            ? (style.overflowY === 'scroll' ? 'auto' : 'scroll') : ''
    };
})())JS";

class LayoutCapture : public std::enable_shared_from_this<LayoutCapture> {
public:
    LayoutCapture(Command command, Current current, nlohmann::json params, CommandResult complete)
        : command_(std::move(command)), current_(std::move(current)),
          params_(std::move(params)), complete_(std::move(complete)) {}

    void start() { send(Stage::frame, "Page.getFrameTree", nlohmann::json::object()); }

private:
    enum class Stage { frame, world, plan, dom, css, sheet, pin, verify_pin,
                       probe_refresh, verify_refresh, prepared, capture };

    void send(Stage stage, const std::string& method, const nlohmann::json& params) {
        if (!current_()) { restore({}, "capture_document_changed"); return; }
        command_(method, params,
            [operation = shared_from_this(), stage](nlohmann::json result, std::string error) {
                operation->received(stage, std::move(result), std::move(error));
            });
    }

    void received(Stage stage, nlohmann::json result, std::string error) {
        if (stage == Stage::sheet && result.is_object() && result.contains("styleSheetId") &&
            result["styleSheetId"].is_string()) sheet_ = result["styleSheetId"].get<std::string>();
        if (stage == Stage::capture) captured_ = true;
        if (!error.empty()) { restore({}, std::move(error)); return; }
        if (!current_()) { restore({}, "capture_document_changed"); return; }
        if (!result.is_object()) { restore({}, "capture_layout_failed"); return; }
        switch (stage) {
        case Stage::frame:
            if (!result.contains("frameTree") || !result["frameTree"].is_object() ||
                !result["frameTree"].contains("frame") || !result["frameTree"]["frame"].is_object() ||
                !result["frameTree"]["frame"].contains("id") ||
                !result["frameTree"]["frame"]["id"].is_string()) break;
            frame_ = result["frameTree"]["frame"]["id"].get<std::string>();
            send(Stage::world, "Page.createIsolatedWorld",
                {{"frameId", frame_}, {"worldName", "acecode.full-page-capture"}});
            return;
        case Stage::world:
            if (!result.contains("executionContextId") || !result["executionContextId"].is_number_integer()) break;
            context_ = result["executionContextId"];
            send(Stage::plan, "Runtime.evaluate", {{"expression", kGutterPlan},
                {"contextId", context_}, {"returnByValue", true}});
            return;
        case Stage::plan:
            if (!result.contains("result") || !result["result"].is_object() ||
                !result["result"].contains("value") || !result["result"]["value"].is_object()) break;
            result = result["result"]["value"].get<nlohmann::json>();
            if (!result.contains("pin") || !result["pin"].is_string() ||
                !result.contains("refresh") || !result["refresh"].is_string()) break;
            pin_ = result["pin"].get<std::string>();
            refresh_ = result["refresh"].get<std::string>();
            if (!refresh_.empty() && refresh_ != "auto" && refresh_ != "scroll") break;
            if (pin_.empty() && refresh_.empty()) take();
            else send(Stage::dom, "DOM.enable", nlohmann::json::object());
            return;
        case Stage::dom:
            send(Stage::css, "CSS.enable", nlohmann::json::object());
            return;
        case Stage::css:
            send(Stage::sheet, "CSS.createStyleSheet", {{"frameId", frame_}, {"force", true}});
            return;
        case Stage::sheet:
            if (sheet_.empty()) break;
            if (pin_.empty()) send(Stage::probe_refresh, "CSS.setStyleSheetText",
                {{"styleSheetId", sheet_}, {"text", refresh_rule()}});
            else send(Stage::pin, "CSS.setStyleSheetText", {{"styleSheetId", sheet_}, {"text", pin_}});
            return;
        case Stage::pin:
            send(Stage::verify_pin, "Runtime.evaluate",
                {{"expression", "getComputedStyle(document.documentElement).scrollbarGutter.startsWith('stable')"},
                 {"contextId", context_}, {"returnByValue", true}});
            return;
        case Stage::probe_refresh:
            send(Stage::verify_refresh, "Runtime.evaluate",
                {{"expression", "getComputedStyle(document.documentElement).overflowY === '" + refresh_ + "'"},
                 {"contextId", context_}, {"returnByValue", true}});
            return;
        case Stage::verify_pin:
        case Stage::verify_refresh:
            if (!result.contains("result") || !result["result"].is_object() ||
                !result["result"].contains("value") || result["result"]["value"] != true) {
                restore({}, "capture_layout_unsupported");
                return;
            }
            if (stage == Stage::verify_refresh) {
                send(Stage::prepared, "CSS.setStyleSheetText", {{"styleSheetId", sheet_}, {"text", ""}});
                return;
            }
            take();
            return;
        case Stage::prepared:
            take();
            return;
        case Stage::capture:
            restore(std::move(result), {});
            return;
        }
        restore({}, "capture_layout_failed");
    }

    void take() { send(Stage::capture, "Page.captureScreenshot", params_); }

    std::string refresh_rule() const { return ":root{overflow-y:" + refresh_ + "!important}"; }

    void restore(nlohmann::json result, std::string error) {
        if (sheet_.empty()) { finish(std::move(result), std::move(error)); return; }
        if (!captured_ || refresh_.empty()) { clear(std::move(result), std::move(error)); return; }
        command_("CSS.setStyleSheetText", {{"styleSheetId", sheet_}, {"text", refresh_rule()}},
            [operation = shared_from_this(), result = std::move(result), error = std::move(error)]
            (nlohmann::json, std::string refresh_error) mutable {
                if (!refresh_error.empty()) {
                    operation->clear(std::move(result), error.empty() ? std::move(refresh_error) : std::move(error));
                    return;
                }
                operation->command_("Runtime.evaluate",
                    {{"expression", "document.documentElement.clientWidth"},
                     {"contextId", operation->context_}, {"returnByValue", true}},
                    [operation, result = std::move(result), error = std::move(error)]
                    (nlohmann::json, std::string layout_error) mutable {
                        operation->clear(std::move(result), error.empty() ? std::move(layout_error) : std::move(error));
                    });
            });
    }

    void clear(nlohmann::json result, std::string error) {
        command_("CSS.setStyleSheetText", {{"styleSheetId", sheet_}, {"text", ""}},
            [operation = shared_from_this(), result = std::move(result), error = std::move(error)]
            (nlohmann::json, std::string cleanup_error) mutable {
                operation->finish(std::move(result), error.empty() ? std::move(cleanup_error) : std::move(error));
            });
    }

    void finish(nlohmann::json result, std::string error) {
        if (auto complete = std::move(complete_)) complete(std::move(result), std::move(error));
    }

    Command command_;
    Current current_;
    nlohmann::json params_;
    CommandResult complete_;
    nlohmann::json context_;
    std::string frame_, sheet_, pin_, refresh_;
    bool captured_ = false;
};

}

void capture_preserving_layout(Command command, Current current,
                               nlohmann::json params, CommandResult complete) {
    std::make_shared<LayoutCapture>(std::move(command), std::move(current),
        std::move(params), std::move(complete))->start();
}

}
