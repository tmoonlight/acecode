// 具体进度提示(openspec add-tool-preamble,设置 > 常规 > 工作模式 > 适合日常工作)
// 在 AgentLoop 里的端到端行为,用 StubLlmProvider 喂流式事件,真实 SessionManager
// 落盘到临时目录。开启时 loading 只说正在做什么、不带参数:
//   - 等待 / 推理:回合开头「正在分析你的请求」,一批工具跑完后按这批工具的类型
//     给场景文案;推理摘要带 **加粗** 标题时本步用标题;永远不出「正在推理」
//     「正在等待模型响应」,detail(片段计数、命令预览、字节数)一律为空;
//   - 准备调用 / 执行:工具现在进行时模板(tool_start.preamble 同源),工具行
//     (TUI 进度头、tool_start 自己的参数)照常带参数;
//   - 正文开始流出:「正在撰写回复」;
//   - TUI 经 on_thinking_title 收到同样的文案。
// 历史里残留的 <text_preamble> 标签只剥掉、不再当文案;关闭时一切保持旧文案。
// 回归背景:先说一句话 / 必填 preamble 参数 / <text_preamble> 标签三版都靠模型配合,
// grok-4.7 不照做(用户会话 20260925-053811-55cc 12 个工具步零标签),用户要求
// loading 不再依赖模型输出,也不要「正在推理」这种笼统文案。

#include <gtest/gtest.h>

#include "agent/agent_loop.hpp"
#include "permissions/permissions.hpp"
#include "llm/llm_provider.hpp"
#include "session/event_dispatcher.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "tool/tool_executor.hpp"
#include "tool_preamble/tool_preamble.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using acecode::AgentCallbacks;
using acecode::AgentLoop;
using acecode::ChatMessage;
using acecode::PermissionManager;
using acecode::PermissionMode;
using acecode::SessionEvent;
using acecode::SessionEventKind;
using acecode::StreamEvent;
using acecode::StreamEventType;
using acecode::ToolCall;
using acecode::ToolContext;
using acecode::ToolDef;
using acecode::ToolExecutor;
using acecode::ToolImpl;
using acecode::ToolPreambleConfig;
using acecode::ToolResult;
using acecode::ToolSource;
using acecode_test::ScriptedResponse;
using acecode_test::StubLlmProvider;

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

struct ReceivedArgs {
    std::mutex mu;
    std::vector<std::string> args;
};

ToolImpl make_probe_tool(int delay_ms, std::shared_ptr<ReceivedArgs> received) {
    ToolDef def;
    def.name = "probe_read";
    def.description = "Tool preamble probe";
    def.parameters = {
        {"type", "object"},
        {"properties", nlohmann::json::object({
            {"file_path", {{"type", "string"}}},
        })},
    };
    ToolImpl impl;
    impl.definition = def;
    impl.is_read_only = true;
    impl.source = ToolSource::Builtin;
    impl.execute = [delay_ms, received](const std::string& args, const ToolContext&) -> ToolResult {
        if (received) {
            std::lock_guard<std::mutex> lk(received->mu);
            received->args.push_back(args);
        }
        if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        return ToolResult{"probe ok", true};
    };
    return impl;
}

// 非只读探针:走顺序执行的写工具路径(有 TUI 进度头)。
ToolImpl make_write_probe_tool(std::shared_ptr<ReceivedArgs> received) {
    ToolImpl impl = make_probe_tool(0, std::move(received));
    impl.definition.name = "probe_write";
    impl.definition.description = "Tool preamble write probe";
    impl.is_read_only = false;
    return impl;
}

