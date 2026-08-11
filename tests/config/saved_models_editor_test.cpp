// tests/config/saved_models_editor_test.cpp
//
// 覆盖 src/config/saved_models_editor.cpp。这是 saved_models 增删改的唯一
// 校验入口(daemon HTTP / TUI 命令都过这里),九个错误码每个都有 UI 文案
// 依赖,任一被绕过都会让坏数据进 cfg.json。
//
// 触发场景 / 期望:
//   - 各类无效 draft → 对应错误码 + cfg 不变(回滚验证)
//   - 成功路径:add / update / remove 后 cfg.saved_models 长度与字段正确
//   - update 改名走 delete+add;若 old_name 是 default,同步改 default_model_name
//   - remove default with multiple models → OK,清空 default_model_name
//   - remove the only default model → OK,清空 default_model_name

#include <gtest/gtest.h>

#include "config/saved_models_editor.hpp"

#include "config/config.hpp"
#include "config/saved_models.hpp"

using acecode::AppConfig;
using acecode::add_saved_model;
using acecode::ModelProfile;
using acecode::normalize_model_endpoint_identity;
using acecode::remove_saved_model;
using acecode::SavedModelDraft;
using acecode::SavedModelEditError;
using acecode::update_saved_model;

namespace {

AppConfig make_cfg_with_one_default() {
    AppConfig cfg;
    cfg.provider = "copilot";
    cfg.copilot.model = "gpt-4o";
    ModelProfile a;
    a.name = "copilot-fast";
    a.provider = "copilot";
    a.model = "gpt-4o";
    cfg.saved_models.push_back(a);
    cfg.default_model_name = "copilot-fast";
    return cfg;
}

SavedModelDraft good_openai_draft(const std::string& name = "local-lm") {
    SavedModelDraft d;
    d.name = name;
    d.provider = "openai";
    d.model = "llama-3";
    d.base_url = "http://localhost:1234/v1";
    d.api_key = "sk-x";
    return d;
}

SavedModelDraft good_anthropic_draft(const std::string& name = "claude") {
    SavedModelDraft d;
    d.name = name;
    d.provider = "anthropic";
    d.model = "claude-test";
    d.base_url = "https://api.anthropic.com/v1";
    d.api_key = "sk-ant-test";
    return d;
}

} // namespace

// 场景:add 一个空名字 → INVALID_NAME,cfg 不变。
TEST(SavedModelsEditor, AddRejectsEmptyName) {
    auto cfg = make_cfg_with_one_default();
    auto before = cfg.saved_models.size();
    SavedModelDraft d = good_openai_draft("");
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::INVALID_NAME);
    EXPECT_EQ(cfg.saved_models.size(), before);
}

// 场景:add 名字以 ( 开头 → RESERVED_NAME。前缀 ( 给 (session:...) 留。
TEST(SavedModelsEditor, AddRejectsReservedNamePrefix) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft("(my-fav)");
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::RESERVED_NAME);
}

// 场景:add 重名 → NAME_TAKEN。
TEST(SavedModelsEditor, AddRejectsNameTaken) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft("copilot-fast");  // 已存在
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::NAME_TAKEN);
}

// 场景:add 未知 provider → UNKNOWN_PROVIDER。
TEST(SavedModelsEditor, AddRejectsUnknownProvider) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft();
    d.provider = "unknown";
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::UNKNOWN_PROVIDER);
}

// 场景:add openai 但 model 空 → MISSING_MODEL。
TEST(SavedModelsEditor, AddRejectsMissingModel) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft();
    d.model = "";
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::MISSING_MODEL);
}

// 场景:add openai 但 base_url 空 → MISSING_BASE_URL。
TEST(SavedModelsEditor, AddRejectsMissingBaseUrl) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft();
    d.base_url = "";
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::MISSING_BASE_URL);
}

// 场景:add openai 但 api_key 空 → INVALID_API_KEY(沿用 validate_saved_models 规则)。
TEST(SavedModelsEditor, AddRejectsEmptyApiKey) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft();
    d.api_key = "";
    d.base_url = "https://remote.example/v1";
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::INVALID_API_KEY);
}

