#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace acecode::desktop {

class AnnotationNavigationGuard {
public:
    bool blocked() const { return reserved_ || navigating_; }
    bool reserved() const { return reserved_; }
    bool navigating() const { return navigating_; }
    bool armed() const { return armed_; }
    bool arm() { if (!reserved_) return false; armed_ = true; return true; }
    std::uint64_t ticket() const { return ticket_; }
    bool sending() const { return !lease_.empty(); }
    bool reserve() {
        if (blocked() || sending()) return false;
        reserved_ = true;
        ++ticket_;
        return true;
    }
    bool start(bool continuation = false) {
        if (navigating_) return continuation;
        if (!reserved_ || !armed_ || sending()) return false;
        reserved_ = false;
        armed_ = false;
        navigating_ = true;
        return true;
    }
    void finish() { reserved_ = false; armed_ = false; navigating_ = false; ++ticket_; }
    bool acquire(const std::string& lease) {
        if (blocked() || lease.empty() || lease.size() > 128 || (sending() && lease_ != lease)) return false;
        lease_ = lease;
        return true;
    }
    bool release(const std::string& lease) {
        if (lease.empty() || (!lease_.empty() && lease_ != lease)) return false;
        lease_.clear();
        return true;
    }
private:
    bool reserved_ = false;
    bool navigating_ = false;
    bool armed_ = false;
    std::string lease_;
    std::uint64_t ticket_ = 0;
};

template <typename Page, typename Current, typename Confirm>
bool reserve_annotation_transition(const std::vector<std::shared_ptr<Page>>& pages,
                                   Current current, Confirm confirm) {
    std::vector<std::shared_ptr<Page>> reserved;
    std::vector<std::uint64_t> revisions;
    std::vector<std::uint64_t> sequences;
    std::vector<std::uint64_t> tickets;
    const auto rollback = [&] {
        for (std::size_t index = 0; index < reserved.size(); ++index) {
            if (reserved[index]->annotations.navigation.ticket() == tickets[index])
                reserved[index]->annotations.navigation.finish();
        }
    };
    bool work = false;
    for (const auto& page : pages) {
        if (!page || !current(page) || !page->annotations.navigation.reserve()) {
            rollback();
            return false;
        }
        reserved.push_back(page);
        revisions.push_back(page->annotations.revision());
        sequences.push_back(page->annotations.sequence());
        tickets.push_back(page->annotations.navigation.ticket());
        work = work || page->annotations.has_unsent_work();
    }
    bool accepted = !work || confirm();
    for (std::size_t index = 0; index < reserved.size(); ++index) {
        accepted = accepted && current(reserved[index]) &&
            revisions[index] == reserved[index]->annotations.revision() &&
            sequences[index] == reserved[index]->annotations.sequence() &&
            tickets[index] == reserved[index]->annotations.navigation.ticket();
    }
    if (!accepted) rollback();
    return accepted;
}

}
