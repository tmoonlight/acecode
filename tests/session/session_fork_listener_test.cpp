// 覆盖 session_fork_listeners:SessionManager::fork_session_to_new_id 成功写出新会话后
// 发出「会话分叉」通知(Agent Browser 按会话 cookie 存储据此给子会话复制父会话的
// cookie,openspec add-agent-browser-cookie-modes D7)。Web 分叉路由与线程服务分叉
// 都只走 fork_session_to_new_id,所以在它身上测一次就覆盖了两个入口。

#include <gtest/gtest.h>

#include "session/session_fork_listeners.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using acecode::ChatMessage;
using acecode::SessionManager;
using acecode::SessionStorage;

namespace {

class TempCwd {
public:
    TempCwd() {
        const auto base = fs::temp_directory_path() / "acecode_fork_listener_test";
        fs::create_directories(base);
        std::ostringstream oss;
        oss << "fork_listener_"
            << ::testing::UnitTest::GetInstance()->current_test_info()->name();
        path_ = (base / oss.str()).string();
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempCwd() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

ChatMessage user_message(const std::string& content, const std::string& uuid) {
    ChatMessage message;
    message.role = "user";
    message.content = content;
    message.uuid = uuid;
    return message;
}

struct ForkRecord {
    std::string project_dir;
    std::string new_session_id;
    std::string forked_from;
};

// 注册监听者并在析构时注销:监听表是进程级单例,不能把本用例的回调留给别的用例。
class ScopedForkListener {
public:
    explicit ScopedForkListener(std::vector<ForkRecord>& records)
        : id_(acecode::add_session_fork_listener(
              [&records](const std::string& project_dir,
                         const std::string& new_session_id,
                         const std::string& forked_from) {
                  records.push_back({project_dir, new_session_id, forked_from});
              })) {}
    ~ScopedForkListener() { acecode::remove_session_fork_listener(id_); }

private:
    std::uint64_t id_;
};

} // namespace

// 场景:Web 上把会话 src 从某条消息处分叉出一个新会话。
// 期望:恰好通知一次,带上新会话 id、源会话 id 与新会话所在的项目目录。
TEST(SessionForkListener, SuccessfulForkNotifiesWithNewAndSourceIds) {
    TempCwd cwd;
    SessionManager sm;
    sm.start_session(cwd.path(), "openai", "gpt-4");
    std::vector<ForkRecord> records;
    ScopedForkListener listener(records);

    const auto new_id = sm.fork_session_to_new_id(
        {user_message("hi", "u-1")}, "T", "src-session", "u-1");
    ASSERT_FALSE(new_id.empty());
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].new_session_id, new_id);
    EXPECT_EQ(records[0].forked_from, "src-session");
    EXPECT_EQ(fs::path(records[0].project_dir),
              fs::path(SessionStorage::get_project_dir(cwd.path())));
}

// 场景:会话还没启动就调用分叉(调用方出错),fork_session_to_new_id 返回空。
// 期望:不发通知 —— 否则 Agent Browser 会为一个并不存在的会话记下 cookie 复制记录。
TEST(SessionForkListener, FailedForkDoesNotNotify) {
    SessionManager sm;
    std::vector<ForkRecord> records;
    ScopedForkListener listener(records);

    const auto new_id = sm.fork_session_to_new_id(
        {user_message("hi", "u-1")}, "T", "src-session", "u-1");
    EXPECT_TRUE(new_id.empty());
    EXPECT_TRUE(records.empty());
}

// 场景:没有来源会话 id 的复制(forked_from 为空)。
// 期望:不发通知 —— 没有父会话就没有可复制的 cookie。
TEST(SessionForkListener, ForkWithoutSourceIdDoesNotNotify) {
    TempCwd cwd;
    SessionManager sm;
    sm.start_session(cwd.path(), "openai", "gpt-4");
    std::vector<ForkRecord> records;
    ScopedForkListener listener(records);

    const auto new_id = sm.fork_session_to_new_id(
        {user_message("hi", "u-1")}, "T", "", "u-1");
    ASSERT_FALSE(new_id.empty());
    EXPECT_TRUE(records.empty());
}

// 场景:监听者抛异常(实现缺陷)。
// 期望:分叉本身照常成功,后续注册的监听者仍收到通知。
TEST(SessionForkListener, ThrowingListenerDoesNotBreakForkOrOtherListeners) {
    TempCwd cwd;
    SessionManager sm;
    sm.start_session(cwd.path(), "openai", "gpt-4");
    const auto throwing = acecode::add_session_fork_listener(
        [](const std::string&, const std::string&, const std::string&) {
            throw std::runtime_error("boom");
        });
    std::vector<ForkRecord> records;
    ScopedForkListener listener(records);

    const auto new_id = sm.fork_session_to_new_id(
        {user_message("hi", "u-1")}, "T", "src-session", "u-1");
    acecode::remove_session_fork_listener(throwing);
    ASSERT_FALSE(new_id.empty());
    EXPECT_EQ(records.size(), 1u);
}
