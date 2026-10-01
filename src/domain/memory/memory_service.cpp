#include "memory_service.hpp"

#include "memory_paths.hpp"

#include "utils/utf8_path.hpp"

namespace fs = std::filesystem;

namespace acecode {

MemoryService::MemoryService(fs::path global_dir, fs::path state_db_path, MemoryConfig config)
    : global_dir_(std::move(global_dir)),
      state_(std::make_unique<MemoryStateStore>(std::move(state_db_path))),
      config_(std::move(config)) {
    global_ = std::make_shared<MemoryRegistry>(global_dir_, "global", state_.get());
}

std::shared_ptr<MemoryService> MemoryService::create_default(const MemoryConfig& config) {
    return std::make_shared<MemoryService>(get_memory_dir(), get_memory_state_db_path(), config);
}

MemoryConfig MemoryService::config() const {
    std::lock_guard<std::mutex> lock(mu_);
    return config_;
}

void MemoryService::update_config(const MemoryConfig& config) {
    std::lock_guard<std::mutex> lock(mu_);
    config_ = config;
}

bool MemoryService::enabled() const {
    std::lock_guard<std::mutex> lock(mu_);
    return config_.enabled;
}

std::shared_ptr<MemoryRegistry> MemoryService::workspace(const std::string& project_dir) {
    if (project_dir.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    auto found = workspaces_.find(project_dir);
    if (found != workspaces_.end()) return found->second;
    auto registry = std::make_shared<MemoryRegistry>(
        workspace_memory_dir(project_dir), workspace_scope_key(project_dir), state_.get());
    workspaces_.emplace(project_dir, registry);
    return registry;
}

std::shared_ptr<MemoryRegistry> MemoryService::scope(MemoryScope scope,
                                                     const std::string& project_dir) {
    if (scope == MemoryScope::Global) return global_;
    return workspace(project_dir);
}

bool MemoryService::delete_entry(MemoryScope scope, const std::string& project_dir,
                                 const std::string& name, std::string& error) {
    auto registry = this->scope(scope, project_dir);
    if (!registry) {
        error = "this session has no workspace memory";
        return false;
    }
    registry->reload();
    const auto existing = registry->find(name);
    if (!registry->remove(name, error)) return false;
    state_->add_tombstone(registry->scope_key(), name,
                          existing ? existing->description : std::string{}, memory_now_ms());
    return true;
}

bool MemoryService::reset_scope(MemoryScope scope, const std::string& project_dir,
                                std::string& error) {
    auto registry = this->scope(scope, project_dir);
    if (!registry) {
        error = "this session has no workspace memory";
        return false;
    }
    if (!registry->reset(error)) return false;
    state_->clear_consolidation(registry->scope_key());
    return true;
}

std::string MemoryService::workspace_scope_key(const std::string& project_dir) {
    // 只取最后一段(projects/<hash> 的 hash);手工切分,不经 std::filesystem::path,
    // 中文路径按系统代码页隐式转换会抛异常(CLAUDE.md「cwd 一律以 UTF-8 传递」)。
    std::string trimmed = project_dir;
    while (!trimmed.empty() && (trimmed.back() == '/' || trimmed.back() == '\\')) trimmed.pop_back();
    const std::size_t cut = trimmed.find_last_of("/\\");
    return "workspace:" + (cut == std::string::npos ? trimmed : trimmed.substr(cut + 1));
}

std::string MemoryService::scope_key(MemoryScope scope, const std::string& project_dir) {
    return scope == MemoryScope::Global ? std::string("global") : workspace_scope_key(project_dir);
}

} // namespace acecode
