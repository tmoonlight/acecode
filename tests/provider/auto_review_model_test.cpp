// 自动模式（试验）裁决模型的解析与缓存(openspec add-auto-review-mode,规格
// 「裁决模型的选择」)。约定只有一条:配置了且存在就用它,其余一律返回空,由调用方
// 改用会话当前模型 —— 没有回退链,不会悄悄落到别的模型上。

#include <gtest/gtest.h>

#include "provider/auto_review_model.hpp"

#include "test_support/agent/stub_provider.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using acecode::AppConfig;
using acecode::auto_review_model_choice;
using acecode::AutoReviewModelChoice;
using acecode::AutoReviewProviderCache;
using acecode::ModelProfile;

namespace {

ModelProfile profile(const std::string& name, const std::string& model = "m-1",
                     const std::string& key = "k-1") {
    ModelProfile out;
    out.name = name;
    out.provider = "openai";
    out.base_url = "https://example.invalid/v1";
    out.api_key = key;
    out.model = model;
    return out;
}

AutoReviewModelChoice choice_for(const ModelProfile& value) {
    AutoReviewModelChoice choice;
    choice.configured_name = value.name;
    choice.profile = value;
    return choice;
}

// 记录每次创建;按需返回空或抛异常,模拟连接建不起来。
struct CountingFactory {
    std::shared_ptr<std::vector<std::string>> created = std::make_shared<std::vector<std::string>>();
    std::shared_ptr<bool> fail = std::make_shared<bool>(false);
    std::shared_ptr<bool> raise = std::make_shared<bool>(false);

    AutoReviewProviderCache::Factory make() const {
        return [created = created, fail = fail, raise = raise](const ModelProfile& p)
                   -> std::shared_ptr<acecode::LlmProvider> {
            created->push_back(p.name + "/" + p.model);
            if (*raise) throw std::runtime_error("boom");
            if (*fail) return nullptr;
            auto provider = std::make_shared<acecode_test::StubLlmProvider>();
            provider->set_model(p.model);
            return provider;
        };
    }
};

} // namespace

// 场景:从配置里取出裁决模型 —— 没配置、配置了且存在、配置的名字已被删除。
// 期望:没配置时名字与模型都为空;存在时带回完整的模型条目;已删除时保留配置的
// 名字但模型为空(调用方据此记一条回退日志)。
TEST(AutoReviewModel, ChoiceReflectsTheConfiguration) {
    AppConfig config;
    config.saved_models = {profile("main"), profile("reviewer", "m-review")};

    auto none = auto_review_model_choice(config);
    EXPECT_TRUE(none.configured_name.empty());
    EXPECT_FALSE(none.profile.has_value());

    config.auto_review.model_name = "reviewer";
    auto found = auto_review_model_choice(config);
    EXPECT_EQ(found.configured_name, "reviewer");
    ASSERT_TRUE(found.profile.has_value());
    EXPECT_EQ(found.profile->model, "m-review");

    config.auto_review.model_name = "deleted";
    auto missing = auto_review_model_choice(config);
    EXPECT_EQ(missing.configured_name, "deleted");
    EXPECT_FALSE(missing.profile.has_value());
}

// 场景:同一个会话里连续多次裁决,配置没有变化。
// 期望:provider 只创建一次,之后复用同一个实例(每次裁决都重建连接既慢又浪费)。
TEST(AutoReviewModel, ReusesTheProviderWhileTheConfigurationIsUnchanged) {
    CountingFactory factory;
    AutoReviewProviderCache cache(factory.make());
    const auto first = cache.resolve(choice_for(profile("reviewer")));
    const auto second = cache.resolve(choice_for(profile("reviewer")));
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first, second);
    EXPECT_EQ(factory.created->size(), 1u);
}

// 场景:用户在设置里换了裁决模型,或者改了同名模型的凭据 / 模型标识。
// 期望:下一次裁决拿到重建后的 provider(修改立即生效,不需要重启)。
TEST(AutoReviewModel, RebuildsWhenTheConfigurationChanges) {
    CountingFactory factory;
    AutoReviewProviderCache cache(factory.make());
    const auto a = cache.resolve(choice_for(profile("reviewer", "m-1", "k-1")));
    const auto b = cache.resolve(choice_for(profile("reviewer", "m-2", "k-1")));
    const auto c = cache.resolve(choice_for(profile("reviewer", "m-2", "k-2")));
    const auto d = cache.resolve(choice_for(profile("other", "m-2", "k-2")));
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    EXPECT_NE(c, d);
    EXPECT_EQ(factory.created->size(), 4u);
    EXPECT_EQ(b->model(), "m-2");
}

// 场景:没有配置裁决模型,或配置的名字已不在 saved_models 里。
// 期望:返回空,且不调用工厂 —— 调用方改用会话当前模型。之后重新配置一个存在的
// 模型,能正常解析出来(之前的空结果没有被缓存住)。
TEST(AutoReviewModel, ReturnsNullWhenNothingUsableIsConfigured) {
    CountingFactory factory;
    AutoReviewProviderCache cache(factory.make());

    EXPECT_EQ(cache.resolve(AutoReviewModelChoice{}), nullptr);
    AutoReviewModelChoice missing;
    missing.configured_name = "deleted";
    EXPECT_EQ(cache.resolve(missing), nullptr);
    EXPECT_EQ(cache.resolve(missing), nullptr);
    EXPECT_TRUE(factory.created->empty());

    EXPECT_NE(cache.resolve(choice_for(profile("reviewer"))), nullptr);
    EXPECT_EQ(factory.created->size(), 1u);
}

// 场景:配置的模型建不起连接(工厂返回空,或者抛异常)。
// 期望:返回空而不是把异常抛给审批门;失败不缓存,下一次裁决会再试;连接恢复后
// 能拿到 provider。回归意义:这里抛出去的话,一次配置错误会让所有需要确认的
// 调用都失败。
TEST(AutoReviewModel, CreationFailureFallsBackAndIsRetried) {
    CountingFactory factory;
    AutoReviewProviderCache cache(factory.make());

    *factory.fail = true;
    EXPECT_EQ(cache.resolve(choice_for(profile("reviewer"))), nullptr);
    EXPECT_EQ(cache.resolve(choice_for(profile("reviewer"))), nullptr);
    EXPECT_EQ(factory.created->size(), 2u);

    *factory.fail = false;
    *factory.raise = true;
    EXPECT_EQ(cache.resolve(choice_for(profile("reviewer"))), nullptr);

    *factory.raise = false;
    EXPECT_NE(cache.resolve(choice_for(profile("reviewer"))), nullptr);
    EXPECT_EQ(factory.created->size(), 4u);
}
