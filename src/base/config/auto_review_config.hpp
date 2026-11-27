#pragma once

// 自动模式（试验）的配置段 auto_review(openspec add-auto-review-mode)。
// 解析与序列化单独放在这里:config.cpp 已到行数上限,新配置段不再往里加。

#include <nlohmann/json.hpp>

#include <string>

namespace acecode {

struct AutoReviewConfig {
    // 裁决模型:saved_models 里的一个名字。留空 = 用会话当前的模型裁决
    // (严格跟随,不做回退链)。
    std::string model_name;
};

// 从 config.json 的根对象读 auto_review 段;没有该段时保持默认值。类型不对时抛
// std::runtime_error。名字是否还在 saved_models 里不在这里校验 —— 模型被删掉后
// 运行时回退到会话当前模型,不该让整份配置加载失败。
void read_auto_review_config(const nlohmann::json& root, AutoReviewConfig& config);

// 写回根对象。留空(默认值)时整段省略。
void write_auto_review_config(nlohmann::json& root, const AutoReviewConfig& config);

} // namespace acecode
