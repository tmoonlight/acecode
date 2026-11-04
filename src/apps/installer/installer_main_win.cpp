// acecode-setup.exe 入口。同一个可执行文件既是安装程序(资源区带安装包时)也是卸载程序
// (安装目录里的 uninstall.exe,带 --uninstall 启动)。
//
//   acecode-setup.exe [--silent] [--install-dir <目录>]
//   uninstall.exe --uninstall [--silent]
//
// 退出码:0 完成;1 失败或取消;2 安装包缺失或损坏;3 找不到安装记录;5 ACECode 正在运行;
// 64 命令行有误。

#include "desktop/dpi_win.hpp"
#include "install_engine.hpp"
#include "installer_strings.hpp"
#include "installer_window_win.hpp"
#include "platform/install_record.hpp"
#include "windows_shell.hpp"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace {

using namespace acecode::installer;

constexpr int kExitFailed = 1;
constexpr int kExitPayload = 2;
constexpr int kExitNoRecord = 3;
constexpr int kExitRunning = 5;
constexpr int kExitUsage = 64;

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length > 0 ? length : 0), L'\0');
    if (length > 0) {
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                              out.data(), length);
    }
    return out;
}

std::string narrow(const wchar_t* text) {
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string out(static_cast<std::size_t>(length - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
    return out;
}

std::vector<std::string> utf8_arguments() {
    std::vector<std::string> args;
    int count = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (!argv) return args;
    for (int i = 1; i < count; ++i) args.push_back(narrow(argv[i]));
    ::LocalFree(argv);
    return args;
}

bool prefers_chinese() {
    return PRIMARYLANGID(::GetUserDefaultUILanguage()) == LANG_CHINESE;
}

void show_message(const InstallerStrings& strings, const std::string& text, bool silent) {
    setup_log(text);
    if (silent) return;
    ::MessageBoxW(nullptr, wide(text).c_str(), wide(strings.setup_title).c_str(),
                  MB_OK | MB_ICONERROR);
}

class SilentObserver final : public StepObserver {
public:
    void on_step(InstallStep, double) override {}
};

int exit_code_for(const Outcome& outcome) {
    if (outcome.ok) return 0;
    if (outcome.failure == FailureKind::running) return kExitRunning;
    if (outcome.failure == FailureKind::no_record) return kExitNoRecord;
    return kExitFailed;
}

int run_uninstall(const InstallerArgs& args, const InstallerStrings& strings) {
    const auto dir = resolve_uninstall_dir(args);
    if (!dir) {
        show_message(strings, failure_message(strings, FailureKind::no_record), args.silent);
        return kExitNoRecord;
    }
    cleanup_stale_uninstall_copies(*dir);
    if (!args.relaunched) {
        std::string error;
        const auto copy = prepare_uninstall_copy(&error);
        if (!copy) {
            show_message(strings, error, args.silent);
            return kExitFailed;
        }
        const auto relaunch = uninstall_relaunch_args(args, ::GetCurrentProcessId(), *dir);
        const std::string folder = copy->substr(0, copy->find_last_of('\\'));
        // 「设置 > 应用」或脚本可能以安装目录为当前目录启动卸载;不离开它,目录就删不掉。
        ::SetCurrentDirectoryW(wide(folder).c_str());
        // 静默卸载由脚本调用,脚本会等进程结束再看退出码:这里等副本跑完并转交它的退出码。
        // 有界面时直接退出,让副本能删掉安装目录里的这个 exe。
        if (args.silent) {
            const auto code = shell::launch_and_wait(*copy, relaunch, folder);
            if (!code) setup_log("failed to start the uninstaller copy");
            return code ? static_cast<int>(*code) : kExitFailed;
        }
        if (!shell::launch(*copy, relaunch, folder)) {
            show_message(strings, "failed to start the uninstaller copy", args.silent);
            return kExitFailed;
        }
        return 0;
    }
    if (args.silent) {
        SilentObserver observer;
        return exit_code_for(uninstall(*dir, observer));
    }
    shell::wait_for_process_exit(args.wait_pid, 15000);
    ui::WindowConfig config;
    config.mode = InstallerMode::uninstall;
    config.strings = strings;
    config.install_dir = *dir;
    if (const auto existing = find_existing_install()) {
        if (acecode::install_record::same_install_dir(existing->install_dir, *dir)) {
            config.existing = existing;
        }
    }
    return ui::run_installer_window(config);
}

int run_install(const InstallerArgs& args, const InstallerStrings& strings) {
    PayloadProblem problem = PayloadProblem::none;
    std::string detail;
    auto payload = load_embedded_payload(&problem, &detail);
    if (!payload) {
        const std::string text = problem == PayloadProblem::wrong_target ? strings.payload_wrong_target
                                 : problem == PayloadProblem::damaged    ? strings.payload_damaged
                                                                         : strings.payload_missing;
        show_message(strings, text + " " + detail, args.silent);
        return kExitPayload;
    }
    const auto existing = find_existing_install();
    std::string dir = args.install_dir;
    if (dir.empty()) dir = existing ? existing->install_dir
                                    : default_install_dir(shell::local_app_data_dir());
    cleanup_stale_uninstall_copies(dir);
    const bool add_to_path = existing ? existing->add_to_path : true;
    const bool desktop_shortcut = existing ? existing->desktop_shortcut : false;
    if (args.silent) {
        InstallRequest request;
        request.install_dir = dir;
        request.add_to_path = add_to_path;
        request.desktop_shortcut = desktop_shortcut;
        request.terminal_shortcut_name = strings.terminal_shortcut;
        SilentObserver observer;
        return exit_code_for(install(*payload, request, observer));
    }
    ui::WindowConfig config;
    config.mode = InstallerMode::install;
    config.strings = strings;
    config.payload = payload;
    config.existing = existing;
    config.install_dir = dir;
    config.add_to_path = add_to_path;
    config.desktop_shortcut = desktop_shortcut;
    return ui::run_installer_window(config);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const InstallerStrings strings = installer_strings(prefers_chinese());
    const ArgsParseResult parsed = parse_installer_args(utf8_arguments());
    if (!parsed.args) {
        show_message(strings, parsed.error, false);
        return kExitUsage;
    }
    acecode::desktop::enable_desktop_dpi_awareness();
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    cleanup_stale_uninstall_copies();
    const int code = parsed.args->mode == InstallerMode::uninstall
                         ? run_uninstall(*parsed.args, strings)
                         : run_install(*parsed.args, strings);
    if (SUCCEEDED(com)) ::CoUninitialize();
    return code;
}
