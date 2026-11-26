#pragma once

#include "page_annotation_host_navigation.hpp"
#ifdef _WIN32
#include "agent_browser_windows_helpers.hpp"
#endif
#ifdef __APPLE__
#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
#include "agent_browser_macos_helpers.hpp"
#endif

namespace acecode::desktop {

using AnnotationCompletion = std::function<void(bool, nlohmann::json)>;

template <typename Page>
bool annotation_retire(const std::shared_ptr<Page>& page) {
    if (!page) return false;
    page->annotations.requests.stop();
    page->annotations.suspend();
#ifdef __APPLE__
    ++page->annotation_incarnation;
#endif
    return true;
}

#ifdef _WIN32
template <typename Host>
auto annotation_event_page(Host& host, const std::string& id, ICoreWebView2* source) {
    const auto page = host.find_page(id);
    return page && !page->closing && page->webview.Get() == source ? page : nullptr;
}

template <typename Page>
void annotation_suspend_for_navigation(const std::shared_ptr<Page>& page, const std::string& uri) {
    if (!normalize_agent_browser_url(uri, nullptr) || annotation_fragment_navigation(page->state.url, uri)) return;
    page->annotations.suspend();
}
#endif

template <typename Host, typename Page>
void annotation_evaluate(const std::shared_ptr<Host>& host, const std::shared_ptr<Page>& page,
                         std::uint64_t sequence, const std::string& script, AnnotationCompletion done) {
    const auto pending = page->annotations.requests.add([done](nlohmann::json response) {
        done(response.value("ok", false), response.value("result", nlohmann::json()));
    });
    if (!pending) return;
    const auto finish = [pending](bool ok, nlohmann::json result) {
        pending->finish({{"ok", ok}, {"result", std::move(result)}});
    };
#ifdef _WIN32
    host->evaluate_annotation(page, sequence, script, finish,
        [pending] { return !pending->completed(); });
#elif defined(__APPLE__)
    const auto incarnation = page->annotation_incarnation;
    const std::weak_ptr<Host> weak = host;
    const std::weak_ptr<Page> weak_page = page;
    host->evaluate_value(page, script,
        [weak, weak_page, sequence, incarnation, finish](bool ok, nlohmann::json result, std::string) {
            const auto current_host = weak.lock();
            const auto current = weak_page.lock();
            const bool accepted = ok && current_host && current && !current->closing &&
                current_host->find_page(current->id) == current &&
                current->annotation_incarnation == incarnation && current->annotations.sequence() == sequence;
            finish(accepted, accepted ? std::move(result) : nlohmann::json());
        });
#endif
}

template <typename Host>
void annotation_dispatch(const std::shared_ptr<Host>& host, const std::string& page_id,
                         const std::string& command_json, std::function<void(std::string)> completion) {
    const auto page = host ? host->find_page(page_id) : nullptr;
    const auto command = nlohmann::json::parse(command_json, nullptr, false);
    // 发送成功后的 cleanup（remove）在页面已经关闭时也要能落地：记录归会话，
    // 页签没了不影响删除。其余动作要求页面还活着。
    const bool cleanup = command.is_object() && command.value("action", nlohmann::json()) == "remove";
    if (!page || page->closing || (!cleanup && (!page->webview ||
        page->state.content_state != kAgentBrowserContentStateLive))) {
        completion(nlohmann::json{{"ok", false}, {"error", "Annotation page is unavailable"}}.dump());
        return;
    }
    if (command.is_object() && command.value("action", nlohmann::json()) == "screenshot") {
        const auto pending = page->annotations.requests.add(
            [completion](nlohmann::json response) { completion(response.dump()); });
        if (!pending) {
            completion(nlohmann::json{{"ok", false}, {"error", "Annotation capture is already running"}}.dump());
            return;
        }
        host->capture_screenshot(page, annotation_capture_params(), pending,
            annotation_capture_validation_expression());
        return;
    }
    const std::weak_ptr<Host> weak = host;
    const std::weak_ptr<typename Host::Page> weak_page = page;
    host->annotation_poller.start([weak] {
        poll_annotations(weak, [](const auto& current_host, const auto& current, auto sequence,
            const std::string& script, AnnotationCompletion done) {
            annotation_evaluate(current_host, current, sequence, script, std::move(done));
        });
    });
    const auto evaluate = [weak, weak_page](const std::string& script, AnnotationCompletion done) {
        const auto current_host = weak.lock();
        const auto current = weak_page.lock();
        if (!current_host || !current || current->closing || current_host->find_page(current->id) != current) {
            done(false, nullptr);
            return;
        }
        annotation_evaluate(current_host, current, current->annotations.sequence(), script, std::move(done));
    };
    if (annotation_host_submission(page->annotations, page_id, command, completion,
        [weak, weak_page](const std::string& serialized) {
            const auto current_host = weak.lock();
            const auto current = weak_page.lock();
            if (current_host && current && !current->closing && current_host->find_page(current->id) == current)
                current_host->update_page(current, [serialized](AgentBrowserState& state) { state.page_annotations_json = serialized; });
        }, [evaluate, document_id = page->annotations.document_id()](const std::vector<int>& numbers) {
            const auto expression = page_annotation_runtime_expression(
                page_annotation_remove_pins_payload(document_id, numbers));
            evaluate(expression, [](bool, nlohmann::json) {});
        })) return;
    std::string expression;
    std::string error;
    if (!page->annotations.prepare(page_id, command, expression, error)) {
        completion(nlohmann::json{{"ok", false}, {"error", error}}.dump());
        return;
    }
    const auto sequence = page->annotations.sequence();
    evaluate(expression, [weak, weak_page, sequence, completion](bool ok, nlohmann::json result) {
        annotation_host_result(weak, weak_page, sequence, completion, ok, result);
    });
}

}