fs::path make_temp_dir(const std::string& name) {
    static std::atomic<unsigned> seq{0};
    auto p = fs::temp_directory_path() /
        (name + "_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
         "_" + std::to_string(seq.fetch_add(1)));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

StreamEvent reasoning_event(std::string text) {
    StreamEvent evt;
    evt.type = StreamEventType::ReasoningDelta;
    evt.content = std::move(text);
    return evt;
}

StreamEvent delta_event(std::string text) {
    StreamEvent evt;
    evt.type = StreamEventType::Delta;
    evt.content = std::move(text);
    return evt;
}

StreamEvent call_event(const std::string& name, const std::string& args,
                       const std::string& id) {
    StreamEvent evt;
    evt.type = StreamEventType::ToolCall;
    evt.tool_call = ToolCall{id, name, args};
    return evt;
}

StreamEvent probe_call_event(const std::string& id = "call-1") {
    return call_event("probe_read", R"({"file_path":"registry.hpp"})", id);
}

StreamEvent done_event() {
    StreamEvent evt;
    evt.type = StreamEventType::Done;
    return evt;
}

class ToolPreambleHarness {
public:
    explicit ToolPreambleHarness(std::string cwd, int tool_delay_ms = 0)
        : cwd_(std::move(cwd)), received_(std::make_shared<ReceivedArgs>()) {
        tools_.register_tool(make_probe_tool(tool_delay_ms, received_));
        tools_.register_tool(make_write_probe_tool(received_));
        AgentCallbacks cb;
        cb.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lk(busy_mu_);
            busy_ = busy;
            if (!busy) busy_cv_.notify_all();
        };
        cb.on_thinking_title = [this](const std::string& title) {
            std::lock_guard<std::mutex> lk(events_mu_);
            thinking_titles_.push_back(title);
        };
        cb.on_delta = [this](const std::string& token) {
            std::lock_guard<std::mutex> lk(events_mu_);
            tui_deltas_ += token;
        };
        cb.on_message = [this](const std::string& role, const std::string& content, bool is_tool) {
            if (is_tool || role != "assistant") return;
            std::lock_guard<std::mutex> lk(events_mu_);
            tui_assistant_messages_.push_back(content);
        };
        cb.on_tool_progress_start = [this](const std::string&, const std::string&,
                                           const std::string& preamble) {
            std::lock_guard<std::mutex> lk(events_mu_);
            progress_preambles_.push_back(preamble);
        };
        auto accessor = [this]() -> std::shared_ptr<acecode::LlmProvider> { return provider_; };
        perms_.set_mode(PermissionMode::Yolo);
        loop_ = std::make_unique<AgentLoop>(accessor, tools_, cb, cwd_, perms_);
        sub_ = loop_->events().subscribe([this](const SessionEvent& e) {
            std::lock_guard<std::mutex> lk(events_mu_);
            events_.push_back(e);
        });
        session_manager_.start_session(cwd_, "stub", "stub-model");
        loop_->set_session_manager(&session_manager_);
    }

    ~ToolPreambleHarness() {
        if (loop_ && sub_ != 0) loop_->events().unsubscribe(sub_);
        if (loop_) loop_->shutdown();
        loop_.reset();
    }

    StubLlmProvider& provider() { return *provider_; }
    AgentLoop& loop() { return *loop_; }

    void configure(bool enabled) {
        ToolPreambleConfig cfg;
        cfg.enabled = enabled;
        loop_->set_tool_preamble_config(cfg);
    }

    bool submit_and_wait(std::chrono::milliseconds timeout = 10s) {
        {
            std::lock_guard<std::mutex> lk(busy_mu_);
            busy_ = true;
        }
        loop_->submit("go");
        std::unique_lock<std::mutex> lk(busy_mu_);
        return busy_cv_.wait_for(lk, timeout, [this] { return !busy_; });
    }

    std::vector<SessionEvent> events_of(SessionEventKind kind) const {
        std::lock_guard<std::mutex> lk(events_mu_);
        std::vector<SessionEvent> out;
        for (const auto& e : events_) {
            if (e.kind == kind) out.push_back(e);
        }
        return out;
    }

    std::vector<std::string> progress_labels(const std::string& phase) const {
        std::vector<std::string> labels;
        for (const auto& e : events_of(SessionEventKind::AgentProgress)) {
            if (e.payload.value("phase", std::string{}) == phase) {
                labels.push_back(e.payload.value("label", std::string{}));
            }
        }
        return labels;
    }

    std::vector<std::string> progress_details(const std::string& phase) const {
        std::vector<std::string> details;
        for (const auto& e : events_of(SessionEventKind::AgentProgress)) {
            if (e.payload.value("phase", std::string{}) == phase) {
                details.push_back(e.payload.value("detail", std::string{}));
            }
        }
        return details;
    }

    // Web 看到的 token 流(daemon 剥过标签)。
    std::string streamed_tokens() const {
        std::string out;
        for (const auto& e : events_of(SessionEventKind::Token)) {
            out += e.payload.value("text", std::string{});
        }
        return out;
    }

    // Web 看到的 assistant Message 帧正文(按顺序)。
    std::vector<std::string> assistant_message_frames() const {
        std::vector<std::string> out;
        for (const auto& e : events_of(SessionEventKind::Message)) {
            if (e.payload.value("role", std::string{}) == "assistant") {
                out.push_back(e.payload.value("content", std::string{}));
            }
        }
        return out;
    }

    std::vector<nlohmann::json> tool_starts() const {
        std::vector<nlohmann::json> out;
        for (const auto& e : events_of(SessionEventKind::ToolStart)) out.push_back(e.payload);
        return out;
    }

    std::vector<std::string> thinking_titles() const {
        std::lock_guard<std::mutex> lk(events_mu_);
        return thinking_titles_;
    }
    std::vector<std::string> progress_preambles() const {
        std::lock_guard<std::mutex> lk(events_mu_);
        return progress_preambles_;
    }
    std::string tui_deltas() const {
        std::lock_guard<std::mutex> lk(events_mu_);
        return tui_deltas_;
    }
    std::vector<std::string> tui_assistant_messages() const {
        std::lock_guard<std::mutex> lk(events_mu_);
        return tui_assistant_messages_;
    }
    std::vector<std::string> received_args() const {
        std::lock_guard<std::mutex> lk(received_->mu);
        return received_->args;
    }

    // 落盘的 assistant(tool_calls) 消息(按顺序)。
    std::vector<ChatMessage> persisted_tool_call_messages() const {
        std::vector<ChatMessage> out;
        for (const auto& m : session_manager_.load_active_messages()) {
            if (m.role == "assistant" && m.tool_calls.is_array() && !m.tool_calls.empty()) {
                out.push_back(m);
            }
        }
        return out;
    }

    std::vector<ChatMessage> persisted_assistant_text_messages() const {
        std::vector<ChatMessage> out;
        for (const auto& m : session_manager_.load_active_messages()) {
            if (m.role == "assistant" && (!m.tool_calls.is_array() || m.tool_calls.empty())) {
                out.push_back(m);
            }
        }
        return out;
    }

    std::string system_prompt_of_turn(int turn) const {
        for (const auto& m : provider_->messages_for_turn(turn)) {
            if (m.role == "system") return m.content;
        }
        return {};
    }

    std::optional<ToolDef> tool_def_of_turn(int turn, const std::string& name) const {
        for (const auto& def : provider_->tools_for_turn(turn)) {
            if (def.name == name) return def;
        }
        return std::nullopt;
    }

private:
    std::string cwd_;
    std::shared_ptr<ReceivedArgs> received_;
    std::shared_ptr<StubLlmProvider> provider_ = std::make_shared<StubLlmProvider>();
    ToolExecutor tools_;
    PermissionManager perms_;
    acecode::SessionManager session_manager_;
    std::unique_ptr<AgentLoop> loop_;
    acecode::EventDispatcher::SubscriptionId sub_ = 0;

    mutable std::mutex events_mu_;
    std::vector<SessionEvent> events_;
    std::vector<std::string> thinking_titles_;
    std::vector<std::string> progress_preambles_;
    std::string tui_deltas_;
    std::vector<std::string> tui_assistant_messages_;

    std::mutex busy_mu_;
    std::condition_variable busy_cv_;
    bool busy_ = false;
};

