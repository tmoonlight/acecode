#pragma once

// 图像生成的纯决策层(openspec add-image-generation-tool)。
//
// 这里只放不碰网络、不碰文件系统、不碰 UI 的判断,进 acecode_testable 单测:
//   - 端点解析:凭据从 saved_models 借还是用本段自己的
//   - quality 档位 → 模型名
//   - 无人值守判定:什么时候「问了也没人答」,此时必须降级而不是放行
//   - 成本确认问题的构造(降级项必须排第一,见下)
//
// 成本控制是本工具唯一的安全机制,而它有两条互补的防线:
//   1. 降级选项**恒为第一项** —— question_policy=timeout 与 active goal 的
//      无人值守分支都以「超时自动采纳第一个选项」收尾,把降级放首位让这些
//      路径不需要任何特判就收敛到低成本档。
//   2. 完全不弹出的路径(headless / question_policy=deny)返回的是「自行
//      决策并继续」,等于把选择权交还给模型,而模型不知道账户余额。所以这
//      些路径必须在提问**之前**就把档位钉死,不能依赖第 1 条。

#include "../../config/config.hpp"
#include "../question_policy.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace acecode::image_generation {

enum class Quality {
    Standard = 0,
    High = 1,
    Ultra = 2,
};

// 解析后的端点凭据。ok=false 时 reason 说明为什么不可用(用于启动日志与
// 「未配置」徽标),此时工具不应注册。
struct ResolvedEndpoint {
    bool ok = false;
    std::string base_url;
    std::string api_key;
    std::string reason;  // ok=false 时非空
};

// 高档位降级的原因。None = 未降级。
enum class DowngradeReason {
    None = 0,
    Unattended,     // 无人值守应答路径,硬降级
    UserChoice,     // 用户在确认里选了降级
    QuestionUnavailable,  // 没有可用的提问通道
};

struct QualityDecision {
    Quality quality = Quality::Standard;
    Quality requested = Quality::Standard;
    DowngradeReason downgrade = DowngradeReason::None;
    bool needs_confirmation = false;  // true = 调用方应发起 AskUserQuestion
};

// "standard" / "high" / "ultra";其它值(含空)回退到 fallback。
Quality parse_quality(const std::string& value, Quality fallback);
const char* quality_name(Quality quality);

// 档位 → 模型名。配置里未填的档位回退到该档的默认模型名。
std::string model_for_quality(const ImageGenerationConfig& cfg, Quality quality);

// 端点解析。source=saved_model 时按名在 saved_models 里找,借用其
// base_url + api_key;source=inline 时用本段自己的字段。
// 任何一步拿不到非空 api_key 都返回 ok=false —— 注册一个必然失败的工具
// 比不注册更糟:模型会反复调用它然后反复失败。
ResolvedEndpoint resolve_endpoint(const AppConfig& cfg);

// Images endpoints require a base URL, not a chat-only full endpoint.
bool can_reuse_connection(const ModelProfile& profile);

// 是否处于「提问不会真正呈现给用户」的状态。
// headless 与 deny 策略都会自动应答,把决定权交回模型 —— 这正是最无人
// 监督的场景,不能让模型自己决定花多少钱。
//
// 注意 timeout 策略与 active goal **不**算无人值守:它们会真的弹出组件,
// 用户在场就能回答;无人回答时由「降级项排第一」兜底。
bool is_unattended_answer_path(bool headless_active,
                               QuestionPolicy policy);

// 综合判定本次调用最终用哪一档,以及要不要发起确认。
// has_question_channel=false 表示当前 runtime 没有可用的提问通道。
QualityDecision decide_quality(const ImageGenerationConfig& cfg,
                               const std::string& requested_quality,
                               bool headless_active,
                               QuestionPolicy policy,
                               bool has_question_channel);

// 构造成本确认的 questions_payload(wire 格式与 AskUserQuestion 一致)。
// 第一项恒为降级到 standard。
nlohmann::json build_cost_confirmation_payload(const ImageGenerationConfig& cfg,
                                               Quality requested);

// 从 AskUserQuestion 的响应里判断用户是否选择了继续用高档位。
// 取消、超时、答不出来一律按降级处理 —— 成本确认 fail-safe 到便宜的一侧。
bool confirmation_kept_high_quality(const nlohmann::json& response,
                                    const ImageGenerationConfig& cfg,
                                    Quality requested);

// 成本确认里那道题的固定 id/文本,构造与解析两侧共用。
extern const char* const kCostQuestionId;

} // namespace acecode::image_generation
