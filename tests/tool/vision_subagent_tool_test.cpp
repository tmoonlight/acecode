#include <gtest/gtest.h>

#include "config/config.hpp"
#include "session/attachment_store.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "tool/builtin_tool_registry.hpp"
#include "tool/tool_executor.hpp"
#include "tool/vision_subagent_tool.hpp"
#include "utils/utf8_path.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

class CapturingProvider : public acecode::LlmProvider {
public:
    acecode::ChatResponse chat(
        const std::vector<acecode::ChatMessage>& messages,
        const std::vector<acecode::ToolDef>& tools) override {
        messages_ = messages;
        tools_ = tools;
        acecode::ChatResponse response;
        response.content = "vision result";
        response.finish_reason = "stop";
        return response;
    }

    void chat_stream(
        const std::vector<acecode::ChatMessage>&,
        const std::vector<acecode::ToolDef>&,
        const acecode::StreamCallback& callback,
        std::atomic<bool>* = nullptr) override {
        acecode::StreamEvent done;
        done.type = acecode::StreamEventType::Done;
        callback(done);
    }

    std::string name() const override { return "capturing"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "capturing-model"; }
    void set_model(const std::string&) override {}

    std::vector<acecode::ChatMessage> messages_;
    std::vector<acecode::ToolDef> tools_;
};

fs::path temp_cwd(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_vision_tool_" + hint + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

acecode::ModelProfile model_profile(
    const std::string& name,
    std::vector<std::string> capabilities) {
    acecode::ModelProfile profile;
    profile.name = name;
    profile.provider = "openai";
    profile.base_url = "http://localhost/v1";
    profile.api_key = "sk-test";
    profile.model = name + "-id";
    profile.capabilities = std::move(capabilities);
    return profile;
}

acecode::ChatMessage user_with_image(const acecode::AttachmentRecord& record) {
    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content = "see image";
    msg.content_parts = nlohmann::json::array({
        nlohmann::json{{"type", "text"}, {"text", "see image"}},
        nlohmann::json{{"type", "image"}, {"attachment", acecode::attachment_to_json(record)}},
    });
    return msg;
}

acecode::AttachmentRecord save_image(
    const std::string& project_dir,
    const std::string& session_id,
    const std::string& name) {
    std::string error;
    auto record = acecode::save_attachment(
        project_dir,
        session_id,
        name,
        "image/png",
        "png-bytes-" + name,
        &error);
    if (!record.has_value()) {
        ADD_FAILURE() << error;
        return {};
    }
    return *record;
}

} // namespace

TEST(VisionSubagentTool, MissingVisionModelFailsClearly) {
    acecode::AppConfig cfg;
    cfg.saved_models.push_back(model_profile("text-only", {"tool_use"}));
    auto tool = acecode::create_vision_analyze_tool(cfg);

    acecode::ToolContext ctx;
    auto result = tool.execute(
        R"({"prompt":"what is in the image?","attachment":{"id":"att_1","session_id":"s","name":"screen.png","kind":"image","mime_type":"image/png","path":"x","blob_url":"/b","size_bytes":1}})",
        ctx);

    EXPECT_FALSE(result.success);
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["error"], "NO_VISION_MODEL");
}

TEST(VisionSubagentTool, UsesLatestActiveSessionImageAndDoesNotPersistChildCall) {
    auto cwd = temp_cwd("latest");
    const std::string session_id = "sid-vision-latest";
    const std::string project_dir =
        acecode::SessionStorage::get_project_dir(acecode::path_to_utf8(cwd));
    fs::remove_all(project_dir);

    acecode::SessionManager sm;
    sm.start_session(acecode::path_to_utf8(cwd), "stub", "stub-model", session_id);
    auto first = save_image(project_dir, session_id, "first.png");
    auto second = save_image(project_dir, session_id, "second.png");
    sm.on_message(user_with_image(first));
    sm.on_message(user_with_image(second));
    const auto before = sm.load_active_messages().size();
    const auto sessions_before = acecode::SessionStorage::list_sessions(project_dir).size();

    acecode::AppConfig cfg;
    cfg.saved_models.push_back(model_profile("vision-one", {"vision", "tool_use"}));
    auto provider = std::make_shared<CapturingProvider>();
    acecode::ModelProfile selected;
    acecode::VisionSubagentToolOptions opts;
    opts.provider_factory = [&](const acecode::ModelProfile& profile) {
        selected = profile;
        return provider;
    };
    auto tool = acecode::create_vision_analyze_tool(cfg, opts);

    acecode::ToolContext ctx;
    ctx.session_manager = &sm;
    auto result = tool.execute(R"({"prompt":"describe the screenshot"})", ctx);

    EXPECT_TRUE(result.success) << result.output;
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["model_name"], "vision-one");
    EXPECT_EQ(json["attachment_id"], second.id);
    EXPECT_EQ(selected.name, "vision-one");
    ASSERT_EQ(provider->messages_.size(), 1u);
    ASSERT_TRUE(provider->tools_.empty());
    ASSERT_TRUE(provider->messages_[0].content_parts.is_array());
    EXPECT_EQ(provider->messages_[0].content_parts[1]["attachment"]["id"], second.id);
    EXPECT_EQ(sm.load_active_messages().size(), before)
        << "internal vision call must not append child messages to the active session";
    EXPECT_EQ(acecode::SessionStorage::list_sessions(project_dir).size(), sessions_before)
        << "internal vision call must not create a resumable child session";

    fs::remove_all(project_dir);
    fs::remove_all(cwd);
}

