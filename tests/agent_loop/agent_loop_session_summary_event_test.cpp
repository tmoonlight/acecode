#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "permissions/permissions.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "tool/tool_executor.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace fs = std::filesystem;

// 会话显示标题的实时同步:用户消息落盘后 AgentLoop 必须发 session_updated{summary},
// 侧栏与顶部标题栏从同一个事件刷新。回归背景(bug 表现):前端曾各自从消息正文推标题
// —— 顶部标题栏拿最后一条 user 消息全文(9k 字符的 @session 展开文本也照收),侧栏拿
// 服务端 80 字节的 summary,两处经常不一致;有了这个事件,前端不再需要看正文。

namespace {

fs::path summary_temp_cwd(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_summary_event_" + hint + "_" +
         std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::remove_all(acecode::SessionStorage::get_project_dir(dir.string()));
    return dir;
}

class SummaryEventHarness {
public:
    explicit SummaryEventHarness(const std::string& hint)
        : cwd_(summary_temp_cwd(hint)), session_id_("sid-" + hint) {
        sm_->start_session(cwd_.string(), "stub", "stub-1", session_id_);

        acecode::AgentCallbacks callbacks;
        auto accessor = [this]() -> std::shared_ptr<acecode::LlmProvider> {
            return provider_;
        };
        loop_ = std::make_unique<acecode::AgentLoop>(
            accessor, tools_, callbacks, cwd_.string(), permissions_);
        loop_->set_session_manager(sm_.get());
        sub_ = loop_->events().subscribe([this](const acecode::SessionEvent& event) {
            std::lock_guard<std::mutex> lk(mu_);
            events_.push_back(event);
            if (event.kind == acecode::SessionEventKind::Done) {
                ++done_count_;
                cv_.notify_all();
            }
        });
    }

    ~SummaryEventHarness() {
        if (loop_ && sub_ != 0) loop_->events().unsubscribe(sub_);
        loop_.reset();
        sm_.reset();  // 先关 SQLite 句柄,Windows 上才能删目录。
        fs::remove_all(cwd_);
        fs::remove_all(acecode::SessionStorage::get_project_dir(cwd_.string()));
    }

    acecode::AgentLoop& loop() { return *loop_; }
    acecode_test::StubLlmProvider& provider() { return *provider_; }
    acecode::SessionManager& session_manager() { return *sm_; }
    const std::string& session_id() const { return session_id_; }
    std::string project_dir() const {
        return acecode::SessionStorage::get_project_dir(cwd_.string());
    }

    bool wait_for_done(int count, std::chrono::milliseconds timeout = 10s) {
        std::unique_lock<std::mutex> lk(mu_);
        const bool ok = cv_.wait_for(lk, timeout, [&] { return done_count_ >= count; });
        lk.unlock();
        // Done 之后 busy 还会翻回 false;等它落定再断言,避免析构时踩到工作线程。
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (loop_->is_busy() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(2ms);
        }
        return ok;
    }

    std::vector<acecode::SessionEvent> events() const {
        std::lock_guard<std::mutex> lk(mu_);
        return events_;
    }

    // 按发生顺序收集所有 session_updated{summary} 的 summary 值。
    std::vector<std::string> summaries() const {
        std::vector<std::string> out;
        for (const auto& event : events()) {
            if (event.kind != acecode::SessionEventKind::SessionUpdated) continue;
            if (!event.payload.is_object() || !event.payload.contains("summary")) continue;
            out.push_back(event.payload.value("summary", std::string{}));
        }
        return out;
    }

private:
    fs::path cwd_;
    std::string session_id_;
    std::shared_ptr<acecode_test::StubLlmProvider> provider_ =
        std::make_shared<acecode_test::StubLlmProvider>();
    acecode::ToolExecutor tools_;
    acecode::PermissionManager permissions_;
    std::unique_ptr<acecode::SessionManager> sm_ =
        std::make_unique<acecode::SessionManager>();
    std::unique_ptr<acecode::AgentLoop> loop_;
    std::uint64_t sub_ = 0;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    int done_count_ = 0;
    std::vector<acecode::SessionEvent> events_;
};

const char* kDisplayText = "@规划 AgentLoop 文件拆分与目录结构 继续";

std::string referenced_session_prompt() {
    return "Referenced ACECode session context follows. Treat it as prior conversation "
           "context for the current request.\n\n[Referenced session]\n" +
           std::string(9000, 'x') + "\n[End referenced session]\n\nCurrent user request:\n" +
           kDisplayText;
}

} // namespace

