#include "native_windows.hpp"
#include "element_target.hpp"
#include "keyboard_input.hpp"
#include "coordinate_transform.hpp"
#include "application_identity.hpp"
#include "accessibility_context.hpp"
#include "surface_policy.hpp"
#include "pointer_overlay.hpp"
#include "pointer_capture.hpp"
#include "pointer_appearance.hpp"
#include "utils/base64.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ole2.h>
#include <uiautomation.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <appmodel.h>
#if __has_include(<winrt/Windows.Graphics.Capture.h>)
#define ACECODE_HAS_WGC 1
#include <d3d11.h>
#include <dxgi1_2.h>
#include <roapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#else
#define ACECODE_HAS_WGC 0
#endif
#endif

namespace acecode::computer_use {
using json = nlohmann::json;

namespace {
struct NativeError : std::runtime_error {
    std::string code;
    NativeError(std::string code_, std::string message) : std::runtime_error(std::move(message)), code(std::move(code_)) {}
};

std::string required_string(const json& request, const char* key, std::size_t maximum = 16384) {
    if (!request.contains(key) || !request[key].is_string()) throw NativeError("invalid_argument", std::string(key) + " must be a string.");
    auto value = request[key].get<std::string>();
    if (value.size() > maximum) throw NativeError("invalid_argument", std::string(key) + " is too long.");
    return value;
}

double number(const json& request, const char* key) {
    if (!request.contains(key) || !request[key].is_number()) throw NativeError("invalid_argument", std::string(key) + " must be a number.");
    double result = request[key].get<double>();
    if (!std::isfinite(result)) throw NativeError("invalid_argument", std::string(key) + " must be finite.");
    return result;
}

#ifdef _WIN32
template<class T> class ComRef {
public:
    ComRef() = default;
    ~ComRef() { if (value_) value_->Release(); }
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    ComRef(ComRef&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
    ComRef& operator=(ComRef&& other) noexcept {
        if (this != &other) { if (value_) value_->Release(); value_ = other.value_; other.value_ = nullptr; }
        return *this;
    }
    T* get() const { return value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    T** put() { if (value_) value_->Release(); value_ = nullptr; return &value_; }
    void** out() { return reinterpret_cast<void**>(put()); }
private:
    T* value_ = nullptr;
};

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream message;
        message << operation << " failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(result)
                << "). The target may be unavailable or protected by Windows permissions.";
        throw NativeError("native_error", message.str());
    }
}

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count) throw NativeError("invalid_argument", "Text must be valid UTF-8.");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

template<class F> std::string bstr_text(F getter, std::size_t limit = 512) {
    BSTR value = nullptr;
    HRESULT result = getter(&value);
    std::wstring copy;
    if (SUCCEEDED(result) && value) copy.assign(value, std::min<std::size_t>(SysStringLen(value), limit));
    SysFreeString(value);
    return utf8(copy);
}

RECT bounds(HWND window) {
    RECT result{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &result, sizeof(result)))) {
        if (!GetWindowRect(window, &result)) throw NativeError("window_unavailable", "The target window no longer exists.");
    }
    if (result.right <= result.left || result.bottom <= result.top) throw NativeError("window_unavailable", "The target has no visible bounds.");
    return result;
}

bool same_rect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool targetable(HWND window) {
    if (!IsWindow(window) || !IsWindowVisible(window) || window == GetShellWindow() || PointerOverlay::owns_window(window)) return false;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    RECT rect{};
    return !cloaked && GetWindowRect(window, &rect) && rect.right > rect.left && rect.bottom > rect.top;
}

std::uint64_t process_start(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    BOOL ok = GetProcessTimes(process, &created, &exited, &kernel, &user);
    CloseHandle(process);
    return ok ? (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime : 0;
}

std::string process_path(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    std::wstring result(32768, L'\0');
    DWORD length = static_cast<DWORD>(result.size());
    BOOL ok = QueryFullProcessImageNameW(process, 0, result.data(), &length);
    CloseHandle(process);
    if (!ok) return {};
    result.resize(length);
    return utf8(result);
}

std::string window_application_id(HWND window, DWORD pid) {
    ComRef<IPropertyStore> properties;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(window, IID_IPropertyStore, properties.out())) && properties) {
        PROPVARIANT value{};
        if (SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
            std::string id = utf8(value.pwszVal);
            PropVariantClear(&value);
            if (!id.empty()) return id;
        } else PropVariantClear(&value);
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    UINT32 length = 0;
    LONG status = GetApplicationUserModelId(process, &length, nullptr);
    std::wstring id;
    if (status == ERROR_INSUFFICIENT_BUFFER && length > 0 && length <= 32768) {
        id.resize(length);
        status = GetApplicationUserModelId(process, &length, id.data());
        if (status == ERROR_SUCCESS) id.resize(length && id[length - 1] == L'\0' ? length - 1 : length);
        else id.clear();
    }
    CloseHandle(process);
    return utf8(id);
}

json window_json(HWND window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    wchar_t title[1024]{};
    GetWindowTextW(window, title, 1024);
    auto executable = process_path(pid);
    auto app = window_application_id(window, pid);
    return {{"id", reinterpret_cast<std::uintptr_t>(window)}, {"pid", pid}, {"app", app.empty() ? executable : app}, {"executable_path", executable},
            {"title", utf8(title)}, {"minimized", IsIconic(window) != FALSE}};
}

struct RelatedWindow {
    HWND window = nullptr;
    HWND anchor = nullptr;
    DWORD pid = 0;
    DWORD thread = 0;
    surface_policy::Relation relation = surface_policy::Relation::root;
    std::vector<std::uintptr_t> owners;
    std::wstring class_name;
    int z_index = 0;
};

std::vector<std::uintptr_t> owner_chain(HWND candidate, HWND root, DWORD root_pid, bool& same_process) {
    std::vector<std::uintptr_t> owners;
    same_process = true;
    for (HWND owner = GetWindow(candidate, GW_OWNER); owner && owners.size() < 32; owner = GetWindow(owner, GW_OWNER)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(owner, &pid);
        if (pid != root_pid) { same_process = false; break; }
        auto id = reinterpret_cast<std::uintptr_t>(owner);
        if (std::find(owners.begin(), owners.end(), id) != owners.end()) { same_process = false; break; }
        owners.push_back(id);
        if (owner == root) break;
    }
    return owners;
}

bool belongs_to_requested_root(HWND window, HWND root, DWORD root_pid) {
    if (!window) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != root_pid) return false;
    if (window == root || IsChild(root, window)) return true;
    HWND top = GetAncestor(window, GA_ROOT);
    bool same_process = false;
    auto owners = owner_chain(top ? top : window, root, root_pid, same_process);
    return same_process && std::find(owners.begin(), owners.end(), reinterpret_cast<std::uintptr_t>(root)) != owners.end();
}

std::vector<RelatedWindow> related_windows(HWND root) {
    DWORD root_pid = 0;
    GetWindowThreadProcessId(root, &root_pid);
    std::vector<HWND> candidates;
    EnumWindows([](HWND window, LPARAM context) -> BOOL {
        auto& windows = *reinterpret_cast<std::vector<HWND>*>(context);
        if (targetable(window)) windows.push_back(window);
        return windows.size() < 512;
    }, reinterpret_cast<LPARAM>(&candidates));
    std::map<HWND, HWND> combo_anchors;
    struct ComboContext { HWND root; DWORD pid; std::map<HWND, HWND>* anchors; std::size_t visited = 0; } combos{root, root_pid, &combo_anchors};
    auto inspect_combo = [](HWND child, LPARAM context) -> BOOL {
        auto& value = *reinterpret_cast<ComboContext*>(context);
        if (++value.visited > 2048) return FALSE;
        wchar_t class_name[128]{};
        GetClassNameW(child, class_name, 128);
        if (_wcsicmp(class_name, L"ComboBox") != 0) return TRUE;
        COMBOBOXINFO info{};
        info.cbSize = sizeof(info);
        if (GetComboBoxInfo(child, &info) && info.hwndList && targetable(info.hwndList)
            && belongs_to_requested_root(child, value.root, value.pid)) (*value.anchors)[info.hwndList] = child;
        return TRUE;
    };
    EnumChildWindows(root, inspect_combo, reinterpret_cast<LPARAM>(&combos));
    for (HWND candidate : candidates)
        if (candidate != root && belongs_to_requested_root(candidate, root, root_pid))
            EnumChildWindows(candidate, inspect_combo, reinterpret_cast<LPARAM>(&combos));
    for (const auto& combo : combo_anchors)
        if (std::find(candidates.begin(), candidates.end(), combo.first) == candidates.end()) candidates.push_back(combo.first);

    std::vector<RelatedWindow> result;
    int z_index = static_cast<int>(candidates.size());
    for (HWND candidate : candidates) {
        RelatedWindow value;
        value.window = candidate;
        value.thread = GetWindowThreadProcessId(candidate, &value.pid);
        value.z_index = z_index--;
        wchar_t class_name[128]{};
        GetClassNameW(candidate, class_name, 128);
        value.class_name = class_name;
        surface_policy::Evidence evidence;
        evidence.root = reinterpret_cast<std::uintptr_t>(root);
        evidence.candidate = reinterpret_cast<std::uintptr_t>(candidate);
        evidence.root_pid = root_pid;
        evidence.candidate_pid = value.pid;
        evidence.visible = targetable(candidate);
        value.owners = owner_chain(candidate, root, root_pid, evidence.owner_chain_same_process);
        evidence.owner_chain = value.owners;
        evidence.standard_menu = value.class_name == L"#32768";
        evidence.combo_list = _wcsicmp(class_name, L"ComboLBox") == 0;
        if (evidence.standard_menu) {
            GUITHREADINFO gui{};
            gui.cbSize = sizeof(gui);
            if (GetGUIThreadInfo(value.thread, &gui)) {
                evidence.menu_active = (gui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE)) != 0;
                evidence.menu_owner_in_root_tree = belongs_to_requested_root(gui.hwndMenuOwner, root, root_pid);
                evidence.menu_thread_matches = gui.hwndMenuOwner && GetWindowThreadProcessId(gui.hwndMenuOwner, nullptr) == value.thread;
                value.anchor = gui.hwndMenuOwner;
            }
        } else if (evidence.combo_list) {
            auto found = combo_anchors.find(candidate);
            evidence.exact_combo_list_match = found != combo_anchors.end();
            if (found != combo_anchors.end()) value.anchor = found->second;
        } else if (!value.owners.empty()) value.anchor = reinterpret_cast<HWND>(value.owners.front());
        auto relation = surface_policy::classify(evidence);
        if (!relation) continue;
        value.relation = *relation;
        result.push_back(std::move(value));
    }
    // Main image is stable at index zero; related images retain their Z order.
    std::stable_sort(result.begin(), result.end(), [root](const RelatedWindow& a, const RelatedWindow& b) {
        if (a.window == root || b.window == root) return a.window == root && b.window != root;
        return a.z_index < b.z_index;
    });
    return result;
}