// 场景:add 成功 → cfg.saved_models 增 1,字段正确。
TEST(SavedModelsEditor, AddAppendsValidDraft) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft("local-lm");
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::OK);
    ASSERT_EQ(cfg.saved_models.size(), 2u);
    EXPECT_EQ(cfg.saved_models[1].name, "local-lm");
    EXPECT_EQ(cfg.saved_models[1].base_url, "http://localhost:1234/v1");
    EXPECT_EQ(cfg.saved_models[1].api_key, "sk-x");
}

// 场景:add Anthropic 成功 → 保存 base_url/api_key/request_headers。
TEST(SavedModelsEditor, AddAppendsValidAnthropicDraft) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_anthropic_draft("claude");
    d.request_headers = {{"anthropic-beta", "prompt-caching-2024-07-31"}};
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::OK);
    ASSERT_EQ(cfg.saved_models.size(), 2u);
    EXPECT_EQ(cfg.saved_models[1].provider, "anthropic");
    EXPECT_EQ(cfg.saved_models[1].base_url, "https://api.anthropic.com/v1");
    EXPECT_EQ(cfg.saved_models[1].api_key, "sk-ant-test");
    EXPECT_EQ(cfg.saved_models[1].request_headers.at("anthropic-beta"),
              "prompt-caching-2024-07-31");
}

// 场景:add 带 context_window → profile 保存该手动上下文窗口。
TEST(SavedModelsEditor, AddStoresContextWindowOverride) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft("local-lm");
    d.context_window = 64000;
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::OK);
    ASSERT_EQ(cfg.saved_models.size(), 2u);
    ASSERT_TRUE(cfg.saved_models[1].context_window.has_value());
    EXPECT_EQ(*cfg.saved_models[1].context_window, 64000);
}

// 场景:add 带 capabilities → profile 保存能力标签。
TEST(SavedModelsEditor, AddStoresCapabilities) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft("vision-lm");
    d.capabilities = {"vision", "tool_use"};
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::OK);
    ASSERT_EQ(cfg.saved_models.size(), 2u);
    EXPECT_EQ(cfg.saved_models[1].capabilities,
              (std::vector<std::string>{"vision", "tool_use"}));
}

// 场景:add 带 request_headers → profile 保存模板,不解析环境变量。
TEST(SavedModelsEditor, AddStoresRequestHeaders) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft("gateway-lm");
    d.request_headers = {
        {"Authorization", "Bearer {env:ACE_TOKEN}"},
        {"X-Team", "acecode"}
    };
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::OK);
    ASSERT_EQ(cfg.saved_models.size(), 2u);
    EXPECT_EQ(cfg.saved_models[1].request_headers.at("Authorization"),
              "Bearer {env:ACE_TOKEN}");
    EXPECT_EQ(cfg.saved_models[1].request_headers.at("X-Team"), "acecode");
}

// 场景:add copilot 带 request_headers → INVALID_REQUEST_HEADER,cfg 不变。
TEST(SavedModelsEditor, AddRejectsRequestHeadersOnCopilot) {
    auto cfg = make_cfg_with_one_default();
    SavedModelDraft d;
    d.name = "copilot-with-headers";
    d.provider = "copilot";
    d.model = "gpt-4o";
    d.request_headers = {{"X-Team", "acecode"}};
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::INVALID_REQUEST_HEADER);
    EXPECT_EQ(cfg.saved_models.size(), 1u);
}

// 场景:add request_headers 非法 header 名/保留字段 → INVALID_REQUEST_HEADER。
TEST(SavedModelsEditor, AddRejectsInvalidRequestHeader) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft("gateway-lm");
    d.request_headers = {{"Content-Type", "text/plain"}};
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::INVALID_REQUEST_HEADER);
    EXPECT_EQ(cfg.saved_models.size(), 1u);
}

