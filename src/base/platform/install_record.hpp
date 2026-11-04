#pragma once

// 安装程序(acecode-setup)在安装目录里留下的安装记录,以及 Windows「设置 > 应用」
// 里的卸载登记。安装程序写入;自升级换完文件后只刷新其中的版本号,
// 让「设置 > 应用」显示的版本与实际文件一致。便携 zip 解压出来的目录没有记录,
// 所有函数对它都是空操作。

#include <filesystem>
#include <optional>
#include <string>

namespace acecode::install_record {

inline constexpr const char* kMarkerFileName = ".acecode-install.json";
// HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\ACECode
inline constexpr const char* kUninstallRegistryParent =
    "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
inline constexpr const char* kUninstallKeyName = "ACECode";

struct InstallRecord {
    int schema = 1;
    std::string version;
    std::string target;
    bool add_to_path = false;
    bool desktop_shortcut = false;
    std::string installed_at;
};

std::string serialize(const InstallRecord& record);
std::optional<InstallRecord> parse(const std::string& text, std::string* error = nullptr);

std::optional<InstallRecord> read_marker(const std::filesystem::path& install_dir);
bool write_marker(const std::filesystem::path& install_dir,
                  const InstallRecord& record,
                  std::string* error = nullptr);

// 比较两个 Windows 目录是否指同一处:忽略大小写、斜杠方向和末尾分隔符。
bool same_install_dir(const std::string& a, const std::string& b);

// 自升级成功后调用。install_dir 有安装记录时更新记录里的版本;Windows 上若卸载
// 登记的 InstallLocation 指向同一目录,再更新 DisplayVersion。返回是否改动了记录。
bool refresh_installed_version(const std::filesystem::path& install_dir,
                               const std::string& version);

}  // namespace acecode::install_record
