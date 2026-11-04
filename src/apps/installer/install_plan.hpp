#pragma once

// 安装程序的纯逻辑:命令行、安装目录、PATH 列表、卸载登记项、进度分段、版本比较。
// 不碰系统,所有平台都编译,方便单测。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace acecode::installer {

inline constexpr const char* kProductName = "ACECode";
inline constexpr const char* kPublisher = "ACECode";
inline constexpr const char* kProductUrl = "https://github.com/tmoonlight/acecode";
inline constexpr const char* kUninstallerFileName = "uninstall.exe";
inline constexpr const char* kDesktopExecutable = "acecode-desktop.exe";
inline constexpr const char* kCliExecutable = "acecode.exe";
// 旧 Inno Setup 安装包(installer/windows/acecode.iss)的卸载登记键名。
inline constexpr const char* kLegacyInnoUninstallKey =
    "{7C3E2A91-5B64-4F0E-9C2A-ACEC0DE00001}_is1";

enum class InstallerMode { install, uninstall };

struct InstallerArgs {
    InstallerMode mode = InstallerMode::install;
    bool silent = false;
    std::string install_dir;
    // 卸载程序先把自己复制到临时目录再重启;副本带着原进程 PID,等它退出后再删安装目录。
    unsigned long wait_pid = 0;
    bool relaunched = false;
};

struct ArgsParseResult {
    std::optional<InstallerArgs> args;
    std::string error;
};

// argv 不含程序名。
ArgsParseResult parse_installer_args(const std::vector<std::string>& argv);
std::vector<std::string> uninstall_relaunch_args(const InstallerArgs& args,
                                                 unsigned long parent_pid,
                                                 const std::string& install_dir);
// 按 Windows CreateProcess 的规则给单个参数加引号。
std::string quote_windows_arg(const std::string& arg);

std::string join_windows_path(const std::string& dir, const std::string& name);
std::string trim_windows_dir(const std::string& dir);
std::string default_install_dir(const std::string& local_app_data);
// 用户自选的目录不以 ACECode 结尾时,在下面建 ACECode 子目录:卸载会删除整个安装
// 目录,不能把用户自己的文件夹当成安装目录。
std::string normalize_chosen_install_dir(const std::string& picked);
// 只接受带盘符或 UNC 前缀的绝对路径,且不能是盘符根。
bool is_acceptable_install_dir(const std::string& dir);

// PATH 列表(分号分隔)。比较忽略大小写、引号和末尾反斜杠,其它条目原样保留。
std::vector<std::string> split_path_list(const std::string& value);
bool path_list_contains(const std::string& value, const std::string& dir);
std::string path_list_with(const std::string& value, const std::string& dir);
std::string path_list_without(const std::string& value, const std::string& dir);

struct RegistryValue {
    std::string name;
    bool is_dword = false;
    std::string text;
    std::uint32_t number = 0;
};

struct UninstallEntryInput {
    std::string install_dir;
    std::string version;
    std::uint64_t installed_bytes = 0;
    std::string install_date;  // yyyymmdd
};

std::vector<RegistryValue> uninstall_entry_values(const UninstallEntryInput& input);
std::string format_install_date(int year, int month, int day);

// 进度条分段:解压占大头,其余步骤各占一小段。
enum class InstallStep { preparing, extracting, replacing, integrating, finished };
double overall_progress(InstallStep step, double step_fraction);

// 数字段逐段比较,"1.0.20" > "1.0.9";不认识的段当 0。
int compare_versions(const std::string& a, const std::string& b);

enum class InstallAction { install, update, reinstall };
InstallAction classify_install(const std::optional<std::string>& installed_version,
                               const std::string& payload_version);

}  // namespace acecode::installer
