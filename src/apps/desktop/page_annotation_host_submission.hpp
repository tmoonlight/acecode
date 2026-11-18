#pragma once

#include "agent_browser_proxy_request.hpp"
#include "page_annotation_session.hpp"

namespace acecode::desktop {

using AnnotationEvaluation = std::function<void(const std::string&,
    std::function<void(bool, nlohmann::json)>)>;
using AnnotationUpdate = std::function<void(const std::string&)>;
using AnnotationCapture = std::function<void(const std::shared_ptr<PendingProxyCall>&,
    const std::string&)>;

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
    AnnotationUpdate update, AnnotationCapture capture) {
    if (!command.is_object() || !command.contains("action") ||
        (command["action"] != "capture" && command["action"] != "validate" &&
         command["action"] != "acknowledge")) return false;
    std::string expression;
    std::string error;
    if (!session.submission(command, expression, error)) {
        completion(nlohmann::json{{"ok", false}, {"error", error}}.dump());
    } else if (command["action"] == "acknowledge") {
        const auto snapshot = session.snapshot();
        update(snapshot.dump());
        completion(nlohmann::json{{"ok", true}, {"page_id", page_id},
            {"annotations", snapshot}}.dump());
    } else if (command["action"] == "validate") {
        evaluate(expression, [completion, page_id](bool ok, nlohmann::json result) {
            completion(nlohmann::json{{"ok", ok && result.is_object() && result.value("valid", false)},
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
            done(ok && value.is_object() && value.value("valid", false));
        });
    };
}

}
