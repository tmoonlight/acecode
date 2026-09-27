// Explicit opt-in integration test. It operates only its own temporary Win32
// window and controls; never register this interactive executable with ctest.
#include "computer_use/native_windows.hpp"
#include "computer_use/element_target.hpp"
#include "config/vocab/pointer_appearance.hpp"
#include "test_support/computer_use/native_control_checks.hpp"
#include "test_support/computer_use/native_smoke_backend.hpp"
#include "test_support/computer_use/ole_drag_fixture.hpp"
#include "utils/base64.hpp"
#include <windows.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using nlohmann::json;
using acecode::computer_use::test::SmokeBackend;

namespace {
std::atomic<HWND> fixture{nullptr};
std::atomic<HWND> edit_control{nullptr};
std::atomic<int> clicks{0};
std::string fixture_pointer_style = acecode::computer_use::pointer_appearance::kDefaultStyle;
std::string fixture_pointer_color = acecode::computer_use::pointer_appearance::kDefaultColor;
std::atomic<int> wheel_events{0};
std::atomic<int> drag_events{0};
std::atomic<int> canvas_left{240};
std::atomic<int> canvas_top{195};
std::atomic<int> canvas_drops{0};
bool canvas_dragging = false;
POINT canvas_grab_offset{};
std::atomic<HWND> overlay{nullptr};
std::atomic<HWND> element_overlay{nullptr};
std::atomic<HWND> related_popup{nullptr};
std::atomic<HWND> unrelated_popup{nullptr};
std::unique_ptr<acecode::computer_use::test::OleDragFixture> ole_fixture;
std::atomic<bool> fixture_menu_active{false};
std::atomic<unsigned> fixture_menu_commands{0};
constexpr UINT show_overlay = WM_APP + 1;
constexpr UINT hide_overlay = WM_APP + 2;
constexpr UINT focus_button = WM_APP + 3;
constexpr UINT show_element_overlay = WM_APP + 4;
constexpr UINT hide_element_overlay = WM_APP + 5;
constexpr UINT show_related_popups = WM_APP + 6;
constexpr UINT hide_related_popups = WM_APP + 7;
constexpr UINT move_related_popup = WM_APP + 8;
constexpr UINT create_ole_fixture = WM_APP + 9;
constexpr UINT close_ole_fixture = WM_APP + 10;
constexpr UINT show_combo = WM_APP + 11;
constexpr UINT hide_combo = WM_APP + 12;
constexpr UINT show_fixture_menu = WM_APP + 13;
constexpr UINT close_fixture_menu = WM_APP + 14;
LRESULT CALLBACK fixture_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_GETMINMAXINFO) {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMaxTrackSize = {8192, 8192};
        return 0;
    }
    if (message == WM_CREATE) {
        edit_control = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"Initial fixture text", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            20, 20, 360, 30, window, reinterpret_cast<HMENU>(101), GetModuleHandleW(nullptr), nullptr);
        CreateWindowW(L"BUTTON", L"Fixture action", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            20, 70, 160, 36, window, reinterpret_cast<HMENU>(102), GetModuleHandleW(nullptr), nullptr);
        CreateWindowW(L"EDIT", L"private-password-fixture", WS_CHILD | WS_VISIBLE | ES_PASSWORD,
            20, 125, 360, 30, window, reinterpret_cast<HMENU>(103), GetModuleHandleW(nullptr), nullptr);
        HWND choices = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | LBS_NOTIFY,
            20, 170, 180, 50, window, reinterpret_cast<HMENU>(105), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(choices, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"First fixture item"));
        SendMessageW(choices, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Selected fixture item"));
        SendMessageW(choices, LB_SETCURSEL, 1, 0);
        HWND combo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            210, 170, 170, 120, window, reinterpret_cast<HMENU>(106), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"First combo item"));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Second combo item"));
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        if (!acecode::computer_use::test::install_secondary_action_checkbox(window)) return -1;
        return 0;
    }
    if (message == WM_COMMAND && LOWORD(wparam) == 102 && HIWORD(wparam) == BN_CLICKED) { ++clicks; return 0; }
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
        ++wheel_events;
        if (message == WM_MOUSEWHEEL) canvas_top += static_cast<short>(HIWORD(wparam)) / WHEEL_DELTA * 8;
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    if (message == WM_LBUTTONDOWN) {
        POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        RECT object{canvas_left.load(), canvas_top.load(), canvas_left + 50, canvas_top + 23};
        if (PtInRect(&object, point)) {
            canvas_grab_offset = {point.x - object.left, point.y - object.top};
            canvas_dragging = true;
            SetCapture(window);
        }
        return 0;
    }
    if (message == WM_MOUSEMOVE && (wparam & MK_LBUTTON)) {
        ++drag_events;
        if (canvas_dragging) {
            canvas_left = static_cast<short>(LOWORD(lparam)) - canvas_grab_offset.x;
            canvas_top = static_cast<short>(HIWORD(lparam)) - canvas_grab_offset.y;
            InvalidateRect(window, nullptr, TRUE);
        }
        return 0;
    }
    if (message == WM_LBUTTONUP && canvas_dragging) {
        canvas_left = static_cast<short>(LOWORD(lparam)) - canvas_grab_offset.x;
        canvas_top = static_cast<short>(HIWORD(lparam)) - canvas_grab_offset.y;
        canvas_dragging = false;
        ++canvas_drops;
        ReleaseCapture();
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    if (message == WM_CAPTURECHANGED) canvas_dragging = false;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT object{canvas_left.load(), canvas_top.load(), canvas_left + 50, canvas_top + 23};
        FillRect(dc, &object, GetSysColorBrush(COLOR_BTNFACE));
        FrameRect(dc, &object, GetSysColorBrush(COLOR_WINDOWTEXT));
        DrawTextW(dc, L"Drag", -1, &object, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == show_overlay) {
        RECT rect{};
        GetWindowRect(window, &rect);
        overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"STATIC", L"Owned screenshot occluder", WS_POPUP | WS_VISIBLE,
            rect.left + 180, rect.top + 80, 180, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        UpdateWindow(overlay.load());
        return 0;
    }
    if (message == hide_overlay) { if (overlay) DestroyWindow(overlay.exchange(nullptr)); return 0; }
    if (message == show_element_overlay) {
        element_overlay = CreateWindowW(L"BUTTON", L"Owned sibling overlay", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            20, 70, 160, 36, window, reinterpret_cast<HMENU>(104), GetModuleHandleW(nullptr), nullptr);
        SetWindowPos(element_overlay.load(), HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
    }
    if (message == hide_element_overlay) { if (element_overlay) DestroyWindow(element_overlay.exchange(nullptr)); return 0; }
    if (message == show_related_popups) {
        RECT rect{};
        GetWindowRect(window, &rect);
        related_popup = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"STATIC", L"Owned transient fixture", WS_POPUP | WS_VISIBLE | WS_BORDER,
            rect.right - 50, rect.top + 80, 200, 120, window, nullptr, GetModuleHandleW(nullptr), nullptr);
        unrelated_popup = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Unrelated same-thread fixture", WS_POPUP | WS_VISIBLE | WS_BORDER,
            rect.left + 20, rect.bottom + 30, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        return 0;
    }
    if (message == hide_related_popups) {
        if (related_popup) DestroyWindow(related_popup.exchange(nullptr));
        if (unrelated_popup) DestroyWindow(unrelated_popup.exchange(nullptr));
        return 0;
    }
    if (message == move_related_popup) {
        RECT rect{};
        GetWindowRect(related_popup.load(), &rect);
        SetWindowPos(related_popup.load(), nullptr, rect.left + 10, rect.top + 10, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    if (message == create_ole_fixture) {
        ole_fixture = std::make_unique<acecode::computer_use::test::OleDragFixture>();
        if (!ole_fixture->create(window, 700, 160)) ole_fixture.reset();
        return 0;
    }
    if (message == close_ole_fixture) { ole_fixture.reset(); return 0; }
    if (message == show_combo || message == hide_combo) {
        SendMessageW(GetDlgItem(window, 106), CB_SHOWDROPDOWN, message == show_combo ? TRUE : FALSE, 0);
        return 0;
    }
    if (message == show_fixture_menu) {
        RECT rect{};
        GetWindowRect(window, &rect);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 201, L"Owned fixture menu item");
        fixture_menu_active = true;
        const auto command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
            rect.left + 80, rect.top + 60, window, nullptr);
        if (command == 201) ++fixture_menu_commands;
        fixture_menu_active = false;
        DestroyMenu(menu);
        return 0;
    }
    if (message == close_fixture_menu) { if (fixture_menu_active) EndMenu(); return 0; }
    if (message == focus_button) { SetFocus(GetDlgItem(window, 102)); return 0; }
    if (message == WM_DESTROY) { ole_fixture.reset(); PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
json call(SmokeBackend& backend, json request) {
    // Direct mode receives the same private appearance fields as the real
    // broker. Helper mode must still use its own configured override below.
    request["pointer_appearance"] = {{"style", fixture_pointer_style}, {"color", fixture_pointer_color}};
    auto result = backend.dispatch(request);
    if (!result.value("success", false)) throw std::runtime_error(result.dump());
    return result;
}

void configure_fixture_pointer(const std::string& style, const std::string& color) {
    acecode::computer_use::set_pointer_appearance(style, color);
    fixture_pointer_style = style;
    fixture_pointer_color = color;
}

struct RestorePointerAppearance {
    ~RestorePointerAppearance() {
        try { configure_fixture_pointer(acecode::computer_use::pointer_appearance::kDefaultStyle,
            acecode::computer_use::pointer_appearance::kDefaultColor); } catch (...) {}
    }
};

int element_index(const json& state, const std::string& automation_id) {
    for (const auto& element : state["output"]["accessibility"]["elements"])
        if (element.value("automation_id", std::string()) == automation_id) return element["index"].get<int>();
    throw std::runtime_error("Fixture accessibility element not found: " + automation_id);
}

std::wstring edit_text() {
    wchar_t value[512]{};
    SendMessageW(edit_control.load(), WM_GETTEXT, 512, reinterpret_cast<LPARAM>(value));
    return value;
}
} // namespace

int main(int argc, char** argv) {
    // The launch action intentionally accepts no arbitrary command arguments.
    // A copied test executable recognizes its own dedicated fixture basename.
    wchar_t executable_path[32768]{};
    GetModuleFileNameW(nullptr, executable_path, 32768);
    const std::filesystem::path executable(executable_path);
    if (executable.filename() == L"acecode-computer-use-launch-fixture.exe") {
        auto marker = executable;
        marker.replace_extension(L"marker");
        std::ofstream output(marker);
        output << "owned launch fixture " << GetCurrentProcessId() << '\n';
        return output.good() ? 0 : 3;
    }
    const bool observe_only = argc == 2 && std::string(argv[1]) == "--observe-owned-window";
    const bool wait_for_foreground = argc == 2 && std::string(argv[1]) == "--wait-for-foreground";
    const bool via_helper = argc == 2 && std::string(argv[1]) == "--run-owned-window-via-helper";
    if (argc != 2 || (!observe_only && !wait_for_foreground && !via_helper && std::string(argv[1]) != "--run-owned-window")) {
        std::cerr << "Use --run-owned-window or --run-owned-window-via-helper for temporary-window input, --wait-for-foreground to wait for its title bar to be clicked, or --observe-owned-window for capture and launch checks.\n";
        return 2;
    }
    HWND previous = GetForegroundWindow();
    std::thread ui([observe_only] {
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASSW klass{};
        klass.lpfnWndProc = fixture_proc;
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpszClassName = L"ACECodeComputerUseFixture";
        klass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&klass);
        HWND window = CreateWindowW(klass.lpszClassName, L"ACECode Computer Use verification", WS_OVERLAPPEDWINDOW,
            160, 160, 460, 290, nullptr, nullptr, klass.hInstance, nullptr);
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
        if (!observe_only) SetForegroundWindow(window);
        fixture = window;
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    });
    int result = 0;
    try {
        for (int attempt = 0; !fixture && attempt < 100; ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        require(fixture != nullptr, "Fixture failed to start");
        HWND window = fixture.load();
        DWORD foreground_pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
        std::cout << "Fixture visible=" << IsWindowVisible(window) << " foreground=" << (GetForegroundWindow() == window)
                  << " current_pid=" << GetCurrentProcessId() << " foreground_pid=" << foreground_pid
                  << " backend=" << (via_helper ? "broker-helper" : "direct") << '\n';
        auto id = reinterpret_cast<std::uintptr_t>(window);
        SmokeBackend backend(via_helper);
        RestorePointerAppearance restore_pointer_appearance;
        auto launch_directory = std::filesystem::temp_directory_path() / ("acecode-computer-use-launch-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(launch_directory);
        auto launch_path = launch_directory / "acecode-computer-use-launch-fixture.exe";
        std::filesystem::copy_file(executable, launch_path);
        auto launch_marker = launch_path;
        launch_marker.replace_extension("marker");
        call(backend, {{"action", "launch_app"}, {"app", launch_path.u8string()}});
        for (int attempt = 0; !std::filesystem::exists(launch_marker) && attempt < 100; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        require(std::filesystem::exists(launch_marker), "Owned executable was not launched");
        std::error_code remove_error;
        for (int attempt = 0; std::filesystem::exists(launch_path) && attempt < 100; ++attempt) {
            std::filesystem::remove(launch_path, remove_error);
            if (remove_error) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::filesystem::remove(launch_marker, remove_error);
        std::filesystem::remove(launch_directory, remove_error);
        auto listed = call(backend, {{"action", "list_windows"}});
        bool found = false;
        for (const auto& item : listed["output"]["windows"]) if (item["id"] == id) found = true;
        require(found, "Fixture was absent from window discovery");
        const auto recovered = call(backend, {{"action", "get_window"}, {"window", id}});
        require(recovered["output"]["window"]["id"] == id, "Window recovery returned a different window");
        call(backend, {{"action", "get_window"}, {"window", id}, {"app", recovered["output"]["window"]["app"]}});
        const auto mismatch = backend.dispatch({{"action", "get_window"}, {"window", id}, {"app", "unrelated-fixture-application"}});
        require(!mismatch["success"].get<bool>() && mismatch["error"] == "window_app_mismatch", "Window recovery accepted an unrelated application");
        auto observe = [&] { return call(backend, {{"action", "get_window_state"}, {"window", id}, {"include_text", true}}); };
        auto state = observe();
        require(state["attachments"].size() == 1, "Expected a PNG screenshot attachment");
        const auto& screenshot = state["output"]["screenshots"][0];
        require(screenshot["capture_method"] == "windows_graphics_capture", "MSVC smoke must exercise Windows Graphics Capture");
        require(screenshot["width"].get<int>() <= 2560 && screenshot["height"].get<int>() <= 2560, "Screenshot exceeds provider dimensions");
        auto png = acecode::base64_decode(state["attachments"][0]["data_url"].get<std::string>().substr(22));
        require(png && png->size() > 24 && png->substr(1, 3) == "PNG", "Screenshot attachment is not PNG");
        auto png_dimension = [](const std::string& bytes, std::size_t start) {
            std::uint32_t value = 0;
            for (std::size_t index = 0; index < 4; ++index) value = (value << 8) | static_cast<unsigned char>(bytes[start + index]);
            return value;
        };
        require(png_dimension(*png, 16) == screenshot["width"] && png_dimension(*png, 20) == screenshot["height"], "PNG dimensions differ from screenshot metadata");
        require(state["output"].dump().find("private-password-fixture") == std::string::npos, "Password value leaked into observation");
        require(state["output"]["accessibility"].value("selected_elements", json::array()).dump().find("Selected fixture item") != std::string::npos,
            "Selected list item was absent from accessibility context");
        SendMessageW(window, show_overlay, 0, 0);
        require(overlay != nullptr, "Owned occluder did not open");
        auto occluded = observe();
        require(occluded["output"]["screenshots"][0]["capture_method"] == "windows_graphics_capture", "Obscured target did not use WGC");
        SendMessageW(window, hide_overlay, 0, 0);
        SetWindowPos(window, nullptr, 160, 160, 3500, 800, SWP_NOZORDER | SWP_NOACTIVATE);
        DwmFlush();
        auto scaled = observe();
        const auto& scaled_geometry = scaled["output"]["geometry"];
        require(scaled_geometry["native_width"].get<int>() > 2560 && scaled_geometry["width"].get<int>() == 2560, "Large window was not scaled to provider limit");
        auto scaled_png = acecode::base64_decode(scaled["attachments"][0]["data_url"].get<std::string>().substr(22));
        require(scaled_png && png_dimension(*scaled_png, 16) == scaled_geometry["width"]
            && png_dimension(*scaled_png, 20) == scaled_geometry["height"], "Scaled PNG dimensions differ from coordinates");
        SetWindowPos(window, nullptr, 160, 160, 460, 290, SWP_NOZORDER | SWP_NOACTIVATE);
        DwmFlush();
        std::cout << "PASS: owned executable launch, window discovery, WGC including obscured capture, PNG dimensions, large-window scaling, UIA password redaction\n";
        SendMessageW(window, show_related_popups, 0, 0);
        DwmFlush();
        auto group = observe();
        const auto popup_id = reinterpret_cast<std::uintptr_t>(related_popup.load());
        const auto unrelated_id = reinterpret_cast<std::uintptr_t>(unrelated_popup.load());
        std::string popup_screenshot;
        for (const auto& shot : group["output"]["screenshots"]) {
            require(shot["window"] != unrelated_id, "Same-thread unrelated popup was included in observation");
            if (shot["window"] == popup_id) popup_screenshot = shot["id"].get<std::string>();
        }
        if (popup_screenshot.empty()) throw std::runtime_error("Owned popup was absent from related screenshots: "
            + json{{"surfaces", group["output"].value("surfaces", json::array())},
                   {"warnings", group["output"].value("warnings", json::array())}, {"expected", popup_id}}.dump());
        require(group["attachments"].size() == group["output"]["screenshots"].size(), "Related screenshots and attachments differ in number");
        for (const auto& attachment : group["attachments"]) {
            const auto& metadata = attachment["metadata"]["computer_use"];
            require(metadata["observation_id"] == group["output"]["observation_id"], "Attachment observation identity differs");
            auto image = acecode::base64_decode(attachment["data_url"].get<std::string>().substr(22));
            require(image && png_dimension(*image, 16) == metadata["geometry"]["width"]
                && png_dimension(*image, 20) == metadata["geometry"]["height"], "Related image dimensions differ from coordinate metadata");
        }
        SendMessageW(window, move_related_popup, 0, 0);
        DwmFlush();
        auto changed_popup = backend.dispatch({{"action", "click"}, {"window", id},
            {"observation_id", group["output"]["observation_id"]}, {"screenshot_id", popup_screenshot}, {"x", 10}, {"y", 10}});
        require(!changed_popup["success"].get<bool>() && changed_popup["error"] == "stale_surface", "Moved popup observation was accepted");
        group = observe();
        SendMessageW(window, hide_related_popups, 0, 0);
        auto closed_popup = backend.dispatch({{"action", "press_key"}, {"window", id},
            {"observation_id", group["output"]["observation_id"]}, {"key", "Enter"}});
        require(!closed_popup["success"].get<bool>() && closed_popup["error"] == "stale_surface", "Closed popup observation was accepted for keyboard input");
        std::cout << "PASS: related popup capture, per-image identity and dimensions, unrelated popup exclusion, moved/closed popup rejection\n";
        // Hit testing itself needs no input or foreground focus. Keep the owned
        // fixture visible briefly so a same-window sibling overlay can be tested.
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        DwmFlush();
        IUIAutomation* automation_raw = nullptr;
        require(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_IUIAutomation, reinterpret_cast<void**>(&automation_raw))), "Hit-test UI Automation initialization failed");
        std::unique_ptr<IUIAutomation, void(*)(IUIAutomation*)> automation(automation_raw, [](IUIAutomation* value) { value->Release(); });
        IUIAutomationElement* button_raw = nullptr;
        require(SUCCEEDED(automation->ElementFromHandle(GetDlgItem(window, 102), &button_raw)), "Hit-test button was not found");
        std::unique_ptr<IUIAutomationElement, void(*)(IUIAutomationElement*)> button(button_raw, [](IUIAutomationElement* value) { value->Release(); });
        RECT button_rect{}, window_rect{};
        require(SUCCEEDED(button->get_CurrentBoundingRectangle(&button_rect)), "Cannot read button bounds");
        GetWindowRect(window, &window_rect);
        auto click_target = acecode::computer_use::detail::resolve_element_click_target(automation.get(), button.get(), window, button_rect, window_rect);
        require(click_target.error == acecode::computer_use::detail::ElementTargetError::none, "Visible owned button has no verified click target");
        SendMessageW(window, show_element_overlay, 0, 0);
        DwmFlush();
        click_target = acecode::computer_use::detail::resolve_element_click_target(automation.get(), button.get(), window, button_rect, window_rect);
        require(click_target.error == acecode::computer_use::detail::ElementTargetError::obscured, "Same-window sibling overlay was accepted as the selected button");
        SendMessageW(window, hide_element_overlay, 0, 0);
        SendMessageW(window, show_combo, 0, 0);
        COMBOBOXINFO combo_info{};
        combo_info.cbSize = sizeof(combo_info);
        require(GetComboBoxInfo(GetDlgItem(window, 106), &combo_info) && IsWindowVisible(combo_info.hwndList), "Owned ComboBox did not open its dropdown");
        auto combo_state = observe();
        bool found_combo = false;
        for (const auto& shot : combo_state["output"]["screenshots"])
            if (shot["window"] == reinterpret_cast<std::uintptr_t>(combo_info.hwndList) && shot["relation"] == "combo_list") found_combo = true;
        require(found_combo, "Standard ComboBox dropdown was absent from related screenshots");
        auto ambiguous_image = backend.dispatch({{"action", "click"}, {"window", id},
            {"observation_id", combo_state["output"]["observation_id"]}, {"x", 10}, {"y", 10}});
        require(!ambiguous_image["success"].get<bool>() && ambiguous_image["error"] == "screenshot_required", "Multiple screenshots accepted an implicit coordinate target");
        combo_state = observe();
        SendMessageW(window, hide_combo, 0, 0);
        auto closed_combo = backend.dispatch({{"action", "press_key"}, {"window", id},
            {"observation_id", combo_state["output"]["observation_id"]}, {"key", "Enter"}});
        require(!closed_combo["success"].get<bool>() && closed_combo["error"] == "stale_surface", "Closed ComboBox dropdown observation was accepted");
        PostMessageW(window, show_fixture_menu, 0, 0);
        for (int attempt = 0; !fixture_menu_active && attempt < 40; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        require(fixture_menu_active, "Owned standard popup menu did not open");
        auto menu_state = observe();
        bool found_menu = false;
        for (const auto& shot : menu_state["output"]["screenshots"])
            if (shot["relation"] == "menu") found_menu = true;
        require(found_menu, "Standard popup menu was absent from related screenshots");
        SendMessageW(window, close_fixture_menu, 0, 0);
        for (int attempt = 0; fixture_menu_active && attempt < 40; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        require(!fixture_menu_active, "Owned popup menu did not close");
        auto closed_menu = backend.dispatch({{"action", "press_key"}, {"window", id},
            {"observation_id", menu_state["output"]["observation_id"]}, {"key", "Enter"}});
        require(!closed_menu["success"].get<bool>() && closed_menu["error"] == "stale_surface", "Closed standard menu observation was accepted");
        SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        std::cout << "PASS: UIA clickable-point targeting, same-window overlays, standard ComboBox/menu capture, screenshot ambiguity and closed transient rejection\n";
        if (observe_only) {
            PostMessageW(window, WM_CLOSE, 0, 0);
            ui.join();
            return 0;
        }
        if (wait_for_foreground) {
            SetWindowTextW(window, L"ACECode test - click this title bar to verify input");
            SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            std::cout << "WAIT: click the owned test window title bar to grant foreground focus (120 seconds).\n" << std::flush;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
            while (GetForegroundWindow() != window && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            require(GetForegroundWindow() == window, "Owned test window did not receive foreground focus; no input was sent");
            const auto released = [] {
                for (int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN})
                    if (GetAsyncKeyState(key) & 0x8000) return false;
                return true;
            };
            for (int attempt = 0; !released() && attempt < 100; ++attempt)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            require(GetForegroundWindow() == window && released(), "Test window focus or released input was not stable; no input was sent");
            SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        RECT pointer_button{};
        require(GetWindowRect(GetDlgItem(window, 102), &pointer_button), "Cannot locate the pointer verification control");
        const POINT pointer_target{pointer_button.left + 30, pointer_button.top + 18};
        require(SetCursorPos(pointer_target.x, pointer_target.y), "Cannot position the cursor inside the owned fixture");
        state = observe();
        const auto& system_pointer = state["output"]["screenshots"][0]["cursor"];
        require(system_pointer.value("visible", false) && system_pointer.value("source", "") == "system",
            "Window capture omitted the real system cursor inside its target");
        const auto verify_agent_pointer = [&](const json& observation, const std::string& artifact_name) {
            const auto& shot = observation["output"]["screenshots"][0];
            const auto& cursor = shot["cursor"];
            require(cursor.value("visible", false) && cursor.value("source", "") == "agent",
                "Successful input did not produce an agent pointer in the next capture");
            require(observation["attachments"][0]["metadata"]["computer_use"]["cursor"] == cursor,
                "Pointer attachment metadata differs from its screenshot");
            const auto x = cursor["x"].get<double>() * shot["scaleX"].get<double>() + shot["originX"].get<double>();
            const auto y = cursor["y"].get<double>() * shot["scaleY"].get<double>() + shot["originY"].get<double>();
            require(x >= shot["originX"].get<double>() && y >= shot["originY"].get<double>()
                && cursor["x"].get<double>() < shot["width"].get<double>() && cursor["y"].get<double>() < shot["height"].get<double>(),
                "Pointer coordinates lie outside the returned image");
            HWND visual = nullptr;
            EnumWindows([](HWND candidate, LPARAM context) -> BOOL {
                wchar_t klass[128]{};
                GetClassNameW(candidate, klass, 128);
                if (IsWindowVisible(candidate) && std::wstring(klass).find(L"ACECode.ComputerUse.PointerOverlay.") == 0) {
                    *reinterpret_cast<HWND*>(context) = candidate;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&visual));
            require(visual != nullptr, "No visible desktop pointer feedback window exists");
            const auto style = GetWindowLongPtrW(visual, GWL_EXSTYLE);
            require((style & (WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT))
                == (WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT), "Pointer feedback can intercept user input");
            RECT visual_bounds{};
            require(GetWindowRect(visual, &visual_bounds), "Cannot read the visible pointer geometry");
            require(std::abs(visual_bounds.left + cursor["hotspot_x"].get<double>() * shot["scaleX"].get<double>() - x) < 1.1
                && std::abs(visual_bounds.top + cursor["hotspot_y"].get<double>() * shot["scaleY"].get<double>() - y) < 1.1,
                "Desktop pointer hotspot and scaled screenshot coordinates disagree");
            require(GetForegroundWindow() == window, "Pointer feedback stole foreground focus");
            const auto listing = call(backend, {{"action", "list_windows"}});
            for (const auto& item : listing["output"]["windows"])
                require(item["id"] != reinterpret_cast<std::uintptr_t>(visual), "Pointer overlay leaked into the tool window list");
            auto image = acecode::base64_decode(observation["attachments"][0]["data_url"].get<std::string>().substr(22));
            require(image.has_value(), "Pointer screenshot cannot be decoded");
            const auto artifact = std::filesystem::temp_directory_path() / artifact_name;
            std::ofstream file(artifact, std::ios::binary);
            file.write(image->data(), static_cast<std::streamsize>(image->size()));
            require(file.good(), "Cannot save pointer verification screenshot");
            std::cout << "Pointer capture: " << artifact.u8string() << '\n';
            return visual;
        };
        auto old_observation = state["output"]["observation_id"];
        call(backend, {{"action", "set_value"}, {"window", id}, {"observation_id", old_observation},
            {"element_index", element_index(state, "101")}, {"value", "ACECode Unicode \xe4\xb8\xad\xe6\x96\x87"}});
        require(edit_text() == L"ACECode Unicode \u4e2d\u6587", "UIA set_value failed");
        auto stale = backend.dispatch({{"action", "click"}, {"window", id}, {"observation_id", old_observation}, {"element_index", 0}});
        require(!stale["success"].get<bool>() && stale["error"] == "stale_observation", "Consumed observation was accepted");
        state = observe();
        verify_agent_pointer(state, "acecode-computer-use-pointer-uia.png");
        const auto verify_png_pointer_fill = [&](const json& observation, unsigned char blue, unsigned char green, unsigned char red) {
            const auto& shot = observation["output"]["screenshots"][0];
            const auto& cursor = shot["cursor"];
            const double dpi_scale = GetDpiForWindow(window) / 96.0;
            const int x = static_cast<int>(std::floor(cursor["x"].get<double>() + 5 * dpi_scale / shot["scaleX"].get<double>()));
            const int y = static_cast<int>(std::floor(cursor["y"].get<double>() + 10 * dpi_scale / shot["scaleY"].get<double>()));
            auto png = acecode::base64_decode(observation["attachments"][0]["data_url"].get<std::string>().substr(22));
            require(png.has_value(), "Cannot decode themed pointer PNG");
            Microsoft::WRL::ComPtr<IWICImagingFactory> imaging;
            require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(imaging.GetAddressOf()))), "Cannot initialize pointer PNG verification");
            Microsoft::WRL::ComPtr<IWICStream> stream;
            require(SUCCEEDED(imaging->CreateStream(stream.GetAddressOf()))
                && SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(png->data()), static_cast<DWORD>(png->size()))),
                "Cannot open themed pointer PNG bytes");
            Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
            Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
            Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
            require(SUCCEEDED(imaging->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()))
                && SUCCEEDED(decoder->GetFrame(0, frame.GetAddressOf()))
                && SUCCEEDED(imaging->CreateFormatConverter(converter.GetAddressOf()))
                && SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                    nullptr, 0, WICBitmapPaletteTypeCustom)), "Cannot decode themed pointer image pixels");
            WICRect sample{x, y, 1, 1};
            BYTE pixel[4]{};
            require(SUCCEEDED(converter->CopyPixels(&sample, 4, 4, pixel)), "Pointer fill sample lies outside its PNG");
            require(pixel[0] == blue && pixel[1] == green && pixel[2] == red && pixel[3] == 255,
                "Actual PNG pointer fill differs from configured theme color");
        };
        verify_png_pointer_fill(state, 0xeb, 0x63, 0x25);
        configure_fixture_pointer("plain", "#f97316");
        const auto plain_pointer = observe();
        verify_agent_pointer(plain_pointer, "acecode-computer-use-pointer-plain-theme.png");
        verify_png_pointer_fill(plain_pointer, 0x16, 0x73, 0xf9);
        configure_fixture_pointer("ace", "#f97316");
        const auto ace_pointer = observe();
        verify_agent_pointer(ace_pointer, "acecode-computer-use-pointer-ace-theme.png");
        verify_png_pointer_fill(ace_pointer, 0x16, 0x73, 0xf9);
        const auto& plain_cursor = plain_pointer["output"]["screenshots"][0]["cursor"];
        const auto& ace_cursor = ace_pointer["output"]["screenshots"][0]["cursor"];
        require(ace_cursor["width"].get<double>() > plain_cursor["width"].get<double>()
            && ace_cursor["hotspot_x"] == plain_cursor["hotspot_x"] && ace_cursor["hotspot_y"] == plain_cursor["hotspot_y"],
            "Switching ACE/plain did not update sprite shape or changed the input hotspot");
        configure_fixture_pointer(acecode::computer_use::pointer_appearance::kDefaultStyle,
            acecode::computer_use::pointer_appearance::kDefaultColor);
        state = observe();
        std::cout << "PASS: ACE/plain desktop pointer shapes and actual PNG theme pixels preserve the hotspot\n";
        call(backend, {{"action", "click"}, {"window", id}, {"observation_id", state["output"]["observation_id"]},
            {"element_index", element_index(state, "102")}});
        for (int attempt = 0; clicks == 0 && attempt < 40; ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        if (clicks != 1) {
            POINT cursor{};
            GetCursorPos(&cursor);
            std::cerr << "Owned click diagnostic: " << json{{"clicks", clicks.load()}, {"cursor_x", cursor.x}, {"cursor_y", cursor.y},
                {"foreground", reinterpret_cast<std::uintptr_t>(GetForegroundWindow())}, {"expected", id},
                {"hit", reinterpret_cast<std::uintptr_t>(WindowFromPoint(cursor))},
                {"button", reinterpret_cast<std::uintptr_t>(GetDlgItem(window, 102))}}.dump() << '\n';
        }
        require(clicks == 1, "SendInput did not click the fixture button");
        state = observe();
        verify_agent_pointer(state, "acecode-computer-use-pointer-click.png");
        call(backend, {{"action", "perform_secondary_action"}, {"window", id}, {"observation_id", state["output"]["observation_id"]},
            {"element_index", element_index(state, "101")}, {"secondary_action", "Focus"}});
        state = observe();
        // Plain Win32 EDIT does not implement Ctrl+A. Exercise its supported
        // navigation chords and verify the selection before replacing text.
        call(backend, {{"action", "press_key"}, {"window", id}, {"observation_id", state["output"]["observation_id"]}, {"key", "Ctrl+Home"}});
        state = observe();
        call(backend, {{"action", "press_key"}, {"window", id}, {"observation_id", state["output"]["observation_id"]}, {"key", "Ctrl+Shift+End"}});
        DWORD selected_start = 0, selected_end = 0;
        for (int attempt = 0; attempt < 40; ++attempt) {
            SendMessageW(edit_control.load(), EM_GETSEL, reinterpret_cast<WPARAM>(&selected_start), reinterpret_cast<LPARAM>(&selected_end));
            if (selected_start == 0 && selected_end == edit_text().size()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        require(selected_start == 0 && selected_end == edit_text().size(), "Native navigation chord did not select the owned edit text");
        const std::wstring typed_text = L"Typed \u4e2d\u6587 \U0001d11e";
        state = observe();
        call(backend, {{"action", "type_text"}, {"window", id}, {"observation_id", state["output"]["observation_id"]}, {"text", "Typed \xe4\xb8\xad\xe6\x96\x87 \xf0\x9d\x84\x9e"}});
        for (int attempt = 0; edit_text() != typed_text && attempt < 40; ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        if (edit_text() != typed_text) {
            const auto actual = edit_text();
            DWORD selection_start = 0, selection_end = 0;
            SendMessageW(edit_control.load(), EM_GETSEL, reinterpret_cast<WPARAM>(&selection_start), reinterpret_cast<LPARAM>(&selection_end));
            std::cerr << "Owned edit diagnostic: " << json{{"utf16", std::vector<unsigned>(actual.begin(), actual.end())},
                {"selection_start", selection_start}, {"selection_end", selection_end}}.dump() << '\n';
        }
        require(edit_text() == typed_text, "Unicode SendInput typing failed");
        state = observe();
        const auto canvas_before_scroll = canvas_top.load();
        const auto canvas_point = [&](const json& observation, int x, int y) {
            POINT physical{x, y};
            require(ClientToScreen(window, &physical), "Cannot map owned canvas coordinates");
            const auto& geometry = observation["output"]["geometry"];
            return std::pair<double, double>{
                (physical.x - geometry["originX"].get<double>()) / geometry["scaleX"].get<double>(),
                (physical.y - geometry["originY"].get<double>()) / geometry["scaleY"].get<double>()};
        };
        const auto wheel_point = canvas_point(state, canvas_left + 25, canvas_top + 11);
        call(backend, {{"action", "scroll"}, {"window", id}, {"observation_id", state["output"]["observation_id"]},
            {"x", wheel_point.first}, {"y", wheel_point.second}, {"scrollX", 0}, {"scrollY", 120}});
        for (int attempt = 0; (wheel_events == 0 || canvas_top != canvas_before_scroll - 8) && attempt < 40; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        require(wheel_events > 0 && canvas_top == canvas_before_scroll - 8, "Mouse wheel did not scroll the owned canvas content");
        state = observe();
        const auto canvas_before_drag_x = canvas_left.load(), canvas_before_drag_y = canvas_top.load();
        const auto drag_from = canvas_point(state, canvas_before_drag_x + 25, canvas_before_drag_y + 11);
        const auto drag_to = canvas_point(state, canvas_before_drag_x + 115, canvas_before_drag_y + 16);
        call(backend, {{"action", "drag"}, {"window", id}, {"observation_id", state["output"]["observation_id"]},
            {"from_x", drag_from.first}, {"from_y", drag_from.second}, {"to_x", drag_to.first}, {"to_y", drag_to.second}});
        for (int attempt = 0; canvas_drops == 0 && attempt < 40; ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        require(drag_events > 0 && canvas_drops == 1 && canvas_left == canvas_before_drag_x + 90 && canvas_top == canvas_before_drag_y + 5,
            "Native drag did not move the owned canvas object to its expected destination");
        state = observe();
        SendMessageW(window, focus_button, 0, 0);
        auto focus_drift = backend.dispatch({{"action", "type_text"}, {"window", id}, {"observation_id", state["output"]["observation_id"]}, {"text", "must not type"}});
        require(!focus_drift["success"].get<bool>() && focus_drift["error"] == "focus_changed", "Changed keyboard focus was accepted");
        state = observe();
        SetWindowPos(window, nullptr, 180, 170, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        DwmFlush();
        auto moved = backend.dispatch({{"action", "press_key"}, {"window", id}, {"observation_id", state["output"]["observation_id"]}, {"key", "Tab"}});
        require(!moved["success"].get<bool>() && moved["error"] == "stale_observation", "Moved window observation was accepted");
        std::cout << "PASS: UIA set_value, click, focus, key chord, Unicode input, scroll, drag, consumed/moved observation and focus drift rejection\n";
        auto click_observed_label = [&](const json& snapshot, const std::string& label) {
            for (const auto& element : snapshot["output"]["accessibility"]["elements"]) {
                if (element.value("name", std::string()) != label) continue;
                const auto& box = element["bounds"];
                call(backend, {{"action", "click"}, {"window", id}, {"observation_id", snapshot["output"]["observation_id"]},
                    {"screenshot_id", element["screenshot_id"]},
                    {"x", box["x"].get<double>() + box["width"].get<double>() / 2},
                    {"y", box["y"].get<double>() + box["height"].get<double>() / 2}});
                return;
            }
            throw std::runtime_error("Owned transient control was absent from observation: " + label);
        };
        SendMessageW(window, show_combo, 0, 0);
        click_observed_label(observe(), "Second combo item");
        for (int attempt = 0; SendMessageW(GetDlgItem(window, 106), CB_GETCURSEL, 0, 0) != 1 && attempt < 40; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        require(SendMessageW(GetDlgItem(window, 106), CB_GETCURSEL, 0, 0) == 1, "Screenshot coordinates did not select the owned ComboBox item");
        const auto menu_commands_before = fixture_menu_commands.load();
        PostMessageW(window, show_fixture_menu, 0, 0);
        for (int attempt = 0; !fixture_menu_active && attempt < 40; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        require(fixture_menu_active, "Owned menu did not open for input verification");
        click_observed_label(observe(), "Owned fixture menu item");
        for (int attempt = 0; fixture_menu_commands == menu_commands_before && attempt < 40; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        require(!fixture_menu_active && fixture_menu_commands == menu_commands_before + 1, "Native click did not execute the owned menu item");
        std::cout << "PASS: screenshot-targeted input selected the owned ComboBox and menu items\n";
        SendMessageW(window, create_ole_fixture, 0, 0);
        require(ole_fixture != nullptr, "OLE drag fixture failed to initialize");
        const auto ole_id = reinterpret_cast<std::uintptr_t>(ole_fixture->window());
        // Keep only this disposable test surface above unrelated always-on-top
        // windows so fallback pixels and the real drop target are observable.
        SetWindowPos(ole_fixture->window(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        call(backend, {{"action", "activate_window"}, {"window", ole_id}});
        DwmFlush();
        auto ole_state = call(backend, {{"action", "get_window_state"}, {"window", ole_id}});
        const auto ole_capture_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (ole_state["output"]["screenshots"].empty() && std::chrono::steady_clock::now() < ole_capture_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            DwmFlush();
            ole_state = call(backend, {{"action", "get_window_state"}, {"window", ole_id}});
        }
        if (ole_state["output"]["screenshots"].empty()) {
            RECT own_rect{};
            GetWindowRect(ole_fixture->window(), &own_rect);
            for (HWND other = GetTopWindow(nullptr); other && other != ole_fixture->window(); other = GetWindow(other, GW_HWNDNEXT)) {
                RECT other_rect{}, intersection{};
                DWORD process = 0;
                if (!IsWindowVisible(other) || !GetWindowRect(other, &other_rect) || !IntersectRect(&intersection, &own_rect, &other_rect)) continue;
                GetWindowThreadProcessId(other, &process);
                std::cerr << "OLE overlap diagnostic: " << json{{"window", reinterpret_cast<std::uintptr_t>(other)}, {"pid", process},
                    {"rect", {other_rect.left, other_rect.top, other_rect.right, other_rect.bottom}}}.dump() << '\n';
            }
            throw std::runtime_error("Owned OLE window capture failed: " + ole_state["output"].dump());
        }
        POINT source{}, target{};
        require(ole_fixture->screen_points(source, target), "OLE fixture coordinates unavailable");
        const auto& mapping = ole_state["output"]["geometry"];
        auto pixel_x = [&](LONG x) { return (x - mapping["originX"].get<double>()) / mapping["scaleX"].get<double>(); };
        auto pixel_y = [&](LONG y) { return (y - mapping["originY"].get<double>()) / mapping["scaleY"].get<double>(); };
        call(backend, {{"action", "drag"}, {"window", ole_id}, {"observation_id", ole_state["output"]["observation_id"]},
            {"from_x", pixel_x(source.x)}, {"from_y", pixel_y(source.y)}, {"to_x", pixel_x(target.x)}, {"to_y", pixel_y(target.y)}});
        for (int attempt = 0; ole_fixture->state().drag_completions == 0 && attempt < 150; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto ole_result = ole_fixture->state();
        require(ole_result.drag_starts == 1 && ole_result.drag_completions == 1 && ole_result.drops == 1
            && ole_result.last_result == DRAGDROP_S_DROP && ole_result.dropped_text == acecode::computer_use::test::OleDragFixture::payload(),
            "Native drag did not complete a real owned OLE drop");
        SendMessageW(window, close_ole_fixture, 0, 0);
        std::cout << "PASS: native drag completed an actual OLE transfer inside the owned fixture\n";
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        call(backend, {{"action", "activate_window"}, {"window", id}});
        acecode::computer_use::test::verify_secondary_actions(backend, window, [] { return clicks.load(); });
        std::cout << "PASS: UIA Toggle, Select, Expand, Collapse and Invoke changed real controls\n";
        std::vector<RECT> monitor_work_areas;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM context) -> BOOL {
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info)) reinterpret_cast<std::vector<RECT>*>(context)->push_back(info.rcWork);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&monitor_work_areas));
        require(!monitor_work_areas.empty(), "No interactive display was found for coordinate validation");
        const auto click_image_button = [&](const json& snapshot) {
            const auto& elements = snapshot["output"]["accessibility"]["elements"];
            const auto index = element_index(snapshot, "102");
            const auto& button = elements.at(static_cast<std::size_t>(index));
            const auto& box = button["bounds"];
            const auto before = clicks.load();
            call(backend, {{"action", "click"}, {"window", id}, {"observation_id", snapshot["output"]["observation_id"]},
                {"screenshot_id", button["screenshot_id"]},
                {"x", box["x"].get<double>() + box["width"].get<double>() / 2},
                {"y", box["y"].get<double>() + box["height"].get<double>() / 2}});
            for (int attempt = 0; clicks == before && attempt < 40; ++attempt)
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            require(clicks == before + 1, "Screenshot coordinates did not click the expected button");
            POINT cursor{};
            GetCursorPos(&cursor);
            require(WindowFromPoint(cursor) == GetDlgItem(window, 102), "Screenshot mapping placed the pointer outside the intended control");
        };
        for (const auto& work : monitor_work_areas) {
            require(SetWindowPos(window, HWND_TOPMOST, work.left + 16, work.top + 16, 460, 290, SWP_NOACTIVATE),
                "Cannot position the fixture on an available display");
            DwmFlush();
            click_image_button(observe());
        }
        const auto& work = monitor_work_areas.front();
        require(SetWindowPos(window, HWND_TOPMOST, work.left + 16, work.top + 16, 3000, 1000, SWP_NOACTIVATE),
            "Cannot resize the fixture for scaled screenshot input");
        DwmFlush();
        const auto scaled_input = observe();
        require(scaled_input["output"]["geometry"]["scaleX"].get<double>() > 1.0, "Scaled input test did not produce a reduced screenshot");
        click_image_button(scaled_input);
        const auto final_pointer = verify_agent_pointer(observe(), "acecode-computer-use-pointer-scaled.png");
        std::cout << "PASS: screenshot-coordinate clicks on " << monitor_work_areas.size()
            << " available display(s) and a scaled large-window capture\n";
        call(backend, {{"action", "release"}});
        require(!IsWindowVisible(final_pointer), "Releasing the tool left its pointer visible on the desktop");
        std::cout << "PASS: system cursor capture, visible desktop feedback after UIA and click, pointer hotspot scaling and release\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    if (fixture) {
        SendMessageW(fixture.load(), hide_overlay, 0, 0);
        SendMessageW(fixture.load(), close_fixture_menu, 0, 0);
        SendMessageW(fixture.load(), hide_related_popups, 0, 0);
        PostMessageW(fixture.load(), WM_CLOSE, 0, 0);
    }
    ui.join();
    if (previous && IsWindow(previous)) SetForegroundWindow(previous);
    return result;
}