// 场景:add context_window 为负数 → INVALID_CONTEXT_WINDOW,cfg 不变。
TEST(SavedModelsEditor, AddRejectsNegativeContextWindow) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft("local-lm");
    d.context_window = -1;
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::INVALID_CONTEXT_WINDOW);
    EXPECT_EQ(cfg.saved_models.size(), 1u);
}

// 场景:add capabilities 重复 → INVALID_CAPABILITY,cfg 不变。
TEST(SavedModelsEditor, AddRejectsDuplicateCapabilities) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft("vision-lm");
    d.capabilities = {"vision", "vision"};
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::INVALID_CAPABILITY);
    EXPECT_EQ(cfg.saved_models.size(), 1u);
}

// 场景:add codex 被屏蔽,不能新增到可选 saved_models。
TEST(SavedModelsEditor, AddRejectsDisabledCodexProvider) {
    auto cfg = make_cfg_with_one_default();
    SavedModelDraft d;
    d.name = "codex";
    d.provider = "codex";
    d.model = "gpt-5.5";
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::PROVIDER_DISABLED);
    EXPECT_EQ(cfg.saved_models.size(), 1u);
}

// 场景:update 也不能把现有 entry 切到 codex。
TEST(SavedModelsEditor, UpdateRejectsSwitchToDisabledCodexProvider) {
    auto cfg = make_cfg_with_one_default();
    add_saved_model(cfg, good_openai_draft("local-lm"));

    SavedModelDraft d;
    d.name = "local-lm";
    d.provider = "codex";
    d.model = "gpt-5.5";
    EXPECT_EQ(update_saved_model(cfg, "local-lm", d),
              SavedModelEditError::PROVIDER_DISABLED);
    EXPECT_EQ(cfg.saved_models[1].provider, "openai");
}

// 场景:update 不存在的 name → NOT_FOUND,cfg 不变。
TEST(SavedModelsEditor, UpdateRejectsNotFound) {
    auto cfg = make_cfg_with_one_default();
    auto before = cfg.saved_models;
    auto d = good_openai_draft("nope");
    EXPECT_EQ(update_saved_model(cfg, "nope", d), SavedModelEditError::NOT_FOUND);
    EXPECT_EQ(cfg.saved_models.size(), before.size());
}

// 场景:update 改名时旧名是默认 → OK,default_model_name 同步指向新名。
TEST(SavedModelsEditor, UpdateRenamesDefaultAndUpdatesDefaultName) {
    auto cfg = make_cfg_with_one_default();  // default = copilot-fast
    auto d = good_openai_draft("copilot-fast-v2");
    d.provider = "copilot";
    d.base_url.clear();
    d.api_key.clear();
    EXPECT_EQ(update_saved_model(cfg, "copilot-fast", d),
              SavedModelEditError::OK);
    EXPECT_EQ(cfg.saved_models[0].name, "copilot-fast-v2");
    EXPECT_EQ(cfg.default_model_name, "copilot-fast-v2");
}

// 场景:update 同名改字段(非 default 条目) → 字段被替换。
TEST(SavedModelsEditor, UpdateReplacesFieldsForSameName) {
    auto cfg = make_cfg_with_one_default();
    add_saved_model(cfg, good_openai_draft("local-lm"));
    SavedModelDraft d = good_openai_draft("local-lm");
    d.api_key = "sk-new";
    EXPECT_EQ(update_saved_model(cfg, "local-lm", d), SavedModelEditError::OK);
    EXPECT_EQ(cfg.saved_models[1].api_key, "sk-new");
}

// 场景:update context_window=0 清除已有手动 override。
TEST(SavedModelsEditor, UpdateCanClearContextWindowOverride) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft("local-lm");
    d.context_window = 64000;
    add_saved_model(cfg, d);

    SavedModelDraft updated = good_openai_draft("local-lm");
    updated.context_window = 0;
    EXPECT_EQ(update_saved_model(cfg, "local-lm", updated), SavedModelEditError::OK);
    EXPECT_FALSE(cfg.saved_models[1].context_window.has_value());
}

