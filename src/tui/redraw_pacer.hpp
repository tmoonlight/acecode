#pragma once

#include <atomic>
#include <cstdint>

namespace acecode { namespace tui {

inline constexpr int kMaxRecordedFrameLatencyMs = 5000;

struct TuiRedrawFrameTicket {
    std::uint64_t scheduled_generation = 0;
    std::int64_t started_at_ms = 0;
};

// Backpressure for redraws whose intermediate states can be coalesced
// (thinking animation ticks and streamed deltas). Direct input and
// correctness-bearing events intentionally bypass this class.
class TuiRedrawPacer {
public:
    // Accept one scheduled redraw only when the previous accepted generation
    // completed and the requested minimum interval has elapsed.
    bool try_request_scheduled_redraw(std::int64_t now_ms,
                                      int minimum_interval_ms) noexcept;

    // Capture the latest requested generation at the start of an actual frame.
    // Completing this ticket cannot consume a request accepted later.
    TuiRedrawFrameTicket begin_frame(std::int64_t started_at_ms) const noexcept;
    void complete_frame(const TuiRedrawFrameTicket& ticket,
                        std::int64_t completed_at_ms) noexcept;

    int last_frame_latency_ms() const noexcept;
    std::int64_t last_frame_completed_at_ms() const noexcept;
    bool scheduled_redraw_pending() const noexcept;

    // Exposed for deterministic diagnostics and unit tests.
    std::uint64_t requested_generation() const noexcept;
    std::uint64_t completed_generation() const noexcept;

private:
    std::atomic<std::uint64_t> requested_generation_{0};
    std::atomic<std::uint64_t> completed_generation_{0};
    std::atomic<int> last_frame_latency_ms_{0};
    std::atomic<std::int64_t> last_frame_completed_at_ms_{0};
};

}} // namespace acecode::tui
