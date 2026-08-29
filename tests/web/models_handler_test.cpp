// 覆盖 src/web/handlers/models_handler.cpp。前端 model-picker 要靠这里
// 拼出 saved_models 行;一旦回归:
//   - list_models 漏 saved_models 行 → 用户无法切换模型
//   - find_model_by_name 大小写敏感配错 → POST 切换 400

#include <gtest/gtest.h>

#include "web/handlers/models_handler.hpp"

#include "config/config.hpp"
#include "config/saved_models.hpp"

#include <filesystem>
#include <fstream>

using acecode::AppConfig;
using acecode::ModelProfile;
using acecode::SessionModelState;
using acecode::web::find_model_by_name;
using acecode::web::list_models;
using acecode::web::model_state_to_json;

namespace {

// 构造一个最小 cfg,带两个 saved_models 条目。
AppConfig make_cfg_with_two() {
    AppConfig cfg;

    ModelProfile a;
    a.name = "copilot-fast"; a.provider = "copilot"; a.model = "gpt-4o";
    cfg.saved_models.push_back(a);

    ModelProfile b;
    b.name = "local-lm"; b.provider = "openai"; b.model = "llama-3";
    b.base_url = "http://localhost:1234/v1"; b.api_key = "x";
    b.context_window = 64000;
    b.capabilities = {"vision", "tool_use"};
    b.request_headers = {{"X-Team", "acecode"}};
    cfg.saved_models.push_back(b);

    return cfg;
}

} // namespace

// 场景: list_models 输出顺序 = saved_models 顺序。
TEST(ModelsHandler, ListIncludesAllSavedModels) {
    auto cfg = make_cfg_with_two();
    auto arr = list_models(cfg);
    ASSERT_TRUE(arr.is_array());
    ASSERT_EQ(arr.size(), 2u);

    EXPECT_EQ(arr[0]["name"], "copilot-fast");
    EXPECT_EQ(arr[1]["name"], "local-lm");
    EXPECT_TRUE(arr[1].contains("base_url"));
    EXPECT_EQ(arr[1]["api_key"], "x");
    EXPECT_EQ(arr[1]["has_api_key"], true);
    EXPECT_EQ(arr[1]["context_window"], 64000);
    EXPECT_EQ(arr[1]["capabilities"], nlohmann::json::array({"vision", "tool_use"}));
    EXPECT_EQ(arr[1]["request_headers"]["X-Team"], "acecode");
}

// 场景:Anthropic 是可运行 provider,Web 模型列表必须暴露完整配置字段。
TEST(ModelsHandler, ListAndFindIncludeAnthropicProvider) {
    auto cfg = make_cfg_with_two();
    ModelProfile c;
    c.name = "claude";
    c.provider = "anthropic";
    c.model = "claude-test";
    c.base_url = "https://api.anthropic.com/v1";
    c.api_key = "sk-ant-test";
    c.request_headers = {{"anthropic-beta", "prompt-caching-2024-07-31"}};
    cfg.saved_models.push_back(c);

    auto arr = list_models(cfg);
    ASSERT_EQ(arr.size(), 3u);
    EXPECT_EQ(arr[2]["name"], "claude");
    EXPECT_EQ(arr[2]["provider"], "anthropic");
    EXPECT_EQ(arr[2]["base_url"], "https://api.anthropic.com/v1");
    EXPECT_EQ(arr[2]["api_key"], "sk-ant-test");
    EXPECT_EQ(arr[2]["has_api_key"], true);
    EXPECT_EQ(arr[2]["request_headers"]["anthropic-beta"],
              "prompt-caching-2024-07-31");

    auto found = find_model_by_name(cfg, "claude");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->provider, "anthropic");
}

// 场景:旧配置里遗留 codex saved model 时,Web 模型列表不暴露已屏蔽 provider。
TEST(ModelsHandler, ListAndFindSkipDisabledCodexProvider) {
    auto cfg = make_cfg_with_two();
    ModelProfile c;
    c.name = "codex";
    c.provider = "codex";
    c.model = "gpt-5.5";
    cfg.saved_models.push_back(c);

    auto arr = list_models(cfg);
    ASSERT_EQ(arr.size(), 2u);
    EXPECT_FALSE(find_model_by_name(cfg, "codex").has_value());
}

