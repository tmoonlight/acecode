#include "open_in_explorer.hpp"

#include "utils/utf8_path.hpp"

#include <system_error>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#  include <shlobj.h>
#  include <exdisp.h>
#  include <oleauto.h>
#  include <wrl/client.h>

#  include "utils/joining_thread.hpp"
#  include "utils/scope_exit.hpp"

#  include <chrono>
#  include <thread>
#  include <utility>
#else
#  include <cstdlib>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace acecode::desktop {
namespace {

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

// x64 上 PIDLIST_ABSOLUTE 带 __unaligned 限定,unique_ptr 套不上,单独包一层。
class UniquePidl {
public:
    UniquePidl() noexcept = default;
    explicit UniquePidl(PIDLIST_ABSOLUTE pidl) noexcept : pidl_(pidl) {}
    UniquePidl(UniquePidl&& other) noexcept : pidl_(std::exchange(other.pidl_, nullptr)) {}
    UniquePidl& operator=(UniquePidl&& other) noexcept {
        if (this != &other) reset(std::exchange(other.pidl_, nullptr));
        return *this;
    }
    ~UniquePidl() { reset(); }

    void reset(PIDLIST_ABSOLUTE pidl = nullptr) noexcept {
        if (pidl_) ::CoTaskMemFree(pidl_);
        pidl_ = pidl;
    }
    PIDLIST_ABSOLUTE get() const noexcept { return pidl_; }
    explicit operator bool() const noexcept { return pidl_ != nullptr; }

private:
    PIDLIST_ABSOLUTE pidl_ = nullptr;
};

// 目录窗口在 ShellExecuteEx 返回后约 0.3 秒才显示;找不到就不再管前台。
constexpr auto kFolderWindowWait = std::chrono::seconds(2);
constexpr auto kFolderWindowPoll = std::chrono::milliseconds(25);

UniquePidl parse_shell_item(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(::SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr))) return {};
    return UniquePidl(pidl);
}

// 旧做法:新起一个 explorer.exe,它再拉起独立的 Explorer 宿主进程放窗口,
// 实测约 1 秒且宿主进程在窗口关闭后不退出。只在 Shell 接口失败时兜底。
bool launch_explorer_select(const fs::path& path, std::string& error) {
    const std::wstring parameters = L"/select,\"" + path.wstring() + L"\"";
    const std::wstring working_directory = path.parent_path().wstring();
    const auto code = reinterpret_cast<intptr_t>(::ShellExecuteW(
        nullptr, L"open", L"explorer.exe", parameters.c_str(),
        working_directory.c_str(), SW_SHOWNORMAL));
    if (code > 32) return true;
    error = "ShellExecute failed: " + std::to_string(code);
    return false;
}

bool shell_window_shows(IDispatch* dispatch, PCIDLIST_ABSOLUTE folder) {
    ComPtr<IServiceProvider> provider;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IFolderView> folder_view;
    ComPtr<IPersistFolder2> persist;
    if (FAILED(dispatch->QueryInterface(IID_PPV_ARGS(provider.GetAddressOf()))) ||
        FAILED(provider->QueryService(SID_STopLevelBrowser,
                                      IID_PPV_ARGS(browser.GetAddressOf()))) ||
        FAILED(browser->QueryActiveShellView(view.GetAddressOf())) ||
        FAILED(view.As(&folder_view)) ||
        FAILED(folder_view->GetFolder(IID_PPV_ARGS(persist.GetAddressOf())))) {
        return false;
    }
    PIDLIST_ABSOLUTE current = nullptr;
    if (FAILED(persist->GetCurFolder(&current)) || !current) return false;
    const bool equal = ::ILIsEqual(current, folder) != FALSE;
    ::CoTaskMemFree(current);
    return equal;
}

// IShellWindows::FindWindowSW 在 Windows 11 上对资源管理器窗口一律返回 S_FALSE
// (实测路径 / PIDL 字节数组都一样),只能逐个窗口取当前文件夹比对。
HWND find_folder_window(IShellWindows* windows, PCIDLIST_ABSOLUTE folder) {
    long count = 0;
    if (FAILED(windows->get_Count(&count))) return nullptr;
    for (long i = 0; i < count; ++i) {
        VARIANT index;
        ::VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> dispatch;
        if (FAILED(windows->Item(index, dispatch.GetAddressOf())) || !dispatch) continue;
        if (!shell_window_shows(dispatch.Get(), folder)) continue;
        ComPtr<IWebBrowserApp> app;
        SHANDLE_PTR window = 0;
        if (SUCCEEDED(dispatch.As(&app)) && SUCCEEDED(app->get_HWND(&window)) && window) {
            return reinterpret_cast<HWND>(window);
        }
    }
    return nullptr;
}