TEST(VisionSubagentTool, ExplicitModelAndAttachmentAreHonored) {
    acecode::AppConfig cfg;
    cfg.saved_models.push_back(model_profile("vision-a", {"vision"}));
    cfg.saved_models.push_back(model_profile("vision-b", {"vision"}));

    acecode::AttachmentRecord record;
    record.id = "att_explicit";
    record.session_id = "sid";
    record.name = "screen.png";
    record.kind = "image";
    record.mime_type = "image/png";
    record.path = "C:/tmp/screen.png";
    record.blob_url = "/api/sessions/sid/attachments/att_explicit/blob";
    record.size_bytes = 3;

    auto provider = std::make_shared<CapturingProvider>();
    acecode::ModelProfile selected;
    acecode::VisionSubagentToolOptions opts;
    opts.provider_factory = [&](const acecode::ModelProfile& profile) {
        selected = profile;
        return provider;
    };
    opts.choose_index = [](std::size_t) { return 0u; };
    auto tool = acecode::create_vision_analyze_tool(cfg, opts);

    nlohmann::json args = {
        {"prompt", "read it"},
        {"model_name", "vision-b"},
        {"attachment", acecode::attachment_to_json(record)},
    };
    acecode::ToolContext ctx;
    auto result = tool.execute(args.dump(), ctx);

    EXPECT_TRUE(result.success) << result.output;
    EXPECT_EQ(selected.name, "vision-b");
    ASSERT_EQ(provider->messages_.size(), 1u);
    EXPECT_EQ(provider->messages_[0].content_parts[1]["attachment"]["id"], "att_explicit");
}

TEST(VisionSubagentTool, RandomChooserIsUsedWhenMultipleVisionModelsExist) {
    acecode::AppConfig cfg;
    cfg.saved_models.push_back(model_profile("vision-a", {"vision"}));
    cfg.saved_models.push_back(model_profile("vision-b", {"vision"}));

    acecode::AttachmentRecord record;
    record.id = "att";
    record.session_id = "sid";
    record.name = "screen.png";
    record.kind = "image";
    record.mime_type = "image/png";
    record.path = "C:/tmp/screen.png";
    record.blob_url = "/api/sessions/sid/attachments/att/blob";
    record.size_bytes = 1;

    auto provider = std::make_shared<CapturingProvider>();
    acecode::ModelProfile selected;
    acecode::VisionSubagentToolOptions opts;
    opts.provider_factory = [&](const acecode::ModelProfile& profile) {
        selected = profile;
        return provider;
    };
    opts.choose_index = [](std::size_t count) {
        EXPECT_EQ(count, 2u);
        return 1u;
    };

    auto tool = acecode::create_vision_analyze_tool(cfg, opts);
    nlohmann::json args = {
        {"prompt", "read it"},
        {"attachment", acecode::attachment_to_json(record)},
    };
    acecode::ToolContext ctx;
    auto result = tool.execute(args.dump(), ctx);

    EXPECT_TRUE(result.success) << result.output;
    EXPECT_EQ(selected.name, "vision-b");
}

TEST(VisionSubagentTool, RegisteredInSharedBuiltinToolSet) {
    acecode::AppConfig cfg;
    cfg.web_search.enabled = false;
    acecode::ToolExecutor tools;

    acecode::register_session_builtin_tools(tools, cfg);

    EXPECT_TRUE(tools.has_tool("vision_analyze"));
    EXPECT_TRUE(tools.is_read_only("vision_analyze"));
}

