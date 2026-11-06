#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace acecode::desktop {

struct OpenInExplorerResult {
    bool ok = false;
    std::string error;
};

struct ValidatedOpenDirectory {
    bool ok = false;
    std::filesystem::path path;
    std::string error;
};

enum class OpenInExplorerTargetKind {
    Directory,
    File,
};

struct ValidatedOpenTarget {
    bool ok = false;
    std::filesystem::path path;
    OpenInExplorerTargetKind kind = OpenInExplorerTargetKind::Directory;
    std::string error;
};

using DirectoryOpenLauncher = std::function<bool(const std::filesystem::path&, std::string&)>;
using OpenInExplorerLauncher = std::function<bool(
    const std::filesystem::path&,
    OpenInExplorerTargetKind,
    std::string&)>;

enum class FileManagerWindowAction {
    None,
    Raise,
};

// 资源管理器窗口出现后,要不要由我们把它拉到前台。窗口句柄用整数表示(0 = 没有)。
// foreground_before: 发起打开时的前台窗口;foreground_now: 窗口出现后的前台窗口;
// opened_window: 显示目标文件夹的资源管理器窗口。前台仍停在发起方 = 新窗口被压在
// 后面,需要拉起;前台已是别的窗口 = 用户已经切走,不抢焦点。
FileManagerWindowAction decide_file_manager_window_action(
    std::uintptr_t foreground_before,
    std::uintptr_t foreground_now,
    std::uintptr_t opened_window);

ValidatedOpenTarget validate_open_in_explorer_request(
    const std::string& path_utf8);

ValidatedOpenDirectory validate_open_directory_request(
    const std::string& path_utf8);

// Windows 默认实现会等资源管理器窗口出现后才返回(通常 0.3–0.6 秒,最长约
// 2 秒);Desktop 的 WebView UI 线程经异步 bridge 放到后台调用。
OpenInExplorerResult open_path_in_file_manager(
    const std::string& path_utf8,
    OpenInExplorerLauncher launcher = {});

OpenInExplorerResult open_directory_in_file_manager(
    const std::string& path_utf8,
    DirectoryOpenLauncher launcher = {});

} // namespace acecode::desktop
