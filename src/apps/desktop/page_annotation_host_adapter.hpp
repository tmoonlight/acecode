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

#ifdef _WIN32
template <typename Host>
auto annotation_event_page(Host& host, const std::string& id, ICoreWebView2* source) {
    const auto page = host.find_page(id);
    return page && !page->closing && page->webview.Get() == source ? page : nullptr;
}

template <typename Host, typename Page>
bool annotation_start_navigation(Host& host, const std::shared_ptr<Page>& page,
    ICoreWebView2NavigationStartingEventArgs* args, const std::string& uri, std::uint64_t navigation_id) {
    if (!normalize_agent_browser_url(uri, nullptr) || annotation_fragment_navigation(page->state.url, uri)) return true;
    BOOL redirected = FALSE;
    args->get_IsRedirected(&redirected);
    const bool continuation = page->annotations.navigation.navigating() &&
        (navigation_id == page->current_navigation_id || redirected || page->annotation_certificate_retry);
    page->annotation_certificate_retry = false;
    if ((!page->annotations.navigation.blocked() && !host.reserve_annotation_pages({page}, nullptr)) ||
        !page->annotations.navigation.start(continuation)) {
        args->put_Cancel(TRUE);
        return false;
    }
    page->annotations.suspend();
    return true;
}
#endif

template <typename Host, typename Page>
bool annotation_reserve_pages(Host& host, const std::vector<std::shared_ptr<Page>>& pages,
                              std::string* error, bool hold = false) {
    const bool accepted = annotation_reserve(host, pages, [&host] {
#ifdef _WIN32
        return MessageBoxW(static_cast<HWND>(host.parent_window),
            L"Leave this page? Unsent annotations will be kept but cannot be sent after replacement.",
            L"Page annotations", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) == IDYES;
#elif defined(__APPLE__)
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"Leave this page?";
        alert.informativeText = @"Unsent annotations will be kept but cannot be sent after replacement.";
        [alert addButtonWithTitle:@"Stay"];
        [alert addButtonWithTitle:@"Leave"];
        const bool leave = [alert runModal] == NSAlertSecondButtonReturn;
        [alert release];
        return leave;
#else
        return false;
#endif
    }, error);
    if (accepted && !hold) for (const auto& page : pages) page->annotations.navigation.arm();
    return accepted;
}

template <typename Host, typename Page>
void annotation_evaluate(const std::shared_ptr<Host>& host, const std::shared_ptr<Page>& page,
                         std::uint64_t sequence, const std::string& script, AnnotationCompletion done) {
#ifdef _WIN32
    host->evaluate_annotation(page, sequence, script, std::move(done));
#elif defined(__APPLE__)
    const auto incarnation = page->annotation_incarnation;
    const std::weak_ptr<Host> weak = host;
    const std::weak_ptr<Page> weak_page = page;
    host->evaluate_value(page, script,
        [weak, weak_page, sequence, incarnation, done](bool ok, nlohmann::json result, std::string) {
            const auto current_host = weak.lock();
            const auto current = weak_page.lock();
            const bool accepted = ok && current_host && current && !current->closing &&
                current_host->find_page(current->id) == current &&
                current->annotation_incarnation == incarnation && current->annotations.sequence() == sequence;
            done(accepted, accepted ? std::move(result) : nlohmann::json());
        });
#endif
}

template <typename Host>
void annotation_dispatch(const std::shared_ptr<Host>& host, const std::string& page_id,
                         const std::string& command_json, std::function<void(std::string)> completion) {
    const auto page = host ? host->find_page(page_id) : nullptr;
    const auto command = nlohmann::json::parse(command_json, nullptr, false);
    const bool receipt = command.is_object() && (command.value("action", nlohmann::json()) == "acknowledge" ||
        command.value("action", nlohmann::json()) == "release");
    if (!page || page->closing || (!receipt && (!page->webview ||
        page->state.content_state != kAgentBrowserContentStateLive))) {
        completion(nlohmann::json{{"ok", false}, {"error", "Annotation page is unavailable"}}.dump());
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
    if (annotation_host_submission(page->annotations, page_id, command, completion, evaluate,
        [weak, weak_page](const std::string& serialized) {
            const auto current_host = weak.lock();
            const auto current = weak_page.lock();
            if (current_host && current && !current->closing && current_host->find_page(current->id) == current)
                current_host->update_page(current, [serialized](AgentBrowserState& state) { state.page_annotations_json = serialized; });
        }, [weak, weak_page](const auto& pending, const std::string& script) {
            const auto current_host = weak.lock();
            const auto current = weak_page.lock();
            if (current_host && current && !current->closing && current_host->find_page(current->id) == current)
                current_host->capture_screenshot(current, {{"fullPage", true}}, pending, script);
            else finish_proxy_call(pending, {{"ok", false}, {"error", "Annotation page is unavailable"}});
        }, annotation_lifecycle(weak, weak_page))) return;
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

#ifdef __APPLE__
template <typename Host, typename Page>
bool annotation_allow_action(Host& host, const std::shared_ptr<Page>& page, WKNavigationAction* action) {
    if (action.targetFrame && !action.targetFrame.mainFrame) return true;
    if (annotation_fragment_navigation(page->state.url, agent_browser_macos::utf8_string(action.request.URL.absoluteString))) {
        if (page->annotations.navigation.reserved() && page->annotations.navigation.armed()) page->annotations.navigation.finish();
        return true;
    }
    if (page->annotations.navigation.navigating()) return action.navigationType == WKNavigationTypeOther;
    if (!page->annotations.navigation.reserved() && !annotation_reserve_pages(host, std::vector{page}, nullptr)) return false;
    return page->annotations.navigation.armed();
}
#endif

}
