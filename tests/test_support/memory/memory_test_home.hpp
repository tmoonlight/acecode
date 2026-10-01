#pragma once

// 记忆相关测试的公共夹具:把 HOME / USERPROFILE 指到一个独立的临时目录,数据目录
// (<home>/.acecode)、全局记忆目录、projects/<hash> 工作区目录都落在里面,析构时
// 恢复环境变量并整个删除,不碰开发者真实的 ~/.acecode。

#include "memory/memory_paths.hpp"
#include "memory/memory_service.hpp"
#include "session/session_storage.hpp"
#include "utils/encoding.hpp"
#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX  // FTXUI 与 std::min / std::max 都不允许 windows.h 的宏
#  endif
#  include <windows.h>
#endif

namespace acecode_test {

class MemoryTestHome {
public:
    explicit MemoryTestHome(const std::string& tag) {
        const char* existing = std::getenv(home_env());
        previous_ = existing ? existing : "";
        root_ = std::filesystem::temp_directory_path() /
                ("acecode-" + tag + "-" + std::to_string(std::random_device{}()));
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_);
        set_home(acecode::path_to_utf8(root_));
        std::filesystem::create_directories(acecode::get_memory_dir(), ec);
    }

    ~MemoryTestHome() {
        set_home(previous_);
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    MemoryTestHome(const MemoryTestHome&) = delete;
    MemoryTestHome& operator=(const MemoryTestHome&) = delete;

    const std::filesystem::path& root() const { return root_; }

    // 新的记忆服务实例(相当于「一个 ACECode 进程」):全局目录与状态库都在临时 HOME 下。
    std::shared_ptr<acecode::MemoryService> service(acecode::MemoryConfig config = {}) const {
        return std::make_shared<acecode::MemoryService>(
            acecode::get_memory_dir(), acecode::get_memory_state_db_path(), config);
    }

    // 建一个工作区目录(会话 cwd),返回它的 UTF-8 路径。
    std::string workspace_cwd(const std::string& name) const {
        const auto dir = root_ / "workspaces" / name;
        std::filesystem::create_directories(dir);
        return acecode::path_to_utf8(dir);
    }

    // 与会话存储同源的项目目录 <data_dir>/projects/<hash>。
    static std::string project_dir(const std::string& cwd) {
        return acecode::SessionStorage::get_project_dir(cwd);
    }

private:
    static const char* home_env() {
#ifdef _WIN32
        return "USERPROFILE";
#else
        return "HOME";
#endif
    }

    static void set_home(const std::string& value) {
#ifdef _WIN32
        _putenv_s(home_env(), value.c_str());
        SetEnvironmentVariableW(acecode::utf8_to_wide(home_env()).c_str(),
                                acecode::utf8_to_wide(value).c_str());
#else
        setenv(home_env(), value.c_str(), 1);
#endif
    }

    std::filesystem::path root_;
    std::string previous_;
};

} // namespace acecode_test
