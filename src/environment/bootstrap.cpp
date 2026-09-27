#include "bootstrap.hpp"

#include "terminal_runtime.hpp"
#include "utils/logger.hpp"
#include "utils/state_file.hpp"
#include "utils/utf8_path.hpp"

#include <exception>
#include <filesystem>

namespace acecode::environment {

namespace {

std::string default_config_file_path() {
    return path_to_utf8(path_from_utf8(get_acecode_dir()) / "config.json");
}

// 只把 toolchains / console 两段写回磁盘:走 read-modify-write 而不是整份
// save_config(cfg),避免把 load_config() 应用的环境变量覆盖值(API key 等)
// 落进 config.json。
bool persist_environment_sections(const AppConfig& cfg) {
    const std::string path = default_config_file_path();
    try {
        AppConfig disk = load_config_from_path(path, /*apply_environment_overrides=*/false);
        disk.toolchains = cfg.toolchains;
        disk.console = cfg.console;
        save_config(disk, path);
        return true;
    } catch (const std::exception& e) {
        LOG_WARN(std::string("[environment] failed to persist detection results: ") + e.what());
        return false;
    }
}

}  // namespace

bool redetect_toolchains_into(ToolchainsConfig& cfg) {
    return merge_detected_toolchains(cfg, detect_toolchains());
}

bool persist_resolved_terminal(ConsoleConfig& console, const ResolvedTerminal& resolved) {
    if (!resolved.usable || resolved.id.empty()) return false;
    bool changed = false;
    if (console.default_shell != resolved.id) {
        console.default_shell = resolved.id;
        changed = true;
    }
    // 只记绝对路径。裸名(powershell.exe / cmd.exe)走 PATH,记成显式路径反而会在
    // 下次启动被判成"配置路径不存在"而报回退。
    if (path_from_utf8(resolved.program).is_absolute()) {
        auto it = console.shell_paths.find(resolved.id);
        if (it == console.shell_paths.end() || it->second != resolved.program) {
            console.shell_paths[resolved.id] = resolved.program;
            changed = true;
        }
    }
    return changed;
}

BootstrapReport bootstrap(AppConfig& cfg, const BootstrapOptions& options) {
    BootstrapReport report;

    // 1. 工具链首次探测:claim 成功的那个进程负责探测 + 落盘。
    if (options.persist_detection) {
        const auto claim = try_claim_state_flag(kToolchainsAutodetectedFlag);
        if (claim.claimed) {
            report.toolchains_detected = true;
            const auto detected = detect_toolchains();
            if (fill_unset_toolchains(cfg.toolchains, detected)) {
                if (!persist_environment_sections(cfg)) write_state_flag(kToolchainsAutodetectedFlag, false);
            }
            for (const auto& [id, dir] : detected.dirs) {
                LOG_INFO("[environment] first-launch toolchain detected: " + id + "=" + dir);
            }
        }
    }

    // 2. PATH 前缀(每次启动都做,注入项来自当前配置)。
    report.toolchain_path = apply_toolchain_path(cfg.toolchains);

    // 3. 默认终端探测 + 首次落盘。
    if (options.probe_terminal) {
        const auto resolution = terminal().reresolve(cfg.console);
        if (resolution.resolved.usable) report.terminal = resolution.resolved;
        if (options.persist_detection && cfg.console.default_shell.empty() &&
                resolution.resolved.usable) {
            const auto claim = try_claim_state_flag(kTerminalAutodetectedFlag);
            if (claim.claimed) {
                report.terminal_detected = true;
                if (persist_resolved_terminal(cfg.console, resolution.resolved)) {
                    if (!persist_environment_sections(cfg)) write_state_flag(kTerminalAutodetectedFlag, false);
                }
                LOG_INFO("[environment] first-launch terminal detected: " +
                         resolution.resolved.id + " (" + resolution.resolved.program + ")");
            }
        }
    }
    return report;
}

}  // namespace acecode::environment