// 场景:update capabilities 为空数组时清除旧能力标签。
TEST(SavedModelsEditor, UpdateCanClearCapabilities) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft("vision-lm");
    d.capabilities = {"vision", "tool_use"};
    add_saved_model(cfg, d);

    SavedModelDraft updated = good_openai_draft("vision-lm");
    updated.capabilities_supplied = true;
    updated.capabilities = {};
    EXPECT_EQ(update_saved_model(cfg, "vision-lm", updated), SavedModelEditError::OK);
    EXPECT_TRUE(cfg.saved_models[1].capabilities.empty());
}

// 场景:update request_headers 为空 map 时清除旧请求头模板。
TEST(SavedModelsEditor, UpdateCanClearRequestHeaders) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft("gateway-lm");
    d.request_headers = {{"X-Team", "acecode"}};
    add_saved_model(cfg, d);

    SavedModelDraft updated = good_openai_draft("gateway-lm");
    updated.request_headers_supplied = true;
    updated.request_headers = {};
    EXPECT_EQ(update_saved_model(cfg, "gateway-lm", updated), SavedModelEditError::OK);
    EXPECT_TRUE(cfg.saved_models[1].request_headers.empty());
}

// 场景:remove 唯一默认条目 → OK,清空 default,允许回到零模型。
TEST(SavedModelsEditor, RemoveOnlyDefaultClearsDefault) {
    auto cfg = make_cfg_with_one_default();
    EXPECT_EQ(remove_saved_model(cfg, "copilot-fast"),
              SavedModelEditError::OK);
    EXPECT_TRUE(cfg.saved_models.empty());
    EXPECT_TRUE(cfg.default_model_name.empty());
}

// 场景:remove 多模型里的默认条目 → OK,删除条目并清空 default。
TEST(SavedModelsEditor, RemoveDefaultWhenOtherModelsExistClearsDefault) {
    auto cfg = make_cfg_with_one_default();
    add_saved_model(cfg, good_openai_draft("local-lm"));
    EXPECT_EQ(remove_saved_model(cfg, "copilot-fast"),
              SavedModelEditError::OK);
    ASSERT_EQ(cfg.saved_models.size(), 1u);
    EXPECT_EQ(cfg.saved_models[0].name, "local-lm");
    EXPECT_TRUE(cfg.default_model_name.empty());
}

// 场景:remove 非默认 → 长度 -1。
TEST(SavedModelsEditor, RemoveDeletesNonDefault) {
    auto cfg = make_cfg_with_one_default();
    add_saved_model(cfg, good_openai_draft("local-lm"));
    EXPECT_EQ(remove_saved_model(cfg, "local-lm"), SavedModelEditError::OK);
    EXPECT_EQ(cfg.saved_models.size(), 1u);
    EXPECT_EQ(cfg.saved_models[0].name, "copilot-fast");
}

// 场景:remove 不存在 → NOT_FOUND。
TEST(SavedModelsEditor, RemoveRejectsNotFound) {
    auto cfg = make_cfg_with_one_default();
    EXPECT_EQ(remove_saved_model(cfg, "ghost"), SavedModelEditError::NOT_FOUND);
}

// 场景:update 改名时新名已被占用 → NAME_TAKEN,cfg 不变。
// 触发:用户编辑面板里把 A 重命名为 B,但 B 早就存在。
TEST(SavedModelsEditor, UpdateRejectsRenameToTakenName) {
    auto cfg = make_cfg_with_one_default();
    add_saved_model(cfg, good_openai_draft("local-lm"));
    add_saved_model(cfg, good_openai_draft("other-lm"));
    auto before_size = cfg.saved_models.size();

    SavedModelDraft d = good_openai_draft("other-lm");
    EXPECT_EQ(update_saved_model(cfg, "local-lm", d), SavedModelEditError::NAME_TAKEN);
    EXPECT_EQ(cfg.saved_models.size(), before_size);
    // local-lm 与 other-lm 都还在
    EXPECT_EQ(cfg.saved_models[1].name, "local-lm");
    EXPECT_EQ(cfg.saved_models[2].name, "other-lm");
}

