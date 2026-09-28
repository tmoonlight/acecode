#pragma once

#ifdef _WIN32
#include "platform/process/unique_resources.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <string>

namespace acecode::platform {
// The process handle is borrowed only for this call. Creation time prevents
// a stale controller from addressing an unrelated process after PID reuse.
inline std::wstring process_stop_event_name(HANDLE process) {
    const DWORD pid = ::GetProcessId(process);
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!pid || !::GetProcessTimes(process, &created, &exited, &kernel, &user))
        return {};
    const auto generation =
        (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) |
        created.dwLowDateTime;
    return L"Local\\ACECode-ProcessStop-" + std::to_wstring(pid) +
           L"-" + std::to_wstring(generation);
}

// Publish only for the process lifetime owner, before its waiting threads
// start. Default object security comes from the creator's Windows token.
inline UniqueHandle create_process_stop_event() {
    const auto name = process_stop_event_name(::GetCurrentProcess());
    if (name.empty()) return {};
    return UniqueHandle(::CreateEventW(nullptr, TRUE, FALSE, name.c_str()));
}

// Open, signal and release the endpoint in one call. An absent endpoint
// (including an older daemon) returns false so the controller can fall back.
inline bool request_process_stop(HANDLE process) noexcept {
    try {
        const auto name = process_stop_event_name(process);
        if (name.empty()) return false;
        UniqueHandle event(::OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str()));
        return event && ::SetEvent(event.get()) != FALSE;
    } catch (...) {
        return false;
    }
}
} // namespace acecode::platform
#endif
