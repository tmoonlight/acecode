#pragma once

#ifdef _WIN32

#include <windows.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace acecode::computer_use::test {

// Call from the fixture's UI thread. The parent owns and destroys this control.
inline HWND install_secondary_action_checkbox(HWND fixture) {
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(fixture, &process);
    if (!thread || thread != GetCurrentThreadId() || process != GetCurrentProcessId()) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (GetDlgItem(fixture, 107)) {
        SetLastError(ERROR_ALREADY_EXISTS);
        return nullptr;
    }
    return CreateWindowW(L"BUTTON", L"Fixture checkbox", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        20, 225, 180, 22, fixture, reinterpret_cast<HMENU>(static_cast<INT_PTR>(107)), GetModuleHandleW(nullptr), nullptr);
}

namespace control_checks_detail {

inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("Secondary-action fixture: " + message);
}

inline LRESULT read_control(HWND control, UINT message, WPARAM wparam = 0, LPARAM lparam = 0) {
    require(control != nullptr, "required native control is missing");
    DWORD_PTR result = 0;
    const auto completed = SendMessageTimeoutW(control, message, wparam, lparam, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result);
    require(completed != 0,
        "control-state query failed (message " + std::to_string(message) + ", error " + std::to_string(GetLastError()) + ")");
    return static_cast<LRESULT>(result);
}

template<class Predicate>
inline void await_state(Predicate predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    do {
        if (predicate()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < deadline);
    require(predicate(), message);
}

inline std::string first_list_item(HWND list) {
    const auto length = read_control(list, LB_GETTEXTLEN, 0);
    require(length >= 0 && length <= 4096, "ListBox 105 has no readable first item");
    std::vector<wchar_t> text(static_cast<std::size_t>(length) + 1);
    const auto copied = read_control(list, LB_GETTEXT, 0, reinterpret_cast<LPARAM>(text.data()));
    require(copied == length && copied > 0, "ListBox 105 first item changed while reading");
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(copied), nullptr, 0, nullptr, nullptr);
    require(bytes > 0, "ListBox 105 first item is not valid UTF-16");
    std::string result(static_cast<std::size_t>(bytes), '\0');
    require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(copied), result.data(), bytes, nullptr, nullptr) == bytes,
        "ListBox 105 first item UTF-8 conversion failed");
    return result;
}

template<class Backend>
inline nlohmann::json call(Backend& backend, const nlohmann::json& request, const std::string& stage) {
    auto result = backend.dispatch(request);
    require(result.value("success", false), stage + " failed: " + result.dump());
    return result;
}

// Resolve the element anew immediately before each action; no cached UIA index
// or previously consumed observation is allowed across state transitions.
template<class Backend>
inline void act(Backend& backend, HWND fixture, const char* action,
                const std::string& automation_id, const std::string& name = {}) {
    const auto window_id = reinterpret_cast<std::uintptr_t>(fixture);
    const auto observation = call(backend, {{"action", "get_window_state"}, {"window", window_id}, {"include_text", true}},
        std::string("observe before ") + action);
    const auto& output = observation.at("output");
    const nlohmann::json* match = nullptr;
    for (const auto& element : output.at("accessibility").at("elements")) {
        // An expanded combo can also be exposed by its related popup's UIA
        // provider. Native automation IDs are scoped to the chosen surface.
        if (element.value("surface_window", std::uintptr_t{}) != window_id) continue;
        if ((!automation_id.empty() && element.value("automation_id", std::string()) != automation_id)
            || (!name.empty() && element.value("name", std::string()) != name)) continue;
        bool supported = false;
        for (const auto& available : element.at("actions")) if (available == action) supported = true;
        if (!supported) continue;
        if (match) {
            // The standard ComboBox provider can repeat the same visible
            // control within its expanded tree. Accept only identical records
            // apart from their traversal index; still reject ambiguous targets.
            auto previous = *match;
            auto candidate = element;
            previous.erase("index");
            candidate.erase("index");
            require(previous == candidate, std::string(action) + " matched different fixture UIA controls");
            continue;
        }
        match = &element;
    }
    require(match != nullptr, std::string(action) + " has no supporting UIA element (id=" + automation_id + ", name=" + name + ")");
    require(match->value("enabled", false) && !match->value("offscreen", true), std::string(action) + " target is disabled or offscreen");
    call(backend, {{"action", "perform_secondary_action"}, {"window", window_id},
        {"observation_id", output.at("observation_id")}, {"element_index", match->at("index")},
        {"secondary_action", action}}, action);
}

} // namespace control_checks_detail

// Run on the backend worker, after the caller restores the fixture's foreground
// activation. Only UIA actions mutate the controls; Win32 messages read results.
// Backend requires dispatch(json), so the same check supports the broker adapter.
template<class Backend>
inline void verify_secondary_actions(Backend& backend, HWND fixture, const std::function<int()>& read_clicks) {
    using namespace control_checks_detail;
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(fixture, &process);
    require(thread != 0 && process == GetCurrentProcessId(), "window is not owned by this test process");
    require(thread != GetCurrentThreadId(), "UIA verification must not run on the fixture UI thread");
    require(static_cast<bool>(read_clicks), "button click-counter callback is missing");

    const HWND checkbox = GetDlgItem(fixture, 107);
    const auto before_check = read_control(checkbox, BM_GETCHECK);
    require(before_check == BST_UNCHECKED || before_check == BST_CHECKED, "checkbox is not in a two-state condition");
    act(backend, fixture, "toggle", "107");
    const auto expected_check = before_check == BST_UNCHECKED ? BST_CHECKED : BST_UNCHECKED;
    await_state([&] { return read_control(checkbox, BM_GETCHECK) == expected_check; }, "UIA Toggle did not change checkbox 107 state");

    const HWND list = GetDlgItem(fixture, 105);
    const auto first_item = first_list_item(list);
    require(read_control(list, LB_GETCURSEL) != 0, "ListBox 105 first item was already selected before Select verification");
    act(backend, fixture, "select", "", first_item);
    await_state([&] { return read_control(list, LB_GETCURSEL) == 0; }, "UIA Select did not select ListBox 105 first item");

    const HWND combo = GetDlgItem(fixture, 106);
    require(read_control(combo, CB_GETDROPPEDSTATE) == FALSE, "ComboBox 106 was already expanded before Expand verification");
    act(backend, fixture, "expand", "106");
    await_state([&] { return read_control(combo, CB_GETDROPPEDSTATE) != FALSE; }, "UIA Expand did not open ComboBox 106");
    act(backend, fixture, "collapse", "106");
    await_state([&] { return read_control(combo, CB_GETDROPPEDSTATE) == FALSE; }, "UIA Collapse did not close ComboBox 106");

    const int before_clicks = read_clicks();
    act(backend, fixture, "invoke", "102");
    await_state([&] { return read_clicks() == before_clicks + 1; }, "UIA Invoke did not increment the button 102 click counter exactly once");
}

} // namespace acecode::computer_use::test

#endif
