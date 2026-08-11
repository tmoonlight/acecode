// 覆盖 src/provider/models_dev_registry.{hpp,cpp}：
// - validate_registry_schema 的最小校验语义（顶层 object + 至少一个 provider 含 models）
// - find_provider_entry 大小写不敏感
// - initialize_registry/reload_registry_from_disk：user_override > bundled > empty
// - 损坏 JSON 不抛异常并降级到 empty 源

#include <gtest/gtest.h>

#include "provider/models_dev_registry.hpp"
#include "provider/models_dev_paths.hpp"
#include "config/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
void set_env(const char* k, const char* v) { _putenv_s(k, v ? v : ""); }
#else
void set_env(const char* k, const char* v) {
    if (v) ::setenv(k, v, 1);
    else ::unsetenv(k);
}
#endif

fs::path tmp_dir(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("acecode_reg_" + tag);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void write_file(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << contents;
}

const char* kMinimalRegistry =
    R"({"anthropic":{"id":"anthropic","name":"Anthropic","models":{"claude":{"id":"claude","limit":{"context":200000}}}}})";

} // namespace

// 场景：合法注册表（顶层 object + 至少一个 provider 有非空 models）。
TEST(ModelsDevRegistry, ValidateAcceptsObjectWithModels) {
    auto j = nlohmann::json::parse(kMinimalRegistry);
    EXPECT_TRUE(acecode::validate_registry_schema(j));
}

// 场景：顶层数组、空对象、provider 无 models 字段 → 全部判为非法。
TEST(ModelsDevRegistry, ValidateRejectsBadShapes) {
    EXPECT_FALSE(acecode::validate_registry_schema(nlohmann::json::array()));
    EXPECT_FALSE(acecode::validate_registry_schema(nlohmann::json::object()));
    auto no_models = nlohmann::json::parse(R"({"x":{"name":"foo"}})");
    EXPECT_FALSE(acecode::validate_registry_schema(no_models));
    auto empty_models = nlohmann::json::parse(R"({"x":{"models":{}}})");
    EXPECT_FALSE(acecode::validate_registry_schema(empty_models));
}

// 场景：find_provider_entry 应忽略大小写差异。
TEST(ModelsDevRegistry, FindProviderIsCaseInsensitive) {
    auto j = nlohmann::json::parse(kMinimalRegistry);
    EXPECT_NE(nullptr, acecode::find_provider_entry(j, "ANTHROPIC"));
    EXPECT_NE(nullptr, acecode::find_provider_entry(j, "Anthropic"));
    EXPECT_EQ(nullptr, acecode::find_provider_entry(j, "openai"));
    EXPECT_EQ(nullptr, acecode::find_provider_entry(j, ""));
}

// 场景：initialize_registry 命中 user_override 时跳过 bundled。
TEST(ModelsDevRegistry, UserOverrideBeatsBundled) {
    auto override_dir = tmp_dir("override");
    auto override_path = override_dir / "custom.json";
    write_file(override_path, R"({"openai":{"id":"openai","models":{"gpt":{"id":"gpt","limit":{"context":4096}}}}})");

    auto seed = tmp_dir("seed");
    write_file(seed / "api.json", kMinimalRegistry);

    set_env("ACECODE_MODELS_DEV_DIR", seed.string().c_str());

    acecode::AppConfig cfg;
    cfg.models_dev.user_override_path = override_path.string();
    acecode::initialize_registry(cfg, "");

    auto reg = acecode::current_registry();
    ASSERT_TRUE(reg);
    EXPECT_TRUE(reg->contains("openai"));
    EXPECT_FALSE(reg->contains("anthropic"));
    EXPECT_EQ(acecode::current_registry_source().kind,
              acecode::RegistrySource::Kind::UserOverride);

    // cleanup so other tests start fresh
    set_env("ACECODE_MODELS_DEV_DIR", nullptr);
}

// 场景：user_override 不存在时回退到 bundled，并设置正确的 RegistrySource。
TEST(ModelsDevRegistry, FallsBackToBundled) {
    auto seed = tmp_dir("seed_only");
    write_file(seed / "api.json", kMinimalRegistry);
    write_file(seed / "MANIFEST.json", R"({"upstream_commit":"abc"})");
    set_env("ACECODE_MODELS_DEV_DIR", seed.string().c_str());

    acecode::AppConfig cfg;  // no user_override
    acecode::initialize_registry(cfg, "");

    auto reg = acecode::current_registry();
    ASSERT_TRUE(reg);
    EXPECT_TRUE(reg->contains("anthropic"));

    const auto& src = acecode::current_registry_source();
    EXPECT_EQ(src.kind, acecode::RegistrySource::Kind::Bundled);
    ASSERT_TRUE(src.manifest.has_value());
    EXPECT_EQ((*src.manifest)["upstream_commit"], "abc");

    set_env("ACECODE_MODELS_DEV_DIR", nullptr);
}