// 场景: 空 saved_models 时,list_models 返回空数组。
TEST(ModelsHandler, ListEmptySavedReturnsEmptyArray) {
    AppConfig cfg;
    auto arr = list_models(cfg);
    ASSERT_TRUE(arr.is_array());
    EXPECT_TRUE(arr.empty());
}

// 场景: find_model_by_name 命中 saved_models 条目。
TEST(ModelsHandler, FindBySavedName) {
    auto cfg = make_cfg_with_two();
    auto e = find_model_by_name(cfg, "local-lm");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->provider, "openai");
    EXPECT_EQ(e->model, "llama-3");
}

// 场景: 未命中 → nullopt。Caller 转 400。
TEST(ModelsHandler, FindUnknownReturnsNullopt) {
    auto cfg = make_cfg_with_two();
    EXPECT_FALSE(find_model_by_name(cfg, "nonexistent").has_value());
    EXPECT_FALSE(find_model_by_name(cfg, "").has_value());
}

// 场景: name 大小写敏感 — TUI /model 也是大小写敏感,必须保持一致。
TEST(ModelsHandler, FindIsCaseSensitive) {
    auto cfg = make_cfg_with_two();
    EXPECT_TRUE(find_model_by_name(cfg, "copilot-fast").has_value());
    EXPECT_FALSE(find_model_by_name(cfg, "COPILOT-FAST").has_value());
    EXPECT_FALSE(find_model_by_name(cfg, "Copilot-Fast").has_value());
}

// 场景: current session model state 序列化必须包含前端 footer/selector
// 需要的完整字段。
TEST(ModelsHandler, ModelStateToJsonIncludesCurrentSessionFields) {
    SessionModelState state;
    state.name = "copilot-fast";
    state.provider = "copilot";
    state.model = "gpt-5";
    state.context_window = 400000;

    auto j = model_state_to_json(state);
    EXPECT_EQ(j["name"], "copilot-fast");
    EXPECT_EQ(j["provider"], "copilot");
    EXPECT_EQ(j["model"], "gpt-5");
    EXPECT_EQ(j["context_window"], 400000);
    EXPECT_EQ(j["deleted"], false);
}

// 场景: no-model session 也要序列化为空字段,前端用它显示“未配置模型”。
TEST(ModelsHandler, ModelStateToJsonAllowsEmptyModelState) {
    SessionModelState state;

    auto j = model_state_to_json(state);
    EXPECT_EQ(j["name"], "");
    EXPECT_EQ(j["provider"], "");
    EXPECT_EQ(j["model"], "");
    EXPECT_EQ(j["context_window"], 0);
    EXPECT_EQ(j["deleted"], false);
}

// 场景: saved_models.name 已被删除的 session 仍要把悬空 name 返回给
// Desktop/Web,并显式标记 deleted 让 UI 显示红色 "(deleted)"。
TEST(ModelsHandler, ModelStateToJsonIncludesDeletedFlag) {
    SessionModelState state;
    state.name = "fast";
    state.deleted = true;

    auto j = model_state_to_json(state);
    EXPECT_EQ(j["name"], "fast");
    EXPECT_EQ(j["provider"], "");
    EXPECT_EQ(j["model"], "");
    EXPECT_EQ(j["context_window"], 0);
    EXPECT_EQ(j["deleted"], true);
}

// ------------------- 增删改 helper 测试 -------------------

#include "config/saved_models_editor.hpp"

using acecode::SavedModelDraft;
using acecode::SavedModelEditError;
using acecode::web::apply_acemodel_context_fallbacks;
using acecode::web::http_status_for_edit_error;
using acecode::web::model_probe_connection_fingerprint;
using acecode::web::ModelProbeRequest;
using acecode::web::parse_model_probe_request;
using acecode::web::parse_openai_model_ids;
using acecode::web::parse_openai_models;
using acecode::web::parse_model_draft;
using acecode::web::profile_to_json;