namespace {

// 把若干字节写到 cwd 下的文件,返回绝对路径。模拟 browser/shell 工具写出的截图。
std::string write_file(const fs::path& dir, const std::string& name,
                       const std::string& bytes) {
    const fs::path p = dir / name;
    std::ofstream ofs(p, std::ios::binary);
    ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ofs.close();
    return acecode::path_to_utf8(p);
}

struct VisionPathFixture {
    fs::path cwd;
    std::string project_dir;
    acecode::SessionManager sm;
    acecode::AppConfig cfg;
    std::shared_ptr<CapturingProvider> provider = std::make_shared<CapturingProvider>();
    acecode::ModelProfile selected;

    explicit VisionPathFixture(const std::string& hint) {
        cwd = temp_cwd(hint);
        project_dir = acecode::SessionStorage::get_project_dir(acecode::path_to_utf8(cwd));
        fs::remove_all(project_dir);
        sm.start_session(acecode::path_to_utf8(cwd), "stub", "stub-model",
                         "sid-vision-" + hint);
        cfg.saved_models.push_back(model_profile("vision-one", {"vision"}));
    }

    ~VisionPathFixture() {
        fs::remove_all(project_dir);
        fs::remove_all(cwd);
    }

    acecode::ToolImpl tool() {
        acecode::VisionSubagentToolOptions opts;
        opts.provider_factory = [this](const acecode::ModelProfile& profile) {
            selected = profile;
            return provider;
        };
        return acecode::create_vision_analyze_tool(cfg, opts);
    }
};

} // namespace

// 场景:image_path 用相对路径(基于 ToolContext.cwd)→ 读取 + 物化 + 发送给视觉模型。
TEST(VisionSubagentTool, ImagePathRelativeIsMaterializedAndSent) {
    VisionPathFixture fx("path_rel");
    write_file(fx.cwd, "shot.png", "fake-png-bytes");

    auto tool = fx.tool();
    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(fx.cwd);
    ctx.session_manager = &fx.sm;

    auto result = tool.execute(
        R"({"prompt":"describe","image_path":"shot.png"})", ctx);

    EXPECT_TRUE(result.success) << result.output;
    EXPECT_EQ(fx.selected.name, "vision-one");
    ASSERT_EQ(fx.provider->messages_.size(), 1u);
    ASSERT_TRUE(fx.provider->messages_[0].content_parts.is_array());
    // 物化后的附件应作为 image part 发给视觉模型。
    EXPECT_EQ(fx.provider->messages_[0].content_parts[1]["type"], "image");
}

// 场景:image_path 用 workspace 外的绝对路径 → 同样物化并发送。
TEST(VisionSubagentTool, ImagePathAbsoluteOutsideWorkspace) {
    VisionPathFixture fx("path_abs");
    // 写到一个独立的临时目录(不在 fx.cwd 之下),用绝对路径引用。
    auto outside = temp_cwd("path_abs_outside");
    const std::string abs_path = write_file(outside, "external.png", "fake-png-bytes");

    auto tool = fx.tool();
    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(fx.cwd);
    ctx.session_manager = &fx.sm;

    nlohmann::json args = {{"prompt", "describe"}, {"image_path", abs_path}};
    auto result = tool.execute(args.dump(), ctx);

    EXPECT_TRUE(result.success) << result.output;
    ASSERT_EQ(fx.provider->messages_.size(), 1u);

    fs::remove_all(outside);
}

// 场景:image_path 指向非图片文件 → 清晰拒绝。
TEST(VisionSubagentTool, ImagePathNonImageRejected) {
    VisionPathFixture fx("path_notimg");
    write_file(fx.cwd, "notes.txt", "plain text");

    auto tool = fx.tool();
    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(fx.cwd);
    ctx.session_manager = &fx.sm;

    auto result = tool.execute(
        R"({"prompt":"describe","image_path":"notes.txt"})", ctx);

    EXPECT_FALSE(result.success);
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["error"], "NOT_IMAGE");
}

// 场景:image_path 指向不存在的路径 → 清晰拒绝。
TEST(VisionSubagentTool, ImagePathMissingRejected) {
    VisionPathFixture fx("path_missing");

    auto tool = fx.tool();
    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(fx.cwd);
    ctx.session_manager = &fx.sm;

    auto result = tool.execute(
        R"({"prompt":"describe","image_path":"nope.png"})", ctx);

    EXPECT_FALSE(result.success);
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["error"], "IMAGE_PATH_NOT_FOUND");
}

