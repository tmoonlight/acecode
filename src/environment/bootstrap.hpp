#pragma once

// 三个入口(TUI main.cpp / daemon cli.cpp / headless_runner.cpp)在 load_config 之后
// 各调一次的环境引导:
//   1. 首次启动探测工具链目录并落盘(state.json 标记 toolchains_autodetected)
//   2. 把工具链目录前插到进程 PATH
//   3. 启动探测默认终端并发布到 TerminalRuntime;首次启动把结果落盘
//      (state.json 标记 terminal_autodetected)
//
// headless 走 persist=false:只生效不写 config,`acecode -p` 不该悄悄改用户配置。
// state 标记用 try_claim_state_flag,Desktop 多 workspace 同时拉起多个 daemon 时
// 只有一个真的做探测与落盘。

#include "config/config.hpp"
#include "terminal_resolver.hpp"
#include "toolchains.hpp"

#include <optional>

namespace acecode::environment {

struct BootstrapOptions {
    bool persist_detection = true;  // 首次启动探测结果写回 config.json
    bool probe_terminal = true;     // 跑终端启动探测(测试可关)
};

struct BootstrapReport {
    PathPrefixResult toolchain_path;
    bool toolchains_detected = false;   // 本次做了首次工具链探测
    bool terminal_detected = false;     // 本次做了首次终端探测并落盘
    std::optional<ResolvedTerminal> terminal;
};

BootstrapReport bootstrap(AppConfig& cfg, const BootstrapOptions& options);

// 供设置页路由复用:重新探测工具链并合并进配置(找到的覆盖,没找到的保留),
// 返回是否有变化;不落盘、不改 PATH,由调用方保存后再 apply_toolchain_path。
bool redetect_toolchains_into(ToolchainsConfig& cfg);

// 供设置页路由复用:把解析结果落到配置(类型 + 绝对路径),返回是否有变化。
bool persist_resolved_terminal(ConsoleConfig& console, const ResolvedTerminal& resolved);

inline constexpr const char* kToolchainsAutodetectedFlag = "toolchains_autodetected";
inline constexpr const char* kTerminalAutodetectedFlag = "terminal_autodetected";

}  // namespace acecode::environment
