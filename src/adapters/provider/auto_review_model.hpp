#pragma once

// 自动模式（试验）的裁决模型(openspec add-auto-review-mode,design D6)。
//
// 配置 auto_review.model_name 是 saved_models 里的一个名字,留空 = 用会话当前模型。
// 这里做两件事:
//   1. 从一份配置里取出「当前配置的裁决模型」(宿主在自己的配置锁下调用)。
//   2. 会话级缓存:同一份连接配置复用同一个 provider 实例,配置变了才重建。
// 与摘要模型(SummaryModelFallback)不同,这里没有回退链 —— 解析不出来就返回空,
// 调用方改用会话当前的 provider。裁决是安全判断,不该悄悄落到某个随手配的小模型上。

#include "config/config.hpp"
#include "llm/llm_provider.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace acecode {

struct AutoReviewModelChoice {
    std::string configured_name;           // 配置里的名字;空 = 没配置
    std::optional<ModelProfile> profile;   // 名字在 saved_models 里找得到时有值
};

// 宿主提供的取得函数:每次裁决时调用,返回当时配置下的选择。
using AutoReviewModelAccessor = std::function<AutoReviewModelChoice()>;

AutoReviewModelChoice auto_review_model_choice(const AppConfig& config);

class AutoReviewProviderCache {
public:
    using Factory = std::function<std::shared_ptr<LlmProvider>(const ModelProfile&)>;

    // factory 为空时用 create_provider_from_entry;单测注入假工厂。
    explicit AutoReviewProviderCache(Factory factory = {});

    // 返回配置的裁决模型对应的 provider。以下情况返回空,调用方回退到会话当前模型:
    // 没配置、名字已不存在(记一条日志,同一个名字只记一次)、创建失败。
    std::shared_ptr<LlmProvider> resolve(const AutoReviewModelChoice& choice);

private:
    Factory factory_;
    std::mutex mu_;   // 叶子锁:保护下面的缓存;创建 provider 在锁外进行。
    std::string key_;
    std::shared_ptr<LlmProvider> provider_;
    std::string warned_name_;
};

} // namespace acecode