// 场景:超大 image_path 在读取字节前就被大小校验拒绝(sparse file,瞬时创建)。
TEST(VisionSubagentTool, ImagePathTooLargeRejectedBeforeRead) {
    VisionPathFixture fx("path_toolarge");
    const fs::path big = fx.cwd / "big.png";
    {
        std::ofstream ofs(big, std::ios::binary);
        ofs.put('x');
        ofs.close();
    }
    std::error_code ec;
    fs::resize_file(big, acecode::kMaxAttachmentBytes + 1, ec);
    ASSERT_FALSE(ec) << ec.message();

    auto tool = fx.tool();
    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(fx.cwd);
    ctx.session_manager = &fx.sm;

    auto result = tool.execute(
        R"({"prompt":"describe","image_path":"big.png"})", ctx);

    EXPECT_FALSE(result.success);
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["error"], "IMAGE_TOO_LARGE");
}

// 场景:无 active session 时使用 image_path → 清晰拒绝,不崩溃。
TEST(VisionSubagentTool, ImagePathWithoutActiveSessionRejected) {
    auto cwd = temp_cwd("path_nosession");
    const std::string abs_path = write_file(cwd, "shot.png", "fake-png-bytes");

    acecode::AppConfig cfg;
    cfg.saved_models.push_back(model_profile("vision-one", {"vision"}));
    auto provider = std::make_shared<CapturingProvider>();
    acecode::VisionSubagentToolOptions opts;
    opts.provider_factory = [&](const acecode::ModelProfile&) { return provider; };
    auto tool = acecode::create_vision_analyze_tool(cfg, opts);

    acecode::ToolContext ctx;  // 无 session_manager
    ctx.cwd = acecode::path_to_utf8(cwd);
    nlohmann::json args = {{"prompt", "describe"}, {"image_path", abs_path}};
    auto result = tool.execute(args.dump(), ctx);

    EXPECT_FALSE(result.success);
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["error"], "NO_ACTIVE_SESSION");

    fs::remove_all(cwd);
}

// 场景:无任何视觉模型时使用 image_path → 在物化前就因 NO_VISION_MODEL 拒绝。
TEST(VisionSubagentTool, ImagePathRejectedWhenNoVisionModel) {
    auto cwd = temp_cwd("path_novision");
    const std::string abs_path = write_file(cwd, "shot.png", "fake-png-bytes");

    acecode::AppConfig cfg;
    cfg.saved_models.push_back(model_profile("text-only", {"tool_use"}));
    auto tool = acecode::create_vision_analyze_tool(cfg);

    acecode::SessionManager sm;
    const std::string project_dir =
        acecode::SessionStorage::get_project_dir(acecode::path_to_utf8(cwd));
    fs::remove_all(project_dir);
    sm.start_session(acecode::path_to_utf8(cwd), "stub", "stub-model", "sid-novision");

    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(cwd);
    ctx.session_manager = &sm;
    nlohmann::json args = {{"prompt", "describe"}, {"image_path", abs_path}};
    auto result = tool.execute(args.dump(), ctx);

    EXPECT_FALSE(result.success);
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["error"], "NO_VISION_MODEL");

    fs::remove_all(project_dir);
    fs::remove_all(cwd);
}

// ---------------------------------------------------------------------------
// 自调用防护(回归会话 20260830-024351-9599)
//
// bug 表现:主模型 Aurora-aurora 自己就带 `vision` 能力标签、图片也确实发到了它
// 手上,它却先 skill_view 了 vision-image-reader、再调 vision_analyze;子调用从
// 候选里挑中的还是它自己,于是绕一圈用同一个模型看了同一张图,白烧约 5k token。
// 用户制止后它直接看图,一次成功。
//
// 根因是模型无法自我判定"我看得见图吗",而工具/skill 只给了"当模型不能可靠看图
// 时使用"这种软条件。下面这组用例守住修复后的硬约束。
// ---------------------------------------------------------------------------

