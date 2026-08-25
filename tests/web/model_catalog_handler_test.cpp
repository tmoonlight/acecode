#include <gtest/gtest.h>

#include "provider/builtin_model_catalog.hpp"
#include "web/handlers/model_catalog_handler.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {

nlohmann::json catalog_fixture() {
    nlohmann::json models = nlohmann::json::object();
    for (int index = 0; index < 140; ++index) {
        const std::string id = "model-" + std::to_string(index);
        models[id] = {
            {"name", "Model " + std::to_string(index)},
            {"limit", {{"context", 32000 + index}, {"output", 4096}}},
            {"tool_call", true},
        };
    }
    models["exact-model"] = {
        {"name", "Exact Display"},
        {"limit", {{"context", 100000}, {"output", 12000}}},
        {"reasoning", true},
        {"reasoning_options", {
            {"type", "effort"}, {"values", {"none", "low", "high"}}}},
        {"tool_call", true},
        {"attachment", true},
        {"deprecated", true},
    };
    return {
        {"acemodel", {
            {"name", "Registry ACEModel"},
            {"api", "https://wrong.example/v1"},
            {"env", {"WRONG_API_KEY"}},
            {"models", {{"registry-only", {{"name", "Registry Only"}}}}},
        }},
        {"anthropic", {
            {"name", "Anthropic"},
            {"env", {"ANTHROPIC_API_KEY"}},
            {"doc", "https://docs.anthropic.test"},
            {"models", {{"claude-test", {{"name", "Claude Test"}}}}},
        }},
        {"github-copilot", {
            {"name", "GitHub Copilot"},
            {"api", "https://api.githubcopilot.com"},
            {"models", {{"gpt-managed", {{"name", "Managed GPT"}}}}},
        }},
        {"lmstudio", {
            {"name", "LMStudio"},
            {"api", "http://127.0.0.1:1234/v1"},
            {"models", {{"local-model", {{"name", "Local Model"}}}}},
        }},
        {"openai", {
            {"name", "OpenAI"},
            {"env", {"OPENAI_API_KEY"}},
            {"models", {{"gpt-test", {{"name", "GPT Test"}}}}},
        }},
        {"openrouter", {
            {"name", "OpenRouter"},
            {"api", "https://openrouter.ai/api/v1"},
            {"env", {"OPENROUTER_API_KEY"}},
            {"models", std::move(models)},
        }},
        {"xai", {
            {"name", "xAI"},
            {"env", {"XAI_API_KEY"}},
            {"doc", "https://docs.x.ai/docs/models"},
            {"models", {{"grok-test", {
                {"name", "Grok Test"},
                {"limit", {{"context", 100000}, {"output", 0}}},
            }}}},
        }},
    };
}

std::filesystem::path repo_root() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

nlohmann::json contract_fixture() {
    std::ifstream input(repo_root() / "tests" / "fixtures" /
                        "model_catalog_contract.json");
    if (!input) return nullptr;
    return nlohmann::json::parse(input);
}

const nlohmann::json* provider_by_id(const nlohmann::json& providers,
                                     const std::string& id) {
    for (const auto& provider : providers) {
        if (provider.value("id", "") == id) return &provider;
    }
    return nullptr;
}

} // namespace

