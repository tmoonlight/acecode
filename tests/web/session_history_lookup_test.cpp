// 覆盖 web/handlers/session_history_handler.hpp 的 find_session_history_file:
// GET /api/sessions/:id/messages 给「还没恢复的会话」找记录文件用的查找顺序。
// 一旦回归:
//   - 顺序错 → 读到别的工作区里同名会话的记录
//   - 兜底提前执行 → 每次点开普通会话都要逐个探测全部无工作区会话目录
//     (实测 372 个目录时每次多花约 70 ms,目录越多越慢)

#include <gtest/gtest.h>

#include "web/handlers/session_history_handler.hpp"

#include "session/session_storage.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using acecode::web::find_session_history_file;

namespace {

class SessionHistoryLookupTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::random_device device;
        std::mt19937_64 generator(device());
        root_ = fs::temp_directory_path() /
                ("acecode_history_lookup_" + std::to_string(generator()));
        fs::create_directories(root_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    // 建一个项目目录;has_session 为 true 时在里面放一份该会话的记录文件。
    std::string project_dir(const std::string& name, bool has_session) {
        const fs::path dir = root_ / name;
        fs::create_directories(dir);
        const std::string utf8 = acecode::path_to_utf8(dir);
        if (has_session) {
            std::ofstream out(acecode::path_from_utf8(
                acecode::SessionStorage::session_path(utf8, kSessionId)), std::ios::binary);
            out << "{}\n";
        }
        return utf8;
    }

    // 路径分隔符的写法不保证一致,按「是不是同一个文件」比较。
    static bool is_session_file_in(const std::string& found, const std::string& dir) {
        if (found.empty()) return false;
        std::error_code ec;
        return fs::equivalent(
            acecode::path_from_utf8(found),
            acecode::path_from_utf8(acecode::SessionStorage::session_path(dir, kSessionId)), ec);
    }

    static constexpr const char* kSessionId = "20261110_090000_abcd";
    fs::path root_;
};

// 场景:侧栏点开一个普通工作区会话,记录文件就在请求指明的工作区目录里。
// 期望:直接返回该文件,兜底(到无工作区会话里找)一次都不调用。
// 回归:兜底先执行,每次打开会话都白白探测一遍全部无工作区会话目录。
TEST_F(SessionHistoryLookupTest, HitInListedDirectorySkipsLastResort) {
    const auto workspace = project_dir("workspace", true);
    const auto daemon = project_dir("daemon", false);
    int last_resort_calls = 0;
    const auto found = find_session_history_file({workspace, daemon}, kSessionId, [&] {
        ++last_resort_calls;
        return std::string{};
    });
    EXPECT_TRUE(is_session_file_in(found, workspace)) << found;
    EXPECT_EQ(last_resort_calls, 0);
}

// 场景:请求指明的工作区和 daemon 自己的目录里都有同名会话文件。
// 期望:取排在前面的那个(请求指明的工作区优先),顺序与改动前一致。
TEST_F(SessionHistoryLookupTest, EarlierDirectoryWins) {
    const auto workspace = project_dir("workspace", true);
    const auto daemon = project_dir("daemon", true);
    const auto found = find_session_history_file({workspace, daemon}, kSessionId, {});
    EXPECT_TRUE(is_session_file_in(found, workspace)) << found;
    EXPECT_FALSE(is_session_file_in(found, daemon)) << found;
}

// 场景:点开的是无工作区会话(侧栏「任务」区),前面的目录里都没有它。
// 期望:这时才调用兜底,且只调用一次,返回兜底目录里的文件。
TEST_F(SessionHistoryLookupTest, MissEverywhereFallsBackToLastResortOnce) {
    const auto workspace = project_dir("workspace", false);
    const auto no_workspace = project_dir("no-workspace", true);
    int last_resort_calls = 0;
    const auto found = find_session_history_file({workspace}, kSessionId, [&] {
        ++last_resort_calls;
        return no_workspace;
    });
    EXPECT_TRUE(is_session_file_in(found, no_workspace)) << found;
    EXPECT_EQ(last_resort_calls, 1);
}

// 场景:哪里都没有这个会话 —— 兜底说没有(空串)、兜底给的目录里也没有、
// 或者根本没提供兜底。期望:都返回空串,路由据此回 404,而不是抛异常或返回半截路径。
TEST_F(SessionHistoryLookupTest, UnknownSessionYieldsEmptyPath) {
    const auto workspace = project_dir("workspace", false);
    const auto empty_fallback = project_dir("empty-fallback", false);
    EXPECT_TRUE(find_session_history_file({workspace}, kSessionId, [] {
        return std::string{};
    }).empty());
    EXPECT_TRUE(find_session_history_file({workspace}, kSessionId, [&] {
        return empty_fallback;
    }).empty());
    EXPECT_TRUE(find_session_history_file({workspace, ""}, kSessionId, {}).empty());
}

} // namespace
