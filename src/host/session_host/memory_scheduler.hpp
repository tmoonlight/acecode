#pragma once

#include "config/config.hpp"
#include "memory/memory_service.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace acecode {

// 调度器向宿主(daemon / TUI)要的东西。全部可空:为空时对应能力关闭。
struct MemorySchedulerHost {
    // 本进程服务的工作区项目目录(<data_dir>/projects/<hash>)。
    std::function<std::vector<std::string>()> project_dirs;
    // 会话是否正在进行回合(注册表忙碌状态 / TUI 当前回合)。
    std::function<bool(const std::string& session_id)> session_busy;
    // 完整配置快照(saved_models、默认模型等,用于解析摘要模型)。
    std::function<AppConfig()> app_config;
    // config.json 路径;调度器每轮比较修改时间,变了就重读 memory 段(D10)。
    std::string config_path;
    // 在会话里发一条只进界面的系统通知(/memory flush 完成时)。
    std::function<void(const std::string& session_id, const std::string& text)> notify;
    // 调度周期;测试注入更短的间隔。
    std::chrono::milliseconds interval{std::chrono::minutes(2)};
    // 当前时间(epoch 毫秒);测试注入假时钟。
    std::function<std::int64_t()> now_ms;
    // 测试注入的模型调用:给定 (saved model 名, system, user) 返回模型输出;
    // 为空时按配置解析 provider 并发起无工具的非流式请求。
    std::function<std::string(const std::string& model_name, const std::string& system,
                              const std::string& user, std::string& error,
                              bool& context_overflow)> complete;
};

struct MemoryFlushReport {
    bool started = false;
    std::string message;   // 立即回复给用户的文本
};

// 记忆摘要状态(GET /api/memory 与个性化页)。
struct MemorySummaryStatus {
    bool enabled = false;
    std::size_t global_inbox = 0;
    std::size_t workspace_inbox = 0;
    std::int64_t last_extraction_ms = 0;
    std::int64_t last_consolidation_ms = 0;
    std::string last_error;
    std::int64_t last_error_ms = 0;
};

MemorySummaryStatus read_memory_summary_status(MemoryService& memory,
                                               const std::string& project_dir);

class MemorySummaryScheduler {
public:
    MemorySummaryScheduler(std::shared_ptr<MemoryService> memory, MemorySchedulerHost host);
    ~MemorySummaryScheduler();
    MemorySummaryScheduler(const MemorySummaryScheduler&) = delete;
    MemorySummaryScheduler& operator=(const MemorySummaryScheduler&) = delete;

    void start();
    void stop();

    // 跑一轮调度(测试直接调用;线程里每个周期调用一次)。
    void tick();

    // /memory flush:记忆摘要未开启时只返回说明;否则排队,调度线程立即提炼该
    // 会话并整合全局与该工作区,完成后经 host.notify 报告。
    MemoryFlushReport request_flush(const std::string& session_id,
                                    const std::string& project_dir);

    // 立即同步执行一次 flush(测试用;线程里也走这条)。返回完成通知文本。
    std::string run_flush(const std::string& session_id, const std::string& project_dir);

    // 调度线程与对象共享的状态(实现细节,定义在 .cpp)。线程只持有它的 shared_ptr,
    // 不捕获调度器对象本身。
    struct State;

private:
    std::shared_ptr<State> state_;
};

} // namespace acecode