TEST(ModelCatalogHandler, SummaryUsesCanonicalShapeAndProviderPolicies) {
    const auto providers = acecode::build_catalog(catalog_fixture());
    acecode::RegistrySource source;
    source.kind = acecode::RegistrySource::Kind::Bundled;
    source.manifest = nlohmann::json{{"generated_at", "2026-08-10T00:00:00Z"}};

    const auto summary = acecode::web::model_catalog_summary_to_json(
        providers, source, 7);
    ASSERT_TRUE(summary.contains("catalog"));
    ASSERT_TRUE(summary.contains("providers"));
    EXPECT_FALSE(summary.contains("recommended_models"));
    EXPECT_EQ(summary["catalog"]["source"], "bundled");
    EXPECT_EQ(summary["catalog"]["version"], 7);
    EXPECT_EQ(summary["catalog"]["updated_at"], "2026-08-10T00:00:00Z");

    const auto* acemodel = provider_by_id(summary["providers"], "acemodel");
    const auto* anthropic = provider_by_id(summary["providers"], "anthropic");
    const auto* copilot = provider_by_id(summary["providers"], "copilot");
    const auto* grok = provider_by_id(summary["providers"], "grok");
    const auto* custom = provider_by_id(summary["providers"], "custom-openai");
    const auto* local = provider_by_id(summary["providers"], "lmstudio");
    const auto* openrouter = provider_by_id(summary["providers"], "openrouter");
    const auto* xai = provider_by_id(summary["providers"], "xai");
    const auto& expected_acemodel = acecode::acemodel_catalog_provider();
    const auto acemodel_count = std::count_if(
        summary["providers"].begin(), summary["providers"].end(),
        [](const nlohmann::json& provider) {
            return provider.value("id", "") == "acemodel";
        });
    EXPECT_EQ(acemodel_count, 1);
    ASSERT_NE(acemodel, nullptr);
    ASSERT_NE(anthropic, nullptr);
    ASSERT_NE(copilot, nullptr);
    ASSERT_NE(grok, nullptr);
    ASSERT_NE(custom, nullptr);
    ASSERT_NE(local, nullptr);
    ASSERT_NE(openrouter, nullptr);
    ASSERT_NE(xai, nullptr);
    EXPECT_EQ((*acemodel)["runtime_provider"], "openai");
    EXPECT_EQ((*acemodel)["name"], expected_acemodel.name);
    EXPECT_EQ((*acemodel)["base_url"], *expected_acemodel.base_url);
    EXPECT_EQ((*acemodel)["auth_mode"], "required");
    EXPECT_FALSE((*acemodel)["endpoint_editable"].get<bool>());
    EXPECT_EQ((*acemodel)["model_input"], "catalog");
    EXPECT_EQ((*acemodel)["api_key_env"], expected_acemodel.env.front());
    EXPECT_EQ((*acemodel)["models_dev_provider_id"], expected_acemodel.id);
    EXPECT_EQ((*acemodel)["group"], "custom");
    EXPECT_EQ((*acemodel)["endpoint_modes"], nlohmann::json::array({"base_url"}));
    EXPECT_EQ((*anthropic)["runtime_provider"], "anthropic");
    EXPECT_EQ((*anthropic)["doc"], "https://docs.anthropic.test");
    EXPECT_EQ((*copilot)["auth_mode"], "managed");
    EXPECT_FALSE((*copilot)["endpoint_editable"].get<bool>());
    EXPECT_EQ((*grok)["runtime_provider"], "grok");
    EXPECT_EQ((*grok)["auth_mode"], "managed");
    EXPECT_FALSE((*grok)["endpoint_editable"].get<bool>());
    EXPECT_TRUE((*grok)["base_url"].get<std::string>().empty());
    EXPECT_EQ((*grok)["models_dev_provider_id"], "xai");
    EXPECT_TRUE((*custom)["endpoint_editable"].get<bool>());
    EXPECT_EQ((*custom)["auth_mode"], "required");
    EXPECT_EQ((*custom)["endpoint_modes"],
              nlohmann::json::array({"base_url", "full_url"}));
    EXPECT_EQ((*local)["auth_mode"], "none");
    EXPECT_EQ((*openrouter)["runtime_provider"], "openai");
    EXPECT_EQ((*openrouter)["auth_mode"], "required");
    EXPECT_EQ((*xai)["runtime_provider"], "openai");
    EXPECT_EQ((*xai)["base_url"], "https://api.x.ai/v1");
    EXPECT_EQ((*xai)["models_dev_provider_id"], "xai");
    for (const auto& provider : summary["providers"]) {
        EXPECT_FALSE(provider.contains("models"));
        for (const char* key : {"id", "name", "runtime_provider", "base_url",
                                "doc", "auth_mode", "endpoint_editable",
                                "model_input", "api_key_env"}) {
            EXPECT_TRUE(provider.contains(key)) << key;
        }
    }
}

TEST(ModelCatalogHandler, SearchIsBoundedStableAndExactIdFirst) {
    const auto providers = acecode::build_catalog(catalog_fixture());
    auto exact = acecode::web::query_model_catalog_to_json(
        providers, "OPENROUTER", "EXACT-MODEL", 1);
    ASSERT_TRUE(exact.has_value());
    EXPECT_EQ((*exact)["provider_id"], "OPENROUTER");
    EXPECT_EQ((*exact)["limit"], 1);
    ASSERT_EQ((*exact)["models"].size(), 1u);
    const auto& model = (*exact)["models"][0];
    EXPECT_EQ(model["id"], "exact-model");
    EXPECT_EQ(model["deprecated"], true);
    EXPECT_EQ(model["reasoning"]["supported_efforts"],
              nlohmann::json::array({"low", "high"}));
    EXPECT_EQ(model["reasoning"]["mandatory"], false);

    auto capped = acecode::web::query_model_catalog_to_json(
        providers, "openrouter", "model", 9999);
    ASSERT_TRUE(capped.has_value());
    EXPECT_EQ((*capped)["limit"], acecode::web::kMaxModelCatalogQueryLimit);
    EXPECT_EQ((*capped)["models"].size(),
              static_cast<std::size_t>(acecode::web::kMaxModelCatalogQueryLimit));
    EXPECT_EQ((*capped)["models"][0]["id"], "exact-model");

    auto by_name = acecode::web::query_model_catalog_to_json(
        providers, "openrouter", "display", 10);
    ASSERT_TRUE(by_name.has_value());
    ASSERT_EQ((*by_name)["models"].size(), 1u);
    EXPECT_EQ((*by_name)["models"][0]["id"], "exact-model");
    EXPECT_FALSE(acecode::web::query_model_catalog_to_json(
        providers, "missing", "", 10).has_value());
}

