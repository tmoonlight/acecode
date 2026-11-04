#include "installer_strings.hpp"

namespace acecode::installer {

InstallerStrings installer_strings(bool chinese) {
    InstallerStrings s;
    if (chinese) {
        s.setup_title = "ACECode 安装";
        s.uninstall_window_title = "卸载 ACECode";
        s.change = "更改";
        s.add_to_path = "添加到 PATH";
        s.desktop_shortcut = "桌面快捷方式";
        s.install = "安装";
        s.update = "更新";
        s.reinstall = "重新安装";
        s.preparing = "正在准备";
        s.extracting = "正在解压文件";
        s.replacing = "正在安装";
        s.integrating = "正在创建快捷方式";
        s.installed_title = "安装完成";
        s.key_placeholder = "ACEModel Key(可选)";
        s.launch = "启动 ACECode";
        s.done = "完成";
        s.failed_title = "安装失败";
        s.retry = "重试";
        s.close = "关闭";
        s.cancel = "取消";
        s.running_title = "请先退出 ACECode";
        s.running_body = "托盘里的 ACECode 也需要退出。";
        s.uninstall_title = "卸载 ACECode";
        s.uninstall_body = "会话、设置和技能会保留。";
        s.uninstall = "卸载";
        s.uninstalling = "正在卸载";
        s.uninstalled_title = "已卸载";
        s.uninstall_failed_title = "卸载失败";
        s.key_save_failed = "ACEModel 配置写入失败";
        s.log_prefix = "日志:";
        s.terminal_shortcut = kTerminalShortcutZh;
        s.payload_missing = "这个安装程序里没有 ACECode 安装包。";
        s.payload_damaged = "安装包已损坏,请重新下载。";
        s.payload_wrong_target = "这个安装包与当前安装程序的架构不符,请重新下载。";
        return s;
    }
    s.setup_title = "ACECode Setup";
    s.uninstall_window_title = "Uninstall ACECode";
    s.change = "Change";
    s.add_to_path = "Add to PATH";
    s.desktop_shortcut = "Desktop shortcut";
    s.install = "Install";
    s.update = "Update";
    s.reinstall = "Reinstall";
    s.preparing = "Preparing";
    s.extracting = "Extracting files";
    s.replacing = "Installing";
    s.integrating = "Creating shortcuts";
    s.installed_title = "Installed";
    s.key_placeholder = "ACEModel key (optional)";
    s.launch = "Launch ACECode";
    s.done = "Done";
    s.failed_title = "Installation failed";
    s.retry = "Retry";
    s.close = "Close";
    s.cancel = "Cancel";
    s.running_title = "Quit ACECode first";
    s.running_body = "ACECode in the system tray also needs to quit.";
    s.uninstall_title = "Uninstall ACECode";
    s.uninstall_body = "Sessions, settings, and skills are kept.";
    s.uninstall = "Uninstall";
    s.uninstalling = "Uninstalling";
    s.uninstalled_title = "Uninstalled";
    s.uninstall_failed_title = "Uninstall failed";
    s.key_save_failed = "Could not save the ACEModel key";
    s.log_prefix = "Log: ";
    s.terminal_shortcut = kTerminalShortcutEn;
    s.payload_missing = "This setup program does not contain an ACECode package.";
    s.payload_damaged = "The package is damaged. Download the installer again.";
    s.payload_wrong_target = "The package does not match this installer. Download it again.";
    return s;
}

std::string failure_message(const InstallerStrings& strings, FailureKind kind) {
    const bool zh = strings.terminal_shortcut == kTerminalShortcutZh;
    switch (kind) {
        case FailureKind::running:
            return strings.running_body;
        case FailureKind::invalid_dir:
            return zh ? "不能安装到这个位置,请换一个文件夹。"
                      : "ACECode cannot be installed here. Choose another folder.";
        case FailureKind::folder_not_empty:
            return zh ? "所选文件夹不为空,请换一个位置。"
                      : "The selected folder is not empty. Choose another location.";
        case FailureKind::dir_in_use:
            return zh ? "安装目录正被占用。关闭在该目录中打开的终端或资源管理器窗口后重试。"
                      : "The install folder is in use. Close terminals or Explorer windows "
                        "opened there, then retry.";
        case FailureKind::damaged_payload:
            return strings.payload_damaged;
        case FailureKind::extract:
            return zh ? "解压文件失败,请确认磁盘空间充足后重试。"
                      : "Extracting files failed. Check free disk space and retry.";
        case FailureKind::write:
            return zh ? "写入系统设置失败。" : "Writing system settings failed.";
        case FailureKind::no_record:
            return zh ? "没有找到 ACECode 的安装记录。" : "No ACECode installation was found.";
        case FailureKind::remove:
            return zh ? "部分文件无法删除,关闭占用它们的程序后重试。"
                      : "Some files could not be removed. Close programs using them and retry.";
        case FailureKind::none:
            break;
    }
    return {};
}

}  // namespace acecode::installer