struct ProjectDirCleanup {
    fs::path project_dir;
    ~ProjectDirCleanup() {
        std::error_code ec;
        fs::remove_all(project_dir, ec);
    }
};

nlohmann::json preamble_metadata_of(const ChatMessage& message) {
    if (!message.metadata.is_object() || !message.metadata.contains("tool_preamble")) {
        return nullptr;
    }
    return message.metadata["tool_preamble"];
}

constexpr const char* kReadTag =
    "<text_preamble type=\"read\">Reading the loader</text_preamble>\n\n";

StreamEvent tool_call_delta_event(const std::string& name, const std::string& id,
                                  int index, std::size_t bytes) {
    StreamEvent evt;
    evt.type = StreamEventType::ToolCallDelta;
    evt.tool_call = ToolCall{id, name, ""};
    evt.tool_index = index;
    evt.tool_call_argument_bytes = bytes;
    return evt;
}

// 所有 agent_progress 帧的 label,用来断言笼统文案一条都没出现。
std::vector<std::string> all_progress_labels(const ToolPreambleHarness& h) {
    std::vector<std::string> out;
    for (const auto& e : h.events_of(SessionEventKind::AgentProgress)) {
        out.push_back(e.payload.value("label", std::string{}));
    }
    return out;
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

// 探针工具的原生名是 probe_read / probe_write,不在模板表里,所以文案走「调用工具」
// 兜底;具体动词(读取 / 搜索 / 运行…)由 tool_preamble_test 覆盖。
const std::string kProbeBatch = acecode::tool_preamble::batch_activity_label({"probe_read"});
const std::string kProbeAfter = acecode::tool_preamble::after_batch_activity_label({"probe_read"});
const std::string kInitial = acecode::tool_preamble::kInitialActivityLabel;
const std::string kResponding = acecode::tool_preamble::kRespondingActivityLabel;

} // namespace

