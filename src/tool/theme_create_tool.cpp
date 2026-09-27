#include "theme_create_tool.hpp"

#include "permissions/interaction_mode.hpp"
#include "session/session_manager.hpp"
#include "themes/theme_drafts.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"

namespace acecode {
using nlohmann::json;

ToolImpl create_theme_create_tool(std::filesystem::path theme_root) {
    ToolImpl impl;
    impl.definition.name = "theme_create";
    impl.definition.description =
        "Create and install an ACECode background/color theme with mandatory human approval. "
        "First use the ai-theme skill to ask customization level, artwork mode and light/dark/free preference. "
        "Then show all 28 palette colors and any logo/title colors, title-bar settings, background colors and opacities, "
        "then call action=palette with name, mode, colors and optional appearance; "
        "continue only when confirmed=true. Prepare standalone backgrounds and an ACECode UI prototype: "
        "use image_generate only for the selected bitmap mode, or local SVG/user-provided images without generation. "
        "If image generation is unavailable, preview with HTML and ACECode Browser and capture a PNG preview. "
        "Show the result and call action=prototype with draft_id, background_path, preview_path and optional "
        "session_background_path/user_message_background_path. Message artwork affects only user messages. "
        "Only after confirmed=true call action=install "
        "with draft_id alone. install packages and applies the approved theme; previous themes remain available. "
        "Use action=status with draft_id to resume, or omit draft_id to list this session's drafts. "
        "Never replace real user approval with a boolean or auto-answer. Pending confirmation means stop "
        "and wait for user feedback, not retry automatically. Palette/prototype updates invalidate prior approval. "
        "This tool never edits application code. Drafts and resources are kept in ACECode's data directory.";
    impl.definition.parameters = {
        {"type", "object"}, {"required", json::array({"action"})}, {"additionalProperties", false},
        {"properties", {
            {"action", {{"type", "string"}, {"enum", {"palette", "prototype", "install", "status"}}}},
            {"draft_id", {{"type", "string"}, {"description", "ID returned by palette. Omit only to create a palette or list drafts."}}},
            {"name", {{"type", "string"}, {"description", "Palette action: human-readable theme name."}}},
            {"mode", {{"type", "string"}, {"enum", {"light", "dark"}}}},
            {"colors", {{"type", "object"}, {"description", "Palette action: all 28 ACECode HEX tokens (#RRGGBB), no CSS."},
                {"additionalProperties", {{"type", "string"}}}}},
            {"appearance", {{"type", "object"}, {"additionalProperties", false},
                {"description", "Palette action: optional overrides shown with the colors. Omit to remove prior overrides when revising a palette."},
                {"properties", {
                    {"logo_color", {{"type", "string"}, {"pattern", "^#[0-9A-Fa-f]{6}$"},
                        {"description", "ACECode home and sidebar logo main color; white glyphs and logo shape remain."}}},
                    {"home_title_color", {{"type", "string"}, {"pattern", "^#[0-9A-Fa-f]{6}$"},
                        {"description", "Home greeting title color. Other headings and body text retain the palette."}}},
                    {"extend_to_titlebar", {{"type", "boolean"},
                        {"description", "Extend the home background behind the main-column title bar. Dark mode uses white right-side controls."}}},
                    {"home_composer_opacity", {{"type", "number"}, {"minimum", 0}, {"maximum", 1},
                        {"description", "Home input background opacity; text/icons remain opaque. 1 means opaque, 0 transparent."}}},
                    {"home_background_opacity", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                    {"session_background_opacity", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                    {"user_message_background_opacity", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                    {"home_background_color", {{"type", "string"}, {"pattern", "^#[0-9A-Fa-f]{6}$"}}},
                    {"session_background_color", {{"type", "string"}, {"pattern", "^#[0-9A-Fa-f]{6}$"}}},
                    {"user_message_background_color", {{"type", "string"}, {"pattern", "^#[0-9A-Fa-f]{6}$"}}}
                }}}},
            {"background_path", {{"type", "string"}, {"description", "Prototype action: local standalone background image shown to user."}}},
            {"session_background_path", {{"type", "string"}, {"description", "Prototype action: optional standalone conversation background image."}}},
            {"user_message_background_path", {{"type", "string"}, {"description", "Prototype action: optional background for user-sent message bubbles only; never assistant replies."}}},
            {"preview_path", {{"type", "string"}, {"description", "Prototype action: local ACECode UI prototype shown to user."}}}
        }}
    };
    impl.execute = [theme_root = std::move(theme_root)](const std::string& arguments, const ToolContext& ctx) {
        try {
            if (!ctx.session_manager || ctx.session_manager->current_session_id().empty())
                throw themes::ThemeError(400, "THEME_SESSION_REQUIRED", "An active session is required");
            const auto args = json::parse(arguments);
            if (args.value("action", "") != "status" &&
                (headless::active() || !ctx.ask_user_questions ||
                 (ctx.goal_unattended_active && ctx.goal_unattended_active()) ||
                 (ctx.question_policy && ctx.question_policy().policy == QuestionPolicy::Deny) ||
                 (ctx.abort_flag && ctx.abort_flag->load())))
                throw themes::ThemeError(409, "THEME_INTERACTION_REQUIRED",
                    "A connected interactive user must confirm this theme. Resume the draft in an interactive session.");
            auto root = theme_root.empty() ? path_from_utf8(get_acecode_dir()) / "themes" : theme_root;
            themes::ThemeDraftStore drafts(std::move(root));
            const auto confirm = [&](const json& payload) -> json {
                if (headless::active() || !ctx.ask_user_questions ||
                    (ctx.goal_unattended_active && ctx.goal_unattended_active()) ||
                    (ctx.question_policy && ctx.question_policy().policy == QuestionPolicy::Deny) ||
                    (ctx.abort_flag && ctx.abort_flag->load()))
                    return {{"cancelled", true}, {"reason", "A connected interactive user must confirm this theme."}};
                auto response = ctx.ask_user_questions(payload);
                if (ctx.abort_flag && ctx.abort_flag->load()) return {{"cancelled", true}};
                return response;
            };
            const auto state = drafts.execute(args, ctx.session_manager->current_session_id(), ctx.cwd, confirm);
            ToolResult result{state.dump(2), true};
            if (args.value("action", "") == "install" && state.value("stage", "") == "installed") {
                result.metadata["theme_created"] = {{"id", state.at("id")}, {"version", state.at("version")},
                    {"name", state.at("name")}, {"apply", true}};
                result.output += "\nTheme installed. The previous theme remains available in Settings > Appearance.";
            }
            return result;
        } catch (const themes::ThemeError& error) {
            return ToolResult{json{{"error", error.code}, {"message", error.what()}, {"error_path", error.path}}.dump(), false};
        } catch (const json::exception&) {
            return ToolResult{"[Error] Invalid theme_create arguments or draft data. Use status to inspect the draft.", false};
        } catch (const std::exception& error) {
            return ToolResult{std::string("[Error] Theme operation failed: ") + error.what(), false};
        }
    };
    // Installation mutates only user theme data, but retains the standard tool
    // permission gate in addition to the two workflow-specific approvals.
    impl.is_read_only = false;
    return impl;
}

} // namespace acecode
