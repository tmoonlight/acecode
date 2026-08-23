// 覆盖 src/commands/configure_catalog.cpp 的纯函数部分（不触发交互式 stdin）：
// - filter_providers / filter_models 的子串过滤行为
// - format_provider_row / format_model_row / format_model_summary 的渲染输出
// - format_source_line 三种状态
// - lookup_env_key 在命中和缺失时的返回值
//
// 交互式选择函数（run_provider_picker / run_model_picker）需要 stdin TTY，
// 不在单元测试覆盖范围内；它们的逻辑通过这里的 helper 间接保证正确。

#include <gtest/gtest.h>

#include "commands/configure_catalog.hpp"
#include "provider/builtin_model_catalog.hpp"

#include <cstdlib>

using namespace acecode;

namespace {

#ifdef _WIN32
void set_env(const char* k, const char* v) { _putenv_s(k, v ? v : ""); }
#else
void set_env(const char* k, const char* v) {
    if (v) ::setenv(k, v, 1);
    else ::unsetenv(k);
}
#endif

ProviderEntry make_provider(const std::string& id,
                            const std::string& name,
                            std::vector<std::string> env = {}) {
    ProviderEntry p;
    p.id = id;
    p.name = name;
    p.env = std::move(env);
    return p;
}

} // namespace

// 场景：filter_providers 大小写不敏感；空查询返回原列表。
TEST(ConfigureCatalog, FilterProvidersSubstring) {
    ProviderEntry a = make_provider("anthropic", "Anthropic");
    ProviderEntry b = make_provider("openrouter", "OpenRouter");
    ProviderEntry c = make_provider("openai", "OpenAI");
    std::vector<const ProviderEntry*> src{&a, &b, &c};

    EXPECT_EQ(filter_providers(src, "").size(), 3u);
    EXPECT_EQ(filter_providers(src, "OPEN").size(), 2u);
    auto only_anth = filter_providers(src, "thro");
    ASSERT_EQ(only_anth.size(), 1u);
    EXPECT_EQ(only_anth[0]->id, "anthropic");
    EXPECT_EQ(filter_providers(src, "no-such-provider").size(), 0u);
}

// 场景：filter_models 仅按 id 子串匹配，不看 name。
TEST(ConfigureCatalog, FilterModelsByIdOnly) {
    ModelEntry x; x.id = "claude-haiku"; x.name = "Claude Haiku";
    ModelEntry y; y.id = "claude-sonnet"; y.name = "Claude Sonnet";
    ModelEntry z; z.id = "gpt-4o"; z.name = "GPT-4o";
    std::vector<const ModelEntry*> src{&x, &y, &z};

    EXPECT_EQ(filter_models(src, "haiku").size(), 1u);
    EXPECT_EQ(filter_models(src, "claude").size(), 2u);
    EXPECT_EQ(filter_models(src, "GPT").size(), 1u);
    EXPECT_EQ(filter_models(src, "Claude Haiku").size(), 0u);  // 'name' is ignored
}

// 场景：format_provider_row 含 id、模型数、doc；name 与 id 不同时附 (name)。
TEST(ConfigureCatalog, FormatProviderRow) {
    ProviderEntry p = make_provider("openrouter", "OpenRouter");
    p.doc = "https://docs";
    p.models.resize(42);
    auto row = format_provider_row(p);
    EXPECT_NE(row.find("openrouter"), std::string::npos);
    EXPECT_NE(row.find("(OpenRouter)"), std::string::npos);
    EXPECT_NE(row.find("models=42"), std::string::npos);
    EXPECT_NE(row.find("doc=https://docs"), std::string::npos);
}

