#pragma once

#ifdef _WIN32
#include "utils/encoding.hpp"
#include "utils/lifetime_token.hpp"
#include "utils/scope_exit.hpp"

#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <wrl.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace acecode::desktop::detail {

inline bool needs_legacy_windows_file_drop(std::wstring_view version) {
    const auto dot = version.find(L'.');
    if (dot == std::wstring_view::npos || dot == 0 || dot > 3) return false;
    unsigned major = 0;
    for (wchar_t digit : version.substr(0, dot)) {
        if (digit < L'0' || digit > L'9') return false;
        major = major * 10 + static_cast<unsigned>(digit - L'0');
    }
    // File/AdditionalObjects shipped in WebView2 Runtime 113.
    return major > 0 && major < 113;
}

inline FORMATETC windows_file_drop_format() {
    return {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
}

inline std::vector<std::string> windows_file_drop_paths(IDataObject* data) {
    if (!data) return {};
    auto format = windows_file_drop_format();
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&format, &medium))) return {};
    ScopeExit release([&medium] { ::ReleaseStgMedium(&medium); });
    if (medium.tymed != TYMED_HGLOBAL || !medium.hGlobal) return {};
    const auto drop = static_cast<HDROP>(medium.hGlobal);
    const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::string> paths;
    paths.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        const UINT length = ::DragQueryFileW(drop, index, nullptr, 0);
        if (length == 0) continue;
        std::wstring path(length + 1, L'\0');
        if (::DragQueryFileW(drop, index, path.data(), length + 1) != length) continue;
        path.resize(length);
        paths.push_back(wide_to_utf8(path));
    }
    return paths;
}

// WebView2's OLE HWND belongs to the browser process. Do not dereference its
// OleDropTargetInterface or replace its registration. A separate host-owned
// surface catches external file drags only while another application holds
// mouse capture. Ordinary clicks and internal HTML5 drags keep using WebView2.
class LegacyWindowsFileDrop {
public:
    using Handler = std::function<void(std::vector<std::string>, POINTL)>;
    using Feedback = std::function<void(std::optional<POINTL>)>;