// 触发场景:server.cpp 把 saved_models_editor 的错误码翻成 HTTP 状态;
// 这层映射与前端 toast 文案强相关 — NOT_FOUND→404 / NAME_TAKEN→409 /
// IN_USE_AS_DEFAULT→409,任一改错都会让"删默认"这种 UX 走偏(本来要 409
// 提示先改默认,变成 500 用户不知道为啥)。
// 期望行为:固定的状态码映射,新增其它枚举值默认 fallback 400(校验失败)。
TEST(ModelsHandler, ErrorToHttpStatusMapping) {
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::NOT_FOUND), 404);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::NAME_TAKEN), 409);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::IN_USE_AS_DEFAULT), 409);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::INVALID_NAME), 400);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::INVALID_API_KEY), 400);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::RESERVED_NAME), 400);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::UNKNOWN_PROVIDER), 400);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::PROVIDER_DISABLED), 400);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::MISSING_MODEL), 400);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::MISSING_BASE_URL), 400);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::INVALID_CONTEXT_WINDOW), 400);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::INVALID_CAPABILITY), 400);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::INVALID_REQUEST_HEADER), 400);
}

// 触发场景:POST/PUT 成功后把 ModelProfile 序列化回模型管理界面。
// 期望行为:经过认证的响应返回原密钥，供编辑弹窗默认遮罩回填。
TEST(ModelsHandler, ProfileToJsonIncludesApiKey) {
    ModelProfile p;
    p.name = "local";
    p.provider = "openai";
    p.model = "llama-3";
    p.base_url = "http://localhost/v1";
    p.api_key = "sk-secret";
    auto j = profile_to_json(p);
    EXPECT_EQ(j["api_key"], "sk-secret");
    EXPECT_EQ(j["has_api_key"], true);
    EXPECT_EQ(j["base_url"], "http://localhost/v1");
    EXPECT_EQ(j["name"], "local");
}

// 触发场景:成功响应里允许返回非敏感的 context_window override。
TEST(ModelsHandler, ProfileToJsonIncludesContextWindow) {
    ModelProfile p;
    p.name = "local";
    p.provider = "openai";
    p.model = "llama-3";
    p.base_url = "http://localhost/v1";
    p.api_key = "sk-secret";
    p.context_window = 96000;
    auto j = profile_to_json(p);
    EXPECT_EQ(j["context_window"], 96000);
}

// 触发场景:成功响应里允许返回非敏感的 capabilities。
TEST(ModelsHandler, ProfileToJsonIncludesCapabilities) {
    ModelProfile p;
    p.name = "vision";
    p.provider = "openai";
    p.model = "llava";
    p.base_url = "http://localhost/v1";
    p.api_key = "sk-secret";
    p.capabilities = {"vision", "tool_use"};
    auto j = profile_to_json(p);
    EXPECT_EQ(j["capabilities"], nlohmann::json::array({"vision", "tool_use"}));
    EXPECT_EQ(j["api_key"], "sk-secret");
    EXPECT_EQ(j["has_api_key"], true);
}

// 触发场景:request_headers 是可编辑模板,响应可返回模板但不能解析环境变量。
TEST(ModelsHandler, ProfileToJsonIncludesRequestHeaders) {
    ModelProfile p;
    p.name = "gateway";
    p.provider = "openai";
    p.model = "llama-3";
    p.base_url = "http://localhost/v1";
    p.api_key = "sk-secret";
    p.request_headers = {
        {"Authorization", "Bearer {env:ACE_TOKEN}"},
        {"X-Team", "acecode"}
    };
    auto j = profile_to_json(p);
    EXPECT_EQ(j["request_headers"]["Authorization"], "Bearer {env:ACE_TOKEN}");
    EXPECT_EQ(j["request_headers"]["X-Team"], "acecode");
    EXPECT_EQ(j["api_key"], "sk-secret");
    EXPECT_EQ(j["has_api_key"], true);
}

// 触发场景:前端 POST /api/models 漏字段时,后端要给出明确的字段名,
// 否则前端只能笼统报"提交失败"。
// 期望行为:漏 provider 字段 → err 字符串里出现 "provider";返回 nullopt。
TEST(ModelsHandler, ParseDraftReportsMissingField) {
    nlohmann::json body = {{"name", "x"}};
    std::string err;
    auto d = parse_model_draft(body, err);
    EXPECT_FALSE(d.has_value());
    EXPECT_NE(err.find("provider"), std::string::npos);
}

