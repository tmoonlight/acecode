#pragma once

// 进程级默认终端运行时(openspec: agent-default-terminal)。
//
// 启动探测每个候选要真的拉起一个进程(Windows 上 50~100ms),不能每次 bash 调用
// 都做;也不能让 system prompt 的 `Shell:` 行随探测结果抖动打穿 prompt cache。
// 所以解析结果在这里发布一次,bash 工具 / 控制台默认 shell / system prompt 都读
// 快照;设置保存或「重新检测」时 reresolve 重新发布。TUI / daemon / headless 三个
// 入口在 bootstrap 里各接一次(与 web_search / lsp 的单例同款)。

#include "terminal_resolver.hpp"

#include <mutex>
#include <optional>

namespace acecode::environment {

class TerminalRuntime {
public:
    static TerminalRuntime& instance();

    // 完整探测并发布。返回本次结果(含候选明细,供 REST 展示)。
    TerminalResolution reresolve(const ConsoleConfig& console);
    TerminalResolution reresolve(const ConsoleConfig& console,
                                 const ShellProbe& probe,
                                 const LaunchProbe& launch);

    // 当前生效终端快照。未解析过、或没有任何候选可用 → nullopt,消费方走改动前的
    // 行为(Windows cmd.exe /c、POSIX /bin/sh -c)。
    std::optional<ResolvedTerminal> current() const;

    // 上次完整解析结果(含候选)。
    std::optional<TerminalResolution> last() const;
    void publish(const TerminalResolution& resolution);

    // 测试专用:直接发布一个终端 / 清空。
    void set_for_test(const ResolvedTerminal& terminal);
    void reset_for_test();

private:
    mutable std::mutex mu_;
    std::optional<TerminalResolution> last_;
};

TerminalRuntime& terminal();

}  // namespace acecode::environment
