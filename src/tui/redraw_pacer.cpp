#include "tui/redraw_pacer.hpp"

#include <algorithm>
#include <limits>

namespace acecode { namespace tui {

bool TuiRedrawPacer::try_request_scheduled_redraw(
    std::int64_t now_ms,
    int minimum_interval_ms) noexcept {
    std::uint64_t requested =
        requested_generation_.load(std::memory_order_acquire);
    const std::uint64_t completed =
        completed_generation_.load(std::memory_order_acquire);
    if (requested != completed ||
        requested == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }

    minimum_interval_ms = std::max(0, minimum_interval_ms);
    const std::int64_t last_completed_at =
        last_frame_completed_at_ms_.load(std::memory_order_acquire);
    if (last_completed_at > 0 &&
        now_ms >= last_completed_at &&
        now_ms - last_completed_at < minimum_interval_ms) {
        return false;
    }

    return requested_generation_.compare_exchange_strong(
        requested,
        requested + 1,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

TuiRedrawFrameTicket TuiRedrawPacer::begin_frame(
    std::int64_t started_at_ms) const noexcept {
    return {
        requested_generation_.load(std::memory_order_acquire),
        started_at_ms,
    };
}

void TuiRedrawPacer::complete_frame(
    const TuiRedrawFrameTicket& ticket,
    std::int64_t completed_at_ms) noexcept {
    const std::int64_t nonnegative_completed_at =
        std::max<std::int64_t>(0, completed_at_ms);
    const std::int64_t elapsed_ms =
        nonnegative_completed_at >= ticket.started_at_ms
        ? nonnegative_completed_at - ticket.started_at_ms
        : 0;
    last_frame_latency_ms_.store(
        static_cast<int>(std::min<std::int64_t>(
            elapsed_ms, kMaxRecordedFrameLatencyMs)),
        std::memory_order_release);
    last_frame_completed_at_ms_.store(
        nonnegative_completed_at, std::memory_order_release);

    std::uint64_t completed =
        completed_generation_.load(std::memory_order_acquire);
    while (completed < ticket.scheduled_generation &&
           !completed_generation_.compare_exchange_weak(
               completed,
               ticket.scheduled_generation,
               std::memory_order_acq_rel,
               std::memory_order_acquire)) {
    }
}

int TuiRedrawPacer::last_frame_latency_ms() const noexcept {
    return last_frame_latency_ms_.load(std::memory_order_acquire);
}

std::int64_t TuiRedrawPacer::last_frame_completed_at_ms() const noexcept {
    return last_frame_completed_at_ms_.load(std::memory_order_acquire);
}

bool TuiRedrawPacer::scheduled_redraw_pending() const noexcept {
    return requested_generation() > completed_generation();
}

std::uint64_t TuiRedrawPacer::requested_generation() const noexcept {
    return requested_generation_.load(std::memory_order_acquire);
}

std::uint64_t TuiRedrawPacer::completed_generation() const noexcept {
    return completed_generation_.load(std::memory_order_acquire);
}

}} // namespace acecode::tui