// 密钥更新语义依赖“省略”和“提供”可区分；错误类型不能被静默当成省略。
TEST(ModelsHandler, ParseDraftRejectsWrongCredentialTypes) {
    nlohmann::json body = {
        {"name", "lm"},
        {"provider", "copilot"},
        {"model", "gpt-4o"},
        {"base_url", "https://example.test/v1"},
        {"api_key", 42},         // 非字符串
    };
    std::string err;
    auto d = parse_model_draft(body, err);
    EXPECT_FALSE(d.has_value());
    EXPECT_NE(err.find("api_key"), std::string::npos);
}

std::filesystem::path repo_root() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

nlohmann::json model_mutation_fixture() {
    std::ifstream input(repo_root() / "tests" / "fixtures" /
                        "model_mutation_contract.json");
    if (!input) return nullptr;
    return nlohmann::json::parse(input);
}

// 触发场景:必填字段是 null/非字符串(比如前端误把 name 写成数字)。这种
// 情况和可选字段不一样 —— 必须明确报错给前端,提示哪个字段类型错了。
// 期望行为:err 中应出现 "name",且 parse 返回 nullopt;否则前端会以为
// 提交成功但拿到一个 name 为空的 draft。
TEST(ModelsHandler, ParseDraftRejectsNonStringRequiredField) {
    nlohmann::json body = {
        {"name", 42},  // 非 string,name 是必填
        {"provider", "copilot"},
        {"model", "gpt-4o"},
    };
    std::string err;
    auto d = parse_model_draft(body, err);
    EXPECT_FALSE(d.has_value());
    EXPECT_NE(err.find("name"), std::string::npos);
}

// 触发场景:前端 POST /api/models body 完整(含 openai 必填的 base_url +
// api_key)。
// 期望行为:全部字段就绪,返回的 SavedModelDraft 字段值与输入一致;
// err 留空。
TEST(ModelsHandler, ParseDraftAcceptsFullBody) {
    nlohmann::json body = {
        {"name", "local"},
        {"provider", "openai"},
        {"model", "llama-3"},
        {"base_url", "http://localhost/v1"},
        {"api_key", "sk-x"},
        {"context_window", 64000},
        {"capabilities", nlohmann::json::array({"vision", "tool_use"})},
        {"request_headers", {
            {"Authorization", "Bearer {env:ACE_TOKEN}"},
            {"X-Team", "acecode"}
        }},
    };
    std::string err;
    auto d = parse_model_draft(body, err);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->name, "local");
    EXPECT_EQ(d->provider, "openai");
    EXPECT_EQ(d->model, "llama-3");
    EXPECT_EQ(d->base_url, "http://localhost/v1");
    EXPECT_EQ(d->api_key, "sk-x");
    ASSERT_TRUE(d->context_window.has_value());
    EXPECT_EQ(*d->context_window, 64000);
    EXPECT_EQ(d->capabilities, (std::vector<std::string>{"vision", "tool_use"}));
    EXPECT_EQ(d->request_headers.at("Authorization"), "Bearer {env:ACE_TOKEN}");
    EXPECT_EQ(d->request_headers.at("X-Team"), "acecode");
    EXPECT_TRUE(err.empty());
}

// 触发场景:Desktop/Web 保存 Anthropic profile 时走同一个 parse_model_draft。
TEST(ModelsHandler, ParseDraftAcceptsAnthropicBody) {
    nlohmann::json body = {
        {"name", "claude"},
        {"provider", "anthropic"},
        {"model", "claude-test"},
        {"base_url", "https://api.anthropic.com/v1"},
        {"api_key", "sk-ant-x"},
        {"request_headers", {
            {"anthropic-beta", "prompt-caching-2024-07-31"}
        }},
    };
    std::string err;
    auto d = parse_model_draft(body, err);
    ASSERT_TRUE(d.has_value()) << err;
    EXPECT_EQ(d->name, "claude");
    EXPECT_EQ(d->provider, "anthropic");
    EXPECT_EQ(d->base_url, "https://api.anthropic.com/v1");
    EXPECT_EQ(d->api_key, "sk-ant-x");
    EXPECT_EQ(d->request_headers.at("anthropic-beta"),
              "prompt-caching-2024-07-31");
    EXPECT_TRUE(err.empty());
}