// 场景:update 改名(非 default 条目)→ OK,旧 name 被新 name 替换,
// 字段可同时改。
TEST(SavedModelsEditor, UpdateRenamesNonDefaultEntry) {
    auto cfg = make_cfg_with_one_default();
    add_saved_model(cfg, good_openai_draft("local-lm"));

    SavedModelDraft d = good_openai_draft("local-lm-v2");
    d.api_key = "sk-renamed";
    EXPECT_EQ(update_saved_model(cfg, "local-lm", d), SavedModelEditError::OK);

    // 旧 name 不在了,新 name 在,api_key 也跟着改了。
    bool found_old = false, found_new = false;
    for (const auto& e : cfg.saved_models) {
        if (e.name == "local-lm") found_old = true;
        if (e.name == "local-lm-v2") {
            found_new = true;
            EXPECT_EQ(e.api_key, "sk-renamed");
        }
    }
    EXPECT_FALSE(found_old);
    EXPECT_TRUE(found_new);
}

// 场景:add 在每条拒绝分支上都不改 cfg.saved_models — 这是 spec 的回滚保证,
// 任一分支偷偷写就会把坏数据漏到 cfg.json。覆盖六条剩余的拒绝分支
// (INVALID_NAME 已由 AddRejectsEmptyName 覆盖)。
TEST(SavedModelsEditor, AddLeavesCfgUnchangedOnAllRejections) {
    auto cfg = make_cfg_with_one_default();
    auto baseline_size = cfg.saved_models.size();

    auto try_reject = [&](SavedModelDraft d, SavedModelEditError expected) {
        EXPECT_EQ(add_saved_model(cfg, d), expected);
        EXPECT_EQ(cfg.saved_models.size(), baseline_size);
    };

    auto reserved = good_openai_draft("(taken)");
    try_reject(reserved, SavedModelEditError::RESERVED_NAME);

    auto taken = good_openai_draft("copilot-fast");
    try_reject(taken, SavedModelEditError::NAME_TAKEN);

    auto bad_provider = good_openai_draft();
    bad_provider.provider = "unknown";
    try_reject(bad_provider, SavedModelEditError::UNKNOWN_PROVIDER);

    auto disabled_provider = good_openai_draft();
    disabled_provider.provider = "codex";
    try_reject(disabled_provider, SavedModelEditError::PROVIDER_DISABLED);

    auto no_model = good_openai_draft();
    no_model.model = "";
    try_reject(no_model, SavedModelEditError::MISSING_MODEL);

    auto no_url = good_openai_draft();
    no_url.base_url = "";
    try_reject(no_url, SavedModelEditError::MISSING_BASE_URL);

    auto no_key = good_openai_draft();
    no_key.api_key = "";
    no_key.base_url = "https://remote.example/v1";
    try_reject(no_key, SavedModelEditError::INVALID_API_KEY);

    auto bad_context = good_openai_draft();
    bad_context.context_window = -1;
    try_reject(bad_context, SavedModelEditError::INVALID_CONTEXT_WINDOW);

    auto bad_headers = good_openai_draft();
    bad_headers.request_headers = {{"Content-Type", "text/plain"}};
    try_reject(bad_headers, SavedModelEditError::INVALID_REQUEST_HEADER);
}

// 场景:legacy readonly=true 只是外部登录器留下的兼容标记,不阻止正常更新。
TEST(SavedModelsEditor, UpdateAllowsLegacyReadonlyModel) {
    AppConfig cfg;
    ModelProfile locked;
    locked.name = "locked";
    locked.provider = "openai";
    locked.model = "locked-model";
    locked.base_url = "https://models.example.com/v1";
    locked.api_key = "k";
    locked.readonly = true;
    cfg.saved_models.push_back(locked);

    SavedModelDraft d;
    d.name = "locked";
    d.provider = "openai";
    d.model = "changed-model";
    d.base_url = "https://models.example.com/v1";
    d.api_key = "new-k";
    EXPECT_EQ(update_saved_model(cfg, "locked", d), SavedModelEditError::OK);
    EXPECT_EQ(cfg.saved_models[0].model, "changed-model");
    EXPECT_EQ(cfg.saved_models[0].api_key, "new-k");
    EXPECT_FALSE(cfg.saved_models[0].readonly);
}

