#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oleidl.h>
#include <memory>
#include <string>

namespace acecode::computer_use::test {

// Real OLE drag/drop inside one disposable test window. create(), close(), and
// destruction belong to the same STA UI thread, which must pump messages.
// Query methods and request_close() may be used from the test worker thread.
// This helper never injects successful input or operates on another window.
class OleDragFixture {
public:
    struct State {
        unsigned drag_starts = 0;
        unsigned drag_completions = 0;
        unsigned drops = 0;
        HRESULT last_result = S_FALSE;
        DWORD last_effect = DROPEFFECT_NONE;
        bool timed_out = false;
        std::wstring dropped_text;
    };

    OleDragFixture();
    ~OleDragFixture();
    OleDragFixture(const OleDragFixture&) = delete;
    OleDragFixture& operator=(const OleDragFixture&) = delete;

    bool create(HWND owner = nullptr, int x = 160, int y = 160);
    void close();
    void request_close();
    HWND window() const;
    bool screen_points(POINT& source, POINT& target) const;
    State state() const;
    HRESULT initialization_result() const;
    static const wchar_t* payload();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acecode::computer_use::test
#endif