json relationship_fingerprint(const std::vector<RelatedWindow>& windows) {
    std::map<std::uintptr_t, json> ordered;
    for (const auto& window : windows)
        ordered[reinterpret_cast<std::uintptr_t>(window.window)] = {{"pid", window.pid}, {"thread", window.thread},
            {"anchor", reinterpret_cast<std::uintptr_t>(window.anchor)}, {"relation", surface_policy::name(window.relation)},
            {"owners", window.owners}, {"class", utf8(window.class_name)}};
    json fingerprint = json::array();
    for (auto& entry : ordered) fingerprint.push_back({entry.first, std::move(entry.second)});
    return fingerprint;
}

HWND requested_window(const json& request) {
    if (!request.contains("window")) throw NativeError("invalid_argument", "window is required.");
    const auto& object = request["window"];
    const auto& id = object.is_object() && object.contains("id") ? object["id"] : object;
    if (!id.is_number_integer() || (id.is_number_integer() && id.get<std::int64_t>() <= 0))
        throw NativeError("invalid_argument", "window must be a positive window id or an object containing id.");
    HWND window = reinterpret_cast<HWND>(id.get<std::uintptr_t>());
    if (!targetable(window)) throw NativeError("window_unavailable", "Window is closed, hidden, or on another desktop. List windows again.");
    if (object.is_object() && object.contains("pid")) {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (!object["pid"].is_number_integer() || object["pid"].get<std::uint64_t>() != pid)
            throw NativeError("stale_window", "Window process identity changed. List windows again.");
    }
    return window;
}

void interactive_desktop() {
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!input) throw NativeError("desktop_unavailable", "The interactive desktop is locked or unavailable.");
    wchar_t current_name[256]{}, input_name[256]{};
    DWORD needed = 0;
    bool ok = GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, current_name, sizeof(current_name), &needed)
        && GetUserObjectInformationW(input, UOI_NAME, input_name, sizeof(input_name), &needed)
        && std::wcscmp(current_name, input_name) == 0;
    CloseDesktop(input);
    if (!ok) throw NativeError("desktop_unavailable", "Computer Use cannot operate a different or secure input desktop.");
}

void ensure_no_held_input() {
    for (int key : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON})
        if (GetAsyncKeyState(key) & 0x8000) throw NativeError("user_input_active", "Release the held keyboard modifiers or mouse buttons, then observe again.");
}

void activate(HWND window, IUIAutomation* automation) {
    interactive_desktop();
    // AttachThreadInput may reset the joined queues' key state, so this guard
    // must precede activation as well as the final SendInput call.
    ensure_no_held_input();
    if (IsIconic(window)) ShowWindowAsync(window, SW_RESTORE);
    SetForegroundWindow(window);
    if (GetForegroundWindow() != window) {
        // A console worker has no window of its own. Briefly join the current
        // foreground input queue while activating the explicitly selected HWND;
        // never synthesize an Alt key into an unrelated application.
        const DWORD own_thread = GetCurrentThreadId();
        const DWORD foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const DWORD target_thread = GetWindowThreadProcessId(window, nullptr);
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        if (foreground_thread && foreground_thread != own_thread && AttachThreadInput(own_thread, foreground_thread, TRUE)) {
            const bool target_attached = target_thread != own_thread && target_thread != foreground_thread
                && AttachThreadInput(own_thread, target_thread, TRUE);
            BringWindowToTop(window);
            SetForegroundWindow(window);
            if (target_attached) AttachThreadInput(own_thread, target_thread, FALSE);
            AttachThreadInput(own_thread, foreground_thread, FALSE);
        }
    }
    if (GetForegroundWindow() != window && automation) {
        ComRef<IUIAutomationElement> target;
        if (SUCCEEDED(automation->ElementFromHandle(window, target.put())) && target) {
            UIA_HWND target_handle = nullptr;
            int target_pid = 0;
            DWORD current_pid = 0;
            GetWindowThreadProcessId(window, &current_pid);
            if (SUCCEEDED(target->get_CurrentNativeWindowHandle(&target_handle)) && target_handle == reinterpret_cast<UIA_HWND>(window)
                && SUCCEEDED(target->get_CurrentProcessId(&target_pid)) && static_cast<DWORD>(target_pid) == current_pid)
                target->SetFocus();
        }
    }
    for (int attempt = 0; attempt != 15 && GetForegroundWindow() != window; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (GetForegroundWindow() != window)
        throw NativeError("foreground_denied", "Windows denied focus to the selected window. Bring it to the foreground and observe it again.");
}

struct Pixels {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> bgra;
    std::string method;
};

void validate_dimensions(int width, int height) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || static_cast<std::uint64_t>(width) * height > 32000000)
        throw NativeError("capture_size", "Window screenshot dimensions exceed the supported capture limit.");
}

