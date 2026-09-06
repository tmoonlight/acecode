#include "image_generation_policy.hpp"
#include "../../utils/http_url_validation.hpp"

#include <algorithm>

namespace acecode::image_generation {

const char* const kCostQuestionId = "使用更高分辨率生成图像?";

namespace {

// 档位模型名为空时的兜底。配置层已给了默认值,这里防的是用户显式写了空串。
const char* default_model_for(Quality quality) {
    switch (quality) {
        case Quality::High:  return "acemodel-image-2k";
        case Quality::Ultra: return "acemodel-image-4k";
        case Quality::Standard:
        default:             return "acemodel-image";
    }
}

const char* quality_label(Quality quality) {
    switch (quality) {
        case Quality::High:  return "高分辨率";
        case Quality::Ultra: return "超高分辨率";
        case Quality::Standard:
        default:             return "标准分辨率";
    }
}

} // namespace

Quality parse_quality(const std::string& value, Quality fallback) {
    if (value == "standard") return Quality::Standard;
    if (value == "high")     return Quality::High;
    if (value == "ultra")    return Quality::Ultra;
    return fallback;
}

const char* quality_name(Quality quality) {
    switch (quality) {
        case Quality::High:  return "high";
        case Quality::Ultra: return "ultra";
        case Quality::Standard:
        default:             return "standard";
    }
}

std::string model_for_quality(const ImageGenerationConfig& cfg, Quality quality) {
    const std::string* configured = nullptr;
    switch (quality) {
        case Quality::High:  configured = &cfg.model_high; break;
        case Quality::Ultra: configured = &cfg.model_ultra; break;
        case Quality::Standard:
        default:             configured = &cfg.model_standard; break;
    }
    if (configured && !configured->empty()) return *configured;
    return default_model_for(quality);
}

bool can_reuse_connection(const ModelProfile& profile) {
    return profile.provider == "openai" &&
           profile.endpoint_mode.value_or("base_url") != "full_url" &&
           utils::is_valid_http_base_url(profile.base_url);
}

ResolvedEndpoint resolve_endpoint(const AppConfig& cfg) {
    ResolvedEndpoint out;
    const auto& ig = cfg.image_generation;

    if (!ig.enabled) {
        out.reason = "image_generation.enabled = false";
        return out;
    }

    if (ig.source == "saved_model") {
        if (ig.saved_model_name.empty()) {
            out.reason = "image_generation.source = saved_model but "
                         "saved_model_name is empty";
            return out;
        }
        auto it = std::find_if(cfg.saved_models.begin(), cfg.saved_models.end(),
                               [&](const ModelProfile& p) {
                                   return p.name == ig.saved_model_name;
                               });
        if (it == cfg.saved_models.end()) {
            out.reason = "image_generation.saved_model_name '" +
                         ig.saved_model_name + "' not found in saved_models";
            return out;
        }
        if (!can_reuse_connection(*it)) {
            out.reason = "image generation requires an OpenAI-compatible base URL connection";
            return out;
        }
        out.base_url = it->base_url;
        out.api_key = it->api_key;
    } else {
        out.base_url = ig.base_url;
        out.api_key = ig.api_key;
    }

    if (!utils::is_valid_http_base_url(out.base_url)) {
        out.reason = "image_generation base_url is missing or invalid";
        return out;
    }
    if (out.api_key.empty()) {
        out.reason = "image_generation api_key is empty";
        return out;
    }

    // 末尾斜杠会让拼接出 ".../v1//images/generations"。归一化在这里做一次,
    // 调用方就不用各自小心。
    while (!out.base_url.empty() && out.base_url.back() == '/') {
        out.base_url.pop_back();
    }

    out.ok = true;
    return out;
}

bool is_unattended_answer_path(bool headless_active, QuestionPolicy policy) {
    if (headless_active) return true;
    return policy == QuestionPolicy::Deny;
}

QualityDecision decide_quality(const ImageGenerationConfig& cfg,
                               const std::string& requested_quality,
                               bool headless_active,
                               QuestionPolicy policy,
                               bool has_question_channel) {
    const Quality fallback = parse_quality(cfg.default_quality, Quality::Standard);
    QualityDecision decision;
    decision.requested = parse_quality(requested_quality, fallback);
    decision.quality = decision.requested;

    if (decision.requested == Quality::Standard) {
        return decision;  // 便宜档不打扰用户
    }

    if (is_unattended_answer_path(headless_active, policy)) {
        decision.quality = Quality::Standard;
        decision.downgrade = DowngradeReason::Unattended;
        return decision;
    }

    if (!has_question_channel) {
        decision.quality = Quality::Standard;
        decision.downgrade = DowngradeReason::QuestionUnavailable;
        return decision;
    }

    decision.needs_confirmation = true;
    return decision;
}

nlohmann::json build_cost_confirmation_payload(const ImageGenerationConfig& cfg,
                                               Quality requested) {
    const std::string standard_model = model_for_quality(cfg, Quality::Standard);
    const std::string requested_model = model_for_quality(cfg, requested);

    nlohmann::json options = nlohmann::json::array();
    // 第一项必须是降级 —— 所有「超时自动采纳第一项」的路径靠这个收敛。
    options.push_back({
        {"label", "改用标准分辨率"},
        {"value", "改用标准分辨率"},
        {"description",
         "用 " + standard_model + " 生成,成本最低。(推荐)"},
    });
    options.push_back({
        {"label", std::string("继续用") + quality_label(requested)},
        {"value", std::string("继续用") + quality_label(requested)},
        {"description",
         "用 " + requested_model + " 生成,单张成本明显更高。"},
    });

    nlohmann::json question = {
        {"id", kCostQuestionId},
        {"text", kCostQuestionId},
        {"header", "图像成本"},
        {"options", options},
        {"multiSelect", false},
    };
    return nlohmann::json::array({question});
}

bool confirmation_kept_high_quality(const nlohmann::json& response,
                                    const ImageGenerationConfig& cfg,
                                    Quality requested) {
    (void)cfg;
    if (!response.is_object()) return false;
    // 取消 / 超时 / 结构不对 —— 一律按降级处理,成本确认永远 fail 到便宜的一侧。
    if (response.value("cancelled", false)) return false;
    if (response.value("timed_out", false)) return false;
    if (!response.contains("answers") || !response["answers"].is_array()) return false;

    const std::string keep_label =
        std::string("继续用") + quality_label(requested);
    for (const auto& answer : response["answers"]) {
        if (!answer.is_object()) continue;
        if (answer.value("question_id", std::string{}) != kCostQuestionId) continue;
        if (!answer.contains("selected") || !answer["selected"].is_array()) continue;
        for (const auto& selected : answer["selected"]) {
            if (selected.is_string() && selected.get<std::string>() == keep_label) {
                return true;
            }
        }
    }
    return false;
}

} // namespace acecode::image_generation
