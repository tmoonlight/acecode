#include <gtest/gtest.h>

#include "channels/core/conversations.hpp"
#include "channels/core/group_watch.hpp"
#include "channels/core/platforms.hpp"
#include "test_support/channels/core_fakes.hpp"

#include <filesystem>
#include <memory>

// 群聊代答(openspec add-channel-platform-plugins D5):设置校验、汇总文本,以及 Conversations
// 里「群消息缓冲 → 到点汇总提交 → 只能经 send_to_group 发群」的整条路径。
// 用内存版 SessionClient 与假传输层;插件平台用 ScopedPluginPlatforms 注册一个名为 example 的测试平台。

namespace acecode::channels::core {
namespace {

using test::FakeSessions;
using test::FakeTransport;
using test::message;
using test::wait_until;

PlatformSpec example_spec() {
    PlatformSpec spec;
    spec.name = "example";
    spec.label = "Example IM";
    spec.owner_binding = OwnerBinding::Self;
    spec.plugin = PluginLaunch{"C:/plugins/example", "C:/plugins/example/plugin.exe", {}};
    spec.watched_groups = true;
    return spec;
}

im::Address self_address() { return test::private_address("me", "example", "me"); }
im::Address group_member(const std::string& group, const std::string& member) {
    return test::group_address(group, member, "example", "me");
}

im::Inbound group_message(const std::string& group, const std::string& member, const std::string& text,
                          const std::string& id, bool mentioned = false, bool from_self = false) {
    auto inbound = message(group_member(group, member), text, id, mentioned);
    inbound.sender_name = member == "me" ? "我自己" : "成员" + member;
    inbound.chat_name = "项目群";
    inbound.from_self = from_self;
    inbound.timestamp_ms = 1761300000000LL;  // 2025-10-24 10:00 UTC
    return inbound;
}

// ---- 设置与汇总文本 ----

// 场景:用户在卡片里粘贴多个群 ID,用换行、英文逗号和中文逗号混着分隔,还有重复。
// 期望:拆成去重保序的列表,其余字段取默认值。
TEST(GroupWatchSettings, ParsesMixedSeparatorsAndDedupes) {
    std::string error;
    const auto settings = parse_group_watch_settings({{"groups", "g1\ng2,g3\xEF\xBC\x8Cg1"}}, &error);
    ASSERT_TRUE(settings) << error;
    EXPECT_EQ(settings->groups, (std::vector<std::string>{"g1", "g2", "g3"}));
    EXPECT_EQ(settings->poll_interval_s, GroupWatchSettings::kDefaultPollIntervalS);
    EXPECT_TRUE(settings->prefix.empty());
}

// 场景:轮询间隔低于 30 秒、群 ID 带空格以外的非法字符、前缀超过 32 字。
// 期望:逐项拒绝并给出可展示的原因。30 秒下限防止每条群消息都打一次模型。
TEST(GroupWatchSettings, RejectsInvalidValues) {
    std::string error;
    EXPECT_FALSE(parse_group_watch_settings({{"poll_interval_s", 10}}, &error));
    EXPECT_NE(error.find("30"), std::string::npos);
    EXPECT_FALSE(parse_group_watch_settings({{"groups", "g1<script>"}}, &error));
    EXPECT_FALSE(parse_group_watch_settings({{"prefix", std::string(33, 'a')}}, &error));
    EXPECT_FALSE(parse_group_watch_settings({{"prefix", "a\nb"}}, &error));
    EXPECT_TRUE(parse_group_watch_settings({{"poll_interval_s", "300"}, {"prefix", "[AI] "}}, &error)) << error;
}

// 场景:三条消息,一条点名了用户、一条是用户本人发的、一条带换行。
// 期望:首行带群名、时间范围与条数;点名的标「(@我)」,本人标「我」;多行内容缩进续行。
TEST(GroupWatchDigest, FormatsHeaderMarksAndMultiline) {
    std::deque<GroupMessage> messages;
    messages.push_back({"1", 1761300000000LL, "u2", "Bob", "明天开会", false, false});
    messages.push_back({"2", 1761300060000LL, "u3", "Carol", "@张三 看下这个", false, true});
    messages.push_back({"3", 1761300120000LL, "me", "张三", "好\n马上", true, false});
    const auto text = build_group_digest("项目群", messages, 0, 0);
    EXPECT_EQ(text.rfind("[群聊新消息] 群「项目群」10:00–10:02,共 3 条", 0), 0u) << text;
    EXPECT_NE(text.find("[10:01] Carol(@我): @张三 看下这个"), std::string::npos) << text;
    EXPECT_NE(text.find("[10:02] 我: 好\n    马上"), std::string::npos) << text;
    EXPECT_EQ(group_digest_display("项目群", 3), "群聊「项目群」3 条新消息");
}

// 场景:缓冲区收到超过 200 条消息,且有重复投递的 message_id。
// 期望:重复的忽略;只保留最近 200 条,溢出的计入 omitted 并在汇总首行注明。
TEST(GroupWatchDigest, BufferCapsAndCountsOmitted) {
    GroupBuffer buffer;
    for (int i = 0; i < 205; ++i) buffer.add({"id" + std::to_string(i), 0, "u", "U", "x", false, false});
    EXPECT_FALSE(buffer.add({"id204", 0, "u", "U", "dup", false, false}));
    EXPECT_EQ(buffer.size(), GroupBuffer::kMaxMessages);
    EXPECT_EQ(buffer.omitted(), 5u);
    const auto text = build_group_digest("群", buffer.messages(), buffer.omitted(), 0);
    EXPECT_NE(text.find("更早的 5 条已省略"), std::string::npos);
    buffer.consume(10);
    EXPECT_EQ(buffer.size(), GroupBuffer::kMaxMessages - 10);
    EXPECT_EQ(buffer.omitted(), 0u);
}

// ---- Conversations 集成 ----

class GroupDelegate : public ::testing::Test {
protected:
    void SetUp() override {
        home_dir = channels::test::temporary("group-delegate");
        std::filesystem::create_directories(home_dir);
        home = std::make_unique<channels::test::Home>(home_dir);
        transport = std::make_shared<FakeTransport>("example");
        transport->account = "me";
        transport->display_name = "张三";
        transport->start({});
        slot->set(transport);
        store = std::make_unique<ChannelStore>(home_dir / "example");
        store->load();
        store->update_config([](PlatformConfig& config) {
            config.enabled = true;
            config.group_watch.groups = {"g1"};
            config.group_watch.poll_interval_s = 60;
            config.group_watch.prefix = "[代答] ";
            config.group_watch.prompt = "只回答排期问题";
        });
        ConversationDeps deps;
        deps.sessions = &sessions;
        deps.pending_permissions = [](const std::string&) { return std::vector<nlohmann::json>{}; };
        deps.session_cwd = [this](const std::string& id) {
            std::lock_guard<std::mutex> lock(sessions.mu);
            return sessions.sessions[id].cwd;
        };
        deps.catalog = [this](const std::optional<std::string>&) { return sessions.catalog(); };
        deps.resume_target = [this](const rc::RcSessionTarget& target) {
            return sessions.resume_session(target.session_id, {});
        };
        deps.on_bindings_changed = [this] { ++binding_changes; };
        deps.set_delegate = [this](const std::string& id, const ChannelDelegate& delegate) {
            delegates[id] = delegate;
        };
        deps.clock = [this] { return clock; };
        deps.utc_offset_minutes = [] { return 0; };
        deps.media_dir = home_dir / "media";
        conversations = std::make_unique<Conversations>("example", *store, access, slot, deps);
    }

