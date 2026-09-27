#pragma once

// Agent 工具链目录(openspec: agent-toolchain-directories)。
//
// 用户(或首次启动的自动探测)声明 Python / Node.js / C# 工具所在目录,启动时按
// python → node → csharp 顺序前插到**进程自身**的 PATH。这是唯一能覆盖全部子进程
// 生成点(bash 工具、MCP stdio、hooks、LSP、控制台 PTY、worktree git)的办法 ——
// 它们都直接继承 daemon 的环境,而 cpp-mcp 的 stdio transport 根本不暴露 env。
//
// 探测走 platform::which(PATHEXT 感知),排除 Windows 商店的 app-execution alias 占位
// 程序(WindowsApps\python.exe 只是个打开商店的桩,退出码 9009)。

#include "config/config.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace acecode::environment {

// 固定的三个工具链 id(顺序即 PATH 前插顺序)。
const std::vector<std::string>& toolchain_ids();
std::string toolchain_label(const std::string& id);                   // Python / Node.js / C#
std::vector<std::string> toolchain_anchor_commands(const std::string& id);  // python,python3 / node / dotnet

using WhichFn = std::function<std::optional<std::string>(const std::string& command)>;

// 路径是否位于 \Microsoft\WindowsApps\ 下(商店 alias 桩)。大小写与分隔符不敏感。
bool is_windows_app_execution_alias(const std::string& path);

struct ToolchainDetection {
    std::map<std::string, std::string> dirs;     // id → 探测到的目录(未找到则无此键)
    std::map<std::string, std::string> anchors;  // id → 命中的可执行文件完整路径
    std::string dir_for(const std::string& id) const;
};

ToolchainDetection detect_toolchains(const WhichFn& which);
ToolchainDetection detect_toolchains();  // platform::which

// 在指定目录里找该工具链的锚点可执行文件(设置页状态显示用);找不到返回空串。
std::string find_toolchain_anchor_in_dir(const std::string& id, const std::string& dir);

// 按 id 读写配置字段。
std::string& toolchain_dir_ref(ToolchainsConfig& cfg, const std::string& id);
std::string toolchain_dir(const ToolchainsConfig& cfg, const std::string& id);

// 探测结果并入配置:找到的覆盖,没找到的保留(「重新检测」语义)。返回是否有变化。
bool merge_detected_toolchains(ToolchainsConfig& cfg, const ToolchainDetection& detected);
// 只填未设置项(首次启动语义)。返回是否有变化。
bool fill_unset_toolchains(ToolchainsConfig& cfg, const ToolchainDetection& detected);

// (显示名, 目录),按固定顺序,只含非空。
std::vector<std::pair<std::string, std::string>> configured_toolchain_dirs(
    const ToolchainsConfig& cfg);

// 纯函数:把 new_dirs 前插到 current_path。先移除上次注入的 previously_injected 与
// 已存在的同名项(Windows 比较不区分大小写、忽略尾部分隔符),再去重前插。
std::string compute_path_with_prefix(const std::string& current_path,
                                     const std::vector<std::string>& previously_injected,
                                     const std::vector<std::string>& new_dirs,
                                     char separator);

struct PathPrefixResult {
    std::vector<std::pair<std::string, std::string>> applied;  // (显示名, 目录) 实际注入
    std::vector<std::pair<std::string, std::string>> skipped;  // (显示名, 目录) 目录不存在
};

// 真实生效:改进程自身 PATH 并记录注入项;再次调用先摘掉旧项再前插新项,
// 所以设置页保存后无需重启。
PathPrefixResult apply_toolchain_path(const ToolchainsConfig& cfg);

// 当前已注入的 (显示名, 目录) 快照(system prompt 的 Toolchains 行用它)。
std::vector<std::pair<std::string, std::string>> applied_toolchain_dirs();

// 进程 PATH 读写(平台封装,供 apply 与测试用)。
std::string current_process_path();
void set_process_path(const std::string& value);
char path_list_separator();

void reset_toolchain_runtime_for_test();

}  // namespace acecode::environment