// 场景:remove legacy readonly=true 的条目 → OK,条目被删除。
TEST(SavedModelsEditor, RemoveAllowsReadonlyModel) {
    AppConfig cfg;
    ModelProfile locked;
    locked.name = "locked";
    locked.provider = "openai";
    locked.model = "locked-model";
    locked.base_url = "https://models.example.com/v1";
    locked.api_key = "k";
    locked.readonly = true;
    cfg.saved_models.push_back(locked);

    EXPECT_EQ(remove_saved_model(cfg, "locked"), SavedModelEditError::OK);
    EXPECT_TRUE(cfg.saved_models.empty());
}

// 新合同:更新省略 api_key 时保留旧值；提供非空值才替换。
TEST(SavedModelsEditor, UpdatePreservesOrReplacesCredentialExplicitly) {
    AppConfig cfg;
    ModelProfile existing;
    existing.name = "remote";
    existing.provider = "openai";
    existing.model = "old-model";
    existing.base_url = "https://models.example/v1";
    existing.api_key = "old-secret";
    cfg.saved_models.push_back(existing);

    SavedModelDraft preserve;
    preserve.name = "remote";
    preserve.provider = "openai";
    preserve.model = "new-model";
    EXPECT_EQ(update_saved_model(cfg, "remote", preserve),
              SavedModelEditError::OK);
    EXPECT_EQ(cfg.saved_models[0].api_key, "old-secret");
    EXPECT_EQ(cfg.saved_models[0].base_url, "https://models.example/v1");

    preserve.api_key = "replacement-secret";
    preserve.api_key_supplied = true;
    EXPECT_EQ(update_saved_model(cfg, "remote", preserve),
              SavedModelEditError::OK);
    EXPECT_EQ(cfg.saved_models[0].api_key, "replacement-secret");
}

// 新合同:本地无认证端点可明确清除；远端必需认证且失败不改配置。
TEST(SavedModelsEditor, ClearCredentialHonorsAuthenticationPolicyAtomically) {
    AppConfig local_cfg;
    ModelProfile local;
    local.name = "local";
    local.provider = "openai";
    local.model = "local-model";
    local.base_url = "http://LOCALHOST:1234/v1/";
    local.api_key = "optional-secret";
    local_cfg.saved_models.push_back(local);

    SavedModelDraft clear_local;
    clear_local.name = "local";
    clear_local.provider = "openai";
    clear_local.model = "local-model";
    clear_local.clear_api_key = true;
    EXPECT_EQ(update_saved_model(local_cfg, "local", clear_local),
              SavedModelEditError::OK);
    EXPECT_TRUE(local_cfg.saved_models[0].api_key.empty());

    AppConfig remote_cfg;
    ModelProfile remote = local;
    remote.name = "remote";
    remote.base_url = "https://models.example/v1";
    remote.api_key = "must-stay";
    remote_cfg.saved_models.push_back(remote);
    SavedModelDraft clear_remote;
    clear_remote.name = "remote";
    clear_remote.provider = "openai";
    clear_remote.model = "local-model";
    clear_remote.clear_api_key = true;
    EXPECT_EQ(update_saved_model(remote_cfg, "remote", clear_remote),
              SavedModelEditError::INVALID_API_KEY);
    EXPECT_EQ(remote_cfg.saved_models[0].api_key, "must-stay");
}