    void TearDown() override {
        conversations.reset();
        home.reset();
        std::error_code ec;
        std::filesystem::remove_all(home_dir, ec);
    }

    std::optional<BindingRecord> delegate_record() const {
        for (const auto& record : store->bindings())
            if (record.delegate) return record;
        return std::nullopt;
    }

    ScopedPluginPlatforms plugins{{example_spec()}};
    std::filesystem::path home_dir;
    std::unique_ptr<channels::test::Home> home;
    FakeSessions sessions;
    std::shared_ptr<FakeTransport> transport;
    std::shared_ptr<TransportSlot> slot = std::make_shared<TransportSlot>();
    std::unique_ptr<ChannelStore> store;
    AccessControl access;
    std::unique_ptr<Conversations> conversations;
    std::map<std::string, ChannelDelegate> delegates;
    std::chrono::steady_clock::time_point clock = std::chrono::steady_clock::now();
    int binding_changes = 0;
};

// 场景:本人账号模型下,用户自己给自己发消息,另一个人也私聊过来。
// 期望:自己发的建会话并提交(按机主处理),别人的私聊直接忽略,不回配对提示。
TEST_F(GroupDelegate, SelfChatIsOwnerAndOtherPrivateChatsAreIgnored) {
    conversations->handle(message(self_address(), "帮我看下日志", "s1"));
    conversations->handle(message(test::private_address("u9", "example", "me"), "你好", "p1"));
    const auto inputs = sessions.input_log();
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs[0].second.text, "帮我看下日志");
    EXPECT_EQ(transport->sent_count(), 0u);
}

// 场景:关注的群里陆续来了两条消息,轮询间隔(60 秒)还没到;之后时间过了 61 秒。
// 期望:到点前不提交;到点后自动建一个代答会话、写入代答身份,并把两条消息汇总成一条用户消息。
// 回归保护:群消息不能逐条触发模型,也不能经出站投影把 AI 的正文发到群里。
TEST_F(GroupDelegate, MessagesWaitForTheIntervalThenBecomeOneDigest) {
    conversations->handle(group_message("g1", "u2", "周五能上线吗", "g-1", true));
    conversations->handle(group_message("g1", "u3", "顺便问下测试", "g-2"));
    conversations->poll_groups();
    EXPECT_TRUE(sessions.input_log().empty());

    clock += std::chrono::seconds(61);
    conversations->poll_groups();
    const auto inputs = sessions.input_log();
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs[0].second.text.rfind("[群聊新消息] 群「项目群」", 0), 0u) << inputs[0].second.text;
    EXPECT_NE(inputs[0].second.text.find("成员u2(@我): 周五能上线吗"), std::string::npos);
    EXPECT_EQ(inputs[0].second.display_text, "群聊「项目群」2 条新消息");
    EXPECT_TRUE(inputs[0].second.metadata["channel"].value("group_watch", false));