    LegacyWindowsFileDrop(HWND widget, Handler handler, Feedback feedback = {})
        : widget_(widget), delivery_(std::make_shared<Delivery>()) {
        ole_result_ = ::OleInitialize(nullptr);
        if (FAILED(ole_result_)) return;
        delivery_->handler = std::move(handler);
        delivery_->feedback = std::move(feedback);
        static const ATOM atom = [] {
            WNDCLASSW type{};
            type.lpfnWndProc = ::DefWindowProcW;
            type.hInstance = ::GetModuleHandleW(nullptr);
            type.lpszClassName = L"ACECodeLegacyFileDrop";
            return ::RegisterClassW(&type);
        }();
        if (!atom) return;
        window_ = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            MAKEINTRESOURCEW(atom), L"", WS_POPUP, 0, 0, 0, 0, ::GetAncestor(widget, GA_ROOT),
            nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (!window_) return;
        // Alpha zero is also transparent to hit testing. Alpha one preserves
        // the underlying page visually while allowing an OLE drop target.
        if (!::SetLayeredWindowAttributes(window_, 0, 1, LWA_ALPHA)) return;
        delivery_->window = window_;
        target_ = Microsoft::WRL::Make<Target>(delivery_);
        registered_ = target_ && SUCCEEDED(::RegisterDragDrop(window_, target_.Get()));
        if (!registered_) return;
        hook_ = ::SetWinEventHook(EVENT_SYSTEM_CAPTURESTART, EVENT_SYSTEM_CAPTUREEND,
            nullptr, capture_event, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (hook_) hooks().emplace(hook_, lifetime_.ref(*this));
    }

    ~LegacyWindowsFileDrop() {
        lifetime_.revoke();
        if (hook_) {
            hooks().erase(hook_);
            ::UnhookWinEvent(hook_);
        }
        delivery_->handler = {};
        delivery_->feedback = {};
        delivery_->window = nullptr;
        if (registered_) ::RevokeDragDrop(window_);
        if (window_) ::DestroyWindow(window_);
        if (SUCCEEDED(ole_result_)) ::OleUninitialize();
    }

    LegacyWindowsFileDrop(const LegacyWindowsFileDrop&) = delete;
    LegacyWindowsFileDrop& operator=(const LegacyWindowsFileDrop&) = delete;

    bool ready() const { return registered_ && hook_; }
    HWND window() const { return window_; }

    void refresh() {
        RECT bounds{};
        if (window_ && ::GetWindowRect(widget_, &bounds)) {
            const HWND owner = ::GetAncestor(widget_, GA_ROOT);
            HWND above = ::GetWindow(owner, GW_HWNDPREV);
            if (above == window_) above = ::GetWindow(window_, GW_HWNDPREV);
            // Keep the receiver in its owner's z-order group. Raising it above
            // another application would intercept that application's drops.
            ::SetWindowPos(window_, above ? above : HWND_TOP, bounds.left, bounds.top,
                bounds.right - bounds.left, bounds.bottom - bounds.top,
                SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
    }

    void begin_external_drag() {
        if (!ready() || !::IsWindowVisible(widget_) ||
            ::IsIconic(::GetAncestor(widget_, GA_ROOT))) return;
        refresh();
        ::SetWindowPos(window_, nullptr, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOZORDER |
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    }

    void end_drag() {
        if (window_) ::ShowWindow(window_, SW_HIDE);
        auto feedback = delivery_->feedback;
        if (feedback) feedback(std::nullopt);
    }

private:
    struct Delivery {
        Handler handler;
        Feedback feedback;
        HWND window = nullptr; // Nullable, borrowed from the controller.
    };

    class Target final : public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDropTarget> {
    public:
        explicit Target(std::shared_ptr<Delivery> delivery) : delivery_(std::move(delivery)) {}

        HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD,
                                             POINTL point, DWORD* effect) override {
            if (!effect) return E_POINTER;
            auto format = windows_file_drop_format();
            file_drag_ = delivery_->handler && data && SUCCEEDED(data->QueryGetData(&format));
            if (!file_drag_) {
                // Hiding this surface lets OLE hit the original WebView2
                // target on its next update. Its registration is untouched.
                hide();
                *effect = DROPEFFECT_NONE;
                return S_OK;
            }
            return DragOver(0, point, effect);
        }

        HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override {
            if (!effect) return E_POINTER;
            *effect = file_drag_ && delivery_->handler ? (*effect & DROPEFFECT_COPY) : DROPEFFECT_NONE;
            auto feedback = delivery_->feedback;
            if (*effect && feedback) feedback(point);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE DragLeave() override {
            file_drag_ = false;
            auto feedback = delivery_->feedback;
            if (feedback) feedback(std::nullopt);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD,
                                         POINTL point, DWORD* effect) override {
            if (!effect) return E_POINTER;
            auto handler = delivery_->handler;
            auto paths = file_drag_ && handler && (*effect & DROPEFFECT_COPY)
                ? windows_file_drop_paths(data) : std::vector<std::string>{};
            file_drag_ = false;
            hide();
            *effect = paths.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
            if (!paths.empty()) handler(std::move(paths), point);
            return S_OK;
        }

    private:
        void hide() {
            if (delivery_->window) ::ShowWindow(delivery_->window, SW_HIDE);
            auto feedback = delivery_->feedback;
            if (feedback) feedback(std::nullopt);
        }

        // Shared with the owner and OLE; callbacks are cleared before shutdown.
        std::shared_ptr<Delivery> delivery_;
        bool file_drag_ = false;
    };

    static std::map<HWINEVENTHOOK, LifetimeRef<LegacyWindowsFileDrop>>& hooks() {
        // OUTOFCONTEXT callbacks run on the installing GUI thread. Queued
        // notifications use LifetimeRef and never retain the controller.
        static thread_local std::map<HWINEVENTHOOK, LifetimeRef<LegacyWindowsFileDrop>> values;
        return values;
    }

    static void CALLBACK capture_event(HWINEVENTHOOK hook, DWORD event, HWND source,
                                       LONG object, LONG, DWORD, DWORD) {
        if (object != OBJID_WINDOW) return;
        const auto found = hooks().find(hook);
        if (found == hooks().end()) return;
        found->second.with([event, source](LegacyWindowsFileDrop& owner) {
            if (event == EVENT_SYSTEM_CAPTUREEND || owner.is_own_capture(source)) owner.end_drag();
            else owner.begin_external_drag();
        });
    }

    bool is_own_capture(HWND source) const {
        DWORD process = 0;
        if (!source || !::GetWindowThreadProcessId(source, &process) ||
            process == ::GetCurrentProcessId()) return true;
        // Chromium may capture in a hidden window outside the child hierarchy.
        // Match the process of our WebView's children without reading pointers.
        struct Search { DWORD process; bool found; } search{process, false};
        ::EnumChildWindows(widget_, +[](HWND child, LPARAM context) -> BOOL {
            auto& value = *reinterpret_cast<Search*>(context);
            DWORD child_process = 0;
            ::GetWindowThreadProcessId(child, &child_process);
            value.found = child_process == value.process;
            return !value.found;
        }, reinterpret_cast<LPARAM>(&search));
        return search.found;
    }

    HWND widget_;
    HWND window_ = nullptr;
    HWINEVENTHOOK hook_ = nullptr;
    bool registered_ = false;
    HRESULT ole_result_ = E_FAIL;
    // Shared with the COM target so late OLE releases survive owner shutdown.
    std::shared_ptr<Delivery> delivery_;
    Microsoft::WRL::ComPtr<Target> target_;
    LifetimeToken lifetime_;
};

} // namespace acecode::desktop::detail
#endif