namespace {

// 构造一个"当前模型自己能看图"的 ToolContext。model_profile() 把 model id 定为
// `<name>-id`,这里保持一致,否则 (provider, model) 比对不上就测不到剔除逻辑。
acecode::ToolContext ctx_with_active_model(const std::string& saved_model_name,
                                           bool can_read_images) {
    acecode::ToolContext ctx;
    ctx.active_provider_name = "openai";
    ctx.active_model_id = saved_model_name + "-id";
    ctx.active_model_can_read_images = can_read_images;
    return ctx;
}

acecode::AttachmentRecord dummy_image_record(const std::string& id) {
    acecode::AttachmentRecord record;
    record.id = id;
    record.session_id = "sid";
    record.name = "screen.png";
    record.kind = "image";
    record.mime_type = "image/png";
    record.path = "C:/tmp/screen.png";
    record.blob_url = "/api/sessions/sid/attachments/" + id + "/blob";
    record.size_bytes = 3;
    return record;
}

} // namespace

// 触发场景:当前模型自己带 vision 标签,模型没指定 model_name 就调 vision_analyze
//   —— 这正是那次会话里发生的事。
// 期望行为:直接拒绝并给出 ACTIVE_MODEL_HAS_VISION,连 provider 都不构造。
TEST(VisionSubagentTool, ActiveModelWithVisionIsRejectedInsteadOfCallingItself) {
    acecode::AppConfig cfg;
    cfg.saved_models.push_back(model_profile("vision-self", {"tool_use", "vision"}));

    bool provider_was_constructed = false;
    acecode::VisionSubagentToolOptions opts;
    opts.provider_factory = [&](const acecode::ModelProfile&) {
        provider_was_constructed = true;
        return std::make_shared<CapturingProvider>();
    };
    auto tool = acecode::create_vision_analyze_tool(cfg, opts);

    nlohmann::json args = {
        {"prompt", "read it"},
        {"attachment", acecode::attachment_to_json(dummy_image_record("att_self"))},
    };
    auto result = tool.execute(args.dump(), ctx_with_active_model("vision-self", true));

    EXPECT_FALSE(result.success);
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["error"], "ACTIVE_MODEL_HAS_VISION");
    EXPECT_FALSE(provider_was_constructed)
        << "拒绝必须发生在构造 provider 之前,否则仍会付出一次真实往返";
}

// 触发场景:当前模型能看图,但系统里还有别的视觉模型,模型仍未指定 model_name。
// 期望行为:照样拒绝。有别的视觉模型可用不构成绕道的理由 —— 自己看是最省的那条路。
TEST(VisionSubagentTool, ActiveModelWithVisionIsRejectedEvenWhenOtherVisionModelsExist) {
    acecode::AppConfig cfg;
    cfg.saved_models.push_back(model_profile("vision-self", {"vision"}));
    cfg.saved_models.push_back(model_profile("vision-other", {"vision"}));

    auto tool = acecode::create_vision_analyze_tool(cfg);
    nlohmann::json args = {
        {"prompt", "read it"},
        {"attachment", acecode::attachment_to_json(dummy_image_record("att_other"))},
    };
    auto result = tool.execute(args.dump(), ctx_with_active_model("vision-self", true));

    EXPECT_FALSE(result.success);
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["error"], "ACTIVE_MODEL_HAS_VISION");
}

// 触发场景:当前模型能看图,但显式点名了另一个视觉模型。
// 期望行为:放行 —— 显式换模型看是合理意图(想要第二意见),不属于"绕道调用自己"。
TEST(VisionSubagentTool, ExplicitDifferentVisionModelStillWorksWhenActiveModelSees) {
    acecode::AppConfig cfg;
    cfg.saved_models.push_back(model_profile("vision-self", {"vision"}));
    cfg.saved_models.push_back(model_profile("vision-other", {"vision"}));

    auto provider = std::make_shared<CapturingProvider>();
    acecode::ModelProfile selected;
    acecode::VisionSubagentToolOptions opts;
    opts.provider_factory = [&](const acecode::ModelProfile& profile) {
        selected = profile;
        return provider;
    };
    auto tool = acecode::create_vision_analyze_tool(cfg, opts);

    nlohmann::json args = {
        {"prompt", "read it"},
        {"model_name", "vision-other"},
        {"attachment", acecode::attachment_to_json(dummy_image_record("att_explicit_other"))},
    };
    auto result = tool.execute(args.dump(), ctx_with_active_model("vision-self", true));

    EXPECT_TRUE(result.success) << result.output;
    EXPECT_EQ(selected.name, "vision-other");
}