// 新增/更新 payload 的高级字段与密钥动作必须被完整、显式解析。
TEST(ModelsHandler, ParseDraftAcceptsAdvancedCredentialSemantics) {
    nlohmann::json body = {
        {"name", "advanced"},
        {"provider", "openai"},
        {"model", "model-id"},
        {"base_url", "https://openrouter.ai/api/v1"},
        {"models_dev_provider_id", "openrouter"},
        {"credential_source_name", "source"},
        {"endpoint_mode", "base_url"},
        {"max_output_tokens", 65536},
        {"capabilities", nlohmann::json::array({"tool_use", "reasoning"})},
        {"capabilities_source", "catalog"},
        {"reasoning", {
            {"supported", true},
            {"mandatory", false},
            {"default_enabled", true},
            {"enabled", true},
            {"supported_efforts", nlohmann::json::array({"low", "high"})},
            {"default_effort", "low"},
            {"effort", "high"},
            {"supports_max_tokens", true},
            {"max_tokens", 8192},
        }},
    };
    std::string error;
    auto draft = parse_model_draft(body, error);
    ASSERT_TRUE(draft.has_value()) << error;
    EXPECT_TRUE(draft->base_url_supplied);
    EXPECT_FALSE(draft->api_key_supplied);
    EXPECT_EQ(draft->credential_source_name, "source");
    EXPECT_TRUE(draft->endpoint_mode_supplied);
    EXPECT_EQ(draft->max_output_tokens, 65536);
    EXPECT_TRUE(draft->reasoning_supplied);
    ASSERT_TRUE(draft->reasoning.has_value());
    EXPECT_EQ(draft->reasoning->effort, "high");
}

TEST(ModelsHandler, SharedEditPayloadParsesAndClearsSuppliedOverrides) {
    const auto fixture = model_mutation_fixture();
    ASSERT_TRUE(fixture.is_object());
    ASSERT_TRUE(fixture.contains("edit_payload"));

    std::string error;
    auto draft = parse_model_draft(fixture["edit_payload"], error);
    ASSERT_TRUE(draft.has_value()) << error;
    EXPECT_TRUE(draft->context_window_supplied);
    ASSERT_TRUE(draft->context_window.has_value());
    EXPECT_EQ(*draft->context_window, 0);
    EXPECT_TRUE(draft->max_output_tokens_supplied);
    ASSERT_TRUE(draft->max_output_tokens.has_value());
    EXPECT_EQ(*draft->max_output_tokens, 0);
    EXPECT_TRUE(draft->endpoint_mode_supplied);
    EXPECT_FALSE(draft->endpoint_mode.has_value());
    EXPECT_TRUE(draft->request_headers_supplied);
    EXPECT_TRUE(draft->request_headers.empty());
    EXPECT_TRUE(draft->reasoning_supplied);
    ASSERT_TRUE(draft->reasoning.has_value());
    EXPECT_FALSE(draft->reasoning->enabled.has_value());
    EXPECT_FALSE(draft->reasoning->effort.has_value());
    EXPECT_FALSE(draft->reasoning->max_tokens.has_value());
    EXPECT_EQ(draft->reasoning->default_effort, "high");

    AppConfig cfg;
    ModelProfile existing;
    existing.name = "fixture-openrouter";
    existing.provider = "openai";
    existing.model = "old-model";
    existing.models_dev_provider_id = "openrouter";
    existing.base_url = "https://openrouter.ai/api/v1";
    existing.api_key = "preserved-secret";
    existing.context_window = 100000;
    existing.max_output_tokens = 32000;
    existing.endpoint_mode = "base_url";
    existing.request_headers = {{"X-Old", "value"}};
    cfg.saved_models.push_back(existing);

    EXPECT_EQ(acecode::update_saved_model(
                  cfg, "fixture-openrouter", *draft),
              SavedModelEditError::OK);
    ASSERT_EQ(cfg.saved_models.size(), 1u);
    EXPECT_EQ(cfg.saved_models[0].api_key, "preserved-secret");
    EXPECT_FALSE(cfg.saved_models[0].context_window.has_value());
    EXPECT_FALSE(cfg.saved_models[0].max_output_tokens.has_value());
    EXPECT_FALSE(cfg.saved_models[0].endpoint_mode.has_value());
    EXPECT_TRUE(cfg.saved_models[0].request_headers.empty());
    ASSERT_TRUE(cfg.saved_models[0].reasoning.has_value());
    EXPECT_EQ(cfg.saved_models[0].reasoning->default_effort, "high");
}