    const auto record = delegate_record();
    ASSERT_TRUE(record);
    EXPECT_EQ(record->session_id, inputs[0].first);
    EXPECT_EQ(record->chat_name, "项目群");
    const auto& delegate = delegates[record->session_id];
    EXPECT_TRUE(delegate.active());
    EXPECT_EQ(delegate.chat, "g1");
    EXPECT_EQ(delegate.user_name, "张三");
    EXPECT_EQ(delegate.prompt, "只回答排期问题");

    // AI 在代答会话里的普通回复不发到群里。
    sessions.assistant(record->session_id, "这条不应该出现在群里");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(transport->sent_count(), 0u);
}

// 场景:到点时缓冲区里只有用户本人在群里发的消息。
// 期望:不提交(群里没有新的他人发言),这条留作下一次汇总的上下文。
TEST_F(GroupDelegate, OwnMessagesAloneDoNotTriggerADigest) {
    conversations->handle(group_message("g1", "me", "我先吃饭", "g-1", false, true));
    clock += std::chrono::seconds(61);
    conversations->poll_groups();
    EXPECT_TRUE(sessions.input_log().empty());

    conversations->handle(group_message("g1", "u2", "回来了说一声", "g-2"));
    clock += std::chrono::seconds(10);  // 两次检查之间的 5 秒限频
    conversations->poll_groups();
    const auto inputs = sessions.input_log();
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_NE(inputs[0].second.text.find("我: 我先吃饭"), std::string::npos);
}