#if ACECODE_HAS_WGC
Pixels capture_wgc(HWND window, const RECT& expected, std::chrono::steady_clock::time_point deadline) {
    using namespace winrt::Windows::Graphics::Capture;
    using namespace winrt::Windows::Graphics::DirectX;
    using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
    if (!GraphicsCaptureSession::IsSupported()) throw NativeError("capture_unavailable", "Windows Graphics Capture is unavailable.");
    auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(factory->CreateForWindow(window, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
    const auto size = item.Size();
    validate_dimensions(size.Width, size.Height);
    if (size.Width != expected.right - expected.left || size.Height != expected.bottom - expected.top)
        throw NativeError("capture_geometry_changed", "Window capture geometry changed. Observe the window again.");
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    HRESULT created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put());
    if (FAILED(created)) created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put());
    winrt::check_hresult(created);
    auto dxgi_device = device.as<IDXGIDevice>();
    winrt::com_ptr<IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put()));
    auto capture_device = inspectable.as<IDirect3DDevice>();
    auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(capture_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, size);
    auto session = pool.CreateCaptureSession(item);
    // The shared compositor is the sole cursor owner for WGC and screen copies.
    // Older Windows without this property use the explicit screen fallback.
    if (!winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled")) {
        session.Close();
        pool.Close();
        throw NativeError("capture_cursor_unsupported", "This Windows version cannot disable the embedded capture cursor.");
    }
    session.IsCursorCaptureEnabled(false);
    session.StartCapture();
    Direct3D11CaptureFrame frame{nullptr};
    const auto until = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(3));
    while (!frame && std::chrono::steady_clock::now() < until) {
        frame = pool.TryGetNextFrame();
        if (!frame) std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    if (!frame) { session.Close(); pool.Close(); throw NativeError("capture_timeout", "Windows did not produce a frame for this window."); }
    auto actual = frame.ContentSize();
    if (actual.Width != size.Width || actual.Height != size.Height) {
        frame.Close(); session.Close(); pool.Close();
        throw NativeError("capture_geometry_changed", "Window resized during capture. Observe it again.");
    }
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
    D3D11_TEXTURE2D_DESC descriptor{};
    texture->GetDesc(&descriptor);
    descriptor.Usage = D3D11_USAGE_STAGING;
    descriptor.BindFlags = 0;
    descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    descriptor.MiscFlags = 0;
    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::check_hresult(device->CreateTexture2D(&descriptor, nullptr, staging.put()));
    context->CopyResource(staging.get(), texture.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    Pixels result{size.Width, size.Height, {}, "windows_graphics_capture"};
    result.bgra.resize(static_cast<std::size_t>(size.Width) * size.Height * 4);
    for (int y = 0; y < size.Height; ++y)
        std::memcpy(result.bgra.data() + static_cast<std::size_t>(y) * size.Width * 4,
                    static_cast<unsigned char*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch, size.Width * 4);
    context->Unmap(staging.get(), 0);
    frame.Close(); session.Close(); pool.Close();
    return result;
}
#endif

// A fallback may read only an unobstructed target. Inspect every window above it,
// rather than a few sample points that can miss menus or small overlapping windows.
void ensure_unobstructed(HWND window, const RECT& rect) {
    // Screen capture does not require input focus. Complete Z-order and bounds
    // checks below are the authority for whether these pixels belong to the
    // selected surface, including a visible non-activating popup.
    if (!targetable(window) || IsIconic(window))
        throw NativeError("capture_unavailable", "Visible-screen fallback requires a visible, restored target surface.");
    RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    desktop.right = desktop.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    desktop.bottom = desktop.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (rect.left < desktop.left || rect.top < desktop.top || rect.right > desktop.right || rect.bottom > desktop.bottom)
        throw NativeError("capture_unavailable", "Visible-screen fallback cannot capture a partly off-screen window.");
    // A combo list can be a child HWND. Its own root is the final containing
    // surface in the top-level Z order, rather than an obscuring foreign window.
    HWND top_level = GetAncestor(window, GA_ROOT);
    for (HWND other = GetTopWindow(nullptr); other && other != top_level; other = GetWindow(other, GW_HWNDNEXT)) {
        if (!targetable(other)) continue;
        RECT intersection{}, other_rect{};
        if (GetWindowRect(other, &other_rect) && IntersectRect(&intersection, &rect, &other_rect))
            throw NativeError("capture_obscured", "Visible-screen fallback refused an overlapping window. Expose the target or use Windows Graphics Capture.");
    }
    for (HWND child = window; child && child != top_level; child = GetParent(child)) {
        for (HWND sibling = GetWindow(child, GW_HWNDPREV); sibling; sibling = GetWindow(sibling, GW_HWNDPREV)) {
            if (!targetable(sibling)) continue;
            RECT intersection{}, sibling_rect{};
            if (GetWindowRect(sibling, &sibling_rect) && IntersectRect(&intersection, &rect, &sibling_rect))
                throw NativeError("capture_obscured", "A sibling control overlaps the selected visible-screen surface.");
        }
    }
}

Pixels capture_visible(HWND window, const RECT& rect) {
    ensure_unobstructed(window, rect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    validate_dimensions(width, height);
    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* data = nullptr;
    HBITMAP bitmap = screen ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &data, nullptr, 0) : nullptr;
    HGDIOBJ previous = memory && bitmap ? SelectObject(memory, bitmap) : nullptr;
    BOOL copied = previous && BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
    if (copied) GdiFlush(); // Complete GDI writes before reading the DIB memory.
    Pixels result{width, height, {}, "visible_screen_fallback"};
    if (copied) {
        const auto* bytes = static_cast<unsigned char*>(data);
        result.bgra.assign(bytes, bytes + static_cast<std::size_t>(width) * height * 4);
        for (std::size_t i = 3; i < result.bgra.size(); i += 4) result.bgra[i] = 255;
    }
    if (previous) SelectObject(memory, previous);
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(nullptr, screen);
    if (!copied) throw NativeError("capture_failed", "Windows screen capture failed.");
    ensure_unobstructed(window, rect);
    return result;
}

std::pair<int, int> image_size(int width, int height) {
    double scale = std::min(1.0, 2560.0 / std::max(width, height));
    return {std::max(1, static_cast<int>(std::lround(width * scale))), std::max(1, static_cast<int>(std::lround(height * scale)))};
}

std::string encode_png(const Pixels& pixels, int width, int height) {
    ComRef<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, factory.out()), "Create PNG encoder");
    ComRef<IWICBitmap> bitmap;
    check(factory->CreateBitmapFromMemory(pixels.width, pixels.height, GUID_WICPixelFormat32bppBGRA, pixels.width * 4,
        static_cast<UINT>(pixels.bgra.size()), const_cast<BYTE*>(pixels.bgra.data()), bitmap.put()), "Create screenshot bitmap");
    ComRef<IWICBitmapScaler> scaler;
    IWICBitmapSource* source = bitmap.get();
    if (width != pixels.width || height != pixels.height) {
        check(factory->CreateBitmapScaler(scaler.put()), "Create screenshot scaler");
        check(scaler->Initialize(source, width, height, WICBitmapInterpolationModeFant), "Scale screenshot");
        source = scaler.get();
    }
    ComRef<IStream> stream;
    check(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()), "Create PNG stream");
    ComRef<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()), "Create PNG encoder");
    check(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache), "Initialize PNG encoder");
    ComRef<IWICBitmapFrameEncode> frame;
    check(encoder->CreateNewFrame(frame.put(), nullptr), "Create PNG frame");
    check(frame->Initialize(nullptr), "Initialize PNG frame");
    check(frame->SetSize(width, height), "Set PNG dimensions");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format), "Set PNG format");
    check(frame->WriteSource(source, nullptr), "Encode screenshot");
    check(frame->Commit(), "Commit screenshot frame");
    check(encoder->Commit(), "Commit screenshot");
    HGLOBAL memory = nullptr;
    check(GetHGlobalFromStream(stream.get(), &memory), "Read PNG stream");
    STATSTG stats{};
    check(stream->Stat(&stats, STATFLAG_NONAME), "Read PNG size");
    if (stats.cbSize.QuadPart > 16 * 1024 * 1024) throw NativeError("capture_size", "Encoded screenshot exceeds 16 MiB.");
    auto* bytes = static_cast<char*>(GlobalLock(memory));
    if (!bytes) throw NativeError("capture_failed", "Cannot read encoded screenshot.");
    std::string png(bytes, static_cast<std::size_t>(stats.cbSize.QuadPart));
    GlobalUnlock(memory);
    return "data:image/png;base64," + acecode::base64_encode(png);
}

template<class T> ComRef<T> pattern(IUIAutomationElement* element, PATTERNID id, REFIID iid) {
    ComRef<IUnknown> unknown;
    ComRef<T> result;
    if (SUCCEEDED(element->GetCurrentPattern(id, unknown.put())) && unknown)
        unknown->QueryInterface(iid, result.out());
    return result;
}

INPUT mouse_input(DWORD flags, LONG x = 0, LONG y = 0, DWORD data = 0) {
    INPUT result{};
    result.type = INPUT_MOUSE;
    result.mi.dwFlags = flags;
    result.mi.dx = x;
    result.mi.dy = y;
    result.mi.mouseData = data;
    return result;
}

INPUT move_input(POINT point) {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const auto mapped = coordinate_transform::desktop_to_absolute({point.x, point.y}, {left, top}, width, height);
    if (!mapped)
        throw NativeError("invalid_coordinate", "Point is outside the interactive desktop.");
    return mouse_input(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE_NOCOALESCE, mapped->x, mapped->y);
}

INPUT key_input(WORD key, bool up, bool unicode = false) {
    INPUT result{};
    result.type = INPUT_KEYBOARD;
    result.ki.wVk = unicode ? 0 : key;
    result.ki.wScan = unicode ? key : 0;
    result.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (unicode ? KEYEVENTF_UNICODE : 0);
    if (!unicode && (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN || key == VK_HOME || key == VK_END
        || key == VK_PRIOR || key == VK_NEXT || key == VK_INSERT || key == VK_DELETE || key == VK_RCONTROL || key == VK_RMENU))
        result.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    return result;
}

void send_batch(const std::vector<INPUT>& inputs) {
    if (inputs.empty()) return;
    // No human modifier/button may become part of a synthetic chord accidentally.
    ensure_no_held_input();
    UINT sent = SendInput(static_cast<UINT>(inputs.size()), const_cast<INPUT*>(inputs.data()), sizeof(INPUT));
    if (sent != inputs.size()) {
        std::vector<INPUT> release;
        for (UINT i = 0; i < sent; ++i) {
            INPUT input = inputs[i];
            if (input.type == INPUT_KEYBOARD && !(input.ki.dwFlags & KEYEVENTF_KEYUP)) { input.ki.dwFlags |= KEYEVENTF_KEYUP; release.push_back(input); }
            else if (input.type == INPUT_MOUSE) {
                DWORD up = 0;
                if (input.mi.dwFlags & MOUSEEVENTF_LEFTDOWN) up |= MOUSEEVENTF_LEFTUP;
                if (input.mi.dwFlags & MOUSEEVENTF_RIGHTDOWN) up |= MOUSEEVENTF_RIGHTUP;
                if (input.mi.dwFlags & MOUSEEVENTF_MIDDLEDOWN) up |= MOUSEEVENTF_MIDDLEUP;
                if (up) release.push_back(mouse_input(up));
            }
        }
        if (!release.empty()) SendInput(static_cast<UINT>(release.size()), release.data(), sizeof(INPUT));
        throw NativeError("input_denied", "Windows rejected input. Elevated applications and secure desktops cannot be controlled from this process.");
    }
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

#endif
} // namespace

#ifdef _WIN32
detail::ElementClickTarget detail::resolve_element_click_target(IUIAutomation* automation,
    IUIAutomationElement* element, HWND window, const RECT& element_rect, const RECT& window_rect) {
    ElementClickTarget result;
    if (!automation || !element) return result;
    BOOL clickable = FALSE;
    if (FAILED(element->GetClickablePoint(&result.point, &clickable)) || !clickable) {
        RECT visible{};
        if (!IntersectRect(&visible, &element_rect, &window_rect)) {
            result.error = ElementTargetError::outside_window;
            return result;
        }
        result.point = {visible.left + (visible.right - visible.left) / 2,
                        visible.top + (visible.bottom - visible.top) / 2};
    }
    if (!PtInRect(&element_rect, result.point) || !PtInRect(&window_rect, result.point)) {
        result.error = ElementTargetError::outside_window;
        return result;
    }
    HWND hit_window = WindowFromPoint(result.point);
    if (!hit_window || (hit_window != window && !IsChild(window, hit_window))) {
        result.error = ElementTargetError::obscured;
        return result;
    }
    ComRef<IUIAutomationElement> hit;
    ComRef<IUIAutomationTreeWalker> walker;
    if (FAILED(automation->ElementFromPoint(result.point, hit.put())) || !hit
        || FAILED(automation->get_RawViewWalker(walker.put())) || !walker) return result;
    for (int depth = 0; hit && depth < 64; ++depth) {
        BOOL same = FALSE;
        if (FAILED(automation->CompareElements(element, hit.get(), &same))) return result;
        if (same) {
            result.error = ElementTargetError::none;
            return result;
        }
        ComRef<IUIAutomationElement> parent;
        if (FAILED(walker->GetParentElement(hit.get(), parent.put()))) return result;
        hit = std::move(parent);
    }
    result.error = ElementTargetError::obscured;
    return result;
}

