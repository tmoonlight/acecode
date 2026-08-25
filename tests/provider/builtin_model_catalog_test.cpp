#include <gtest/gtest.h>

#include "provider/builtin_model_catalog.hpp"

namespace acecode {

// 场景:所有界面共享的 ACEModel 定义包含固定端点、凭据来源和内置模型。
TEST(BuiltinModelCatalog, AceModelCanonicalMetadata) {
    const ProviderEntry& provider = acemodel_catalog_provider();

    EXPECT_EQ(provider.id, "acemodel");
    EXPECT_EQ(provider.name, "ACEModel");
    ASSERT_TRUE(provider.base_url.has_value());
    EXPECT_EQ(*provider.base_url, "https://ge.bigjuan.xyz/aceapi/v1");
    ASSERT_EQ(provider.env.size(), 1u);
    EXPECT_EQ(provider.env[0], "ACEMODEL_API_KEY");
    EXPECT_TRUE(provider.openai_compatible);
    ASSERT_EQ(provider.models.size(), 3u);
    EXPECT_EQ(provider.models[0].id, "moonlight");
    EXPECT_EQ(provider.models[1].id, "starrylight");
    EXPECT_EQ(provider.models[2].id, "aurora");
    for (const auto& model : provider.models) {
        ASSERT_TRUE(model.context.has_value());
        EXPECT_EQ(*model.context, 250000);
        EXPECT_TRUE(model.tool_call);
    }
}

// 场景:外部 catalog 使用不同大小写时仍被识别为同一个 ACEModel 身份。
TEST(BuiltinModelCatalog, AceModelIdentityIsCaseInsensitive) {
    EXPECT_TRUE(is_acemodel_provider_id("acemodel"));
    EXPECT_TRUE(is_acemodel_provider_id("ACEModel"));
    EXPECT_FALSE(is_acemodel_provider_id("ace-model"));
}

// 场景:探测和运行时可用同一份规范模型元数据，并只匹配固定官方端点。
TEST(BuiltinModelCatalog, AceModelLookupAndEndpointIdentityAreCanonical) {
    const ModelEntry* aurora = find_acemodel_catalog_model("AURORA");
    ASSERT_NE(aurora, nullptr);
    ASSERT_TRUE(aurora->context.has_value());
    EXPECT_EQ(*aurora->context, 250000);
    EXPECT_EQ(find_acemodel_catalog_model("unknown"), nullptr);

    EXPECT_TRUE(is_acemodel_base_url(" HTTPS://GE.BIGJUAN.XYZ/aceapi/v1/ "));
    EXPECT_FALSE(is_acemodel_base_url("https://gateway.example/v1"));
}

} // namespace acecode
