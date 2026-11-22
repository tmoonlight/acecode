#pragma once

#include "page_annotation_host_submission.hpp"
#include "page_annotation_poller.hpp"

namespace acecode::desktop {

inline bool annotation_fragment_navigation(const std::string& current, const std::string& next) {
    return current != next && current.substr(0, current.find('#')) == next.substr(0, next.find('#'));
}

template <typename Host, typename Page, typename Confirm>
bool annotation_reserve(Host& host, const std::vector<std::shared_ptr<Page>>& pages,
                        Confirm confirm, std::string* error) {
    if (reserve_annotation_transition(pages, [&host](const auto& page) {
        return !page->closing && host.find_page(page->id) == page;
    }, confirm)) return true;
    if (error) *error = "Navigation cancelled or annotation submission is in progress";
    return false;
}

inline bool annotation_replaces_document(const std::string& method) {
    return method == "Page.navigate" || method == "Page.reload" ||
        method == "Page.navigateToHistoryEntry" || method == "Page.setDocumentContent";
}

template <typename Host, typename Key>
auto annotation_store_pages(Host& host, const Key& key) {
    std::vector<std::shared_ptr<typename Host::Page>> affected;
    for (const auto& state : host.states()) {
        const auto page = host.find_page(state.page_id);
        if (page && !page->closing && page->webview && page->store == key) affected.push_back(page);
    }
    return affected;
}

template <typename Page>
auto annotation_transition_tickets(const std::vector<std::shared_ptr<Page>>& pages) {
    std::vector<std::pair<std::shared_ptr<Page>, std::uint64_t>> tickets;
    for (const auto& page : pages) tickets.emplace_back(page, page->annotations.navigation.ticket());
    return tickets;
}

template <typename Host, typename Page, typename Key, typename Reload>
void annotation_reload_reserved(Host& host,
    const std::vector<std::pair<std::shared_ptr<Page>, std::uint64_t>>& tickets,
    const Key& key, bool ok, Reload reload) {
    for (const auto& item : tickets) {
        const auto& page = item.first;
        if (page->closing || host.find_page(page->id) != page ||
            page->annotations.navigation.ticket() != item.second) continue;
        if (ok) page->annotations.navigation.arm();
        if (!ok || !page->webview || page->store != key || !reload(page)) page->annotations.navigation.finish();
    }
}

template <typename Host, typename Page, typename Dispatch>
void annotation_proxy_transition(Host& host, const std::shared_ptr<Page>& page,
    const std::string& method, const std::shared_ptr<PendingProxyCall>& pending, Dispatch dispatch) {
    if (annotation_replaces_document(method) && !host.reserve_annotation_pages({page}, nullptr)) {
        finish_proxy_call(pending, {{"ok", false}, {"page_id", page->id}, {"error", "Navigation cancelled"}});
        return;
    }
    const auto ticket = page->annotations.navigation.ticket();
    const auto weak = host.weak_from_this();
    const std::weak_ptr<Page> weak_page = page;
    dispatch([pending, weak, weak_page, ticket, method](nlohmann::json response) {
        const auto current_host = weak.lock();
        const auto current = weak_page.lock();
        if (current_host && current && current_host->find_page(current->id) == current &&
            current->annotations.navigation.ticket() == ticket && annotation_replaces_document(method) &&
            (!response.value("ok", false) || method == "Page.setDocumentContent")) current->annotations.navigation.finish();
        finish_proxy_call(pending, std::move(response));
    });
}

template <typename Host, typename Evaluate>
void poll_annotations(const std::weak_ptr<Host>& weak, Evaluate evaluate) {
    const auto host = weak.lock();
    if (!host) return;
    for (const auto& state : host->states()) {
        const auto page = host->find_page(state.page_id);
        if (page) page->annotations.requests.expire();
        if (!page || host->find_page(page->id) != page || page->closing || !page->webview ||
            state.content_state != kAgentBrowserContentStateLive || !page->annotations.pollable()) continue;
        const auto expression = std::string(kPageAnnotationRuntime) + "(" + nlohmann::json{
            {"action", "read"}, {"document_id", page->annotations.document_id()}}.dump() + ")";
        const auto sequence = page->annotations.sequence();
        const auto poll = page->annotations.begin_poll();
        const std::weak_ptr<typename Host::Page> weak_page = page;
        evaluate(host, page, sequence, expression,
            [weak, weak_page, sequence, poll](bool ok, nlohmann::json result) {
                const auto current = weak_page.lock();
                if (!current || !current->annotations.finish_poll(poll) || !ok ||
                    current->annotations.sequence() != sequence) return;
                annotation_lifecycle(weak, weak_page)(result);
            });
    }
}

}
