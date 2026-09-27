#pragma once

#include <filesystem>
#include <stdexcept>

namespace acecode::test_support {

// 测试搬入更深目录后，固定层数的 parent_path 会指错资源，导致静默 SKIP
// 或扫描空集合后通过。只认同时包含 CMakeLists.txt 和 .git 的祖先目录；
// .git 可以是普通仓库的目录，也可以是 worktree 的文件。找不到时明确失败。
inline std::filesystem::path find_repo_root(
    const std::filesystem::path& start = __FILE__) {
    namespace fs = std::filesystem;
    const fs::path source = fs::absolute(start).lexically_normal();
    fs::path current = fs::is_directory(source) ? source : source.parent_path();
    while (!current.empty()) {
        if (fs::is_regular_file(current / "CMakeLists.txt") &&
            fs::exists(current / ".git")) {
            return current;
        }
        const fs::path parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    throw std::runtime_error(
        "Cannot find repository root (CMakeLists.txt and .git) from " +
        source.u8string());
}

} // namespace acecode::test_support