class ScopedPointerSuppression {
public:
    explicit ScopedPointerSuppression(PointerOverlay& overlay) : overlay_(overlay) { overlay_.suppress(true); }
    ~ScopedPointerSuppression() { try { overlay_.suppress(false); } catch (...) {} }
    ScopedPointerSuppression(const ScopedPointerSuppression&) = delete;
    ScopedPointerSuppression& operator=(const ScopedPointerSuppression&) = delete;
private:
    PointerOverlay& overlay_;
};

struct NativeBackend::Impl {
    struct Element { ComRef<IUIAutomationElement> element; RECT rect{}; bool password = false; std::size_t surface = 0; };
    struct Surface {
        RelatedWindow relation;
        RECT rect{};
        int width = 0, height = 0;
        std::string screenshot_id;
        ComRef<IUIAutomationElement> root;
    };
    ComRef<IUIAutomation> automation;
    PointerOverlay pointer_overlay;
    bool apartment_owned = false;
    DPI_AWARENESS_CONTEXT previous_dpi = nullptr;
    HWND observed_window = nullptr;
    DWORD observed_pid = 0;
    std::uint64_t observed_process_start = 0;
    RECT observed_rect{};
    int image_width = 0, image_height = 0;
    std::string observation_id;
    std::string screenshot_id;
    std::string worker_nonce;
    std::uint64_t generation = 0;
    std::chrono::steady_clock::time_point observed_at;
    std::vector<Element> elements;
    ComRef<IUIAutomationElement> observed_focus;
    ComRef<IUIAutomationElement> observed_root;
    std::vector<Surface> surfaces;
    json observed_relationships;
    HWND observed_foreground = nullptr;
    json observed_gui;
    HWND input_foreground = nullptr;
    std::set<std::string> installed_apps;
    std::map<std::uintptr_t, std::pair<DWORD, std::string>> discovered_window_apps;

