#include "auto_review_model.hpp"

#include "provider/copilot_provider.hpp"
#include "provider/provider_factory.hpp"
#include "provider/summary_model_fallback.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <utility>

namespace acecode {
namespace {

// 裁决请求有自己的阶段时限(engine 侧 20 秒 / 45 秒),这里再给 OpenAI 兼容连接的
// 流超时封个顶,免得被放弃的请求在后台线程里挂到默认的几分钟。
constexpr int kReviewStreamTimeoutMs = 60000;

std::shared_ptr<LlmProvider> default_factory(const ModelProfile& source) {
    ModelProfile profile = source;
    if (profile.provider == "openai") {
        profile.stream_timeout_ms =
            (std::min)(profile.stream_timeout_ms.value_or(kReviewStreamTimeoutMs), kReviewStreamTimeoutMs);
    }
    auto provider = create_provider_from_entry(profile);
    if (!provider) return nullptr;
    if (auto copilot = std::dynamic_pointer_cast<CopilotProvider>(provider)) copilot->try_silent_auth();
    return provider;
}

} // namespace

AutoReviewModelChoice auto_review_model_choice(const AppConfig& config) {
    AutoReviewModelChoice choice;
    choice.configured_name = config.auto_review.model_name;
    if (choice.configured_name.empty()) return choice;
    for (const auto& profile : config.saved_models) {
        if (profile.name == choice.configured_name) {
            choice.profile = profile;
            break;
        }
    }
    return choice;
}

AutoReviewProviderCache::AutoReviewProviderCache(Factory factory)
    : factory_(factory ? std::move(factory) : Factory(default_factory)) {}

std::shared_ptr<LlmProvider> AutoReviewProviderCache::resolve(const AutoReviewModelChoice& choice) {
    if (!choice.profile) {
        std::lock_guard<std::mutex> lock(mu_);
        key_.clear();
        provider_.reset();
        if (!choice.configured_name.empty() && warned_name_ != choice.configured_name) {
            warned_name_ = choice.configured_name;
            LOG_WARN("[auto-review] configured reviewer model '" + choice.configured_name +
                     "' is not in saved_models; using the session model");
        }
        return nullptr;
    }

    // 连接身份(端点、协议、凭据、请求头)加上会影响请求形状的几项;任何一项变了
    // 就重建。身份是哈希,不含明文凭据。
    const ModelProfile& profile = *choice.profile;
    const std::string key = summary_model_identity(profile) + "|" + profile.model + "|" +
        std::to_string(profile.max_output_tokens.value_or(0)) + "|" +
        std::to_string(profile.stream_timeout_ms.value_or(0));
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (provider_ && key_ == key) return provider_;
    }

    std::shared_ptr<LlmProvider> created;
    try {
        created = factory_(profile);
    } catch (const std::exception& e) {
        LOG_WARN("[auto-review] cannot create reviewer provider for '" + profile.name + "': " + e.what());
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (!created) {
        // 创建失败不缓存:下次裁决再试,期间回退到会话当前模型。
        key_.clear();
        provider_.reset();
        if (warned_name_ != profile.name) {
            warned_name_ = profile.name;
            LOG_WARN("[auto-review] reviewer model '" + profile.name +
                     "' could not be initialised; using the session model");
        }
        return nullptr;
    }
    key_ = key;
    provider_ = std::move(created);
    warned_name_.clear();   // 恢复正常后,再出问题时重新提示一次。
    return provider_;
}

} // namespace acecode
