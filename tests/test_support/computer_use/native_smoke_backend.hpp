#pragma once

#include "computer_use/native_windows.hpp"
#include "computer_use/runtime.hpp"

#include <atomic>
#include <cstdint>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace acecode::computer_use::test {

// Both modes run the same owned-window assertions. Helper mode exercises the
// production broker, sidecar discovery, private pipe protocol, and desktop lease.
class SmokeBackend {
public:
    explicit SmokeBackend(bool via_helper)
        : via_helper_(via_helper), session_id_(via_helper ? new_session_id() : std::string{}) {
        if (via_helper_) set_enabled(true);
    }

    ~SmokeBackend() {
        if (via_helper_) {
            release_session(session_id_);
            set_enabled(false);
        }
    }

    SmokeBackend(const SmokeBackend&) = delete;
    SmokeBackend& operator=(const SmokeBackend&) = delete;

    nlohmann::json dispatch(const nlohmann::json& request) {
        // Preserve native and transport errors verbatim; a broker failure must
        // fail the fixture rather than silently falling back to direct dispatch.
        return via_helper_ ? execute(session_id_, request) : native_.dispatch(request);
    }

private:
    static std::string new_session_id() {
        static std::atomic<std::uint64_t> sequence{0};
        return "computer-use-native-smoke-" + std::to_string(GetCurrentProcessId()) +
            "-" + std::to_string(sequence.fetch_add(1) + 1);
    }

    // The fixture's local UIA hit-test checks require COM and per-monitor DPI
    // awareness on this thread even when all actions go through the helper.
    // NativeBackend initialization does not acquire the helper's desktop lease.
    NativeBackend native_;
    const bool via_helper_;
    const std::string session_id_;
};

} // namespace acecode::computer_use::test
