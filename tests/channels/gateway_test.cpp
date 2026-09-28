#include "test_support/channels/test_support.hpp"
#include "channels/bridge.hpp"
#include "session/attachment_store.hpp"
#include "session_host/local_session_client.hpp"
#include "session_host/session_registry.hpp"
#include <condition_variable>
#include <fstream>
#include <thread>

namespace acecode::channels {
namespace {
class ChannelGatewayTest : public testing::Test {
protected:
    const std::filesystem::path dir = test::temporary("gateway");
    test::Home home{dir};
    State state{dir / "whatsapp"};
    test::Client client;
    std::mutex output_mu;
    std::condition_variable output_cv;
    std::vector<Json> outputs;
    std::vector<Json> pending;
    std::string download_path;
    std::unique_ptr<Gateway> gateway;
    const Address dm{"100@s.whatsapp.net", "200@s.whatsapp.net", "200@s.whatsapp.net", false};
    GatewayDeps deps() {
        GatewayDeps deps{client};
        deps.request = [this](const std::string& method, const Json& params) {
            if (method == "download") return Json{{"path", download_path}};
            { std::lock_guard<std::mutex> lock(output_mu); outputs.push_back(params); }
            output_cv.notify_all(); return Json::object();
        };
        deps.permissions = [this](const std::string&) { return pending; };
        deps.transcript = [](const std::string&) { return Json::array(); };
        deps.session_cwd = [this](const std::string& id) {
            auto cwd = dir / id; std::filesystem::create_directories(cwd); return path_to_utf8(cwd);
        };
        return deps;
    }
    void SetUp() override {
        state.set_enabled(true);
        state.set_access(dm.account, dm.sender, true);
        gateway = std::make_unique<Gateway>(state, deps());
    }
    void TearDown() override {
        gateway.reset(); std::error_code ec; std::filesystem::remove_all(dir, ec);
    }
    Json message(std::string id = "m1", std::string text = "hello", Address address = {}) {
        if (address.account.empty()) address = dm;
        auto input = address.json(); input["id"] = id; input["text"] = text; return input;
    }
    std::string first() { return gateway->receive(message()).at("session_id"); }
    void wait_outputs(std::size_t count) {
        std::unique_lock<std::mutex> lock(output_mu);
        ASSERT_TRUE(output_cv.wait_for(lock, std::chrono::seconds(2), [&] { return outputs.size() >= count; }));
    }
};
TEST_F(ChannelGatewayTest, CreatesIndependentNoWorkspaceSessionsAndDeduplicatesAcrossRestart) {
    auto id = first();
    ASSERT_EQ(client.creates.size(), 1);
    EXPECT_TRUE(client.creates[0].no_workspace);
    EXPECT_TRUE(client.creates[0].cwd.empty());
    EXPECT_TRUE(client.creates[0].parent_session_id.empty());
    EXPECT_EQ(client.creates[0].permission_mode, "default");
    EXPECT_FALSE(client.creates[0].inherit_dangerous_mode);
    EXPECT_EQ(gateway->receive(message())["status"], "duplicate");
    EXPECT_EQ(client.inputs.size(), 1);
    gateway.reset(); state.load(); gateway = std::make_unique<Gateway>(state, deps());
    EXPECT_EQ(gateway->receive(message())["status"], "duplicate");
    EXPECT_EQ(gateway->receive(message("m2"))["session_id"], id);
    EXPECT_EQ(client.creates.size(), 1);
    ASSERT_EQ(client.resumes.size(), 1);
    EXPECT_TRUE(client.resumes[0].no_workspace);
    EXPECT_FALSE(client.resumes[0].inherit_dangerous_mode);
}
TEST_F(ChannelGatewayTest, RejectedOversizeInputNeverBecomesAPartialPrompt) {
    auto m = message("large", ""); m["error"] = "Message is too large; send a shorter message";
    EXPECT_THROW(gateway->receive(m), std::exception);
    EXPECT_TRUE(client.inputs.empty());
    EXPECT_FALSE(state.receipt(dm, "large"));
    wait_outputs(1);
    std::lock_guard<std::mutex> lock(output_mu);
    ASSERT_EQ(outputs.size(), 1);
    EXPECT_NE(outputs[0]["text"].get<std::string>().find("too large"), std::string::npos);
}
TEST_F(ChannelGatewayTest, UnknownContactNeedsLocalApprovalAndCannotConfigureAccessRemotely) {
    state.set_access(dm.account, dm.sender, false);
    EXPECT_EQ(gateway->receive(message())["status"], "pairing_required");
    EXPECT_TRUE(client.creates.empty());
    EXPECT_TRUE(client.inputs.empty());
    const auto pairs = gateway->control({{"op", "pending"}}).at("pairing");
    ASSERT_EQ(pairs.size(), 1);
    EXPECT_EQ(gateway->receive(message("m2", "/approve " + pairs[0]["code"].get<std::string>()))["status"], "pairing_required");
    EXPECT_FALSE(state.allowed(dm, false));
    gateway->control({{"op", "approve"}, {"code", pairs[0]["code"]}});
    EXPECT_EQ(gateway->receive(message("m3"))["status"], "accepted");
}
TEST_F(ChannelGatewayTest, GroupsRequireMentionAndNeverShareParticipantSessions) {
    auto a = dm; a.chat = "123@g.us"; a.group = true;
    auto m = message("g1", "hello", a); m["mentioned"] = true;
    EXPECT_EQ(gateway->receive(m)["status"], "ignored");
    state.set_access(a.account, a.chat, true);
    m["mentioned"] = false;
    EXPECT_EQ(gateway->receive(m)["status"], "ignored");
    m["mentioned"] = true;
    auto one = gateway->receive(m)["session_id"];
    a.sender = "300@s.whatsapp.net"; state.set_access(a.account, a.sender, true);
    m = message("g2", "hello", a); m["mentioned"] = true;
    EXPECT_NE(gateway->receive(m)["session_id"], one);
    EXPECT_EQ(client.creates.size(), 2);
}
TEST_F(ChannelGatewayTest, ResumeFailureDoesNotReplaceHistoryAndRejectedInputCanRetry) {
    state.bind(dm, "persisted-id"); client.resume_ok = false;
    EXPECT_THROW(gateway->receive(message()), std::exception);
    EXPECT_EQ(state.session(dm), "persisted-id");
    EXPECT_TRUE(client.creates.empty());
    client.resume_ok = true; client.send_ok = false;
    EXPECT_THROW(gateway->receive(message()), std::exception);
    EXPECT_FALSE(state.receipt(dm, "m1"));
    client.send_ok = true;
    EXPECT_EQ(gateway->receive(message())["status"], "accepted");
}
TEST_F(ChannelGatewayTest, PermissionAndStopControlsBypassModelQueueAndStayScoped) {
    const auto id = first();
    client.emit(id, SessionEventKind::PermissionRequest, {{"request_id", "p1"}, {"tool", "bash"}, {"args", {{"command", "test"}}}});
    gateway->receive(message("m2", "/approve another-session"));
    EXPECT_TRUE(client.decisions.empty());
    gateway->receive(message("m3", "/approve p1"));
    ASSERT_EQ(client.decisions.size(), 1);
    EXPECT_EQ(client.decisions[0].first, id);
    EXPECT_EQ(client.decisions[0].second.choice, PermissionDecisionChoice::Allow);
    gateway->control({{"op", "send"}, {"session_id", id}, {"text", "/deny p1"}});
    EXPECT_EQ(client.decisions.size(), 1);
    gateway->receive(message("m4", "/stop"));
    EXPECT_EQ(client.aborts, std::vector<std::string>{id});
    EXPECT_EQ(client.inputs.size(), 1);
}
TEST_F(ChannelGatewayTest, SnapshotDoesNotResurrectClosedPermission) {
    pending = {{{"request_id", "p1"}, {"tool", "bash"}, {"args", Json::object()}}};
    client.after_subscribe = [&](const std::string& id) {
        client.emit(id, SessionEventKind::PermissionClosed, {{"request_id", "p1"}, {"choice", "deny"}});
    };
    const auto id = first();
    EXPECT_TRUE(gateway->control({{"op", "show"}, {"session_id", id}})["permissions"].empty());
}
TEST_F(ChannelGatewayTest, QuestionBridgeIsReusedAndAnswersNeverBecomePrompts) {
    const auto id = first();
    client.emit(id, SessionEventKind::QuestionRequest, {{"request_id", "q1"}, {"questions", Json::array({
        {{"id", "choice"}, {"text", "Which?"}, {"header", "Pick"}, {"options", Json::array({{{"label", "One"}}, {{"label", "Two"}}})}}
    })}});
    gateway->receive(message("m2", "/aq 1"));
    EXPECT_EQ(client.inputs.size(), 1);
    EXPECT_EQ(client.answered.count("q1"), 1);
}
TEST_F(ChannelGatewayTest, RepliesUseNativeQuoteAndDoNotLeakOtherSessions) {
    const auto id = first();
    client.emit("unbound", SessionEventKind::Message, {{"role", "assistant"}, {"content", "secret"}});
    client.emit(id, SessionEventKind::Message, {{"role", "assistant"}, {"content", "reply"}});
    wait_outputs(1);
    std::lock_guard<std::mutex> lock(output_mu);
    ASSERT_EQ(outputs.size(), 1);
    EXPECT_EQ(outputs[0]["chat"], dm.chat); EXPECT_EQ(outputs[0]["quote_id"], "m1");
    EXPECT_EQ(outputs[0]["text"], "reply");
}
TEST_F(ChannelGatewayTest, FilesUseExistingAttachmentStoreAndRejectCacheEscape) {
    std::filesystem::create_directories(state.directory() / "media");
    const auto file = state.directory() / "media" / "file";
    std::ofstream(file, std::ios::binary) << "document bytes";
    download_path = path_to_utf8(file);
    auto m = message(); m["media"] = {{"kind", "document"}, {"name", "file.txt"}, {"mime_type", "text/plain"}};
    EXPECT_EQ(gateway->receive(m)["status"], "accepted");
    ASSERT_EQ(client.inputs.size(), 1);
    const auto& input = client.inputs[0].second;
    ASSERT_EQ(input.content_parts.size(), 2);
    EXPECT_EQ(input.content_parts[1]["type"], "file");
    EXPECT_EQ(input.metadata["attachments"][0]["name"], "file.txt");
    EXPECT_FALSE(std::filesystem::exists(file));
    const auto outside = dir / "secret.txt"; std::ofstream(outside) << "private";
    download_path = path_to_utf8(outside); m["id"] = "m2";
    EXPECT_THROW(gateway->receive(m), std::exception);
    EXPECT_EQ(client.inputs.size(), 1);
}
TEST_F(ChannelGatewayTest, MediaMustBelongToTheStartupCredentialProfile) {
    state.enable_with_access(dm.account, {dm.sender}, std::string(32, 'a'));
    const auto profile = state.transport_directory();
    std::filesystem::create_directories(profile / "media");
    const auto file = profile / "media" / "file";
    std::ofstream(file) << "document bytes";
    download_path = path_to_utf8(file);
    auto input = message();
    input["media"] = {{"kind", "document"}, {"name", "file.txt"}, {"mime_type", "text/plain"}};
    EXPECT_EQ(gateway->receive(input)["status"], "accepted");
    ASSERT_EQ(client.inputs.size(), 1);
    const auto other = state.directory() / "profiles" / std::string(32, 'b') / "media";
    std::filesystem::create_directories(other);
    std::ofstream(other / "file") << "other profile";
    download_path = path_to_utf8(other / "file"); input["id"] = "m2";
    EXPECT_THROW(gateway->receive(input), std::exception);
    EXPECT_EQ(client.inputs.size(), 1);
    EXPECT_TRUE(std::filesystem::exists(other / "file"));
}
class EchoProvider : public LlmProvider {
public:
    ChatResponse chat(const std::vector<ChatMessage>&, const std::vector<ToolDef>&) override {
        ChatResponse response; response.content = "channel reply"; response.finish_reason = "stop"; return response;
    }
    void chat_stream(const std::vector<ChatMessage>&, const std::vector<ToolDef>&,
                     const StreamCallback& callback, std::atomic<bool>*) override {
        StreamEvent delta; delta.type = StreamEventType::Delta; delta.content = "channel reply"; callback(delta);
        StreamEvent done; done.type = StreamEventType::Done; done.finish_reason = "stop"; callback(done);
    }
    std::string name() const override { return "channel-test"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "channel-test"; }
    void set_model(const std::string&) override {}
};
TEST(ChannelGatewayIntegration, RealRegistryRunsAndResumesNoWorkspaceHistory) {
    const auto dir = test::temporary("registry");
    test::Home home(dir);
    {
        ToolExecutor tools;
        PermissionManager permissions;
        permissions.set_dangerous(true);
        permissions.set_mode(PermissionMode::Yolo);
        auto provider = std::make_shared<EchoProvider>();
        SessionRegistryDeps registry_deps;
        registry_deps.provider_accessor = [provider] { return provider; };
        registry_deps.tools = &tools;
        registry_deps.template_permissions = &permissions;
        registry_deps.cwd = path_to_utf8(dir / "host");
        registry_deps.no_workspace_cache_root = path_to_utf8(dir / "no-workspace");
        registry_deps.auto_title_generator = [](const std::string&) { return std::optional<std::string>{"WhatsApp test"}; };
        SessionRegistry registry(registry_deps);
        LocalSessionClient client(registry);
        State state(dir / "whatsapp"); state.set_enabled(true);
        const Address address{"100@s.whatsapp.net", "200@s.whatsapp.net", "200@s.whatsapp.net", false};
        state.set_access(address.account, address.sender, true);
        std::mutex mu; std::condition_variable cv; std::vector<Json> sent;
        GatewayDeps deps{client};
        deps.request = [&](const std::string&, const Json& value) {
            { std::lock_guard<std::mutex> lock(mu); sent.push_back(value); }
            cv.notify_all(); return Json::object();
        };
        deps.session_cwd = [&](const std::string& id) { return registry.acquire(id)->cwd; };
        deps.permissions = [&](const std::string& id) { return registry.acquire(id)->prompter->snapshot_pending_requests(); };
        deps.transcript = [](const std::string&) { return Json::array(); };
        auto message = address.json(); message["id"] = "m1"; message["text"] = "hello";
        std::string id;
        {
            Gateway gateway(state, deps);
            id = gateway.receive(message).at("session_id");
            std::unique_lock<std::mutex> lock(mu);
            ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return !sent.empty(); }));
            EXPECT_EQ(sent[0]["text"], "channel reply");
            EXPECT_EQ(sent[0]["quote_id"], "m1");
        }
        auto entry = registry.acquire(id);
        ASSERT_TRUE(entry);
        EXPECT_TRUE(entry->no_workspace);
        EXPECT_FALSE(entry->perm->is_dangerous());
        EXPECT_EQ(entry->perm->mode(), PermissionMode::Default);
        EXPECT_NE(entry->cwd, registry_deps.cwd);
        EXPECT_FALSE(entry->sm->load_active_messages().empty());
        entry.reset(); registry.destroy(id);
        state.load();
        Gateway resumed(state, deps);
        message["id"] = "m2"; message["text"] = "again";
        EXPECT_EQ(resumed.receive(message)["session_id"], id);
        entry = registry.acquire(id);
        ASSERT_TRUE(entry);
        EXPECT_FALSE(entry->perm->is_dangerous());
        EXPECT_EQ(entry->perm->mode(), PermissionMode::Default);
        std::unique_lock<std::mutex> lock(mu);
        EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return sent.size() >= 2; }));
    }
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}
} // namespace
} // namespace acecode::channels
