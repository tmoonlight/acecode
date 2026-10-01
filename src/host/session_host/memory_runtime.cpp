#include "memory_runtime.hpp"

#include "memory_scheduler.hpp"

#include "memory/memory_paths.hpp"
#include "session/session_purge_listeners.hpp"
#include "tool/memory_read_tool.hpp"
#include "tool/memory_write_tool.hpp"
#include "tool/tool_executor.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>

namespace fs = std::filesystem;

namespace acecode {

MemoryRuntime::MemoryRuntime(std::shared_ptr<MemoryService> service, MemorySurface surface)
    : service_(std::move(service)), surface_(surface) {
    // 会话永久删除后撤回它的观察与摘要条目。只持弱引用:运行时先于监听表析构时
    // 回调直接失效,不会碰到已销毁的服务。
    std::weak_ptr<MemoryService> weak = service_;
    purge_listener_ = add_session_purge_listener(
        [weak](const std::string& project_dir, const std::string& session_id) {
            if (auto memory = weak.lock()) {
                const auto result = forget_memory_session(*memory, session_id, project_dir);
                if (result.observations_removed || result.entries_updated || result.entries_deleted) {
                    LOG_INFO("[memory] forgot session " + session_id + ": " +
                             std::to_string(result.observations_removed) + " observation(s), " +
                             std::to_string(result.entries_updated) + " entr(y/ies) updated, " +
                             std::to_string(result.entries_deleted) + " deleted");
                }
            }
        });
}

MemoryRuntime::~MemoryRuntime() {
    stop_summary_scheduler();
    remove_session_purge_listener(purge_listener_);
}

void MemoryRuntime::register_tools(ToolExecutor& tools) const {
    tools.register_tool(create_memory_read_tool(service_));
    tools.register_tool(create_memory_write_tool(service_));
}

void MemoryRuntime::update_config(const MemoryConfig& config) {
    service_->update_config(config);
}

MemoryForgetResult MemoryRuntime::forget_session(const std::string& session_id,
                                                 const std::string& project_dir) {
    return forget_memory_session(*service_, session_id, project_dir);
}

void MemoryRuntime::start_summary_scheduler(MemorySchedulerHost host) {
    if (surface_ == MemorySurface::Headless || scheduler_) return;
    scheduler_ = std::make_unique<MemorySummaryScheduler>(service_, std::move(host));
    scheduler_->start();
}

void MemoryRuntime::stop_summary_scheduler() {
    if (scheduler_) scheduler_->stop();
}

std::shared_ptr<MemoryRuntime> create_memory_runtime(const AppConfig& config,
                                                     const std::string& data_dir,
                                                     MemorySurface surface) {
    const fs::path memory_dir = data_dir.empty() ? get_memory_dir()
                                                 : path_from_utf8(data_dir) / "memory";
    MemoryConfig runtime_config = config.memory;
    std::error_code ec;
    fs::create_directories(memory_dir, ec);
    if (ec) {
        LOG_ERROR("[memory] failed to create " + path_to_utf8_generic(memory_dir) + ": " +
                  ec.message() + " - memory is disabled for this process");
        runtime_config.enabled = false;
    }
    auto service = std::make_shared<MemoryService>(memory_dir, memory_dir / "state.sqlite3",
                                                   runtime_config);
    if (runtime_config.enabled) service->global().scan();
    return std::make_shared<MemoryRuntime>(std::move(service), surface);
}

} // namespace acecode
