#include <gtest/gtest.h>

#include "utils/models_dev_recommended.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {

std::filesystem::path source_models_dev_dir() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
        "assets" / "models_dev";
}

class RecommendedTempDir {
public:
    RecommendedTempDir() {
        path_ = std::filesystem::temp_directory_path() /
            ("acecode-recommended-models-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }

    ~RecommendedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

// 源码树清单是推荐成员的权威来源，必须按固定顺序严格解析 5 项。
TEST(ModelsDevRecommended, LoadsReviewedSourceTreeManifest) {
    std::string error;
    auto manifest = acecode::load_recommended_models_manifest(
        source_models_dev_dir(), error);
    ASSERT_TRUE(manifest.has_value()) << error;
    ASSERT_EQ(manifest->models.size(), 5u);
    EXPECT_EQ(manifest->selected_at, "2026-08-09");
    EXPECT_EQ(manifest->provider_id, "openrouter");
    EXPECT_EQ(manifest->models.front().model_id, "xiaomi/mimo-v2.5");
    EXPECT_EQ(manifest->models.back().model_id,
              "nvidia/nemotron-3-ultra-550b-a55b:free");
    EXPECT_FALSE(manifest->models.back().privacy_warning.empty());
}

// 安装目录中的推荐清单和 api.json 同级，加载行为不得依赖源码树。
TEST(ModelsDevRecommended, LoadsInstalledManifestBesideSnapshot) {
    RecommendedTempDir temp;
    std::filesystem::copy_file(
        source_models_dev_dir() / "recommended_models.json",
        temp.path() / "recommended_models.json");

    std::string error;
    auto manifest = acecode::load_recommended_models_manifest(temp.path(), error);
    ASSERT_TRUE(manifest.has_value()) << error;
    EXPECT_EQ(manifest->models.size(), 5u);
}

// 文件缺失必须显式失败，不能在 C++ 或 React 中暗藏第二份成员清单。
TEST(ModelsDevRecommended, MissingManifestFailsExplicitly) {
    RecommendedTempDir temp;
    std::string error;
    auto manifest = acecode::load_recommended_models_manifest(temp.path(), error);
    EXPECT_FALSE(manifest.has_value());
    EXPECT_NE(error.find("missing"), std::string::npos) << error;
}

// 损坏 JSON 必须显式失败，不能把空数组伪装成有效推荐结果。
TEST(ModelsDevRecommended, CorruptManifestFailsExplicitly) {
    RecommendedTempDir temp;
    {
        std::ofstream output(temp.path() / "recommended_models.json");
        output << "{broken";
    }
    std::string error;
    auto manifest = acecode::load_recommended_models_manifest(temp.path(), error);
    EXPECT_FALSE(manifest.has_value());
    EXPECT_NE(error.find("invalid JSON"), std::string::npos) << error;
}

// 活动目录只能补充元数据；成员、顺序和缺失项的离线回退必须保持不变。
TEST(ModelsDevRecommended, CatalogSupplementPreservesMembershipAndOrder) {
    std::string error;
    auto manifest = acecode::load_recommended_models_manifest(
        source_models_dev_dir(), error);
    ASSERT_TRUE(manifest.has_value()) << error;

    acecode::ProviderEntry provider;
    provider.id = "openrouter";
    acecode::ModelEntry updated;
    updated.id = "z-ai/glm-5.2";
    updated.name = "GLM live";
    updated.context = 2000000;
    updated.max_output = 262144;
    updated.reasoning = true;
    updated.reasoning_efforts = {"high", "xhigh"};
    updated.tool_call = true;
    updated.deprecated = true;
    provider.models.push_back(updated);

    const auto supplemented = acecode::supplement_recommended_models(
        *manifest, {provider});
    ASSERT_EQ(supplemented.models.size(), 5u);
    EXPECT_EQ(supplemented.models[0].model_id, "xiaomi/mimo-v2.5");
    EXPECT_EQ(supplemented.models[1].model_id,
              "deepseek/deepseek-v4-flash-0731");
    EXPECT_EQ(supplemented.models[2].model_id, "z-ai/glm-5.2");
    EXPECT_EQ(supplemented.models[2].name, "GLM live");
    EXPECT_EQ(supplemented.models[2].context_window, 2000000);
    EXPECT_EQ(supplemented.models[2].max_output_tokens, 262144);
    EXPECT_TRUE(supplemented.models[2].deprecated);
    EXPECT_EQ(supplemented.models[4].model_id,
              "nvidia/nemotron-3-ultra-550b-a55b:free");
    EXPECT_EQ(supplemented.models[4].max_output_tokens, 65536);
}

// 必填字段缺失或成员数不是 5 时必须拒绝，而不是静默补默认值。
TEST(ModelsDevRecommended, RequiredContractMismatchIsRejected) {
    nlohmann::json invalid{
        {"schema_version", 1},
        {"selected_at", "2026-08-09"},
        {"source_url", "https://example.invalid"},
        {"provider_id", "openrouter"},
        {"models", nlohmann::json::array()},
    };
    std::string error;
    EXPECT_FALSE(acecode::parse_recommended_models_manifest(invalid, error).has_value());
    EXPECT_NE(error.find("exactly five"), std::string::npos) << error;
}
