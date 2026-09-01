#include "image_generate_tool.hpp"

#include "image_generation_client.hpp"
#include "image_generation_policy.hpp"
#include "../tool_icons.hpp"
#include "../../headless/headless_mode.hpp"
#include "../../session/output_attachments.hpp"
#include "../../session/session_manager.hpp"
#include "../../session/session_storage.hpp"
#include "../../utils/encoding.hpp"
#include "../../utils/logger.hpp"
#include "../../utils/tool_errors.hpp"
#include "../../utils/utf8_path.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace acecode {

namespace image_generation {
namespace {

constexpr std::size_t kMaxReferenceImages = 5;
// 模型可见文本里改写提示词的截断长度。约 100 token/张 —— 换来的是模型能看出
// 上游往提示词里加了什么,从而在下一轮显式压制(见 design.md 决策 10)。
constexpr std::size_t kRevisedPromptPreviewChars = 200;

const char* const kToolName = "image_generate";

const char* const kToolDescription =
    "Generate an image from a text description, or edit existing images "
    "according to instructions. Use it when the user asks for a picture, "
    "diagram, illustration, mockup, or asks to modify an image.\n"
    "\n"
    "Modes:\n"
    "- Omit `reference_image_paths` to generate a brand new image.\n"
    "- Provide `reference_image_paths` (up to 5 local image files) to edit "
    "them; the instruction goes in `prompt`.\n"
    "\n"
    "Guidelines:\n"
    "- Generation takes 20-60 seconds. Do not retry while a call is running.\n"
    "- The generated image is already shown to the user as an attachment. Do "
    "NOT re-render it in your final reply as a Markdown image or file link.\n"
    "- The image is saved with the session; if the user wants to keep it "
    "long-term, copy it elsewhere and leave the original in place.\n"
    "- Image dimensions and aspect ratio are chosen by the model from the "
    "prompt. Describe the framing you want in words (e.g. \"wide landscape "
    "banner\") instead of asking for pixel sizes.\n"
    "- `quality` raises resolution and cost. Use `standard` unless the user "
    "asked for something print-sized or highly detailed; higher tiers require "
    "the user to confirm the extra cost.";

bool is_supported_reference_image(const std::filesystem::path& path) {
    std::string ext = path_to_utf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".webp" || ext == ".gif" || ext == ".bmp";
}

struct ReferenceResolution {
    bool ok = true;
    std::string error;
    std::vector<std::string> paths;
};

ReferenceResolution resolve_reference_images(const nlohmann::json& args,
                                             const std::string& cwd) {
    ReferenceResolution out;
    if (!args.contains("reference_image_paths")) return out;
    const auto& node = args["reference_image_paths"];
    if (node.is_null()) return out;
    if (!node.is_array()) {
        out.ok = false;
        out.error = ToolErrors::invalid_parameter(
            "reference_image_paths", "must be an array of file paths");
        return out;
    }
    if (node.size() > kMaxReferenceImages) {
        out.ok = false;
        out.error = ToolErrors::invalid_parameter(
            "reference_image_paths",
            "at most " + std::to_string(kMaxReferenceImages) +
                " reference images are supported");
        return out;
    }

    namespace fs = std::filesystem;
    for (const auto& item : node) {
        if (!item.is_string() || item.get<std::string>().empty()) {
            out.ok = false;
            out.error = ToolErrors::invalid_parameter(
                "reference_image_paths", "entries must be non-empty file paths");
            return out;
        }
        const std::string raw = item.get<std::string>();
        fs::path resolved = path_from_utf8(raw);
        if (resolved.is_relative() && !cwd.empty()) {
            resolved = path_from_utf8(cwd) / resolved;
        }
        resolved = resolved.lexically_normal();

        std::error_code ec;
        if (!fs::exists(resolved, ec) || ec) {
            out.ok = false;
            out.error = ToolErrors::file_not_found(path_to_utf8(resolved), cwd);
            return out;
        }
        if (!fs::is_regular_file(resolved, ec) || ec) {
            out.ok = false;
            out.error = ToolErrors::path_not_regular_file(path_to_utf8(resolved));
            return out;
        }
        if (!is_supported_reference_image(resolved)) {
            out.ok = false;
            out.error = ToolErrors::invalid_parameter(
                "reference_image_paths",
                "unsupported image type for " + path_to_utf8(resolved) +
                    " (supported: .png, .jpg, .jpeg, .webp, .gif, .bmp)");
            return out;
        }
        out.paths.push_back(path_to_utf8(resolved));
    }
    return out;
}

std::string extension_for_mime(const std::string& mime) {
    if (mime == "image/jpeg" || mime == "image/jpg") return ".jpg";
    if (mime == "image/webp") return ".webp";
    if (mime == "image/gif") return ".gif";
    if (mime == "image/bmp") return ".bmp";
    return ".png";
}

ResolvedQuestionPolicy effective_policy(const ToolContext& ctx) {
    if (ctx.question_policy) return ctx.question_policy();
    return ResolvedQuestionPolicy{};
}

// 落盘:把 data_url 描述符交给现有的 output attachment 通道。
// 在工具内做而不是等 AgentLoop 统一做,是因为模型可见文本里要给出保存路径 ——
// 后续「再改一下这张」靠这个路径回传 reference_image_paths。已 materialize 的
// 记录再经 AgentLoop 那次是幂等的(stored_attachment_json_from 原样透传)。
struct StoredImage {
    bool ok = false;
    nlohmann::json attachments = nlohmann::json::array();
    std::string saved_path;
    std::string warning;
};

StoredImage store_generated_image(const ToolContext& ctx,
                                  const ImageResponse& response,
                                  const std::string& revised_prompt) {
    StoredImage out;
    if (!ctx.session_manager) {
        out.warning = "no active session storage for the generated image";
        return out;
    }

    const std::string session_id = ctx.session_manager->ensure_active_session_id();
    const std::string project_dir = SessionStorage::get_project_dir(ctx.cwd);
    if (session_id.empty() || project_dir.empty()) {
        out.warning = "no active session storage for the generated image";
        return out;
    }

    nlohmann::json descriptor = {
        {"name", std::string("generated-image") + extension_for_mime(response.mime_type)},
        {"mime_type", response.mime_type},
        {"data_url", "data:" + response.mime_type + ";base64," + response.b64_data},
    };
    if (!revised_prompt.empty()) {
        // 完整版存这里 —— 界面可在图片详情处展示,不占模型上下文。
        descriptor["metadata"] = nlohmann::json{{"revised_prompt", revised_prompt}};
    }

    auto materialized = materialize_output_attachments(
        nlohmann::json::array({descriptor}),
        project_dir,
        session_id,
        // 只发 data_url 描述符,本地路径分支不会走到;真走到了就拒绝。
        [](const std::string&) -> std::string {
            return "local image paths are not accepted here";
        },
        ctx.cwd);

    if (materialized.attachments.is_array() && !materialized.attachments.empty()) {
        out.ok = true;
        out.attachments = std::move(materialized.attachments);
        const auto& first = out.attachments[0];
        if (first.is_object() && first.contains("path") && first["path"].is_string()) {
            out.saved_path = first["path"].get<std::string>();
        }
    }
    if (!materialized.warnings.empty()) {
        out.warning = materialized.warnings.front();
    }
    return out;
}

std::string downgrade_note(DowngradeReason reason) {
    switch (reason) {
        case DowngradeReason::Unattended:
            return "已降级为标准分辨率:当前会话无人值守,无法确认更高的生成成本。";
        case DowngradeReason::UserChoice:
            return "已降级为标准分辨率:用户选择了成本更低的档位。";
        case DowngradeReason::QuestionUnavailable:
            return "已降级为标准分辨率:当前运行环境没有可用的确认通道。";
        case DowngradeReason::None:
        default:
            return {};
    }
}

ToolResult execute_image_generate(const std::string& arguments_json,
                                  const ToolContext& ctx,
                                  const ImageGenerationConfig& cfg,
                                  const ResolvedEndpoint& endpoint) {
    nlohmann::json args;
    try {
        args = arguments_json.empty() ? nlohmann::json::object()
                                      : nlohmann::json::parse(arguments_json);
    } catch (const std::exception& e) {
        return ToolResult{
            std::string("[Error] Failed to parse tool arguments: ") + e.what(),
            false};
    }
    if (!args.is_object()) {
        return ToolResult{"[Error] Tool arguments must be a JSON object", false};
    }

    const std::string prompt =
        args.contains("prompt") && args["prompt"].is_string()
            ? args["prompt"].get<std::string>()
            : std::string{};
    if (prompt.empty()) {
        return ToolResult{ToolErrors::missing_parameter("prompt"), false};
    }

    ReferenceResolution refs = resolve_reference_images(args, ctx.cwd);
    if (!refs.ok) return ToolResult{refs.error, false};

    const std::string requested_quality =
        args.contains("quality") && args["quality"].is_string()
            ? args["quality"].get<std::string>()
            : std::string{};

    QualityDecision decision = decide_quality(
        cfg,
        requested_quality,
        headless::active(),
        effective_policy(ctx).policy,
        static_cast<bool>(ctx.ask_user_questions));

    if (decision.needs_confirmation) {
        nlohmann::json response =
            ctx.ask_user_questions(build_cost_confirmation_payload(cfg, decision.requested));
        if (!confirmation_kept_high_quality(response, cfg, decision.requested)) {
            decision.quality = Quality::Standard;
            decision.downgrade = DowngradeReason::UserChoice;
        }
    }

    if (ctx.abort_flag && ctx.abort_flag->load()) {
        return ToolResult{"[Aborted] image generation cancelled before starting",
                          false};
    }

    ImageRequest request;
    request.base_url = endpoint.base_url;
    request.api_key = endpoint.api_key;
    request.model = model_for_quality(cfg, decision.quality);
    request.prompt = prompt;
    request.reference_image_paths = refs.paths;
    request.timeout_ms = cfg.timeout_ms;

    const bool editing = !refs.paths.empty();
    LOG_INFO(std::string("[image_generate] ") + (editing ? "edit" : "generate") +
             " model=" + request.model +
             " refs=" + std::to_string(refs.paths.size()));

    ImageResponse response = execute_image_request(request, ctx.abort_flag);

    if (response.aborted) {
        return ToolResult{"[Aborted] " + response.error, false};
    }
    if (!response.ok) {
        std::string message = response.quota_error
                                  ? "[Error] Image generation was refused for "
                                    "quota or rate-limit reasons: "
                                  : "[Error] Image generation failed: ";
        return ToolResult{message + response.error, false};
    }

    // 附件无条件带上 —— 视觉降级由序列化层统一处理(design.md 决策 9)。
    // 在这里再判一次会把结果写死进 canonical 消息,用户中途切到无视觉模型后
    // 历史里的图仍会被发出去。
    StoredImage stored = store_generated_image(ctx, response, response.revised_prompt);

    std::string text = editing ? "已编辑图像" : "已生成图像";
    text += ",档位 ";
    text += quality_name(decision.quality);
    if (response.width > 0 && response.height > 0) {
        text += ",尺寸 " + std::to_string(response.width) + "x" +
                std::to_string(response.height);
    }
    text += "。";

    const std::string note = downgrade_note(decision.downgrade);
    if (!note.empty()) text += "\n" + note;

    if (stored.ok && !stored.saved_path.empty()) {
        text += "\n保存位置:" + stored.saved_path;
        text += "\n该文件随会话数据保存;要长期保留请复制到别处,不要移动原件。";
    } else if (!stored.warning.empty()) {
        // 图已经生成、钱已经花了 —— 落盘失败不该让整次调用失败。
        text += "\n[警告] 图片未能保存:" + stored.warning;
        LOG_WARN("[image_generate] failed to store generated image: " +
                 stored.warning);
    }

    if (!response.revised_prompt.empty()) {
        text += "\n上游实际使用的提示词(节选):" +
                truncate_utf8_prefix(response.revised_prompt,
                                     kRevisedPromptPreviewChars);
    }

    text +=
        "\n图片已经作为附件展示给用户,最终回复中不要再用 Markdown 图片或"
        "文件链接渲染一次。";

    ToolSummary summary;
    summary.verb = editing ? "Edited" : "Generated";
    summary.object = truncate_utf8_prefix(prompt, 60);
    summary.icon = tool_icon(kToolName);
    summary.metrics.emplace_back("quality", quality_name(decision.quality));
    if (response.width > 0 && response.height > 0) {
        summary.metrics.emplace_back(
            "size", std::to_string(response.width) + "x" +
                        std::to_string(response.height));
    }

    ToolResult result{text, true};
    result.summary = std::move(summary);
    if (stored.ok) result.attachments = std::move(stored.attachments);
    if (!stored.ok && !stored.warning.empty()) {
        result.attachment_warnings.push_back(stored.warning);
    }
    // 图像行的标题按来源区分「已生成 / 已编辑」,前端投影层读这一位。
    result.metadata["image_generation"] = nlohmann::json{
        {"mode", editing ? "edit" : "generate"},
        {"quality", quality_name(decision.quality)},
        {"downgraded", decision.downgrade != DowngradeReason::None},
    };
    return result;
}

nlohmann::json build_parameters_schema() {
    return nlohmann::json{
        {"type", "object"},
        {"required", nlohmann::json::array({"prompt"})},
        {"properties", {
            {"prompt", {
                {"type", "string"},
                {"description",
                 "What to draw, or — when reference images are supplied — how to "
                 "change them. Describe framing and aspect ratio in words; pixel "
                 "sizes are not configurable."},
            }},
            {"quality", {
                {"type", "string"},
                {"enum", nlohmann::json::array({"standard", "high", "ultra"})},
                {"description",
                 "Resolution tier. `standard` is the default and cheapest. "
                 "`high` and `ultra` cost noticeably more and require the user "
                 "to confirm; they are automatically downgraded when nobody is "
                 "available to confirm."},
            }},
            {"reference_image_paths", {
                {"type", "array"},
                {"maxItems", kMaxReferenceImages},
                {"items", {{"type", "string"}}},
                {"description",
                 "Up to 5 local image files to edit. Omit entirely when "
                 "generating a brand new image."},
            }},
        }},
    };
}

} // namespace
} // namespace image_generation

std::optional<ToolImpl> create_image_generate_tool(const AppConfig& config) {
    using namespace image_generation;

    const ResolvedEndpoint endpoint = resolve_endpoint(config);
    if (!endpoint.ok) {
        if (config.image_generation.enabled) {
            // enabled 但配不出端点是值得诊断的;disabled 是用户的明确选择,不吵。
            LOG_INFO("[image_generate] tool not registered: " + endpoint.reason);
        }
        return std::nullopt;
    }

    const ImageGenerationConfig cfg = config.image_generation;

    ToolDef definition;
    definition.name = kToolName;
    definition.description = kToolDescription;
    definition.parameters = build_parameters_schema();

    ToolImpl impl;
    impl.definition = std::move(definition);
    impl.execute = [cfg, endpoint](const std::string& arguments_json,
                                   const ToolContext& ctx) {
        return execute_image_generate(arguments_json, ctx, cfg, endpoint);
    };
    // 生成图片是写操作(落盘 + 花钱),但成本把关由高档位的确认承担;
    // 再加一道权限确认是重复打扰(design.md 决策 11)。
    impl.is_read_only = false;
    impl.source = ToolSource::Builtin;
    return impl;
}

} // namespace acecode
