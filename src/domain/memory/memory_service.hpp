#pragma once

#include "config/config.hpp"
#include "memory_registry.hpp"
#include "memory_state_store.hpp"
#include "memory_types.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace acecode {

// 记忆存储的进程级门面(openspec unify-memory-system D1/D2):一个全局作用域
// 存储、按会话项目目录惰性打开的工作区存储、共享状态库与运行时记忆配置。
// 作用域一律由调用方传入的「会话项目目录」解析,从不读进程 cwd —— daemon 一个
// 进程服务多个工作区,进程 cwd 会读错目录。
class MemoryService {
public:
    MemoryService(std::filesystem::path global_dir,
                  std::filesystem::path state_db_path,
                  MemoryConfig config);

    // <data_dir>/memory/ + <data_dir>/memory/state.sqlite3。
    static std::shared_ptr<MemoryService> create_default(const MemoryConfig& config);

    MemoryConfig config() const;
    void update_config(const MemoryConfig& config);
    bool enabled() const;

    MemoryRegistry& global() { return *global_; }
    const std::filesystem::path& global_dir() const { return global_dir_; }

    // project_dir 为空(会话没有所属工作区)时返回 nullptr。同一目录复用同一实例。
    std::shared_ptr<MemoryRegistry> workspace(const std::string& project_dir);

    // 按作用域取存储;Workspace 且没有项目目录时返回 nullptr。
    std::shared_ptr<MemoryRegistry> scope(MemoryScope scope, const std::string& project_dir);

    MemoryStateStore& state() { return *state_; }

    // 用户删除条目(/memory forget、设置页删除):删文件与索引行并记墓碑(作用域、
    // 名字、描述作标题),墓碑期内记忆摘要整合不得重新创建。
    bool delete_entry(MemoryScope scope, const std::string& project_dir,
                      const std::string& name, std::string& error);

    // 清空一个作用域的条目、索引、收件箱与归档(设置页「重置」),并清掉该作用域
    // 的整合进度;另一个作用域不受影响。
    bool reset_scope(MemoryScope scope, const std::string& project_dir, std::string& error);

    // 状态库与墓碑使用的作用域键:"global" / "workspace:<projects 下的目录名>"。
    static std::string workspace_scope_key(const std::string& project_dir);
    static std::string scope_key(MemoryScope scope, const std::string& project_dir);

private:
    std::filesystem::path global_dir_;
    std::unique_ptr<MemoryStateStore> state_;
    std::shared_ptr<MemoryRegistry> global_;
    mutable std::mutex mu_;
    MemoryConfig config_;
    std::map<std::string, std::shared_ptr<MemoryRegistry>> workspaces_;
};

} // namespace acecode
