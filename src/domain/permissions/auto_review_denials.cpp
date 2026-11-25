#include "auto_review_denials.hpp"

#include <utility>

namespace acecode::auto_review {

void DenialTracker::record_allow() {
    std::lock_guard<std::mutex> lock(mu_);
    consecutive_ = 0;
}

bool DenialTracker::record_block() {
    std::lock_guard<std::mutex> lock(mu_);
    ++consecutive_;
    ++total_;
    return consecutive_ >= limits_.max_consecutive || total_ >= limits_.max_total;
}

bool DenialTracker::limit_reached() const {
    std::lock_guard<std::mutex> lock(mu_);
    return consecutive_ >= limits_.max_consecutive || total_ >= limits_.max_total;
}

void DenialTracker::record_human_answer() {
    std::lock_guard<std::mutex> lock(mu_);
    consecutive_ = 0;
    total_ = 0;
}

void DenialTracker::lock(std::string anchor) {
    std::lock_guard<std::mutex> lock(mu_);
    locked_ = true;
    lock_anchor_ = std::move(anchor);
}

bool DenialTracker::locked() const {
    std::lock_guard<std::mutex> lock(mu_);
    return locked_;
}

bool DenialTracker::unlock_if_new_user_turn(const std::string& anchor) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!locked_ || anchor == lock_anchor_) return false;
    reset_locked();
    return true;
}

void DenialTracker::sync_mode_generation(std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(mu_);
    if (generation_known_ && generation_ == generation) return;
    // 第一次见到 generation 时也清零:此前的计数不可能属于本段模式。
    reset_locked();
    generation_ = generation;
    generation_known_ = true;
}

void DenialTracker::reset() {
    std::lock_guard<std::mutex> lock(mu_);
    reset_locked();
}

DenialCounts DenialTracker::counts() const {
    std::lock_guard<std::mutex> lock(mu_);
    return {consecutive_, total_, locked_};
}

void DenialTracker::reset_locked() {
    consecutive_ = 0;
    total_ = 0;
    locked_ = false;
    lock_anchor_.clear();
}

} // namespace acecode::auto_review