TEST(ModelCatalogHandler, NativeAliasesRemainUsable) {
    const auto providers = acecode::build_catalog(catalog_fixture());
    auto copilot = acecode::web::query_model_catalog_to_json(
        providers, "copilot", "managed", 10);
    ASSERT_TRUE(copilot.has_value());
    ASSERT_EQ((*copilot)["models"].size(), 1u);
    EXPECT_EQ((*copilot)["models"][0]["id"], "gpt-managed");

    auto xai = acecode::web::query_model_catalog_to_json(
        providers, "xai", "grok", 10);
    ASSERT_TRUE(xai.has_value());
    ASSERT_EQ((*xai)["models"].size(), 1u);
    EXPECT_EQ((*xai)["models"][0]["id"], "grok-test");

    auto grok = acecode::web::query_model_catalog_to_json(
        providers, "grok", "grok", 10);
    ASSERT_TRUE(grok.has_value());
    ASSERT_EQ((*grok)["models"].size(), 1u);
    EXPECT_EQ((*grok)["models"][0]["id"], "grok-test");
    EXPECT_EQ((*grok)["models"][0]["context_window"], 100000);
    EXPECT_TRUE((*grok)["models"][0]["max_output_tokens"].is_null());
}

TEST(ModelCatalogHandler, AceModelUsesBuiltinCatalogEvenWithoutRegistryEntry) {
    const auto providers = acecode::build_catalog(catalog_fixture());
    auto all = acecode::web::query_model_catalog_to_json(
        providers, "acemodel", "", 10);
    ASSERT_TRUE(all.has_value());
    ASSERT_EQ((*all)["models"].size(), 3u);
    EXPECT_EQ((*all)["models"][0]["id"], "aurora");
    EXPECT_EQ((*all)["models"][0]["name"], "Aurora");
    EXPECT_EQ((*all)["models"][1]["id"], "moonlight");
    EXPECT_EQ((*all)["models"][1]["name"], "Moonlight");
    EXPECT_EQ((*all)["models"][2]["id"], "starrylight");
    EXPECT_EQ((*all)["models"][2]["name"], "Starrylight");
    for (const auto& model : (*all)["models"]) {
        EXPECT_EQ(model["context_window"], 250000);
    }

    auto filtered = acecode::web::query_model_catalog_to_json(
        providers, "ACEMODEL", "moon", 10);
    ASSERT_TRUE(filtered.has_value());
    ASSERT_EQ((*filtered)["models"].size(), 1u);
    EXPECT_EQ((*filtered)["models"][0]["id"], "moonlight");
}

TEST(ModelCatalogHandler, SharedContractFixtureMatchesCanonicalResponses) {
    const auto contract = contract_fixture();
    ASSERT_TRUE(contract.is_object());
    ASSERT_TRUE(contract.contains("summary"));
    ASSERT_TRUE(contract.contains("query"));

    const auto providers = acecode::build_catalog(catalog_fixture());
    acecode::RegistrySource source;
    source.kind = acecode::RegistrySource::Kind::Bundled;
    source.manifest = nlohmann::json{{"generated_at", "2026-08-10T00:00:00Z"}};

    const auto summary = acecode::web::model_catalog_summary_to_json(
        providers, source, 7);
    EXPECT_EQ(summary["catalog"], contract["summary"]["catalog"]);
    for (const auto& expected_provider : contract["summary"]["providers"]) {
        const auto* actual = provider_by_id(summary["providers"],
                                            expected_provider["id"]);
        ASSERT_NE(actual, nullptr) << expected_provider["id"];
        EXPECT_EQ(*actual, expected_provider);
    }
    EXPECT_FALSE(summary.contains("recommended_models"));

    auto query = acecode::web::query_model_catalog_to_json(
        providers, "openrouter", "EXACT-MODEL", 1);
    ASSERT_TRUE(query.has_value());
    EXPECT_EQ(*query, contract["query"]);
}