// 响应中的高级字段和 API Key 原值都供经过认证的模型编辑器使用。
TEST(ModelsHandler, JsonIncludesAdvancedFieldsAndCredential) {
    ModelProfile profile;
    profile.name = "advanced";
    profile.provider = "openai";
    profile.model = "model-id";
    profile.base_url = "https://openrouter.ai/api/v1";
    profile.api_key = "never-return-this";
    profile.endpoint_mode = "base_url";
    profile.max_output_tokens = 65536;
    profile.capabilities = {"reasoning"};
    profile.capabilities_source = "catalog";
    acecode::ModelReasoningOptions reasoning;
    reasoning.supported = true;
    reasoning.default_enabled = true;
    reasoning.supported_efforts = {"low", "high"};
    reasoning.supports_max_tokens = false;
    profile.reasoning = reasoning;

    const auto body = profile_to_json(profile);
    EXPECT_EQ(body["has_api_key"], true);
    EXPECT_EQ(body["api_key"], "never-return-this");
    EXPECT_EQ(body["endpoint_mode"], "base_url");
    EXPECT_EQ(body["max_output_tokens"], 65536);
    EXPECT_EQ(body["reasoning"]["supported"], true);
}

// 触发场景:request_headers 值必须是字符串模板。
TEST(ModelsHandler, ParseDraftRejectsNonStringRequestHeaderValue) {
    nlohmann::json body = {
        {"name", "local"},
        {"provider", "openai"},
        {"model", "llama-3"},
        {"base_url", "http://localhost/v1"},
        {"api_key", "sk-x"},
        {"request_headers", {
            {"X-Team", 42}
        }},
    };
    std::string err;
    auto d = parse_model_draft(body, err);
    EXPECT_FALSE(d.has_value());
    EXPECT_NE(err.find("request_headers"), std::string::npos);
}

TEST(ModelsHandler, ParseOpenAiModelIdsAcceptsStandardDataArray) {
    nlohmann::json body = {
        {"data", nlohmann::json::array({
            {{"id", "gpt-4o"}},
            {{"id", "gpt-4o-mini"}},
            {{"id", "gpt-4o"}},
            {{"object", "model"}},
        })},
    };
    auto ids = parse_openai_model_ids(body);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], "gpt-4o");
    EXPECT_EQ(ids[1], "gpt-4o-mini");
}

TEST(ModelsHandler, ParseOpenAiModelIdsAcceptsFallbackShapes) {
    auto from_models = parse_openai_model_ids(nlohmann::json{
        {"models", nlohmann::json::array({"b-model", "a-model"})},
    });
    ASSERT_EQ(from_models.size(), 2u);
    EXPECT_EQ(from_models[0], "a-model");
    EXPECT_EQ(from_models[1], "b-model");

    auto from_array = parse_openai_model_ids(nlohmann::json::array({
        {{"id", "z-model"}},
        "manual-model",
    }));
    ASSERT_EQ(from_array.size(), 2u);
    EXPECT_EQ(from_array[0], "manual-model");
    EXPECT_EQ(from_array[1], "z-model");
}

// 场景:ACEModel 官方 /models 的有效服务器值保持原样；字段缺失或无效时
// 才回填 250K，且不影响非内置模型。
TEST(ModelsHandler, ApplyAceModelContextFallbacksPreservesServerValues) {
    auto parsed = parse_openai_models(nlohmann::json{
        {"data", nlohmann::json::array({
            {{"id", "moonlight"}, {"context_window", "unavailable"}},
            {{"id", "starrylight"}, {"context_window", 128000}},
            {{"id", "aurora"}, {"max_context_tokens", "1000000"}},
            {{"id", "other-model"}},
        })},
    });

    apply_acemodel_context_fallbacks(parsed);

    EXPECT_EQ(parsed.context_windows["moonlight"], 250000);
    EXPECT_EQ(parsed.context_windows["starrylight"], 128000);
    EXPECT_EQ(parsed.context_windows["aurora"], 1000000);
    EXPECT_EQ(parsed.context_windows.count("other-model"), 0u);
}