// 场景：损坏 JSON 不抛异常，降级到 empty 注册表，但仍能查询（返回空）。
TEST(ModelsDevRegistry, CorruptJsonGracefullyEmpty) {
    auto seed = tmp_dir("corrupt");
    write_file(seed / "api.json", "not-json{");
    set_env("ACECODE_MODELS_DEV_DIR", seed.string().c_str());

    acecode::AppConfig cfg;
    EXPECT_NO_THROW(acecode::initialize_registry(cfg, ""));

    auto reg = acecode::current_registry();
    ASSERT_TRUE(reg);
    EXPECT_TRUE(reg->empty());
    EXPECT_EQ(acecode::current_registry_source().kind,
              acecode::RegistrySource::Kind::Empty);

    set_env("ACECODE_MODELS_DEV_DIR", nullptr);
}

// 显式刷新候选必须原子安装；无效候选保留最后有效注册表与推荐 seed。
TEST(ModelsDevRegistry, RefreshCandidateIsFailureSafeAndPreservesBundledSeed) {
    auto seed = tmp_dir("refresh_candidate");
    write_file(seed / "api.json", kMinimalRegistry);
    write_file(seed / "MANIFEST.json", R"({"generated_at":"2026-08-10T00:00:00Z"})");
    write_file(seed / "recommended_models.json", R"({"placeholder":true})");
    set_env("ACECODE_MODELS_DEV_DIR", seed.string().c_str());
    acecode::AppConfig cfg;
    acecode::initialize_registry(cfg, "");
    const auto before = acecode::current_registry();
    ASSERT_TRUE(before->contains("anthropic"));

    EXPECT_FALSE(acecode::install_registry_refresh_candidate(
        nlohmann::json::object(), "https://invalid.test/api.json"));
    EXPECT_EQ(acecode::current_registry().get(), before.get());
    EXPECT_EQ(acecode::current_registry_source().kind,
              acecode::RegistrySource::Kind::Bundled);

    nlohmann::json low_providers = nlohmann::json::object();
    low_providers["only"] = {{"models", nlohmann::json::object()}};
    for (int model = 0; model < 1000; ++model) {
        low_providers["only"]["models"]["model-" + std::to_string(model)] =
            nlohmann::json::object();
    }
    EXPECT_FALSE(acecode::install_registry_refresh_candidate(
        std::move(low_providers), "https://models.dev/api.json"));
    EXPECT_EQ(acecode::current_registry().get(), before.get());
    EXPECT_EQ(acecode::current_registry_source().kind,
              acecode::RegistrySource::Kind::Bundled);

    nlohmann::json garbage_padded_providers = nlohmann::json::object();
    for (int provider = 0; provider < 49; ++provider) {
        auto& models =
            garbage_padded_providers["provider-" + std::to_string(provider)]["models"];
        models = nlohmann::json::object();
        for (int model = 0; model < 21; ++model) {
            models["model-" + std::to_string(model)] = nlohmann::json::object();
        }
    }
    garbage_padded_providers["scalar-padding"] = 1;
    garbage_padded_providers["array-padding"] = nlohmann::json::array({1, 2, 3});
    EXPECT_FALSE(acecode::install_registry_refresh_candidate(
        std::move(garbage_padded_providers), "https://models.dev/api.json"));
    EXPECT_EQ(acecode::current_registry().get(), before.get());
    EXPECT_EQ(acecode::current_registry_source().kind,
              acecode::RegistrySource::Kind::Bundled);

    nlohmann::json low_models = nlohmann::json::object();
    for (int provider = 0; provider < 50; ++provider) {
        auto& models = low_models["provider-" + std::to_string(provider)]["models"];
        models = nlohmann::json::object();
        for (int model = 0; model < 19; ++model) {
            models["model-" + std::to_string(model)] = nlohmann::json::object();
        }
    }
    EXPECT_FALSE(acecode::install_registry_refresh_candidate(
        std::move(low_models), "https://models.dev/api.json"));
    EXPECT_EQ(acecode::current_registry().get(), before.get());

    nlohmann::json valid = nlohmann::json::object();
    for (int provider = 0; provider < 50; ++provider) {
        auto& models = valid["provider-" + std::to_string(provider)]["models"];
        models = nlohmann::json::object();
        for (int model = 0; model < 20; ++model) {
            models["model-" + std::to_string(model)] = nlohmann::json::object();
        }
    }
    EXPECT_TRUE(acecode::install_registry_refresh_candidate(
        std::move(valid), "https://models.dev/api.json"));
    const auto after = acecode::current_registry();
    EXPECT_NE(after.get(), before.get());
    EXPECT_TRUE(after->contains("provider-0"));
    const auto source = acecode::current_registry_source();
    EXPECT_EQ(source.kind, acecode::RegistrySource::Kind::Network);
    ASSERT_TRUE(source.seed_dir.has_value());
    EXPECT_EQ(std::filesystem::path(*source.seed_dir).lexically_normal(),
              seed.lexically_normal());

    set_env("ACECODE_MODELS_DEV_DIR", nullptr);
}
