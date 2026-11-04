#pragma once

// 安装程序用到的 Windows 系统接口:已知文件夹、HKCU 卸载登记、用户 PATH、开始菜单与
// 桌面快捷方式、按安装目录找正在运行的进程、启动程序。只写当前用户范围(HKCU、
// %LOCALAPPDATA%),不需要管理员权限。非 Windows 平台全部返回失败或空值。

#include "install_plan.hpp"

#include <optional>
#include <string>
#include <vector>

namespace acecode::installer::shell {

std::string local_app_data_dir();
std::string programs_menu_dir();
std::string desktop_dir();
std::string temp_dir();
std::string user_profile_dir();
std::string current_executable_path();

bool write_uninstall_entry(const std::string& key_name,
                           const std::vector<RegistryValue>& values,
                           std::string* error);
std::optional<std::string> uninstall_entry_location(const std::string& key_name);
std::optional<std::string> uninstall_entry_version(const std::string& key_name);
bool delete_uninstall_entry(const std::string& key_name);

// 用户 PATH(HKCU\Environment\Path)。保留原来的 REG_EXPAND_SZ / REG_SZ 类型。
bool add_to_user_path(const std::string& dir, std::string* error);
bool remove_from_user_path(const std::string& dir, std::string* error);
bool user_path_contains(const std::string& dir);
void broadcast_environment_change();

bool create_shortcut(const std::string& link_path,
                     const std::string& target,
                     const std::string& working_dir,
                     const std::string& description,
                     std::string* error);
std::optional<std::string> shortcut_target(const std::string& link_path);
// 快捷方式指向 dir 里的文件才删除,别人的同名快捷方式不动。
void remove_shortcut_if_inside(const std::string& link_path, const std::string& dir);

struct RunningProcess {
    unsigned long pid = 0;
    std::string image;
};
std::vector<RunningProcess> processes_running_from(const std::string& dir);
bool launch(const std::string& executable,
            const std::vector<std::string>& args,
            const std::string& working_dir);
// 启动并等待退出,返回退出码;启动失败返回空。
std::optional<unsigned long> launch_and_wait(const std::string& executable,
                                             const std::vector<std::string>& args,
                                             const std::string& working_dir);
void wait_for_process_exit(unsigned long pid, unsigned long timeout_ms);
std::string today_install_date();
std::string now_iso8601();

}  // namespace acecode::installer::shell
