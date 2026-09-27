#pragma once

// 跨平台目录选择器,用于"+ 添加项目" 入口。

#include <optional>
#include <string>

namespace acecode::desktop {

// parent_hwnd: Windows 上传 HWND,Linux Desktop 上传 GtkWindow,作为模态 owner。
// nullptr 也合法;Windows 尝试当前前台窗口,Linux 使用外部选择器工具。
// 返回:用户选定的绝对路径(正斜杠 normalize 由调用方做);取消 / 失败 → nullopt。
std::optional<std::string> pick_folder(void* parent_hwnd);

// 可区分结果的版本:"用户取消"与"环境缺失选择器工具"是两种必须区别对待的
// 失败 —— 前者静默合理,后者必须把原因透传到 UI,否则用户点"添加项目"看到的
// 就是毫无反应(Linux 上 zenity/kdialog 双缺失时的真实事故)。
struct FolderPickOutcome {
    std::optional<std::string> path;  // 有值 = 用户选中
    std::string error;                // 非空 = 环境问题(如缺 zenity/kdialog)
    // path 为空且 error 为空 = 用户取消
};
FolderPickOutcome pick_folder_outcome(void* parent_hwnd);

#if !defined(_WIN32) && !defined(__APPLE__)
// Desktop installs its GTK implementation on the GUI thread. Headless users
// retain the external-tool fallback without linking a GUI toolkit.
using LinuxFolderPicker = FolderPickOutcome (*)(void* parent);
void set_linux_folder_picker(LinuxFolderPicker picker);
#endif

// 原生“另存为”对话框。suggested_filename 只包含建议文件名(例如
// "会话标题.md"),用户可在系统对话框里修改文件名和保存位置。
struct SaveFilePickOutcome {
    std::optional<std::string> path;  // 有值 = 用户确认的完整文件路径
    std::string error;                // 非空 = 对话框环境/系统错误
    // path 为空且 error 为空 = 用户取消
};
SaveFilePickOutcome pick_save_file_outcome(
    void* parent_hwnd,
    const std::string& suggested_filename);
std::optional<std::string> pick_save_file(
    void* parent_hwnd,
    const std::string& suggested_filename);

} // namespace acecode::desktop
