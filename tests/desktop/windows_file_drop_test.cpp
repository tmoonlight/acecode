#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "desktop/web_host_file_drop_win.hpp"

#include <gtest/gtest.h>
#include <shlobj.h>
#include <cstring>

namespace {
using acecode::desktop::detail::LegacyWindowsFileDrop;
using acecode::desktop::detail::needs_legacy_windows_file_drop;
using acecode::desktop::detail::windows_file_drop_paths;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

class DropData final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDataObject> {
public:
    explicit DropData(std::vector<std::wstring> paths = {}) : paths_(std::move(paths)) {}

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        return format && format->cfFormat == CF_HDROP &&
            (format->tymed & TYMED_HGLOBAL) && !paths_.empty() ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!medium) return E_POINTER;
        *medium = {};
        if (FAILED(QueryGetData(format))) return DV_E_FORMATETC;
        size_t characters = 1;
        for (const auto& path : paths_) characters += path.size() + 1;
        HGLOBAL memory = ::GlobalAlloc(GHND, sizeof(DROPFILES) + characters * sizeof(wchar_t));
        if (!memory) return E_OUTOFMEMORY;
        auto* drop = static_cast<DROPFILES*>(::GlobalLock(memory));
        if (!drop) {
            ::GlobalFree(memory);
            return E_OUTOFMEMORY;
        }
        drop->pFiles = sizeof(DROPFILES);
        drop->fWide = TRUE;
        auto* next = reinterpret_cast<wchar_t*>(drop + 1);
        for (const auto& path : paths_) {
            std::memcpy(next, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
            next += path.size() + 1;
        }
        ::GlobalUnlock(memory);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    std::vector<std::wstring> paths_;
};

class OriginalTarget final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDropTarget> {
public:
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*, DWORD, POINTL, DWORD* effect) override {
        ++enters;
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        ++overs;
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { ++leaves; return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject*, DWORD, POINTL, DWORD* effect) override {
        ++drops;
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    int enters = 0;
    int overs = 0;
    int leaves = 0;
    int drops = 0;
};

class WindowsFileDrop : public testing::Test {
protected:
    void SetUp() override {
        ole_result_ = ::OleInitialize(nullptr);
        ASSERT_TRUE(SUCCEEDED(ole_result_));
        window_ = ::CreateWindowExW(0, L"STATIC", L"ACECode owned drop fixture",
            WS_POPUP | WS_VISIBLE, -30000, -30000, 640, 480, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(window_, nullptr);
        original_ = Make<OriginalTarget>();
        ASSERT_EQ(::RegisterDragDrop(window_, original_.Get()), S_OK);
    }
    void TearDown() override {
        if (window_) {
            ::RevokeDragDrop(window_);
            ::DestroyWindow(window_);
        }
        original_.Reset();
        if (SUCCEEDED(ole_result_)) ::OleUninitialize();
    }
    ComPtr<IDropTarget> target(HWND window = nullptr) const {
        return ComPtr<IDropTarget>(static_cast<IDropTarget*>(
            ::GetPropW(window ? window : window_, L"OleDropTargetInterface")));
    }
    HWND window_ = nullptr;
    ComPtr<OriginalTarget> original_;
    HRESULT ole_result_ = E_FAIL;
};

TEST(WindowsFileDropVersion, OnlyKnownPre113RuntimesUseOle) {
    EXPECT_TRUE(needs_legacy_windows_file_drop(L"110.0.1587.69"));
    EXPECT_TRUE(needs_legacy_windows_file_drop(L"112.0.1722.64"));
    EXPECT_FALSE(needs_legacy_windows_file_drop(L"113.0.1774.30"));
    EXPECT_FALSE(needs_legacy_windows_file_drop(L"156.0.4314.50"));
    EXPECT_FALSE(needs_legacy_windows_file_drop(L""));
    EXPECT_FALSE(needs_legacy_windows_file_drop(L"unknown"));
    EXPECT_FALSE(needs_legacy_windows_file_drop(L"0.0.0.0"));
    EXPECT_FALSE(needs_legacy_windows_file_drop(L"110"));
}

TEST_F(WindowsFileDrop, ReadsOrderedUnicodeLongPathsAndFoldersFromHDrop) {
    const std::vector<std::wstring> paths{
        L"C:\\fixture\\notes.txt", L"C:\\fixture\\\u4e2d\u6587 notes.txt",
        L"\\\\server\\share\\folder", L"C:\\" + std::wstring(300, L'x') + L"\\long.txt"};
    auto data = Make<DropData>(paths);
    const auto actual = windows_file_drop_paths(data.Get());
    ASSERT_EQ(actual.size(), paths.size());
    for (size_t index = 0; index < paths.size(); ++index) {
        EXPECT_EQ(actual[index], acecode::wide_to_utf8(paths[index]));
    }
    EXPECT_TRUE(windows_file_drop_paths(nullptr).empty());
    EXPECT_TRUE(windows_file_drop_paths(Make<DropData>().Get()).empty());
}

TEST_F(WindowsFileDrop, DeliversOneNativeBatchWithoutChangingTheBrowserTarget) {
    std::vector<std::vector<std::string>> batches;
    POINTL delivered{};
    LegacyWindowsFileDrop bridge(window_, [&](auto paths, POINTL point) {
        batches.push_back(std::move(paths));
        delivered = point;
    });
    bridge.refresh();
    ASSERT_TRUE(bridge.ready());
    auto wrapped = target(bridge.window());
    ASSERT_NE(wrapped.Get(), original_.Get());
    bridge.refresh();
    EXPECT_EQ(target(bridge.window()).Get(), wrapped.Get());
    EXPECT_EQ(target().Get(), original_.Get());
    auto data = Make<DropData>(std::vector<std::wstring>{L"C:\\first.txt", L"C:\\folder"});
    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    EXPECT_EQ(wrapped->DragEnter(data.Get(), MK_LBUTTON, {17, 23}, &effect), S_OK);
    EXPECT_EQ(effect, DROPEFFECT_COPY);
    EXPECT_EQ(wrapped->DragOver(0, {31, 47}, &effect), S_OK);
    EXPECT_EQ(wrapped->Drop(data.Get(), 0, {31, 47}, &effect), S_OK);
    ASSERT_EQ(batches.size(), 1u);
    EXPECT_EQ(batches.front(), (std::vector<std::string>{"C:\\first.txt", "C:\\folder"}));
    EXPECT_EQ(delivered.x, 31);
    EXPECT_EQ(delivered.y, 47);
    EXPECT_EQ(original_->enters, 0);
    EXPECT_EQ(original_->overs, 0);
    EXPECT_EQ(original_->leaves, 0);
    EXPECT_EQ(original_->drops, 0);
}

TEST_F(WindowsFileDrop, CancellationNeverDeliversAndNonFilesRelinquishTheSurface) {
    int calls = 0;
    LegacyWindowsFileDrop bridge(window_, [&](auto, POINTL) { ++calls; });
    bridge.refresh();
    ASSERT_TRUE(bridge.ready());
    auto wrapped = target(bridge.window());
    auto files = Make<DropData>(std::vector<std::wstring>{L"C:\\cancelled.txt"});
    DWORD effect = DROPEFFECT_COPY;
    wrapped->DragEnter(files.Get(), 0, {}, &effect);
    wrapped->DragLeave();
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(original_->leaves, 0);

    auto text = Make<DropData>();
    bridge.begin_external_drag();
    EXPECT_TRUE(::IsWindowVisible(bridge.window()));
    effect = DROPEFFECT_COPY;
    wrapped->DragEnter(text.Get(), 0, {}, &effect);
    EXPECT_EQ(effect, DROPEFFECT_NONE);
    EXPECT_FALSE(::IsWindowVisible(bridge.window()));
    EXPECT_EQ(target().Get(), original_.Get());
    wrapped->DragOver(0, {}, &effect);
    wrapped->Drop(text.Get(), 0, {}, &effect);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(original_->enters, 0);
    EXPECT_EQ(original_->overs, 0);
    EXPECT_EQ(original_->drops, 0);
}

TEST_F(WindowsFileDrop, ShutdownDestroysOnlyItsOwnSurfaceAndRevokesLateDelivery) {
    int calls = 0;
    ComPtr<IDropTarget> outstanding;
    auto data = Make<DropData>(std::vector<std::wstring>{L"C:\\late.txt"});
    {
        LegacyWindowsFileDrop bridge(window_, [&](auto, POINTL) { ++calls; });
        bridge.refresh();
        outstanding = target(bridge.window());
        DWORD effect = DROPEFFECT_COPY;
        outstanding->DragEnter(data.Get(), 0, {}, &effect);
    }
    EXPECT_EQ(target().Get(), original_.Get());
    EXPECT_EQ(original_->leaves, 0);
    DWORD effect = DROPEFFECT_COPY;
    outstanding->Drop(data.Get(), 0, {}, &effect);
    EXPECT_EQ(calls, 0);
}

TEST_F(WindowsFileDrop, DoesNotReplaceANewerRegistrationOnShutdown) {
    auto replacement = Make<OriginalTarget>();
    {
        LegacyWindowsFileDrop bridge(window_, [](auto, POINTL) {});
        bridge.refresh();
        ASSERT_EQ(::RevokeDragDrop(window_), S_OK);
        ASSERT_EQ(::RegisterDragDrop(window_, replacement.Get()), S_OK);
    }
    EXPECT_EQ(target().Get(), replacement.Get());
}

TEST_F(WindowsFileDrop, ReceiverDoesNotCoverWindowsAboveItsOwnerOrTakeFocus) {
    HWND blocker = ::CreateWindowExW(0, L"STATIC", L"ACECode owned blocking fixture",
        WS_POPUP | WS_VISIBLE, -30000, -30000, 640, 480, nullptr, nullptr,
        ::GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(blocker, nullptr);
    acecode::ScopeExit destroy([blocker] { ::DestroyWindow(blocker); });
    ::SetWindowPos(blocker, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    const HWND foreground = ::GetForegroundWindow();
    LegacyWindowsFileDrop bridge(window_, [](auto, POINTL) {});
    ASSERT_TRUE(bridge.ready());
    bridge.begin_external_drag();
    ASSERT_TRUE(::IsWindowVisible(bridge.window()));
    bool blocker_above = false;
    for (HWND previous = ::GetWindow(bridge.window(), GW_HWNDPREV); previous;
         previous = ::GetWindow(previous, GW_HWNDPREV)) {
        if (previous == blocker) { blocker_above = true; break; }
    }
    EXPECT_TRUE(blocker_above);
    EXPECT_EQ(::GetForegroundWindow(), foreground);
    bridge.end_drag();
    EXPECT_FALSE(::IsWindowVisible(bridge.window()));
}

TEST_F(WindowsFileDrop, RefreshAndShutdownTolerateADestroyedWindow) {
    LegacyWindowsFileDrop bridge(window_, [](auto, POINTL) { FAIL() << "unexpected drop"; });
    bridge.refresh();
    ASSERT_TRUE(::DestroyWindow(window_));
    window_ = nullptr;
    bridge.refresh();
}
} // namespace
#endif