TEST(ModelsHandler, ParseProbeRequestValidatesProviderAndBaseUrl) {
    std::string code;
    std::string err;
    auto unsupported = parse_model_probe_request(
        nlohmann::json{{"provider", "anthropic"}, {"base_url", "http://x"}},
        code,
        err);
    EXPECT_FALSE(unsupported.has_value());
    EXPECT_EQ(code, "UNKNOWN_PROVIDER");

    code.clear();
    err.clear();
    auto missing_url = parse_model_probe_request(
        nlohmann::json{{"provider", "openai"}},
        code,
        err);
    EXPECT_FALSE(missing_url.has_value());
    EXPECT_EQ(code, "MISSING_BASE_URL");

    code.clear();
    err.clear();
    auto ok = parse_model_probe_request(
        nlohmann::json{
            {"catalog_provider_id", "custom-openai"},
            {"provider", "openai"},
            {"base_url", "http://localhost/v1"},
            {"api_key", "sk"},
            {"request_headers", {{"X-Probe", "acecode"}}}
        },
        code,
        err);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->catalog_provider_id, "custom-openai");
    EXPECT_EQ(ok->base_url, "http://localhost/v1");
    EXPECT_EQ(ok->api_key, "sk");
    EXPECT_EQ(ok->request_headers.at("X-Probe"), "acecode");

    code.clear();
    err.clear();
    auto copilot = parse_model_probe_request(
        nlohmann::json{{"provider", "copilot"}},
        code,
        err);
    ASSERT_TRUE(copilot.has_value());
    EXPECT_EQ(copilot->provider, "copilot");
    EXPECT_TRUE(copilot->base_url.empty());

    code.clear();
    err.clear();
    auto grok = parse_model_probe_request(
        nlohmann::json{{"provider", "grok"}},
        code,
        err);
    ASSERT_TRUE(grok.has_value());
    EXPECT_EQ(grok->provider, "grok");
    EXPECT_TRUE(grok->base_url.empty());
}

TEST(ModelsHandler, ProbeConnectionFingerprintIsStableAndSecretOpaque) {
    ModelProbeRequest request;
    request.catalog_provider_id = "custom-openai";
    request.provider = "openai";
    request.base_url = "  http://localhost/v1/  ";
    request.api_key = "sk-secret-value";
    request.request_headers = {
        {"X-Probe", "header-secret"},
        {"X-Team", "acecode"},
    };

    const std::string fingerprint = model_probe_connection_fingerprint(request);
    ASSERT_EQ(fingerprint.size(), 64u);
    EXPECT_EQ(fingerprint.find("secret"), std::string::npos);

    request.base_url = "http://localhost/v1";
    EXPECT_EQ(model_probe_connection_fingerprint(request), fingerprint);

    request.api_key = "sk-other-value";
    EXPECT_NE(model_probe_connection_fingerprint(request), fingerprint);
    request.api_key = "sk-secret-value";
    request.catalog_provider_id = "acemodel";
    EXPECT_NE(model_probe_connection_fingerprint(request), fingerprint);
}

TEST(ModelsHandler, ParseProbeRequestRejectsRequestHeadersForCopilot) {
    std::string code;
    std::string err;
    auto rejected = parse_model_probe_request(
        nlohmann::json{
            {"provider", "copilot"},
            {"request_headers", {{"X-Team", "acecode"}}}
        },
        code,
        err);
    EXPECT_FALSE(rejected.has_value());
    EXPECT_EQ(code, "INVALID_REQUEST_HEADER");
    EXPECT_NE(err.find("request_headers"), std::string::npos);
}

TEST(ModelsHandler, ParseProbeRequestRejectsContentTypeRequestHeader) {
    std::string code;
    std::string err;
    auto rejected = parse_model_probe_request(
        nlohmann::json{
            {"provider", "openai"},
            {"base_url", "http://localhost/v1"},
            {"request_headers", {{"Content-Type", "text/plain"}}}
        },
        code,
        err);
    EXPECT_FALSE(rejected.has_value());
    EXPECT_EQ(code, "INVALID_REQUEST_HEADER");
    EXPECT_NE(err.find("Content-Type"), std::string::npos) << err;
}