void raise_folder_window(HWND window) {
    if (::IsIconic(window)) ::ShowWindow(window, SW_RESTORE);
    ::BringWindowToTop(window);
    if (::SetForegroundWindow(window) && ::GetForegroundWindow() == window) return;
    // 本进程没有前台权(daemon / TUI 发起)时 SetForegroundWindow 会被拒。
    // TOPMOST 脉冲把它抬到普通窗口最上层后立刻撤销,不会让资源管理器一直置顶;
    // 用户自己设成置顶的窗口已经在最上层,不去碰它,免得撤掉用户的置顶。
    if ((::GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
        constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW;
        ::SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, flags);
        ::SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    }
    ::SetForegroundWindow(window);
}

void bring_folder_window_forward(PCIDLIST_ABSOLUTE folder, HWND foreground_before) {
    ComPtr<IShellWindows> windows;
    if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(windows.GetAddressOf()))) || !windows) {
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + kFolderWindowWait;
    HWND opened = nullptr;
    for (;;) {
        opened = find_folder_window(windows.Get(), folder);
        if (opened && ::IsWindowVisible(opened)) break;
        if (std::chrono::steady_clock::now() >= deadline) return;
        std::this_thread::sleep_for(kFolderWindowPoll);
    }
    const auto action = decide_file_manager_window_action(
        reinterpret_cast<std::uintptr_t>(foreground_before),
        reinterpret_cast<std::uintptr_t>(::GetForegroundWindow()),
        reinterpret_cast<std::uintptr_t>(opened));
    if (action == FileManagerWindowAction::Raise) raise_folder_window(opened);
}

bool open_target_on_sta_thread(const fs::path& path,
                               OpenInExplorerTargetKind kind,
                               std::string& error) {
    const HRESULT com = ::CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    acecode::ScopeExit uninitialize([com] {
        if (SUCCEEDED(com)) ::CoUninitialize();
    });
    const HWND foreground_before = ::GetForegroundWindow();
    // 只在本进程此刻持有前台权(Desktop 刚收到点击)时生效,下一次用户输入即失效。
    // 文件夹窗口可能落在任意一个 explorer.exe(Windows 11 会单开宿主进程),
    // 事先拿不到 PID,所以放行 ASFW_ANY。
    ::AllowSetForegroundWindow(ASFW_ANY);

    const std::wstring wide_path = path.wstring();
    UniquePidl item = parse_shell_item(wide_path);
    UniquePidl folder;
    if (kind == OpenInExplorerTargetKind::File) {
        // 与 Chromium / Electron 的 showItemInFolder 相同:直接请正在运行的
        // 资源管理器打开父目录并选中文件,文件夹已开着时复用那个窗口。
        if (!item || FAILED(::SHOpenFolderAndSelectItems(item.get(), 0, nullptr, 0))) {
            if (!launch_explorer_select(path, error)) return false;
        }
        if (item) {
            folder.reset(::ILCloneFull(item.get()));
            if (folder) ::ILRemoveLastID(folder.get());
        }
    } else {
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"open";
        info.lpFile = wide_path.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!::ShellExecuteExW(&info)) {
            error = "ShellExecuteEx failed: " + std::to_string(::GetLastError());
            return false;
        }
        folder = std::move(item);
    }
    if (folder) bring_folder_window_forward(folder.get(), foreground_before);
    return true;
}
#endif