// 场景:开启后,模型第一步只有没加粗的推理(grok 那种英文原始思维链)+ 一个工具调用,
// 第二步纯文本收尾。
// 期望:
//   - 两次 model_waiting 分别是「正在分析你的请求」与这批工具跑完后的场景文案,
//     推理帧沿用同一句,detail 全空;任何帧都没有「正在推理」「正在等待模型响应」;
//   - 推理首句不再兜底成标题("The user wants me to…" 不出现在任何地方);
//   - tool_start.preamble 是工具模板,source=template,tool_running 同句且 detail 为空;
//   - 正文开始流出时有一条「正在撰写回复」;
//   - TUI on_thinking_title 依次收到上述文案(同一句不重复);
//   - 落盘 metadata.tool_preamble = {source:template, title, kind}。
TEST(AgentLoopToolPreamble, ConcreteLabelsReplaceVagueWaitingText) {
    const auto cwd = make_temp_dir("acecode_tool_preamble_concrete");
    ProjectDirCleanup cleanup{acecode::SessionStorage::get_project_dir(cwd.string())};
    ToolPreambleHarness h(cwd.string());
    h.configure(true);
    h.provider().push_events({
        reasoning_event("The user wants me to look at the loader."),
        probe_call_event(), done_event(),
    });
    h.provider().push_text("done");
    ASSERT_TRUE(h.submit_and_wait());

    EXPECT_EQ(h.progress_labels("model_waiting"),
              (std::vector<std::string>{kInitial, kProbeAfter}));
    for (const auto& detail : h.progress_details("model_waiting")) EXPECT_EQ(detail, "");
    for (const auto& label : h.progress_labels("reasoning")) EXPECT_EQ(label, kInitial);
    for (const auto& detail : h.progress_details("reasoning")) EXPECT_EQ(detail, "");
    const auto labels = all_progress_labels(h);
    EXPECT_FALSE(contains(labels, u8"正在推理"));
    EXPECT_FALSE(contains(labels, u8"正在等待模型响应"));
    for (const auto& label : labels) {
        EXPECT_EQ(label.find("The user wants"), std::string::npos) << label;
    }

    const auto starts = h.tool_starts();
    ASSERT_EQ(starts.size(), 1u);
    EXPECT_EQ(starts[0].value("preamble", std::string{}), kProbeBatch);
    EXPECT_EQ(starts[0].value("preamble_source", std::string{}), "template");
    EXPECT_EQ(h.progress_labels("tool_running"), (std::vector<std::string>{kProbeBatch}));
    EXPECT_EQ(h.progress_details("tool_running"), (std::vector<std::string>{""}));
    EXPECT_EQ(h.progress_labels("responding"), (std::vector<std::string>{kResponding}));

    const auto titles = h.thinking_titles();
    ASSERT_GE(titles.size(), 4u);
    EXPECT_EQ(titles[0], kInitial);
    EXPECT_EQ(titles[1], kProbeBatch);
    EXPECT_EQ(titles[2], kProbeAfter);
    EXPECT_EQ(titles.back(), kResponding);

    const auto persisted = h.persisted_tool_call_messages();
    ASSERT_EQ(persisted.size(), 1u);
    const auto meta = preamble_metadata_of(persisted[0]);
    EXPECT_EQ(meta.value("source", std::string{}), "template");
    EXPECT_EQ(meta.value("title", std::string{}), kProbeBatch);
}

