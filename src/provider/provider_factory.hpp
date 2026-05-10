#pragma once

#include "llm_provider.hpp"
#include "../config/saved_models.hpp"
#include <memory>

namespace acecode {

// 基于一个 ModelProfile 构造 provider。对应 openspec/changes/model-profiles
// 任务 4.3 / design.md D4。调用方不需要再持有整份 AppConfig。
// 返回 shared_ptr 以匹配 main.cpp 可替换容器的语义(design D4)。
std::shared_ptr<LlmProvider> create_provider_from_entry(const ModelProfile& entry);

} // namespace acecode