bool platform_open_target(const fs::path& path,
                          OpenInExplorerTargetKind kind,
                          std::string& error) {
#ifdef _WIN32
    // 调用方线程的 COM 状态不确定(Desktop 后台 worker / Crow 线程 / TUI 线程),
    // Shell 接口要求 STA,所以在一个干净的 STA 线程上执行并等它结束。
    bool ok = false;
    std::string thread_error;
    {
        acecode::JoiningThread shell_thread([&ok, &thread_error, path, kind] {
            try {
                ok = open_target_on_sta_thread(path, kind, thread_error);
            } catch (const std::exception& e) {
                thread_error = e.what();
            } catch (...) {
                thread_error = "failed to open path in file manager";
            }
        });
    }
    if (!ok) error = thread_error;
    return ok;
#else
#  ifdef __APPLE__
    const char* opener = "open";
#  else
    const char* opener = "xdg-open";
#  endif
    const fs::path launch_path =
#  ifdef __APPLE__
        path;
#  else
        kind == OpenInExplorerTargetKind::File ? path.parent_path() : path;
#  endif
    const std::string native_path = acecode::path_to_utf8(launch_path);
    pid_t pid = ::fork();
    if (pid < 0) {
        error = "fork failed";
        return false;
    }
    if (pid == 0) {
#  ifdef __APPLE__
        if (kind == OpenInExplorerTargetKind::File) {
            ::execlp(opener, opener, "-R", native_path.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
#  endif
        ::execlp(opener, opener, native_path.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return true;
#endif
}

} // namespace

FileManagerWindowAction decide_file_manager_window_action(
    std::uintptr_t foreground_before,
    std::uintptr_t foreground_now,
    std::uintptr_t opened_window) {
    if (opened_window == 0 || foreground_now == opened_window) {
        return FileManagerWindowAction::None;
    }
    // 切换前台的瞬间 GetForegroundWindow 可能是 0,按「还没人接手」处理。
    if (foreground_now == 0 || foreground_now == foreground_before) {
        return FileManagerWindowAction::Raise;
    }
    return FileManagerWindowAction::None;
}

ValidatedOpenTarget validate_open_in_explorer_request(
    const std::string& path_utf8) {
    if (path_utf8.empty()) {
        return {false, {}, OpenInExplorerTargetKind::Directory, "path required"};
    }

    std::error_code ec;
    auto requested = acecode::path_from_utf8(path_utf8);
    if (requested.empty()) {
        return {false, {}, OpenInExplorerTargetKind::Directory, "path required"};
    }
    if (!requested.is_absolute()) {
        return {false, {}, OpenInExplorerTargetKind::Directory, "path must be absolute"};
    }
    const bool requested_is_directory = fs::is_directory(requested, ec);
    if (ec) {
        return {false, {}, OpenInExplorerTargetKind::Directory, "failed to inspect path"};
    }
    const bool requested_is_file =
        !requested_is_directory && fs::is_regular_file(requested, ec);
    if (ec || (!requested_is_directory && !requested_is_file)) {
        return {
            false,
            {},
            OpenInExplorerTargetKind::Directory,
            "path is not an existing file or directory",
        };
    }

    auto canonical_path = fs::weakly_canonical(requested, ec);
    if (ec || canonical_path.empty()) {
        return {false, {}, OpenInExplorerTargetKind::Directory, "failed to resolve path"};
    }
    const bool canonical_is_directory = fs::is_directory(canonical_path, ec);
    if (ec) {
        return {false, {}, OpenInExplorerTargetKind::Directory, "failed to inspect path"};
    }
    const bool canonical_is_file =
        !canonical_is_directory && fs::is_regular_file(canonical_path, ec);
    if (ec || (!canonical_is_directory && !canonical_is_file)) {
        return {
            false,
            {},
            OpenInExplorerTargetKind::Directory,
            "path is not an existing file or directory",
        };
    }
    return {
        true,
        canonical_path,
        canonical_is_file
            ? OpenInExplorerTargetKind::File
            : OpenInExplorerTargetKind::Directory,
        {},
    };
}

ValidatedOpenDirectory validate_open_directory_request(
    const std::string& path_utf8) {
    auto validated = validate_open_in_explorer_request(path_utf8);
    if (!validated.ok) return {false, {}, validated.error};
    if (validated.kind != OpenInExplorerTargetKind::Directory) {
        return {false, {}, "path is not an existing directory"};
    }
    return {true, std::move(validated.path), {}};
}

OpenInExplorerResult open_path_in_file_manager(
    const std::string& path_utf8,
    OpenInExplorerLauncher launcher) {
    auto validated = validate_open_in_explorer_request(path_utf8);
    if (!validated.ok) return {false, validated.error};

    std::string error;
    auto launch = launcher
        ? std::move(launcher)
        : OpenInExplorerLauncher(platform_open_target);
    if (!launch(validated.path, validated.kind, error)) {
        if (error.empty()) error = "failed to open path in file manager";
        return {false, error};
    }
    return {true, {}};
}

OpenInExplorerResult open_directory_in_file_manager(
    const std::string& path_utf8,
    DirectoryOpenLauncher launcher) {
    auto validated = validate_open_directory_request(path_utf8);
    if (!validated.ok) return {false, validated.error};

    std::string error;
    auto launch = launcher
        ? std::move(launcher)
        : DirectoryOpenLauncher([](const fs::path& path, std::string& launch_error) {
            return platform_open_target(
                path,
                OpenInExplorerTargetKind::Directory,
                launch_error);
        });
    if (!launch(validated.path, error)) {
        if (error.empty()) error = "failed to open directory";
        return {false, error};
    }
    return {true, {}};
}

} // namespace acecode::desktop
