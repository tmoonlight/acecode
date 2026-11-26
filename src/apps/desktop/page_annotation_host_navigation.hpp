#pragma once

#include "page_annotation_host_submission.hpp"
#include "page_annotation_poller.hpp"

namespace acecode::desktop {

inline bool annotation_fragment_navigation(const std::string& current, const std::string& next) {
    return current != next && current.substr(0, current.find('#')) == next.substr(0, next.find('#'));
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
