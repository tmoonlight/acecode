#pragma once

// GET / PUT /api/config/auto-review 的纯函数部分(openspec add-auto-review-mode)。
// 自动模式（试验）的裁决模型:saved_models 里的一个名字,留空 = 跟随会话当前模型。

#include "config/config.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace acecode::web {

// {model_name, configured, models[]}。configured = 名字非空且仍在 saved_models 里;
// 名字已被删除时 configured 为 false,前端据此把它显示成失效的选项。
nlohmann::json auto_review_settings(const AppConfig& config);

// 只接受 {model_name: string}。新提交的非空名字必须存在于 saved_models;
// 原样回写一个早先保存、现已失效的名字不算错(运行时会回退到会话当前模型)。
bool apply_auto_review_settings(AppConfig& config, const nlohmann::json& patch, std::string& error);

} // namespace acecode::web