// 场景:统一列表固定以两个自定义入口、内置 ACEModel 和受管 Copilot 开头,
// 目录项保持输入顺序,但普通 acemodel/github-copilot 条目不得重复出现。
TEST(ConfigureCatalog, UnifiedProviderChoicesOrderAndDeduplicateBuiltins) {
    ProviderEntry openrouter = make_provider("openrouter", "OpenRouter");
    ProviderEntry catalog_acemodel = make_provider("ACEMODEL", "Registry ACEModel");
    ProviderEntry catalog_copilot =
        make_provider("github-copilot", "GitHub Copilot");
    ProviderEntry deepseek = make_provider("deepseek", "DeepSeek");
    std::vector<const ProviderEntry*> providers{
        &openrouter, &catalog_acemodel, &catalog_copilot, nullptr, &deepseek};

    const auto choices = build_configure_provider_choices(providers);
    ASSERT_EQ(choices.size(), 6u);
    EXPECT_EQ(choices[0].kind, ConfigureProviderKind::CustomOpenAI);
    EXPECT_EQ(choices[0].label, "Custom OpenAI-compatible API");
    EXPECT_EQ(choices[1].kind, ConfigureProviderKind::CustomAnthropic);
    EXPECT_EQ(choices[1].label, "Custom Anthropic-compatible API");
    EXPECT_EQ(choices[2].kind, ConfigureProviderKind::Catalog);
    EXPECT_EQ(choices[2].label, "acemodel");
    EXPECT_EQ(choices[2].catalog_provider, &acemodel_catalog_provider());
    EXPECT_NE(choices[2].secondary.find("ACEModel"), std::string::npos);
    EXPECT_EQ(choices[3].kind, ConfigureProviderKind::Copilot);
    EXPECT_EQ(choices[3].label, "GitHub Copilot");
    EXPECT_EQ(choices[4].kind, ConfigureProviderKind::Catalog);
    ASSERT_NE(choices[4].catalog_provider, nullptr);
    EXPECT_EQ(choices[4].catalog_provider->id, "openrouter");
    EXPECT_EQ(choices[5].kind, ConfigureProviderKind::Catalog);
    ASSERT_NE(choices[5].catalog_provider, nullptr);
    EXPECT_EQ(choices[5].catalog_provider->id, "deepseek");

    const auto without_catalog = build_configure_provider_choices({});
    ASSERT_EQ(without_catalog.size(), 4u);
    EXPECT_EQ(without_catalog[2].catalog_provider, &acemodel_catalog_provider());
    EXPECT_EQ(without_catalog[3].kind, ConfigureProviderKind::Copilot);
}

// 场景:当前运行时 Provider 决定初始高亮;未知状态保留旧向导的
// Copilot 默认项,目录 id 匹配大小写不敏感。
TEST(ConfigureCatalog, UnifiedProviderDefaultIndex) {
    ProviderEntry openrouter = make_provider("openrouter", "OpenRouter");
    ProviderEntry deepseek = make_provider("deepseek", "DeepSeek");
    const auto choices = build_configure_provider_choices(
        {&openrouter, &deepseek});
    ASSERT_EQ(choices.size(), 6u);

    AppConfig cfg;
    cfg.provider.clear();
    EXPECT_EQ(default_configure_provider_index(cfg, choices), 3u);

    cfg.provider = "openai";
    cfg.openai.models_dev_provider_id.reset();
    EXPECT_EQ(default_configure_provider_index(cfg, choices), 0u);

    cfg.openai.models_dev_provider_id = "ACEMODEL";
    EXPECT_EQ(default_configure_provider_index(cfg, choices), 2u);

    cfg.openai.models_dev_provider_id = "OPENROUTER";
    EXPECT_EQ(default_configure_provider_index(cfg, choices), 4u);

    cfg.openai.models_dev_provider_id = "missing-provider";
    EXPECT_EQ(default_configure_provider_index(cfg, choices), 0u);

    cfg.provider = "anthropic";
    EXPECT_EQ(default_configure_provider_index(cfg, choices), 1u);

    cfg.provider = "copilot";
    EXPECT_EQ(default_configure_provider_index(cfg, choices), 3u);

    cfg.provider = "unknown";
    EXPECT_EQ(default_configure_provider_index(cfg, choices), 3u);
    EXPECT_EQ(default_configure_provider_index(cfg, {}), 0u);
}

