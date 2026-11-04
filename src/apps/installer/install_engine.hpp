#pragma once

// 安装 / 卸载流程本身,界面和静默模式共用。
//
// 安装:把资源区的 zip 解到安装目录旁边的「<名>.setup-new」,写入卸载程序和安装记录,
// 再用两次目录改名把新目录换进去(旧目录先改名为「<名>.setup-old」,换入失败就改回去),
// 所以中途失败不会留下半新半旧的安装目录。之后写卸载登记、快捷方式和 PATH。
// 卸载:按安装记录确认目录是安装程序装的,再撤销快捷方式、PATH、登记并删除整个目录。
// 用户数据(~/.acecode)两者都不碰。

#include "install_plan.hpp"
#include "payload_archive.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace acecode::installer {

inline constexpr const char* kTerminalShortcutZh = "ACECode 终端";
inline constexpr const char* kTerminalShortcutEn = "ACECode Terminal";

struct Payload {
    PayloadInfo info;
    std::string_view archive;
    std::string_view uninstaller;
};

enum class PayloadProblem { none, missing, damaged, wrong_target };

// 只读描述、核对目标平台与大小;哈希在 install() 的准备阶段再算(在工作线程上)。
std::optional<Payload> load_embedded_payload(PayloadProblem* problem, std::string* detail);
std::string build_target();

struct ExistingInstall {
    std::string install_dir;
    std::string version;
    bool legacy = false;
    bool add_to_path = true;
    bool desktop_shortcut = false;
};
std::optional<ExistingInstall> find_existing_install();

struct InstallRequest {
    std::string install_dir;
    bool add_to_path = true;
    bool desktop_shortcut = false;
    std::string terminal_shortcut_name = kTerminalShortcutEn;
};

enum class FailureKind {
    none,
    running,
    invalid_dir,
    folder_not_empty,
    dir_in_use,
    damaged_payload,
    extract,
    write,
    no_record,
    remove,
};

struct Outcome {
    bool ok = false;
    FailureKind failure = FailureKind::none;
    std::string detail;
};

class StepObserver {
public:
    virtual ~StepObserver() = default;
    virtual void on_step(InstallStep step, double fraction) = 0;
};

Outcome install(const Payload& payload, const InstallRequest& request, StepObserver& observer);
Outcome uninstall(const std::string& install_dir, StepObserver& observer);

bool launch_installed_app(const std::string& install_dir);
std::optional<std::string> resolve_uninstall_dir(const InstallerArgs& args);
// 卸载程序不能删除正在运行的自己:先复制到临时目录,从副本完成卸载。
std::optional<std::string> prepare_uninstall_copy(std::string* error);
void cleanup_stale_uninstall_copies(const std::string& install_dir = {});

void setup_log(const std::string& line);
std::string setup_log_path();

}  // namespace acecode::installer
