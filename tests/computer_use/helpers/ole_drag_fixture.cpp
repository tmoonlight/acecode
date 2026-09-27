#include "test_support/computer_use/ole_drag_fixture.hpp"

#ifdef _WIN32
#include <ole2.h>
#include <shlobj.h>
#include <windowsx.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <mutex>

namespace acecode::computer_use::test {
namespace {
constexpr wchar_t kPayload[] = L"ACECode owned OLE drag fixture";
constexpr wchar_t kClassName[] = L"ACECodeOwnedOleDragFixture";
constexpr UINT_PTR kWatchdogTimer = 1;
constexpr RECT kSource{24, 48, 280, 172};
constexpr RECT kTarget{360, 48, 616, 172};

FORMATETC text_format() { return {CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; }

class FixtureText final : public IDataObject {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IDataObject) return E_NOINTERFACE;
        *out = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (!format) return E_POINTER;
        if (format->cfFormat != CF_UNICODETEXT) return DV_E_FORMATETC;
        if (!(format->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
        if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
        if (format->lindex != -1) return DV_E_LINDEX;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!medium) return E_POINTER;
        *medium = {};
        const auto supported = QueryGetData(format);
        if (FAILED(supported)) return supported;
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(kPayload));
        if (!memory) return E_OUTOFMEMORY;
        void* contents = GlobalLock(memory);
        if (!contents) { GlobalFree(memory); return E_OUTOFMEMORY; }
        std::memcpy(contents, kPayload, sizeof(kPayload));
        GlobalUnlock(memory);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return DATA_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override {
        if (!output) return E_POINTER;
        output->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        auto format = text_format();
        return SHCreateStdEnumFmtEtc(1, &format, output);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
private:
    std::atomic<ULONG> references_{1};
};
} // namespace

struct OleDragFixture::Impl {
    std::atomic<HWND> window{nullptr};
    std::atomic<HRESULT> initialization{E_PENDING};
    DWORD ui_thread = 0;
    bool ole_initialized = false;
    bool registered = false;
    bool pending_drag = false;
    bool dragging = false;
    bool closing = false;
    bool cancelled = false;
    POINT down_point{};
    std::chrono::steady_clock::time_point deadline;
    FixtureText* active_data = nullptr;
    mutable std::mutex state_mu;
    State state;

    bool cursor_in_owned_window() const {
        POINT point{};
        const auto owned = window.load();
        if (!owned || !GetCursorPos(&point)) return false;
        const auto hit = WindowFromPoint(point);
        // Source and target are painted regions of exactly this window.
        return hit == owned;
    }

    bool in_target(POINTL point) const {
        POINT client{point.x, point.y};
        const auto owned = window.load();
        return owned && ScreenToClient(owned, &client) && PtInRect(&kTarget, client);
    }

    bool own_data(IDataObject* data) const {
        if (!data || !active_data) return false;
        IUnknown* identity = nullptr;
        if (FAILED(data->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity)))) return false;
        const bool matches = identity == static_cast<IUnknown*>(active_data);
        identity->Release();
        return matches;
    }

    class Source final : public IDropSource {
    public:
        explicit Source(Impl& fixture) : fixture_(fixture) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
            if (!out) return E_POINTER;
            *out = nullptr;
            if (id != IID_IUnknown && id != IID_IDropSource) return E_NOINTERFACE;
            *out = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
        ULONG STDMETHODCALLTYPE Release() override {
            const auto remaining = --references_;
            if (!remaining) delete this;
            return remaining;
        }
        HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
            if (escape || fixture_.cancelled || fixture_.closing ||
                std::chrono::steady_clock::now() >= fixture_.deadline || !fixture_.cursor_in_owned_window())
                return DRAGDROP_S_CANCEL;
            if (!(keys & MK_LBUTTON)) {
                POINT cursor{};
                if (!GetCursorPos(&cursor) || !fixture_.in_target({cursor.x, cursor.y})) return DRAGDROP_S_CANCEL;
                return DRAGDROP_S_DROP;
            }
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
    private:
        Impl& fixture_;
        std::atomic<ULONG> references_{1};
    };