// 凭据复用只在 Provider、规范化端点和 models.dev Provider ID 全相同时发生。
TEST(SavedModelsEditor, CredentialReuseRequiresExactCompatibleIdentity) {
    AppConfig cfg;
    ModelProfile source;
    source.name = "openrouter-source";
    source.provider = "openai";
    source.model = "source-model";
    source.base_url = "HTTPS://OPENROUTER.AI/api/v1/";
    source.api_key = "daemon-only-secret";
    source.models_dev_provider_id = "openrouter";
    cfg.saved_models.push_back(source);

    SavedModelDraft compatible;
    compatible.name = "openrouter-new";
    compatible.provider = "openai";
    compatible.model = "new-model";
    compatible.base_url = "https://openrouter.ai/api/v1";
    compatible.models_dev_provider_id = "openrouter";
    compatible.credential_source_name = "openrouter-source";
    EXPECT_EQ(add_saved_model(cfg, compatible), SavedModelEditError::OK);
    ASSERT_EQ(cfg.saved_models.size(), 2u);
    EXPECT_EQ(cfg.saved_models[1].api_key, "daemon-only-secret");

    SavedModelDraft mismatch = compatible;
    mismatch.name = "other-provider";
    mismatch.models_dev_provider_id = "other";
    EXPECT_EQ(add_saved_model(cfg, mismatch),
              SavedModelEditError::INVALID_CREDENTIAL_SOURCE);
    EXPECT_EQ(cfg.saved_models.size(), 2u);
    EXPECT_EQ(std::string(to_string(SavedModelEditError::INVALID_CREDENTIAL_SOURCE))
                  .find("daemon-only-secret"),
              std::string::npos);
}

TEST(SavedModelsEditor, CredentialIdentityNormalizesDefaultPortsButNotPathCase) {
    AppConfig cfg;
    ModelProfile source;
    source.name = "source";
    source.provider = "openai";
    source.model = "one";
    source.base_url = "HTTPS://EXAMPLE.TEST:443/Api/v1/";
    source.api_key = "secret";
    cfg.saved_models.push_back(source);

    SavedModelDraft same;
    same.name = "same";
    same.provider = "openai";
    same.model = "two";
    same.base_url = "https://example.test/Api/v1";
    same.credential_source_name = "source";
    EXPECT_EQ(add_saved_model(cfg, same), SavedModelEditError::OK);

    SavedModelDraft different = same;
    different.name = "different";
    different.base_url = "https://example.test/api/v1";
    EXPECT_EQ(add_saved_model(cfg, different),
              SavedModelEditError::INVALID_CREDENTIAL_SOURCE);
    EXPECT_EQ(cfg.saved_models.size(), 2u);
}

TEST(SavedModelsEditor, CredentialIdentityPreservesQueryAndFragmentCase) {
    EXPECT_EQ(normalize_model_endpoint_identity(
                  " HTTPS://HOST.EXAMPLE:443?Tenant=AbC#Region=UsEast "),
              "https://host.example?Tenant=AbC#Region=UsEast");
    EXPECT_NE(normalize_model_endpoint_identity(
                  "https://host.example?Tenant=AbC#Region=UsEast"),
              normalize_model_endpoint_identity(
                  "https://host.example?Tenant=abc#Region=UsEast"));
    EXPECT_EQ(normalize_model_endpoint_identity(
                  "https://HOST.EXAMPLE:443/Api/?Redirect=/#Region=UsEast/"),
              "https://host.example/Api?Redirect=/#Region=UsEast/");

    AppConfig cfg;
    ModelProfile source;
    source.name = "query-source";
    source.provider = "openai";
    source.model = "one";
    source.base_url = "HTTPS://HOST.EXAMPLE:443?Tenant=AbC#Region=UsEast";
    source.api_key = "secret";
    cfg.saved_models.push_back(source);

    SavedModelDraft same;
    same.name = "same-query";
    same.provider = "openai";
    same.model = "two";
    same.base_url = "https://host.example?Tenant=AbC#Region=UsEast";
    same.credential_source_name = "query-source";
    EXPECT_EQ(add_saved_model(cfg, same), SavedModelEditError::OK);

    SavedModelDraft different = same;
    different.name = "different-query";
    different.base_url = "https://host.example?Tenant=abc#Region=UsEast";
    EXPECT_EQ(add_saved_model(cfg, different),
              SavedModelEditError::INVALID_CREDENTIAL_SOURCE);
    EXPECT_EQ(cfg.saved_models.size(), 2u);
}