// 场景:推理摘要第一步带 **加粗** 标题(OpenAI / Codex 那种),第二步没有。
// 期望:标题一出现就发 phase=preamble 的帧并用作本步 loading 与 tool_start.preamble
// (source=reasoning);标题只对本步有效 —— 第二步等待时回到场景文案,不沿用旧标题。
TEST(AgentLoopToolPreamble, ReasoningBoldTitleWinsOnlyForItsStep) {
    const auto cwd = make_temp_dir("acecode_tool_preamble_bold");
    ProjectDirCleanup cleanup{acecode::SessionStorage::get_project_dir(cwd.string())};
    ToolPreambleHarness h(cwd.string());
    h.configure(true);
    h.provider().push_events({
        reasoning_event("**Reading registry sections**\n\nI'm looking at the loader."),
        probe_call_event(), done_event(),
    });
    h.provider().push_events({reasoning_event("no title here"), delta_event("done"), done_event()});
    ASSERT_TRUE(h.submit_and_wait());

    EXPECT_EQ(h.progress_labels("preamble"),
              (std::vector<std::string>{"Reading registry sections"}));
    const auto starts = h.tool_starts();
    ASSERT_EQ(starts.size(), 1u);
    EXPECT_EQ(starts[0].value("preamble", std::string{}), "Reading registry sections");
    EXPECT_EQ(starts[0].value("preamble_source", std::string{}), "reasoning");
    EXPECT_EQ(h.progress_labels("tool_running"),
              (std::vector<std::string>{"Reading registry sections"}));
    EXPECT_EQ(h.progress_labels("model_waiting"),
              (std::vector<std::string>{kInitial, kProbeAfter}));
    const auto reasoning = h.progress_labels("reasoning");
    ASSERT_FALSE(reasoning.empty());
    EXPECT_EQ(reasoning.back(), kProbeAfter);
}

// 场景:工具调用参数正在流(ToolCallDelta),两个并行调用先后露头。
// 期望:tool_planning 的 label 随已露头的工具更新成模板(一个 → 两个),detail 为空
// (不再显示「参数 N 字节」);text 形式工具调用被扣住时(tool_index=-1)用场景文案。
TEST(AgentLoopToolPreamble, ToolPlanningShowsTemplateWithoutArguments) {
    const auto cwd = make_temp_dir("acecode_tool_preamble_planning");
    ProjectDirCleanup cleanup{acecode::SessionStorage::get_project_dir(cwd.string())};
    ToolPreambleHarness h(cwd.string());
    h.configure(true);
    StreamEvent held;
    held.type = StreamEventType::ToolCallDelta;
    held.text_tool_call_hold = true;
    held.tool_call_argument_bytes = 12;
    h.provider().push_events({
        held,
        tool_call_delta_event("probe_read", "call-a", 0, 10),
        tool_call_delta_event("probe_read", "call-b", 1, 10),
        probe_call_event("call-a"), probe_call_event("call-b"), done_event(),
    });
    h.provider().push_text("done");
    ASSERT_TRUE(h.submit_and_wait());

    const auto planning = h.progress_labels("tool_planning");
    ASSERT_EQ(planning.size(), 3u);
    EXPECT_EQ(planning[0], kInitial);
    EXPECT_EQ(planning[1], kProbeBatch);
    EXPECT_EQ(planning[2],
              acecode::tool_preamble::batch_activity_label({"probe_read", "probe_read"}));
    for (const auto& detail : h.progress_details("tool_planning")) EXPECT_EQ(detail, "");
}

// 场景:写工具(顺序执行路径,TUI 有进度头)。期望:TUI 进度头是工具行,前言参数
// 恒为空(参数照常显示在工具行上);loading(tool_running 与 on_thinking_title)是
// 模板文案,detail 为空;工具仍收到原始参数。
TEST(AgentLoopToolPreamble, WriteToolProgressHeaderKeepsToolRow) {
    const auto cwd = make_temp_dir("acecode_tool_preamble_write");
    ProjectDirCleanup cleanup{acecode::SessionStorage::get_project_dir(cwd.string())};
    ToolPreambleHarness h(cwd.string());
    h.configure(true);
    const std::string args = R"({"file_path":"loader.py"})";
    h.provider().push_events({call_event("probe_write", args, "call-w"), done_event()});
    h.provider().push_text("done");
    ASSERT_TRUE(h.submit_and_wait());

    EXPECT_EQ(h.progress_preambles(), (std::vector<std::string>{""}));
    const std::string batch = acecode::tool_preamble::batch_activity_label({"probe_write"});
    EXPECT_EQ(h.progress_labels("tool_running"), (std::vector<std::string>{batch}));
    EXPECT_EQ(h.progress_details("tool_running"), (std::vector<std::string>{""}));
    EXPECT_TRUE(contains(h.thinking_titles(), batch));
    EXPECT_EQ(h.received_args(), (std::vector<std::string>{args}));
}

