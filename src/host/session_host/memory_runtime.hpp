#pragma once

#include "config/config.hpp"
#include "memory/memory_forget.hpp"
#include "memory/memory_service.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace acecode {

class ToolExecutor;
class MemorySummaryScheduler;
struct MemorySchedulerHost;

// 哪个入口创建的运行时。headless 只提供工具与注入,不跑记忆摘要。
enum class MemorySurface {
    Tui,
    Daemon,
    Headless,
};

// 记忆的组合根(openspec unify-memory-system D1):TUI、daemon、headless 各调用
// 一次 create_memory_runtime,把它交给 ToolExecutor、SessionRegistryDeps 与
// AgentLoopServices;子会话沿用父会话的运行时。接口按日后整体迁入
// host/app_runtime 的方式设计,本期不创建那个模块(重构 D10)。
class MemoryRuntime {
public:
    MemoryRuntime(std::shared_ptr<MemoryService> service, MemorySurface surface);
    ~MemoryRuntime();
    MemoryRuntime(const MemoryRuntime&) = delete;
    MemoryRuntime& operator=(const MemoryRuntime&) = delete;

    const std::shared_ptr<MemoryService>& service() const { return service_; }
    MemorySurface surface() const { return surface_; }

    // 注册 memory_read / memory_write。工具始终注册(headless 的 --list-tools 与
    // --disable-tools 能看到它们);记忆关闭时由请求组装把它们滤出模型侧工具表。
    void register_tools(ToolExecutor& tools) const;

    // 进程内设置变更(网页 / TUI 设置页保存后)立即生效。
    void update_config(const MemoryConfig& config);

    MemoryForgetResult forget_session(const std::string& session_id,
                                      const std::string& project_dir);

    // 记忆摘要调度器(只在 daemon 与 TUI 启动;headless 调用是空操作)。
    void start_summary_scheduler(MemorySchedulerHost host);
    void stop_summary_scheduler();
    MemorySummaryScheduler* scheduler() const { return scheduler_.get(); }

private:
    std::shared_ptr<MemoryService> service_;
    MemorySurface surface_;
    std::uint64_t purge_listener_ = 0;
    std::unique_ptr<MemorySummaryScheduler> scheduler_;
};

// data_dir 为空时用默认数据目录(get_acecode_dir())。全局目录创建失败时本进程
// 记忆整体关闭(不改写用户的 config.json)。
std::shared_ptr<MemoryRuntime> create_memory_runtime(const AppConfig& config,
                                                     const std::string& data_dir,
                                                     MemorySurface surface);

} // namespace acecode
