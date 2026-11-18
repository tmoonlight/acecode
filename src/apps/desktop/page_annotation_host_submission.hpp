#pragma once

#include "agent_browser_proxy_request.hpp"
#include "page_annotation_session.hpp"

namespace acecode::desktop {

using AnnotationEvaluation = std::function<void(const std::string&,
    std::function<void(bool, nlohmann::json)>)>;
using AnnotationUpdate = std::function<void(const std::string&)>;
using AnnotationCapture = std::function<void(const std::shared_ptr<PendingProxyCall>&,
    const std::string&)>;
using AnnotationLifecycle = std::function<bool(const nlohmann::json&, const std::string&)>;

template <typename Host, typename Page>
AnnotationLifecycle annotation_lifecycle(std::weak_ptr<Host> weak, std::weak_ptr<Page> weak_page) {
    return [weak, weak_page](const nlohmann::json& result, const std::string& release) {
        const auto host = weak.lock();
        const auto page = weak_page.lock();
        if (!host || !page || page->closing || host->find_page(page->id) != page) return false;
        const auto revision = page->annotations.revision();
        if (!release.empty()) page->annotations.navigation.release(release);
        bool valid = false;
        if (result.is_object() && result.contains("error") && result["error"] == "replaced_document") {
            page->annotations.replace_document();
        } else if (!page->annotations.navigation.blocked()) {
            valid = page->annotations.observe_lifecycle(result);
        }
        const auto snapshot = page->annotations.snapshot().dump();
        if (revision != page->annotations.revision()) {
            host->update_page(page, [snapshot](AgentBrowserState& state) { state.page_annotations_json = snapshot; });
        }
        return valid;
    };
}

template <typename Host, typename Page>
void annotation_host_result(const std::weak_ptr<Host>& weak,
    const std::weak_ptr<Page>& weak_page, std::uint64_t sequence,
    const std::function<void(std::string)>& completion, bool ok,
    const nlohmann::json& result, std::string error = {}) {
    const auto self = weak.lock();
    const auto current = weak_page.lock();
    if (!self || !current || current->closing ||
        self->find_page(current->id) != current || !ok ||
        !current->annotations.complete(sequence, result, error)) {
        if (current) current->annotations.fail(sequence);
        completion(nlohmann::json{{"ok", false}, {"error", error.empty()
            ? "Annotation document changed or runtime failed" : error}}.dump());
        return;
    }
    const auto snapshot = current->annotations.snapshot();
    const auto serialized = snapshot.dump();
    self->update_page(current, [serialized](AgentBrowserState& state) {
        state.page_annotations_json = serialized;
    });
    completion(nlohmann::json{{"ok", true}, {"page_id", current->id},
        {"annotations", snapshot}}.dump());
}

inline bool annotation_host_submission(PageAnnotationSession& session,
    const std::string& page_id, const nlohmann::json& command,
    const std::function<void(std::string)>& completion, AnnotationEvaluation evaluate,
    AnnotationUpdate update, AnnotationCapture capture, AnnotationLifecycle lifecycle = {}) {
    if (!command.is_object() || !command.contains("action") ||
        (command["action"] != "capture" && command["action"] != "validate" &&
         command["action"] != "acknowledge" && command["action"] != "release")) return false;
    std::string expression;
    std::string error;
    if (!session.submission(command, expression, error)) {
        completion(nlohmann::json{{"ok", false}, {"error", error}}.dump());
    } else if (command["action"] == "acknowledge" || command["action"] == "release") {
        const auto snapshot = session.snapshot();
        update(snapshot.dump());
        completion(nlohmann::json{{"ok", true}, {"page_id", page_id},
            {"annotations", snapshot}}.dump());
    } else if (command["action"] == "validate") {
        const auto lease = command.value("lease_id", "");
        evaluate(expression, [completion, page_id, lifecycle, lease](bool ok, nlohmann::json result) {
            const bool valid = ok && result.is_object() && result.contains("valid") && result["valid"] == true;
            const bool current = !lifecycle || lifecycle(result, valid ? "" : lease);
            if (valid && !current && lifecycle) lifecycle(nlohmann::json(), lease);
            completion(nlohmann::json{{"ok", valid && current},
                {"page_id", page_id}, {"error", "Annotation target or document changed"}}.dump());
        });
    } else {
        auto pending = std::make_shared<PendingProxyCall>(
            [completion, document = command["document_id"]](nlohmann::json response) {
                if (response.value("ok", false)) response["annotation_document_id"] = document;
                completion(response.dump());
            });
        capture(pending, expression);
    }
    return true;
}

inline browser_capture::Validate annotation_capture_validation(
    const std::string& expression, AnnotationEvaluation evaluate) {
    if (expression.empty()) return {};
    return [expression, evaluate](std::function<void(bool)> done) {
        evaluate(expression, [done](bool ok, nlohmann::json value) {
            done(ok && value.is_object() && value.contains("valid") && value["valid"] == true);
        });
    };
}

}
