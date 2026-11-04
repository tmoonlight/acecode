#pragma once

// 安装界面文案:系统界面语言是中文时用中文,其余用英文。

#include "install_engine.hpp"

#include <string>

namespace acecode::installer {

struct InstallerStrings {
    std::string setup_title;
    std::string uninstall_window_title;
    std::string change;
    std::string add_to_path;
    std::string desktop_shortcut;
    std::string install;
    std::string update;
    std::string reinstall;
    std::string preparing;
    std::string extracting;
    std::string replacing;
    std::string integrating;
    std::string installed_title;
    std::string key_placeholder;
    std::string launch;
    std::string done;
    std::string failed_title;
    std::string retry;
    std::string close;
    std::string cancel;
    std::string running_title;
    std::string running_body;
    std::string uninstall_title;
    std::string uninstall_body;
    std::string uninstall;
    std::string uninstalling;
    std::string uninstalled_title;
    std::string uninstall_failed_title;
    std::string key_save_failed;
    std::string log_prefix;
    std::string terminal_shortcut;
    std::string payload_missing;
    std::string payload_damaged;
    std::string payload_wrong_target;
};

InstallerStrings installer_strings(bool chinese);
std::string failure_message(const InstallerStrings& strings, FailureKind kind);

}  // namespace acecode::installer