// 场景:历史里模型学会了打 <text_preamble> 标签(前一版要求的),这一步仍然写了一个。
// 期望:标签从 token 流 / TUI 增量里剥掉,但不再当 loading 文案 —— 没有 phase=preamble
// 的帧,tool_start.preamble 是工具模板而不是标签正文;落盘正文保留原文;可见正文为空
// 的工具步不发 assistant Message 帧。
TEST(AgentLoopToolPreamble, HistoricalTagsAreStrippedButNotPublished) {
    const auto cwd = make_temp_dir("acecode_tool_preamble_tag");
    ProjectDirCleanup cleanup{acecode::SessionStorage::get_project_dir(cwd.string())};
    ToolPreambleHarness h(cwd.string());
    h.configure(true);
    h.provider().push_events({delta_event(kReadTag), probe_call_event(), done_event()});
    h.provider().push_text("done");
    ASSERT_TRUE(h.submit_and_wait());

    EXPECT_EQ(h.streamed_tokens(), "done");
    EXPECT_EQ(h.tui_deltas(), "done");
    EXPECT_TRUE(h.progress_labels("preamble").empty());
    const auto starts = h.tool_starts();
    ASSERT_EQ(starts.size(), 1u);
    EXPECT_EQ(starts[0].value("preamble", std::string{}), kProbeBatch);
    EXPECT_FALSE(contains(all_progress_labels(h), "Reading the loader"));
    EXPECT_EQ(h.assistant_message_frames(), (std::vector<std::string>{"done"}));
    const auto persisted = h.persisted_tool_call_messages();
    ASSERT_EQ(persisted.size(), 1u);
    EXPECT_EQ(persisted[0].content, kReadTag);
}

// 场景:开关关闭(工作模式「用于编程」),推理带加粗标题、正文带标签。
// 期望:一切保持旧文案 ——「正在等待模型响应」「正在推理」「正在调用工具 probe_read」;
// 没有 phase=preamble / responding 的帧,进度帧与 tool_start 都不带 preamble 字段,
// TUI 不收到 on_thinking_title;标签照样不露到界面上。
TEST(AgentLoopToolPreamble, DisabledKeepsLegacyTextsAndHidesTags) {
    const auto cwd = make_temp_dir("acecode_tool_preamble_off");
    ProjectDirCleanup cleanup{acecode::SessionStorage::get_project_dir(cwd.string())};
    ToolPreambleHarness h(cwd.string());
    h.configure(false);
    h.provider().push_events({
        reasoning_event("**Reading registry sections**"),
        delta_event(kReadTag), probe_call_event(), done_event(),
    });
    h.provider().push_text("done");
    ASSERT_TRUE(h.submit_and_wait());

    EXPECT_EQ(h.progress_labels("model_waiting"),
              (std::vector<std::string>{u8"正在等待模型响应", u8"正在等待模型响应"}));
    for (const auto& label : h.progress_labels("reasoning")) EXPECT_EQ(label, u8"正在推理");
    EXPECT_EQ(h.progress_labels("tool_running"),
              (std::vector<std::string>{u8"正在调用工具 probe_read"}));
    EXPECT_TRUE(h.progress_labels("preamble").empty());
    EXPECT_TRUE(h.progress_labels("responding").empty());
    for (const auto& e : h.events_of(SessionEventKind::AgentProgress)) {
        EXPECT_FALSE(e.payload.contains("preamble")) << e.payload.dump();
    }
    const auto starts = h.tool_starts();
    ASSERT_EQ(starts.size(), 1u);
    EXPECT_FALSE(starts[0].contains("preamble"));
    EXPECT_TRUE(h.thinking_titles().empty());
    EXPECT_EQ(h.streamed_tokens(), "done");
}