// Copilot saved profile 只保存模型身份，继续走受管登录和固定端点。
TEST(SavedModelsEditor, CopilotProfileRejectsCustomConnectionAndReasoning) {
    AppConfig cfg;
    SavedModelDraft copilot;
    copilot.name = "copilot-fast";
    copilot.provider = "copilot";
    copilot.model = "gpt-5";
    EXPECT_EQ(add_saved_model(cfg, copilot), SavedModelEditError::OK);
    ASSERT_EQ(cfg.saved_models.size(), 1u);
    EXPECT_TRUE(cfg.saved_models[0].base_url.empty());
    EXPECT_TRUE(cfg.saved_models[0].api_key.empty());

    auto custom_key = copilot;
    custom_key.name = "copilot-key";
    custom_key.api_key = "forbidden";
    custom_key.api_key_supplied = true;
    EXPECT_EQ(add_saved_model(cfg, custom_key),
              SavedModelEditError::UNSUPPORTED_MODEL_OPTION);

    auto custom_url = copilot;
    custom_url.name = "copilot-url";
    custom_url.base_url = "https://example.test/v1";
    custom_url.base_url_supplied = true;
    EXPECT_EQ(add_saved_model(cfg, custom_url),
              SavedModelEditError::UNSUPPORTED_MODEL_OPTION);

    auto custom_reasoning = copilot;
    custom_reasoning.name = "copilot-reasoning";
    custom_reasoning.reasoning_supplied = true;
    custom_reasoning.reasoning = acecode::ModelReasoningOptions{};
    EXPECT_EQ(add_saved_model(cfg, custom_reasoning),
              SavedModelEditError::UNSUPPORTED_MODEL_OPTION);
    EXPECT_EQ(cfg.saved_models.size(), 1u);
}

// 高级字段更新可以完整保存；无效推理预算不得部分覆盖旧 profile。
TEST(SavedModelsEditor, AdvancedFieldsPersistAndInvalidReasoningIsAtomic) {
    AppConfig cfg;
    SavedModelDraft draft = good_openai_draft("advanced");
    draft.base_url = "https://openrouter.ai/api/v1/";
    draft.models_dev_provider_id = "openrouter";
    draft.endpoint_mode = "base_url";
    draft.max_output_tokens = 65536;
    draft.capabilities = {"tool_use", "reasoning"};
    draft.capabilities_source = "catalog";
    acecode::ModelReasoningOptions reasoning;
    reasoning.supported = true;
    reasoning.default_enabled = true;
    reasoning.enabled = true;
    reasoning.supported_efforts = {"low", "high"};
    reasoning.default_effort = "low";
    reasoning.effort = "high";
    reasoning.supports_max_tokens = true;
    reasoning.max_tokens = 8192;
    draft.reasoning = reasoning;
    EXPECT_EQ(add_saved_model(cfg, draft), SavedModelEditError::OK);
    ASSERT_EQ(cfg.saved_models.size(), 1u);
    EXPECT_EQ(cfg.saved_models[0].base_url,
              "https://openrouter.ai/api/v1");
    EXPECT_EQ(cfg.saved_models[0].max_output_tokens, 65536);
    ASSERT_TRUE(cfg.saved_models[0].reasoning.has_value());
    EXPECT_EQ(cfg.saved_models[0].reasoning->effort, "high");

    SavedModelDraft invalid;
    invalid.name = "advanced";
    invalid.provider = "openai";
    invalid.model = "llama-3";
    invalid.reasoning_supplied = true;
    reasoning.max_tokens = 65536;
    invalid.reasoning = reasoning;
    EXPECT_EQ(update_saved_model(cfg, "advanced", invalid),
              SavedModelEditError::INVALID_REASONING);
    EXPECT_EQ(cfg.saved_models[0].reasoning->max_tokens, 8192);
}