    Impl() {
        previous_dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#if ACECODE_HAS_WGC
        HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
#else
        HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
        apartment_owned = SUCCEEDED(apartment);
        if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) check(apartment, "Initialize Computer Use");
        GUID nonce{};
        check(CoCreateGuid(&nonce), "Create observation identity");
        wchar_t nonce_text[40]{};
        StringFromGUID2(nonce, nonce_text, 40);
        worker_nonce = utf8(nonce_text);
        HRESULT automation_result = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation, automation.out());
        if (FAILED(automation_result)) automation_result = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation, automation.out());
        check(automation_result, "Initialize UI Automation");
        ComRef<IUIAutomation2> automation2;
        if (SUCCEEDED(automation->QueryInterface(IID_IUIAutomation2, automation2.out()))) {
            automation2->put_ConnectionTimeout(1000);
            automation2->put_TransactionTimeout(1000);
        }
    }

    ~Impl() {
        elements.clear();
        observed_focus = {};
        observed_root = {};
        surfaces.clear();
        automation = {};
        if (apartment_owned) {
#if ACECODE_HAS_WGC
            RoUninitialize();
#else
            CoUninitialize();
#endif
        }
        if (previous_dpi) SetThreadDpiAwarenessContext(previous_dpi);
    }

    json list_windows() {
        json result = json::array();
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            if (targetable(window)) {
                auto* output = reinterpret_cast<json*>(parameter);
                output->push_back(window_json(window));
                if (output->size() >= 256) return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&result));
        for (auto& window : result) {
            auto known = discovered_window_apps.find(window["id"].get<std::uintptr_t>());
            if (known != discovered_window_apps.end() && known->second.first == window["pid"].get<DWORD>()) window["app"] = known->second.second;
        }
        return {{"windows", std::move(result)}};
    }

    json describe_window(HWND window) {
        auto result = window_json(window);
        auto known = discovered_window_apps.find(reinterpret_cast<std::uintptr_t>(window));
        if (known != discovered_window_apps.end() && known->second.first == result["pid"].get<DWORD>()) result["app"] = known->second.second;
        return result;
    }

    json geometry(std::size_t index = 0) const {
        const auto& surface = surfaces.at(index);
        return {{"width", surface.width}, {"height", surface.height}, {"native_width", surface.rect.right - surface.rect.left},
            {"native_height", surface.rect.bottom - surface.rect.top}, {"originX", surface.rect.left}, {"originY", surface.rect.top},
            {"scaleX", static_cast<double>(surface.rect.right - surface.rect.left) / surface.width},
            {"scaleY", static_cast<double>(surface.rect.bottom - surface.rect.top) / surface.height}};
    }

    json element_bounds(const RECT& rect, std::size_t index) const {
        const auto& surface = surfaces.at(index);
        double sx = static_cast<double>(surface.width) / (surface.rect.right - surface.rect.left);
        double sy = static_cast<double>(surface.height) / (surface.rect.bottom - surface.rect.top);
        return {{"x", (rect.left - surface.rect.left) * sx}, {"y", (rect.top - surface.rect.top) * sy},
            {"width", (rect.right - rect.left) * sx}, {"height", (rect.bottom - rect.top) * sy}};
    }

    json accessibility(std::size_t surface_index, std::chrono::steady_clock::time_point deadline) {
        const auto& surface = surfaces.at(surface_index);
        HWND window = surface.relation.window;
        ComRef<IUIAutomationElement> root;
        check(automation->ElementFromHandle(window, root.put()), "Find window accessibility root");
        ComRef<IUIAutomationTreeWalker> walker;
        check(automation->get_ControlViewWalker(walker.put()), "Read accessibility tree");
        json output{{"tree", ""}, {"elements", json::array()}, {"selected_elements", json::array()}, {"truncated", false}};
        int document_priority = 0;
        std::ostringstream tree;
        std::function<void(ComRef<IUIAutomationElement>, int)> visit = [&](ComRef<IUIAutomationElement> element, int depth) {
            if (elements.size() >= 256 || depth > 12 || std::chrono::steady_clock::now() > deadline) { output["truncated"] = true; return; }
            UIA_HWND element_handle = nullptr;
            element->get_CurrentNativeWindowHandle(&element_handle);
            if (depth > 0 && element_handle) {
                for (std::size_t other = 0; other < surfaces.size(); ++other)
                    if (other != surface_index && reinterpret_cast<HWND>(element_handle) == surfaces[other].relation.window) return;
            }
            BOOL password = TRUE, offscreen = FALSE, enabled = FALSE, focused = FALSE, focusable = FALSE;
            element->get_CurrentIsPassword(&password);
            element->get_CurrentIsOffscreen(&offscreen);
            element->get_CurrentIsEnabled(&enabled);
            element->get_CurrentHasKeyboardFocus(&focused);
            element->get_CurrentIsKeyboardFocusable(&focusable);
            CONTROLTYPEID control_type = 0;
            element->get_CurrentControlType(&control_type);
            RECT rect{};
            element->get_CurrentBoundingRectangle(&rect);
            std::string name = password ? "[protected]" : bstr_text([&](BSTR* text) { return element->get_CurrentName(text); });
            std::string role = bstr_text([&](BSTR* text) { return element->get_CurrentLocalizedControlType(text); }, 80);
            std::string automation_id = bstr_text([&](BSTR* text) { return element->get_CurrentAutomationId(text); }, 128);
            json actions = json::array();
            bool selection_supported = false;
            const std::pair<PROPERTYID, const char*> supported[] = {
                {UIA_IsInvokePatternAvailablePropertyId, "invoke"}, {UIA_IsValuePatternAvailablePropertyId, "set_value"},
                {UIA_IsTogglePatternAvailablePropertyId, "toggle"}, {UIA_IsSelectionItemPatternAvailablePropertyId, "select"},
                {UIA_IsExpandCollapsePatternAvailablePropertyId, "expand"}, {UIA_IsScrollPatternAvailablePropertyId, "scroll up"}};
            for (const auto& entry : supported) {
                VARIANT available{};
                VariantInit(&available);
                if (SUCCEEDED(element->GetCurrentPropertyValue(entry.first, &available)) && available.vt == VT_BOOL && available.boolVal == VARIANT_TRUE) {
                    actions.push_back(entry.second);
                    if (entry.first == UIA_IsSelectionItemPatternAvailablePropertyId) selection_supported = true;
                    if (entry.first == UIA_IsExpandCollapsePatternAvailablePropertyId) actions.push_back("collapse");
                    if (entry.first == UIA_IsScrollPatternAvailablePropertyId) {
                        actions.push_back("scroll down"); actions.push_back("scroll left"); actions.push_back("scroll right");
                    }
                }
                VariantClear(&available);
            }
            append_accessibility_focus_actions(actions, enabled != FALSE, offscreen != FALSE, focusable != FALSE);
            const std::size_t index = elements.size();
            json item{{"index", index}, {"role", role}, {"name", name}, {"automation_id", automation_id}, {"bounds", element_bounds(rect, surface_index)},
                {"surface_window", reinterpret_cast<std::uintptr_t>(window)}, {"screenshot_id", surface.screenshot_id},
                {"enabled", enabled != FALSE}, {"offscreen", offscreen != FALSE}, {"password", password != FALSE}, {"focused", focused != FALSE}, {"actions", actions}};
            if (!password) {
                auto value = pattern<IUIAutomationValuePattern>(element.get(), UIA_ValuePatternId, IID_IUIAutomationValuePattern);
                if (value) item["value"] = bstr_text([&](BSTR* text) { return value->get_CurrentValue(text); }, 2048);
            }
            std::string line = std::string(static_cast<std::size_t>(depth), '\t') + "[" + std::to_string(index) + "] " + role + " " + name;
            if (password) line += " (password; value redacted)";
            if (item.contains("value")) line += " value=" + item["value"].get<std::string>();
            tree << line << '\n';
            if (focused) output["focused_element"] = line;
            if (selection_supported && !password) {
                auto selection = pattern<IUIAutomationSelectionItemPattern>(element.get(), UIA_SelectionItemPatternId, IID_IUIAutomationSelectionItemPattern);
                BOOL selected = FALSE;
                if (selection && SUCCEEDED(selection->get_CurrentIsSelected(&selected))) {
                    item["selected"] = selected != FALSE;
                    if (selected) output["selected_elements"].push_back(line);
                }
            }
            RECT visible_intersection{};
            const bool intersects_window = IntersectRect(&visible_intersection, &surface.rect, &rect) != FALSE;
            const int candidate_priority = accessibility_document_priority(password != FALSE, offscreen != FALSE || !intersects_window,
                focused != FALSE, control_type == UIA_DocumentControlTypeId);
            if (candidate_priority > 0) {
                auto text_pattern = pattern<IUIAutomationTextPattern>(element.get(), UIA_TextPatternId, IID_IUIAutomationTextPattern);
                if (text_pattern) {
                    if (candidate_priority > document_priority) {
                        ComRef<IUIAutomationTextRange> document;
                        if (SUCCEEDED(text_pattern->get_DocumentRange(document.put())) && document) {
                            auto text = bstr_text([&](BSTR* value) { return document->GetText(8192, value); }, 8192);
                            if (!text.empty()) { output["document_text"] = std::move(text); document_priority = candidate_priority; }
                        }
                    }
                    if (focused) {
                        ComRef<IUIAutomationTextRangeArray> selected;
                        if (SUCCEEDED(text_pattern->GetSelection(selected.put())) && selected) {
                            int count = 0;
                            selected->get_Length(&count);
                            if (count > 0) { ComRef<IUIAutomationTextRange> range; if (SUCCEEDED(selected->GetElement(0, range.put())) && range)
                                output["selected_text"] = bstr_text([&](BSTR* text) { return range->GetText(2048, text); }, 2048); }
                        }
                    }
                }
            }
            output["elements"].push_back(std::move(item));
            IUIAutomationElement* parent = element.get();
            elements.push_back({std::move(element), rect, password != FALSE, surface_index});
            ComRef<IUIAutomationElement> child;
            walker->GetFirstChildElement(parent, child.put());
            while (child) {
                ComRef<IUIAutomationElement> sibling;
                walker->GetNextSiblingElement(child.get(), sibling.put());
                visit(std::move(child), depth + 1);
                if (elements.size() >= 256 || std::chrono::steady_clock::now() > deadline) { output["truncated"] = true; break; }
                child = std::move(sibling);
            }
        };
        visit(std::move(root), 0);
        output["tree"] = tree.str();
        return output;
    }

    bool in_observed_group(HWND window) const {
        return window && std::any_of(surfaces.begin(), surfaces.end(), [window](const Surface& surface) { return surface.relation.window == window; });
    }

    json gui_snapshot(HWND foreground) const {
        const DWORD thread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
        GUITHREADINFO gui{};
        gui.cbSize = sizeof(gui);
        if (!thread || !GetGUIThreadInfo(thread, &gui)) return nullptr;
        return {{"thread", thread}, {"flags", gui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE)},
            {"active", reinterpret_cast<std::uintptr_t>(gui.hwndActive)}, {"focus", reinterpret_cast<std::uintptr_t>(gui.hwndFocus)},
            {"capture", reinterpret_cast<std::uintptr_t>(gui.hwndCapture)}, {"menu_owner", reinterpret_cast<std::uintptr_t>(gui.hwndMenuOwner)}};
    }

    void validate_surfaces() {
        if (relationship_fingerprint(related_windows(observed_window)) != observed_relationships)
            throw NativeError("stale_surface", "A related window, menu, or dropdown changed. Observe the complete window state again.");
        for (const auto& surface : surfaces) {
            HWND window = surface.relation.window;
            DWORD pid = 0;
            DWORD thread = GetWindowThreadProcessId(window, &pid);
            if (!targetable(window) || pid != observed_pid || thread != surface.relation.thread || !same_rect(surface.rect, bounds(window)))
                throw NativeError("stale_surface", "An observed window or popup moved, closed, or changed identity. Observe again.");
            ComRef<IUIAutomationElement> current;
            BOOL same = FALSE;
            UIA_HWND handle = nullptr;
            if (!surface.root || FAILED(surface.root->get_CurrentNativeWindowHandle(&handle)) || reinterpret_cast<HWND>(handle) != window
                || FAILED(automation->ElementFromHandle(window, current.put())) || !current
                || FAILED(automation->CompareElements(surface.root.get(), current.get(), &same)) || !same)
                throw NativeError("stale_surface", "An observed popup was replaced. Observe again.");
        }
    }

    json observe(const json& request) {
        observation_id.clear();
        screenshot_id.clear();
        elements.clear();
        surfaces.clear();
        observed_focus = {};
        observed_root = {};
        HWND window = requested_window(request);
        if (IsIconic(window)) throw NativeError("window_minimized", "Activate the minimized window before observing it.");
        interactive_desktop();
        observed_window = window;
        GetWindowThreadProcessId(window, &observed_pid);
        observed_process_start = process_start(observed_pid);
        check(automation->ElementFromHandle(window, observed_root.put()), "Bind observed window identity");
        auto related = related_windows(window);
        if (related.empty() || related[0].window != window) throw NativeError("window_unavailable", "The selected window is no longer visible.");
        observed_relationships = relationship_fingerprint(related);
        const bool omitted_surfaces = related.size() > 4;
        if (omitted_surfaces) related.erase(related.begin() + 1, related.end() - 3);
        json warnings = json::array();
        if (omitted_surfaces) warnings.push_back("Only the main window and three topmost related surfaces were included. Select a specific returned window to inspect others.");
        for (auto& relation : related) {
            Surface surface;
            surface.relation = std::move(relation);
            surface.rect = bounds(surface.relation.window);
            validate_dimensions(surface.rect.right - surface.rect.left, surface.rect.bottom - surface.rect.top);
            auto size = image_size(surface.rect.right - surface.rect.left, surface.rect.bottom - surface.rect.top);
            surface.width = size.first;
            surface.height = size.second;
            check(automation->ElementFromHandle(surface.relation.window, surface.root.put()), "Bind observed surface identity");
            surfaces.push_back(std::move(surface));
        }
        observed_rect = surfaces[0].rect;
        image_width = surfaces[0].width;
        image_height = surfaces[0].height;
        const std::string id = worker_nonce + "-" + std::to_string(++generation);
        json output{{"window", describe_window(window)}, {"observation_id", id}, {"coordinate_space", "screenshot_pixels"},
            {"geometry", geometry()}, {"screenshots", json::array()}, {"surfaces", json::array()}, {"accessibility", nullptr},
            {"instruction", "Use this observation_id with the original window id for one action, then observe again. For multiple images, select screenshot_id explicitly. Each image and accessibility element has its own surface coordinates."}};
        json attachments = json::array();
        const auto capture_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        std::size_t attachment_bytes = 0;
        constexpr std::size_t max_attachment_bytes = 24 * 1024 * 1024;
        pointer_overlay.settle();
        const auto pointer = pointer_overlay.snapshot();
        for (std::size_t index = 0; index < surfaces.size(); ++index) {
            auto& surface = surfaces[index];
            json descriptor = geometry(index);
            descriptor["window"] = reinterpret_cast<std::uintptr_t>(surface.relation.window);
            descriptor["relation"] = surface_policy::name(surface.relation.relation);
            descriptor["zIndex"] = surface.relation.z_index;
            if (request.value("include_screenshot", true)) {
                std::string fallback_reason;
                try {
                    if (std::chrono::steady_clock::now() >= capture_deadline) throw NativeError("capture_budget", "Shared capture time budget exhausted.");
                    Pixels pixels;
#if ACECODE_HAS_WGC
                    try { pixels = capture_wgc(surface.relation.window, surface.rect, capture_deadline); }
                    catch (const winrt::hresult_error& error) { fallback_reason = utf8(error.message().c_str()); }
                    catch (const NativeError& error) { fallback_reason = error.what(); }
#else
                    fallback_reason = "This build has no Windows Graphics Capture support.";
#endif
                    json cursor;
                    {
                        ScopedPointerSuppression suppression(pointer_overlay);
                        if (pixels.bgra.empty()) {
                            if (std::chrono::steady_clock::now() >= capture_deadline) throw NativeError("capture_budget", "Shared capture time budget exhausted.");
                            DwmFlush(); // Commit the hidden overlay before copying the visible desktop.
                            pixels = capture_visible(surface.relation.window, surface.rect);
                        }
                        cursor = composite_capture_pointer(surface.relation.window, surface.rect,
                            pixels.width, pixels.height, pixels.bgra, &pointer);
                    }
                    if (cursor.value("visible", false)) {
                        const double sx = static_cast<double>(surface.width) / pixels.width;
                        const double sy = static_cast<double>(surface.height) / pixels.height;
                        for (const auto* key : {"x", "hotspot_x", "width"}) cursor[key] = cursor.at(key).get<double>() * sx;
                        for (const auto* key : {"y", "hotspot_y", "height"}) cursor[key] = cursor.at(key).get<double>() * sy;
                    }
                    descriptor["cursor"] = cursor;
                    auto data_url = encode_png(pixels, surface.width, surface.height);
                    if (data_url.size() > max_attachment_bytes - attachment_bytes) throw NativeError("capture_budget", "Shared screenshot byte budget exhausted.");
                    attachment_bytes += data_url.size();
                    surface.screenshot_id = id + "-image-" + std::to_string(index);
                    descriptor["id"] = surface.screenshot_id;
                    descriptor["observation_id"] = id;
                    descriptor["capture_method"] = pixels.method;
                    if (!fallback_reason.empty()) descriptor["fallback_reason"] = fallback_reason;
                    output["screenshots"].push_back(descriptor);
                    attachments.push_back({{"name", "computer-use-" + id + "-" + std::to_string(index) + ".png"}, {"mime_type", "image/png"},
                        {"data_url", std::move(data_url)}, {"metadata", {{"computer_use", {{"observation_id", id}, {"screenshot_id", surface.screenshot_id},
                            {"window", reinterpret_cast<std::uintptr_t>(surface.relation.window)}, {"geometry", geometry(index)}, {"cursor", cursor}}}}}});
                } catch (const NativeError& error) {
                    descriptor["capture_error"] = error.what();
                    if (!fallback_reason.empty()) descriptor["fallback_reason"] = fallback_reason;
                    warnings.push_back({{"window", reinterpret_cast<std::uintptr_t>(surface.relation.window)}, {"error", error.code},
                        {"message", error.what()}, {"fallback_reason", fallback_reason}});
                }
            }
            output["surfaces"].push_back(std::move(descriptor));
        }
        screenshot_id = surfaces[0].screenshot_id;
        if (request.value("include_text", true)) {
            json accessible{{"tree", ""}, {"elements", json::array()}, {"selected_elements", json::array()}, {"truncated", false}};
            const auto accessibility_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            // Inspect foreground transient controls before spending the shared
            // node budget on a potentially very large main-window document.
            for (std::size_t offset = surfaces.size(); offset > 0; --offset) {
                const auto index = offset - 1;
                const auto element_start = elements.size();
                try {
                    auto part = accessibility(index, accessibility_deadline);
                    accessible["tree"] = accessible["tree"].get<std::string>() + "Surface " + std::to_string(reinterpret_cast<std::uintptr_t>(surfaces[index].relation.window))
                        + " screenshot=" + surfaces[index].screenshot_id + "\n" + part["tree"].get<std::string>();
                    for (auto& element : part["elements"]) accessible["elements"].push_back(std::move(element));
                    for (auto& selected : part["selected_elements"]) accessible["selected_elements"].push_back(std::move(selected));
                    for (const auto* key : {"focused_element", "selected_text", "document_text"})
                        if (part.contains(key) && (!accessible.contains(key) || part.contains("focused_element"))) accessible[key] = std::move(part[key]);
                    accessible["truncated"] = accessible["truncated"].get<bool>() || part["truncated"].get<bool>();
                } catch (const NativeError& error) {
                    elements.resize(element_start);
                    warnings.push_back({{"window", reinterpret_cast<std::uintptr_t>(surfaces[index].relation.window)}, {"accessibility_error", error.what()}});
                }
            }
            output["accessibility"] = std::move(accessible);
        }
        if (request.value("include_screenshot", true) && output["screenshots"].empty()
            && (!request.value("include_text", true) || elements.empty()))
            throw NativeError("capture_unavailable", "No requested surface could be captured and no readable accessibility observation is available. Expose the target and observe again.");
        validate_surfaces();
        observed_foreground = GetForegroundWindow();
        observed_gui = gui_snapshot(observed_foreground);
        if (in_observed_group(observed_foreground)) automation->GetFocusedElement(observed_focus.put());
        if (!warnings.empty()) output["warnings"] = std::move(warnings);
        observation_id = id;
        observed_at = std::chrono::steady_clock::now();
        return {{"success", true}, {"output", std::move(output)}, {"attachments", std::move(attachments)}};
    }

    HWND validate_observation(const json& request) {
        auto id = required_string(request, "observation_id", 256);
        if (id.empty() || id != observation_id) throw NativeError("stale_observation", "Observation is missing, expired, or already used. Get a new window state.");
        observation_id.clear(); // Failed actions require a fresh observation too.
        HWND window = requested_window(request);
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (window != observed_window || pid != observed_pid || process_start(pid) != observed_process_start
            || !same_rect(observed_rect, bounds(window)) || IsIconic(window)
            || std::chrono::steady_clock::now() - observed_at > std::chrono::seconds(90))
            throw NativeError("stale_observation", "Target identity, geometry, or observation age changed. Get a new window state.");
        interactive_desktop();
        ComRef<IUIAutomationElement> current_root;
        BOOL same_root = FALSE;
        UIA_HWND original_handle = nullptr;
        int original_pid = 0;
        if (!observed_root || FAILED(observed_root->get_CurrentNativeWindowHandle(&original_handle))
            || original_handle != reinterpret_cast<UIA_HWND>(window)
            || FAILED(observed_root->get_CurrentProcessId(&original_pid)) || original_pid != static_cast<int>(pid)
            || FAILED(automation->ElementFromHandle(window, current_root.put())) || !current_root
            || FAILED(automation->CompareElements(observed_root.get(), current_root.get(), &same_root)) || !same_root)
            throw NativeError("stale_window", "The observed window was replaced. List windows and observe again.");
        validate_surfaces();
        const auto screenshot_key = request.contains("screenshot_id") ? "screenshot_id" : "screenshotId";
        if (request.contains(screenshot_key)) {
            auto requested = required_string(request, screenshot_key, 256);
            if (requested.empty() || std::none_of(surfaces.begin(), surfaces.end(), [&](const Surface& surface) { return surface.screenshot_id == requested; }))
                throw NativeError("stale_screenshot", "Screenshot does not belong to this observation.");
        }
        return window;
    }

    std::size_t screenshot_surface(const json& request) const {
        const auto screenshot_key = request.contains("screenshot_id") ? "screenshot_id" : "screenshotId";
        std::optional<std::string> requested;
        if (request.contains(screenshot_key)) requested = required_string(request, screenshot_key, 256);
        std::vector<std::string> ids;
        for (const auto& surface : surfaces) ids.push_back(surface.screenshot_id);
        int selected = surface_policy::select_screenshot(ids, requested);
        if (selected == -2) throw NativeError("screenshot_required", "This observation has multiple screenshots. Supply the exact screenshot_id for the intended surface.");
        if (selected < 0) throw NativeError("screenshot_required", "Coordinate actions require a captured screenshot. If the main image is unavailable, explicitly select a related screenshot_id.");
        return static_cast<std::size_t>(selected);
    }

    POINT coordinate(const json& request, const char* xkey, const char* ykey, std::size_t surface_index) {
        const auto& surface = surfaces.at(surface_index);
        if (surface.screenshot_id.empty()) throw NativeError("screenshot_required", "Coordinate actions require an observation with a screenshot.");
        double x = number(request, xkey), y = number(request, ykey);
        const auto mapped = coordinate_transform::screenshot_to_desktop(x, y,
            {surface.rect.left, surface.rect.top, surface.rect.right, surface.rect.bottom}, surface.width, surface.height);
        if (!mapped)
            throw NativeError("invalid_coordinate", "Coordinates are outside the observed screenshot.");
        return {mapped->x, mapped->y};
    }

    Element& selected_element(const json& request) {
        if (!request.contains("element_index") || !request["element_index"].is_number_integer())
            throw NativeError("invalid_argument", "element_index must be an integer from the latest accessibility tree.");
        auto index = request["element_index"].get<std::int64_t>();
        if (index < 0 || static_cast<std::uint64_t>(index) >= elements.size()) throw NativeError("invalid_element", "Element index is not in this observation.");
        auto& element = elements[static_cast<std::size_t>(index)];
        const auto screenshot_key = request.contains("screenshot_id") ? "screenshot_id" : "screenshotId";
        if (request.contains(screenshot_key) && required_string(request, screenshot_key, 256) != surfaces.at(element.surface).screenshot_id)
            throw NativeError("stale_screenshot", "The element belongs to a different observed surface than screenshot_id.");
        RECT current{};
        BOOL enabled = FALSE, offscreen = TRUE;
        if (FAILED(element.element->get_CurrentBoundingRectangle(&current)) || !same_rect(current, element.rect)
            || FAILED(element.element->get_CurrentIsEnabled(&enabled)) || !enabled
            || FAILED(element.element->get_CurrentIsOffscreen(&offscreen)) || offscreen)
            throw NativeError("stale_element", "Element moved, disappeared, or became unavailable. Observe again.");
        return element;
    }

    void check_point(std::size_t surface_index, POINT point) {
        const auto& surface = surfaces.at(surface_index);
        HWND window = surface.relation.window;
        if (!PtInRect(&surface.rect, point)) throw NativeError("invalid_coordinate", "Element is outside the selected observed surface.");
        HWND hit = WindowFromPoint(point);
        if (!hit || (hit != window && !IsChild(window, hit)))
            throw NativeError("target_obscured", "Another window covers the input point. Observe the active window or dialog before acting.");
    }

    void check_keyboard_focus(bool allow_menu = false) {
        if (!in_observed_group(observed_foreground) || GetForegroundWindow() != observed_foreground || observed_gui.is_null()
            || gui_snapshot(GetForegroundWindow()) != observed_gui)
            throw NativeError("focus_changed", "Foreground, menu, capture, or keyboard focus changed. Observe the active surface again.");
        ComRef<IUIAutomationElement> current;
        BOOL same = FALSE;
        HWND menu_owner = reinterpret_cast<HWND>(observed_gui.value("menu_owner", std::uintptr_t{}));
        if (allow_menu && !observed_focus && observed_gui.value("flags", 0U) != 0U
            && belongs_to_requested_root(menu_owner, observed_window, observed_pid)) return;
        if (!observed_focus || FAILED(automation->GetFocusedElement(current.put())) || !current
            || FAILED(automation->CompareElements(observed_focus.get(), current.get(), &same)) || !same)
            throw NativeError("focus_changed", "Keyboard focus changed or was not observable. Activate the target and get a new window state before typing.");
    }

    void check_foreground(HWND window) {
        (void)window;
        if (GetForegroundWindow() != input_foreground || !in_observed_group(input_foreground))
            throw NativeError("foreground_changed", "Foreground window changed before input. Observe again.");
    }

    void prepare_input(HWND window) {
        ensure_no_held_input();
        HWND current = GetForegroundWindow();
        if (current == observed_foreground && in_observed_group(current)) {
            // Reactivating the root would close an observed menu or dropdown.
            input_foreground = current;
            return;
        }
        if (surfaces.size() > 1 || in_observed_group(observed_foreground))
            throw NativeError("foreground_changed", "The observed window group lost foreground. Reobserve before acting on its popups.");
        activate(window, automation.get());
        input_foreground = GetForegroundWindow();
        validate_surfaces();
    }

    json act(const std::string& action, const json& request) {
        HWND window = validate_observation(request);
        std::size_t target_surface = 0;
        if (request.contains("element_index")) target_surface = selected_element(request).surface;
        else if (action == "click" || action == "scroll" || action == "drag") target_surface = screenshot_surface(request);
        ScopedPointerSuppression suppression(pointer_overlay);
        prepare_input(window);
        if (!same_rect(observed_rect, bounds(window))) throw NativeError("stale_observation", "Activating changed window geometry. Observe it again.");
        std::vector<INPUT> inputs;
        std::optional<POINT> feedback_point;
        const auto element_feedback = [&](const RECT& rect, std::size_t surface_index) {
            RECT visible{};
            if (IntersectRect(&visible, &rect, &surfaces.at(surface_index).rect))
                feedback_point = POINT{visible.left + (visible.right - visible.left) / 2,
                    visible.top + (visible.bottom - visible.top) / 2};
        };
        const auto focused_feedback = [&] {
            RECT focused{};
            if (!observed_focus || FAILED(observed_focus->get_CurrentBoundingRectangle(&focused))) return;
            // Keyboard focus may belong to an owned dialog outside the main
            // window. Bind visual feedback to the actual visible surface too.
            for (std::size_t index = 0; index < surfaces.size(); ++index) {
                RECT visible{};
                if (!IntersectRect(&visible, &focused, &surfaces[index].rect)) continue;
                const POINT point{visible.left + (visible.right - visible.left) / 2,
                    visible.top + (visible.bottom - visible.top) / 2};
                const auto hit = WindowFromPoint(point);
                if (hit == surfaces[index].relation.window || IsChild(surfaces[index].relation.window, hit)) {
                    target_surface = index;
                    feedback_point = point;
                    break;
                }
            }
        };
        if (action == "click") {
            POINT point{};
            if (request.contains("element_index")) {
                if (request.contains("x") || request.contains("y")) throw NativeError("invalid_argument", "Choose an element index or coordinates, not both.");
                const auto& element = selected_element(request);
                const auto& surface = surfaces.at(element.surface);
                const auto target = detail::resolve_element_click_target(automation.get(), element.element.get(), surface.relation.window, element.rect, surface.rect);
                if (target.error == detail::ElementTargetError::outside_window)
                    throw NativeError("invalid_coordinate", "The observed element has no clickable point inside this window.");
                if (target.error == detail::ElementTargetError::obscured)
                    throw NativeError("target_obscured", "Another element covers the click target. Observe the current controls before acting.");
                if (target.error != detail::ElementTargetError::none)
                    throw NativeError("stale_element", "The element's clickable point could not be verified. Observe again or use an observed screenshot coordinate.");
                point = target.point;
            } else point = coordinate(request, "x", "y", target_surface);
            check_point(target_surface, point);
            feedback_point = point;
            auto button = request.value("mouse_button", std::string("left"));
            DWORD down = MOUSEEVENTF_LEFTDOWN, up = MOUSEEVENTF_LEFTUP;
            if (button == "right" || button == "r") { down = MOUSEEVENTF_RIGHTDOWN; up = MOUSEEVENTF_RIGHTUP; }
            else if (button == "middle" || button == "m") { down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; }
            else if (button != "left" && button != "l") throw NativeError("invalid_argument", "mouse_button must be left, right, or middle.");
            int count = request.value("click_count", 1);
            if (count < 1 || count > 2) throw NativeError("invalid_argument", "click_count must be 1 or 2.");
            inputs.push_back(move_input(point));
            for (int i = 0; i < count; ++i) { inputs.push_back(mouse_input(down)); inputs.push_back(mouse_input(up)); }
        } else if (action == "type_text") {
            check_keyboard_focus();
            focused_feedback();
            auto text = wide(required_string(request, "text"));
            for (wchar_t character : text) { inputs.push_back(key_input(character, false, true)); inputs.push_back(key_input(character, true, true)); }
        } else if (action == "press_key") {
            check_keyboard_focus(true);
            focused_feedback();
            auto chord = keyboard_input::parse_chord(required_string(request, "key", 256));
            if (!chord) throw NativeError("invalid_key", chord.error);
            for (const auto& key : chord.keys) inputs.push_back(keyboard_input::make_input(key, false));
            for (auto key = chord.keys.rbegin(); key != chord.keys.rend(); ++key) inputs.push_back(keyboard_input::make_input(*key, true));
        } else if (action == "scroll") {
            POINT point = coordinate(request, "x", "y", target_surface);
            check_point(target_surface, point);
            feedback_point = point;
            double dx = number(request, "scrollX"), dy = number(request, "scrollY");
            if (std::abs(dx) > 10000 || std::abs(dy) > 10000) throw NativeError("invalid_argument", "Scroll deltas must be between -10000 and 10000.");
            inputs.push_back(move_input(point));
            if (dx) inputs.push_back(mouse_input(MOUSEEVENTF_HWHEEL, 0, 0, static_cast<DWORD>(static_cast<LONG>(std::lround(dx)))));
            if (dy) inputs.push_back(mouse_input(MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(-static_cast<LONG>(std::lround(dy)))));
        } else if (action == "drag") {
            POINT from = coordinate(request, "from_x", "from_y", target_surface), to = coordinate(request, "to_x", "to_y", target_surface);
            check_point(target_surface, from);
            check_point(target_surface, to);
            feedback_point = to;
            inputs.push_back(move_input(from));
            inputs.push_back(mouse_input(MOUSEEVENTF_LEFTDOWN));
            for (int step = 1; step <= 24; ++step) {
                POINT point{from.x + (to.x - from.x) * step / 24, from.y + (to.y - from.y) * step / 24};
                check_point(target_surface, point);
                inputs.push_back(move_input(point));
            }
            inputs.push_back(mouse_input(MOUSEEVENTF_LEFTUP));
        } else if (action == "set_value") {
            auto& element = selected_element(request);
            element_feedback(element.rect, element.surface);
            auto value = wide(required_string(request, "value"));
            auto pattern_value = pattern<IUIAutomationValuePattern>(element.element.get(), UIA_ValuePatternId, IID_IUIAutomationValuePattern);
            if (!pattern_value) throw NativeError("unsupported_action", "Element does not support the UI Automation Value pattern.");
            BOOL read_only = TRUE;
            check(pattern_value->get_CurrentIsReadOnly(&read_only), "Read element editability");
            if (read_only) throw NativeError("unsupported_action", "The element value is read-only.");
            check_foreground(window);
            BSTR replacement = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
            if (!replacement) throw NativeError("native_error", "Cannot allocate element value.");
            HRESULT changed = pattern_value->SetValue(replacement);
            SysFreeString(replacement);
            check(changed, "Set element value");
        } else if (action == "perform_secondary_action") {
            auto& element = selected_element(request);
            element_feedback(element.rect, element.surface);
            auto secondary = lower(required_string(request, "secondary_action", 80));
            check_foreground(window);
            if (secondary == "invoke") {
                auto value = pattern<IUIAutomationInvokePattern>(element.element.get(), UIA_InvokePatternId, IID_IUIAutomationInvokePattern);
                if (!value) throw NativeError("unsupported_action", "Element does not support Invoke.");
                check(value->Invoke(), "Invoke element");
            } else if (secondary == "toggle") {
                auto value = pattern<IUIAutomationTogglePattern>(element.element.get(), UIA_TogglePatternId, IID_IUIAutomationTogglePattern);
                if (!value) throw NativeError("unsupported_action", "Element does not support Toggle.");
                check(value->Toggle(), "Toggle element");
            } else if (secondary == "select") {
                auto value = pattern<IUIAutomationSelectionItemPattern>(element.element.get(), UIA_SelectionItemPatternId, IID_IUIAutomationSelectionItemPattern);
                if (!value) throw NativeError("unsupported_action", "Element does not support Select.");
                check(value->Select(), "Select element");
            } else if (secondary == "expand" || secondary == "collapse") {
                auto value = pattern<IUIAutomationExpandCollapsePattern>(element.element.get(), UIA_ExpandCollapsePatternId, IID_IUIAutomationExpandCollapsePattern);
                if (!value) throw NativeError("unsupported_action", "Element does not support Expand or Collapse.");
                check(secondary == "expand" ? value->Expand() : value->Collapse(), "Expand or collapse element");
            } else if (secondary == "raise" || secondary == "focus") check(element.element->SetFocus(), "Focus element");
            else if (secondary == "scroll up" || secondary == "scroll down" || secondary == "scroll left" || secondary == "scroll right") {
                auto value = pattern<IUIAutomationScrollPattern>(element.element.get(), UIA_ScrollPatternId, IID_IUIAutomationScrollPattern);
                if (!value) throw NativeError("unsupported_action", "Element does not support Scroll.");
                ScrollAmount horizontal = ScrollAmount_NoAmount, vertical = ScrollAmount_NoAmount;
                if (secondary == "scroll up") vertical = ScrollAmount_LargeDecrement;
                if (secondary == "scroll down") vertical = ScrollAmount_LargeIncrement;
                if (secondary == "scroll left") horizontal = ScrollAmount_LargeDecrement;
                if (secondary == "scroll right") horizontal = ScrollAmount_LargeIncrement;
                check(value->Scroll(horizontal, vertical), "Scroll element");
            } else throw NativeError("unsupported_action", "Unknown secondary action. Use an action supported by the observed element.");
        } else throw NativeError("unsupported_action", "Unknown Computer Use action.");
        if (!inputs.empty()) check_foreground(window);
        send_batch(inputs);
        json output{{"action", action}, {"window", describe_window(window)}, {"requires_observation", true}};
        if (feedback_point) {
            try {
                pointer_overlay.show(*feedback_point, GetDpiForWindow(surfaces.at(target_surface).relation.window),
                    action == "click" || action == "perform_secondary_action");
            } catch (const std::exception&) {
                // The input already succeeded; a visual failure must not invite
                // repeating a click or text insertion.
                output["pointer_warning"] = "The action completed, but its pointer feedback could not be displayed.";
            }
        }
        return {{"success", true}, {"output", std::move(output)}};
    }

    json list_apps() {
        discovered_window_apps.clear();
        auto windows = list_windows()["windows"];
        std::vector<InstalledApplicationIdentity> apps;
        installed_apps.clear();
        ComRef<IShellItem> folder;
        HRESULT hr = SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_IShellItem, folder.out());
        ComRef<IEnumShellItems> enumerator;
        if (SUCCEEDED(hr)) hr = folder->BindToHandler(nullptr, BHID_EnumItems, IID_IEnumShellItems, enumerator.out());
        if (SUCCEEDED(hr)) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            for (int index = 0; index < 1024 && std::chrono::steady_clock::now() < deadline; ++index) {
                ComRef<IShellItem> item;
                if (enumerator->Next(1, item.put(), nullptr) != S_OK) break;
                ComRef<IShellItem2> details;
                if (FAILED(item->QueryInterface(IID_IShellItem2, details.out()))) continue;
                PWSTR id = nullptr, display = nullptr, executable = nullptr;
                if (FAILED(details->GetString(PKEY_AppUserModel_ID, &id)) || !id) continue;
                item->GetDisplayName(SIGDN_NORMALDISPLAY, &display);
                // AppsFolder desktop entries expose their executable through
                // the link target property; packaged windows match by AUMID.
                details->GetString(PKEY_Link_TargetParsingPath, &executable);
                auto app = utf8(id);
                apps.push_back({app, display ? utf8(display) : app, executable ? utf8(executable) : std::string()});
                installed_apps.insert(app);
                CoTaskMemFree(id);
                CoTaskMemFree(display);
                CoTaskMemFree(executable);
            }
        }
        json result = merge_discovered_applications(apps, windows);
        for (const auto& app : result) for (const auto& window : app["windows"])
            discovered_window_apps[window["id"].get<std::uintptr_t>()] = {window["pid"].get<DWORD>(), app["id"].get<std::string>()};
        return {{"apps", std::move(result)}, {"installed_apps_available", SUCCEEDED(hr)}};
    }

    json launch(const json& request) {
        auto app = required_string(request, "app", 32768);
        if (app.empty()) throw NativeError("invalid_argument", "app must be an id returned by list_apps or an absolute .exe path.");
        std::wstring target;
        if (installed_apps.count(app)) target = L"shell:AppsFolder\\" + wide(app);
        else {
            target = wide(app);
            bool absolute = target.size() > 3 && ((std::iswalpha(target[0]) && target[1] == L':' && (target[2] == L'\\' || target[2] == L'/'))
                || (target[0] == L'\\' && target[1] == L'\\'));
            DWORD attributes = GetFileAttributesW(target.c_str());
            if (!absolute || lower(app.substr(app.size() > 4 ? app.size() - 4 : 0)) != ".exe"
                || attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
                throw NativeError("invalid_app", "Use an exact installed app id from list_apps or an existing absolute .exe path. Arguments and URL schemes are not accepted.");
        }
        observation_id.clear();
        interactive_desktop();
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"open";
        info.lpFile = target.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) throw NativeError("launch_failed", "Windows could not launch this application (error " + std::to_string(GetLastError()) + ").");
        return {{"app", app}, {"instruction", "List windows and observe the target before sending input."}};
    }

    json dispatch(const json& request) {
        const auto action = required_string(request, "action", 80);
        std::string pointer_style = pointer_appearance::kDefaultStyle;
        std::string pointer_color = pointer_appearance::kDefaultColor;
        const auto appearance = request.find("pointer_appearance");
        if (appearance != request.end() && appearance->is_object()) {
            const auto style = appearance->find("style");
            if (style != appearance->end() && style->is_string()
                && pointer_appearance::valid_style(style->get_ref<const std::string&>()))
                pointer_style = style->get<std::string>();
            const auto color = appearance->find("color");
            if (color != appearance->end() && color->is_string())
                pointer_color = pointer_appearance::normalize_color(color->get_ref<const std::string&>()).value_or(pointer_appearance::kDefaultColor);
        }
        pointer_overlay.configure(pointer_style, pointer_color);
        if (action == "list_windows") return {{"success", true}, {"output", list_windows()}};
        if (action == "list_apps") return {{"success", true}, {"output", list_apps()}};
        if (action == "get_window") {
            const auto window = describe_window(requested_window(request));
            if (request.contains("app")) {
                const auto expected = required_string(request, "app", 32768);
                const auto current = window.value("app", std::string());
                const bool path_match = expected.find_first_of("\\/") != std::string::npos
                    && current.find_first_of("\\/") != std::string::npos
                    && application_path_key(expected) == application_path_key(current);
                if (expected != current && !path_match)
                    throw NativeError("window_app_mismatch", "The current window does not belong to the expected application. List windows and select it again.");
            }
            return {{"success", true}, {"output", {{"window", window}, {"requires_observation", true}}}};
        }
        if (action == "get_window_state") return observe(request);
        if (action == "launch_app") return {{"success", true}, {"output", launch(request)}};
        if (action == "activate_window") {
            HWND window = requested_window(request);
            observation_id.clear();
            activate(window, automation.get());
            return {{"success", true}, {"output", {{"window", describe_window(window)}, {"requires_observation", true}}}};
        }
        if (action == "release") { observation_id.clear(); elements.clear(); pointer_overlay.hide(); return {{"success", true}, {"output", "Observation released."}}; }
        static const std::set<std::string> actions{"click", "type_text", "press_key", "scroll", "drag", "set_value", "perform_secondary_action"};
        if (!actions.count(action)) throw NativeError("unsupported_action", "Unknown Computer Use action: " + action);
        return act(action, request);
    }
};
#else
struct NativeBackend::Impl {};
#endif

NativeBackend::NativeBackend() : impl_(std::make_unique<Impl>()) {}
NativeBackend::~NativeBackend() = default;

json NativeBackend::dispatch(const json& request) {
    try {
        if (!request.is_object()) throw NativeError("invalid_argument", "Computer Use request must be a JSON object.");
#ifdef _WIN32
        return impl_->dispatch(request);
#else
        return {{"success", false}, {"error", "unsupported_platform"}, {"output", "Computer Use is currently available on Windows only."}};
#endif
    } catch (const NativeError& error) {
        return {{"success", false}, {"error", error.code}, {"output", error.what()}};
#if defined(_WIN32) && ACECODE_HAS_WGC
    } catch (const winrt::hresult_error& error) {
        return {{"success", false}, {"error", "native_error"}, {"output", utf8(error.message().c_str())}};
#endif
    } catch (const std::exception& error) {
        return {{"success", false}, {"error", "invalid_argument"}, {"output", error.what()}};
    }
}

} // namespace acecode::computer_use