    class Target final : public IDropTarget {
    public:
        explicit Target(Impl& fixture) : fixture_(fixture) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
            if (!out) return E_POINTER;
            *out = nullptr;
            if (id != IID_IUnknown && id != IID_IDropTarget) return E_NOINTERFACE;
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
        ULONG STDMETHODCALLTYPE Release() override {
            const auto remaining = --references_;
            if (!remaining) delete this;
            return remaining;
        }
        HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL point, DWORD* effect) override {
            if (!effect) return E_POINTER;
            own_drag_ = fixture_.own_data(data);
            return DragOver(0, point, effect);
        }
        HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override {
            if (!effect) return E_POINTER;
            *effect = own_drag_ && !fixture_.cancelled && fixture_.in_target(point)
                ? (*effect & DROPEFFECT_COPY) : DROPEFFECT_NONE;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE DragLeave() override { own_drag_ = false; return S_OK; }
        HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL point, DWORD* effect) override {
            if (!effect) return E_POINTER;
            const bool accept = fixture_.own_data(data) && fixture_.in_target(point) &&
                !fixture_.cancelled && !fixture_.closing && (*effect & DROPEFFECT_COPY);
            *effect = DROPEFFECT_NONE;
            own_drag_ = false;
            if (!accept) return S_OK;
            auto format = text_format();
            STGMEDIUM medium{};
            const auto result = data->GetData(&format, &medium);
            if (FAILED(result)) return result;
            bool matched = false;
            if (medium.tymed == TYMED_HGLOBAL && GlobalSize(medium.hGlobal) >= sizeof(kPayload)) {
                const auto* text = static_cast<const wchar_t*>(GlobalLock(medium.hGlobal));
                if (text) {
                    matched = std::memcmp(text, kPayload, sizeof(kPayload)) == 0;
                    GlobalUnlock(medium.hGlobal);
                }
            }
            ReleaseStgMedium(&medium);
            if (matched) {
                std::lock_guard<std::mutex> lock(fixture_.state_mu);
                ++fixture_.state.drops;
                fixture_.state.dropped_text = kPayload;
                *effect = DROPEFFECT_COPY;
            }
            return S_OK;
        }
    private:
        Impl& fixture_;
        std::atomic<ULONG> references_{1};
        bool own_drag_ = false;
    };

    Target* target = nullptr;

    void revoke() {
        if (registered) { RevokeDragDrop(window.load()); registered = false; }
        if (target) { target->Release(); target = nullptr; }
    }

    void cancel_drag() {
        cancelled = true;
        pending_drag = false;
        if (GetCapture() == window.load()) ReleaseCapture();
        if (dragging) {
            // Only wakes this fixture's OLE modal loop to cancel. It does not
            // inject OS input or manufacture a successful drop.
            PostMessageW(window.load(), WM_KEYDOWN, VK_ESCAPE, 0);
        }
    }

    void start_drag() {
        pending_drag = false;
        ReleaseCapture();
        cancelled = false;
        dragging = true;
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        {
            std::lock_guard<std::mutex> lock(state_mu);
            ++state.drag_starts;
            state.timed_out = false;
        }
        active_data = new FixtureText;
        auto* source = new Source(*this);
        DWORD effect = DROPEFFECT_NONE;
        const auto timer = SetTimer(window.load(), kWatchdogTimer, 50, nullptr);
        const HRESULT result = timer
            ? DoDragDrop(active_data, source, DROPEFFECT_COPY, &effect)
            : HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY);
        KillTimer(window.load(), kWatchdogTimer);
        source->Release();
        active_data->Release();
        active_data = nullptr;
        dragging = false;
        {
            std::lock_guard<std::mutex> lock(state_mu);
            ++state.drag_completions;
            state.last_result = result;
            state.last_effect = effect;
        }
        InvalidateRect(window.load(), nullptr, TRUE);
        if (closing) DestroyWindow(window.load());
    }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window = window;
        }
        if (!self) return DefWindowProcW(window, message, wparam, lparam);
        switch (message) {
        case WM_LBUTTONDOWN: {
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (!self->dragging && PtInRect(&kSource, point)) {
                self->pending_drag = true;
                self->down_point = point;
                SetCapture(window);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (self->pending_drag && (wparam & MK_LBUTTON)) {
                const int dx = GET_X_LPARAM(lparam) - self->down_point.x;
                const int dy = GET_Y_LPARAM(lparam) - self->down_point.y;
                if (std::abs(dx) >= GetSystemMetrics(SM_CXDRAG) || std::abs(dy) >= GetSystemMetrics(SM_CYDRAG))
                    self->start_drag();
            }
            return 0;
        case WM_LBUTTONUP:
            self->pending_drag = false;
            if (GetCapture() == window) ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            self->pending_drag = false;
            return 0;
        case WM_TIMER:
            if (wparam == kWatchdogTimer && self->dragging && std::chrono::steady_clock::now() >= self->deadline) {
                { std::lock_guard<std::mutex> lock(self->state_mu); self->state.timed_out = true; }
                self->cancel_drag();
            }
            return 0;
        case WM_CLOSE:
            self->closing = true;
            self->cancel_drag();
            if (!self->dragging) DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            self->revoke();
            return 0;
        case WM_NCDESTROY:
            self->window = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            SetBkMode(dc, TRANSPARENT);
            auto source = kSource;
            auto target = kTarget;
            FrameRect(dc, &source, GetSysColorBrush(COLOR_WINDOWTEXT));
            FrameRect(dc, &target, GetSysColorBrush(COLOR_WINDOWTEXT));
            DrawTextW(dc, L"Drag fixed fixture text", -1, &source, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(dc, L"Drop only inside this target", -1, &target, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(window, &paint);
            return 0;
        }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
};

OleDragFixture::OleDragFixture() : impl_(std::make_unique<Impl>()) {}
OleDragFixture::~OleDragFixture() {
    // A live HWND/OLE callback must never retain a freed Impl. UI-thread
    // ownership is a fixture contract; fail fast instead of hiding misuse.
    if (impl_->ui_thread && GetCurrentThreadId() != impl_->ui_thread) std::terminate();
    close();
}

bool OleDragFixture::create(HWND owner, int x, int y) {
    if (impl_->window || impl_->ole_initialized) return false;
    impl_->ui_thread = GetCurrentThreadId();
    impl_->closing = false;
    impl_->cancelled = false;
    impl_->pending_drag = false;
    { std::lock_guard<std::mutex> lock(impl_->state_mu); impl_->state = {}; }
    const auto initialized = OleInitialize(nullptr);
    impl_->initialization = initialized;
    if (FAILED(initialized)) return false;
    impl_->ole_initialized = true;
    WNDCLASSW klass{};
    klass.lpfnWndProc = Impl::procedure;
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = kClassName;
    klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    klass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    if (!RegisterClassW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        impl_->initialization = HRESULT_FROM_WIN32(GetLastError());
        close();
        return false;
    }
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT bounds{0, 0, 640, 210};
    AdjustWindowRect(&bounds, style, FALSE);
    HWND window = CreateWindowW(kClassName, L"ACECode owned OLE drag verification", style,
        x, y, bounds.right - bounds.left, bounds.bottom - bounds.top, owner, nullptr, klass.hInstance, impl_.get());
    if (!window) {
        impl_->initialization = HRESULT_FROM_WIN32(GetLastError());
        close();
        return false;
    }
    impl_->target = new Impl::Target(*impl_);
    const auto registered = RegisterDragDrop(window, impl_->target);
    impl_->initialization = registered;
    if (FAILED(registered)) { close(); return false; }
    impl_->registered = true;
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    return true;
}

void OleDragFixture::close() {
    // Deliberately never destroy an STA-owned fixture from a worker thread.
    if (impl_->ui_thread && GetCurrentThreadId() != impl_->ui_thread) {
        request_close();
        return;
    }
    if (impl_->window) {
        impl_->closing = true;
        impl_->cancel_drag();
        if (impl_->dragging) return;
        impl_->revoke();
        DestroyWindow(impl_->window.load());
    } else {
        impl_->revoke();
    }
    if (impl_->ole_initialized) {
        OleUninitialize();
        impl_->ole_initialized = false;
    }
}

void OleDragFixture::request_close() { if (const auto window = impl_->window.load()) PostMessageW(window, WM_CLOSE, 0, 0); }
HWND OleDragFixture::window() const { return impl_->window.load(); }
HRESULT OleDragFixture::initialization_result() const { return impl_->initialization.load(); }
const wchar_t* OleDragFixture::payload() { return kPayload; }

bool OleDragFixture::screen_points(POINT& source, POINT& target) const {
    const auto owned = window();
    if (!owned) return false;
    source = {(kSource.left + kSource.right) / 2, (kSource.top + kSource.bottom) / 2};
    target = {(kTarget.left + kTarget.right) / 2, (kTarget.top + kTarget.bottom) / 2};
    return ClientToScreen(owned, &source) && ClientToScreen(owned, &target);
}

OleDragFixture::State OleDragFixture::state() const {
    std::lock_guard<std::mutex> lock(impl_->state_mu);
    return impl_->state;
}

} // namespace acecode::computer_use::test
#endif