// 触发场景:提交带 display_text 的用户输入(@session 引用展开)。
// 期望行为:回合结束前收到一条 session_updated{summary},值是显示文本(不是展开后的
// 长文),且与 SessionManager 内存摘要、落盘 meta.summary 三者一致。
TEST(AgentLoopSessionSummaryEvent, UserInputPublishesDisplayTextSummaryBeforeDone) {
    SummaryEventHarness h("display-text");
    h.provider().push_text("ok");

    acecode::UserInput input;
    input.text = referenced_session_prompt();
    input.display_text = kDisplayText;
    h.loop().submit(input);
    ASSERT_TRUE(h.wait_for_done(1));

    const auto summaries = h.summaries();
    ASSERT_EQ(summaries.size(), 1u);
    EXPECT_EQ(summaries[0], kDisplayText);
    EXPECT_EQ(summaries[0].find("Referenced ACECode"), std::string::npos);
    EXPECT_EQ(h.session_manager().current_summary(), kDisplayText);

    const auto events = h.events();
    std::size_t summary_index = events.size();
    std::size_t done_index = events.size();
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].kind == acecode::SessionEventKind::SessionUpdated &&
            summary_index == events.size()) {
            summary_index = i;
        }
        if (events[i].kind == acecode::SessionEventKind::Done) done_index = i;
    }
    EXPECT_LT(summary_index, done_index);

    const auto meta = acecode::SessionStorage::read_meta(
        acecode::SessionStorage::meta_path(h.project_dir(), h.session_id()));
    EXPECT_EQ(meta.summary, kDisplayText);
}

// 触发场景:第二回合换了话题。
// 期望行为:再发一条 summary 事件,值是新一轮的用户文本 —— 无标题会话的显示名跟着
// 最近一条用户消息走(与侧栏列表的既有语义一致)。
TEST(AgentLoopSessionSummaryEvent, EachVisibleUserTurnRefreshesSummary) {
    SummaryEventHarness h("second-turn");
    h.provider().push_text("first");
    h.provider().push_text("second");

    h.loop().submit("第一轮 问题");
    ASSERT_TRUE(h.wait_for_done(1));
    h.loop().submit("第二轮 换个话题");
    ASSERT_TRUE(h.wait_for_done(2));

    const auto summaries = h.summaries();
    ASSERT_EQ(summaries.size(), 2u);
    EXPECT_EQ(summaries[0], "第一轮 问题");
    EXPECT_EQ(summaries[1], "第二轮 换个话题");
    EXPECT_EQ(h.session_manager().current_summary(), "第二轮 换个话题");
}

// 触发场景:很长的普通输入(没有 display_text)。
// 期望行为:事件里的 summary 就是截断后的值(<= 80 字节 + "..."),前端拿到即可显示,
// 不需要也不允许自己再截。
TEST(AgentLoopSessionSummaryEvent, SummaryInEventIsAlreadyBounded) {
    SummaryEventHarness h("bounded");
    h.provider().push_text("ok");

    std::string long_input;
    for (int i = 0; i < 100; ++i) long_input += "很长的输入 ";
    h.loop().submit(long_input);
    ASSERT_TRUE(h.wait_for_done(1));

    const auto summaries = h.summaries();
    ASSERT_EQ(summaries.size(), 1u);
    EXPECT_LE(summaries[0].size(), 80u + 3u);
    EXPECT_EQ(summaries[0].substr(summaries[0].size() - 3), "...");
}