// 场景：format_model_row 缺字段时不输出对应段。
TEST(ConfigureCatalog, FormatModelRowOmitsMissingFields) {
    ModelEntry m;
    m.id = "bare-model";
    auto row = format_model_row(m);
    EXPECT_EQ(row, "bare-model");

    m.context = 200000;
    m.cost_input = 3.0;
    m.cost_output = 15.0;
    m.tool_call = true;
    auto row2 = format_model_row(m);
    EXPECT_NE(row2.find("ctx=200k"), std::string::npos);
    EXPECT_NE(row2.find("in=$3/out=$15"), std::string::npos);
    EXPECT_NE(row2.find("[tools]"), std::string::npos);
}

// 场景：format_model_summary 多行输出，含 capabilities/knowledge。
TEST(ConfigureCatalog, FormatModelSummary) {
    ModelEntry m;
    m.id = "claude-sonnet-4-5";
    m.name = "Claude Sonnet 4.5";
    m.context = 200000;
    m.max_output = 64000;
    m.cost_input = 3.0;
    m.cost_output = 15.0;
    m.tool_call = true;
    m.attachment = true;
    m.reasoning = true;
    m.input_modalities = {"text", "image"};
    m.knowledge_cutoff = "2025-02-28";
    auto s = format_model_summary(m);
    EXPECT_NE(s.find("Selected model: claude-sonnet-4-5"), std::string::npos);
    EXPECT_NE(s.find("(Claude Sonnet 4.5)"), std::string::npos);
    EXPECT_NE(s.find("context:       200k tokens"), std::string::npos);
    EXPECT_NE(s.find("max output:    64000"), std::string::npos);
    EXPECT_NE(s.find("[tools, vision, reasoning]"), std::string::npos);
    EXPECT_NE(s.find("text, image"), std::string::npos);
    EXPECT_NE(s.find("knowledge:     2025-02-28"), std::string::npos);
}

// 场景：format_source_line 区分 copilot / codex / catalog / 两种 custom。
TEST(ConfigureCatalog, FormatSourceLineStates) {
    AppConfig cfg;
    cfg.provider = "copilot";
    EXPECT_EQ(format_source_line(cfg), "copilot");

    cfg.provider = "codex";
    EXPECT_EQ(format_source_line(cfg), "codex");

    cfg.provider = "openai";
    cfg.openai.models_dev_provider_id.reset();
    EXPECT_EQ(format_source_line(cfg), "openai (custom)");

    cfg.openai.models_dev_provider_id = "openrouter";
    EXPECT_EQ(format_source_line(cfg), "openai (provider=openrouter via models.dev)");

    cfg.openai.models_dev_provider_id = "ACEMODEL";
    EXPECT_EQ(format_source_line(cfg), "openai (provider=acemodel)");

    cfg.provider = "anthropic";
    EXPECT_EQ(format_source_line(cfg), "anthropic (custom)");
}

// 场景：lookup_env_key 命中第一个存在的 env 变量；都缺失返回 nullopt。
TEST(ConfigureCatalog, LookupEnvKey) {
    ProviderEntry p = make_provider("groq", "Groq",
                                    {"GROQ_API_KEY", "GROQ_TOKEN"});

    set_env("GROQ_API_KEY", nullptr);
    set_env("GROQ_TOKEN", nullptr);
    EXPECT_FALSE(lookup_env_key(p).has_value());

    set_env("GROQ_TOKEN", "fallback-secret");
    auto hit = lookup_env_key(p);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->env_name, "GROQ_TOKEN");
    EXPECT_EQ(hit->value, "fallback-secret");

    set_env("GROQ_API_KEY", "primary-secret");
    auto hit2 = lookup_env_key(p);
    ASSERT_TRUE(hit2.has_value());
    EXPECT_EQ(hit2->env_name, "GROQ_API_KEY");

    set_env("GROQ_API_KEY", nullptr);
    set_env("GROQ_TOKEN", nullptr);
}