// 场景:到点时代答会话还在处理上一批。
// 期望:本次不提交,会话空闲后的下一次检查再提交,消息不丢。
TEST_F(GroupDelegate, BusySessionDefersTheDigest) {
    conversations->handle(group_message("g1", "u2", "第一批", "g-1"));
    clock += std::chrono::seconds(61);
    conversations->poll_groups();
    const auto record = delegate_record();
    ASSERT_TRUE(record);
    sessions.set_busy(record->session_id, true);
    conversations->handle(group_message("g1", "u2", "第二批", "g-2"));
    clock += std::chrono::seconds(61);
    conversations->poll_groups();
    EXPECT_EQ(sessions.input_log().size(), 1u);

    sessions.set_busy(record->session_id, false);
    clock += std::chrono::seconds(6);
    conversations->poll_groups();
    const auto inputs = sessions.input_log();
    ASSERT_EQ(inputs.size(), 2u);
    EXPECT_NE(inputs[1].second.text.find("第二批"), std::string::npos);
}

// 场景:没有在设置里的群发来消息。
// 期望:完全忽略,不建会话、不回复。
TEST_F(GroupDelegate, UnwatchedGroupsAreIgnored) {
    conversations->handle(group_message("g-other", "u2", "广告", "x-1", true));
    clock += std::chrono::seconds(61);
    conversations->poll_groups();
    EXPECT_TRUE(sessions.input_log().empty());
    EXPECT_TRUE(sessions.created_log().empty());
}

// 场景:代答会话里调用发群工具;连发 6 条;发空消息;另一个普通会话也调用。
// 期望:发到绑定的群并带上配置的前缀;每次汇总后最多 5 条;空消息拒绝;非代答会话返回空(交给别的平台)。
TEST_F(GroupDelegate, SendToGroupAppliesPrefixAndLimits) {
    conversations->handle(group_message("g1", "u2", "在吗", "g-1"));
    clock += std::chrono::seconds(61);
    conversations->poll_groups();
    const auto record = delegate_record();
    ASSERT_TRUE(record);

    const auto first = conversations->send_to_group(record->session_id, "在的");
    ASSERT_TRUE(first);
    EXPECT_TRUE(first->ok) << first->error;
    const auto sent = transport->sent();
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].text, "[代答] 在的");
    EXPECT_EQ(sent[0].to.kind, im::ChatKind::Group);
    EXPECT_EQ(sent[0].to.chat, "g1");

    for (int i = 0; i < 4; ++i) EXPECT_TRUE(conversations->send_to_group(record->session_id, "补充")->ok);
    const auto sixth = conversations->send_to_group(record->session_id, "第六条");
    ASSERT_TRUE(sixth);
    EXPECT_FALSE(sixth->ok);
    EXPECT_NE(sixth->error.find("5 条"), std::string::npos);
    EXPECT_FALSE(conversations->send_to_group(record->session_id, "   ")->ok);
    EXPECT_FALSE(conversations->send_to_group("not-a-delegate", "hi"));
}

// 场景:用户在设置里把群从列表里删掉。
// 期望:解除该群的代答绑定,并清除会话上的代答身份(会话保留为普通历史会话)。
TEST_F(GroupDelegate, RemovingAGroupDropsItsDelegateBinding) {
    conversations->handle(group_message("g1", "u2", "在吗", "g-1"));
    clock += std::chrono::seconds(61);
    conversations->poll_groups();
    const auto record = delegate_record();
    ASSERT_TRUE(record);
    const auto previous = store->config().group_watch;
    auto next = previous;
    next.groups.clear();
    store->update_config([&next](PlatformConfig& config) { config.group_watch = next; });
    conversations->apply_group_settings(previous, next);
    EXPECT_FALSE(delegate_record());
    EXPECT_FALSE(delegates[record->session_id].active());
}

// 场景:修改代答提示词。
// 期望:已有代答会话的身份随之刷新(下一次请求的系统提示就是新约定)。
TEST_F(GroupDelegate, PromptChangeRefreshesExistingDelegates) {
    conversations->handle(group_message("g1", "u2", "在吗", "g-1"));
    clock += std::chrono::seconds(61);
    conversations->poll_groups();
    const auto record = delegate_record();
    ASSERT_TRUE(record);
    const auto previous = store->config().group_watch;
    auto next = previous;
    next.prompt = "什么都别回";
    store->update_config([&next](PlatformConfig& config) { config.group_watch = next; });
    conversations->apply_group_settings(previous, next);
    EXPECT_EQ(delegates[record->session_id].prompt, "什么都别回");
}

} // namespace
} // namespace acecode::channels::core