// 触发场景:当前模型能看图,模型显式点名了当前模型自己。
// 期望行为:与隐式路径同样拒绝。两条路径行为必须一致,否则"显式指定"就成了绕过
//   自调用防护的后门。
TEST(VisionSubagentTool, ExplicitlyNamingTheActiveModelIsRejected) {
    acecode::AppConfig cfg;
    cfg.saved_models.push_back(model_profile("vision-self", {"vision"}));
    cfg.saved_models.push_back(model_profile("vision-other", {"vision"}));

    auto tool = acecode::create_vision_analyze_tool(cfg);
    nlohmann::json args = {
        {"prompt", "read it"},
        {"model_name", "vision-self"},
        {"attachment", acecode::attachment_to_json(dummy_image_record("att_self_named"))},
    };
    auto result = tool.execute(args.dump(), ctx_with_active_model("vision-self", true));

    EXPECT_FALSE(result.success);
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["error"], "ACTIVE_MODEL_HAS_VISION");
}

// 触发场景:同一个真实模型在 saved_models 里有两条不同 name 的记录(实测用户
//   config 里 `aurora` 与 `Aurora-aurora` 都指向 openai/aurora),模型未指定
//   model_name。
// 期望行为:两条都被剔除。剔除按 (provider, model id) 而不是按 saved model 的
//   name —— 只比 name 的话,别名那条会活下来,"绕道调用自己"照样成立。
TEST(VisionSubagentTool, AliasEntriesOfTheActiveModelAreAlsoExcluded) {
    acecode::AppConfig cfg;
    acecode::ModelProfile alias = model_profile("vision-self", {"vision"});
    alias.name = "vision-self-alias"; // 同 provider + 同 model id,仅 name 不同
    cfg.saved_models.push_back(model_profile("vision-self", {"vision"}));
    cfg.saved_models.push_back(alias);

    auto tool = acecode::create_vision_analyze_tool(cfg);
    nlohmann::json args = {
        {"prompt", "read it"},
        {"attachment", acecode::attachment_to_json(dummy_image_record("att_alias"))},
    };
    auto result = tool.execute(args.dump(), ctx_with_active_model("vision-self", true));

    EXPECT_FALSE(result.success);
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["error"], "ACTIVE_MODEL_HAS_VISION")
        << "别名条目没被剔除,自调用防护被绕过";
}

// 触发场景:当前模型看不见图(工具存在的本来目的),系统里有一个视觉模型。
// 期望行为:正常委托。自调用防护对这条主用途必须是 no-op。
TEST(VisionSubagentTool, BlindActiveModelStillDelegatesNormally) {
    acecode::AppConfig cfg;
    cfg.saved_models.push_back(model_profile("text-only", {"tool_use"}));
    cfg.saved_models.push_back(model_profile("vision-helper", {"vision"}));

    auto provider = std::make_shared<CapturingProvider>();
    acecode::ModelProfile selected;
    acecode::VisionSubagentToolOptions opts;
    opts.provider_factory = [&](const acecode::ModelProfile& profile) {
        selected = profile;
        return provider;
    };
    auto tool = acecode::create_vision_analyze_tool(cfg, opts);

    nlohmann::json args = {
        {"prompt", "read it"},
        {"attachment", acecode::attachment_to_json(dummy_image_record("att_blind"))},
    };
    auto result = tool.execute(
        args.dump(), ctx_with_active_model("text-only", /*can_read_images=*/false));

    EXPECT_TRUE(result.success) << result.output;
    EXPECT_EQ(selected.name, "vision-helper");
}

// 触发场景:ToolContext 没接线(独立 ToolExecutor 调用、老单测),active_provider_name
//   为空。
// 期望行为:fail-open —— 不剔除任何候选,维持旧行为。漏接线不该让唯一的视觉模型
//   凭空消失,那会把工具在真正需要它的场景下也打死。
TEST(VisionSubagentTool, UnwiredToolContextKeepsLegacyBehavior) {
    acecode::AppConfig cfg;
    cfg.saved_models.push_back(model_profile("vision-only", {"vision"}));

    auto provider = std::make_shared<CapturingProvider>();
    acecode::ModelProfile selected;
    acecode::VisionSubagentToolOptions opts;
    opts.provider_factory = [&](const acecode::ModelProfile& profile) {
        selected = profile;
        return provider;
    };
    auto tool = acecode::create_vision_analyze_tool(cfg, opts);

    nlohmann::json args = {
        {"prompt", "read it"},
        {"attachment", acecode::attachment_to_json(dummy_image_record("att_unwired"))},
    };
    acecode::ToolContext ctx; // 刻意不设 active_provider_name / active_model_id
    auto result = tool.execute(args.dump(), ctx);

    EXPECT_TRUE(result.success) << result.output;
    EXPECT_EQ(selected.name, "vision-only");
}
