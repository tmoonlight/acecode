#pragma once

#include "agent_browser_proxy_request.hpp"
#include "page_annotation_session.hpp"

namespace acecode::desktop {

using AnnotationEvaluation = std::function<void(const std::string&,
    std::function<void(bool, nlohmann::json)>)>;
using AnnotationUpdate = std::function<void(const std::string&)>;
// 页面状态回报（轮询 / 交互结果）→ 记录失效状态；返回 false 表示回报本身不可信。
using AnnotationLifecycle = std::function<bool(const nlohmann::json&)>;

template <typename Host, typename Page>
AnnotationLifecycle annotation_lifecycle(std::weak_ptr<Host> weak, std::weak_ptr<Page> weak_page) {
    return [weak, weak_page](const nlohmann::json& result) {
        const auto host = weak.lock();
        const auto page = weak_page.lock();
        if (!host || !page || page->closing || host->find_page(page->id) != page) return false;
        const auto revision = page->annotations.revision();
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

// 待发集合的两个写入动作：发送成功后删记录、保存后补图。发送本身走普通消息
// 提交（Q35），原生侧不再有 capture / validate / acknowledge / release 租约。
inline bool annotation_host_submission(PageAnnotationSession& session,
    const std::string& page_id, const nlohmann::json& command,
    const std::function<void(std::string)>& completion, AnnotationUpdate update) {
    if (!command.is_object() || !command.contains("action") ||
        (command["action"] != "remove" && command["action"] != "attach_screenshot")) return false;
    std::string error;
    if (!session.submission(command, error)) {
        completion(nlohmann::json{{"ok", false}, {"error", error}}.dump());
        return true;
    }
    const auto snapshot = session.snapshot();
    update(snapshot.dump());
    completion(nlohmann::json{{"ok", true}, {"page_id", page_id},
        {"annotations", snapshot}}.dump());
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
