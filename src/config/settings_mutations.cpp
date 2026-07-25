#include "settings_mutations.hpp"

#include <algorithm>
#include <utility>

namespace acecode {
namespace {

SettingsMutationResult finish_mutation(
    ConfigMutationResult mutation,
    const SettingsMutationOptions& options) {
    SettingsMutationResult result;
    result.config = mutation.config;
    result.changed = mutation.changed;
    if (!mutation.ok) {
        result.error_kind =
            mutation.error_kind == ConfigMutationErrorKind::Validation
            ? SettingsMutationErrorKind::Validation
            : SettingsMutationErrorKind::Persistence;
        result.error = std::move(mutation.error);
        return result;
    }

    result.ok = true;
    result.persisted = mutation.changed;
    if (options.live_config) {
        *options.live_config = mutation.config;
    }
    if (!mutation.changed) {
        result.runtime_status = SettingsRuntimeStatus::Unchanged;
        return result;
    }

    if (options.apply_live) {
        std::string runtime_error;
        if (options.apply_live(mutation.config, runtime_error)) {
            result.runtime_status = SettingsRuntimeStatus::AppliedLive;
        } else {
            result.runtime_status = SettingsRuntimeStatus::RuntimeApplyFailed;
            result.error_kind = SettingsMutationErrorKind::RuntimeApply;
            result.error = runtime_error.empty()
                ? "setting was saved but could not be applied live"
                : std::move(runtime_error);
        }
        return result;
    }

    if (options.live_config) {
        result.runtime_status = SettingsRuntimeStatus::AppliedLive;
    } else if (options.restart_required_without_live_apply) {
        result.runtime_status = SettingsRuntimeStatus::RestartRequired;
    } else {
        result.runtime_status = SettingsRuntimeStatus::AppliedLive;
    }
    return result;
}

template <typename Mutator>
SettingsMutationResult run_mutation(
    Mutator&& mutator,
    const SettingsMutationOptions& options) {
    return finish_mutation(
        mutate_config(
            std::forward<Mutator>(mutator),
            options.config_path,
            options.live_config),
        options);
}

std::string saved_model_error(SavedModelEditError error) {
    switch (error) {
        case SavedModelEditError::OK:
            return {};
        case SavedModelEditError::INVALID_NAME:
            return "model name is invalid";
        case SavedModelEditError::RESERVED_NAME:
            return "model name uses a reserved prefix";
        case SavedModelEditError::NAME_TAKEN:
            return "model name already exists";
        case SavedModelEditError::UNKNOWN_PROVIDER:
            return "model provider is unknown";
        case SavedModelEditError::PROVIDER_DISABLED:
            return "model provider is disabled";
        case SavedModelEditError::MISSING_MODEL:
            return "model identifier is required";
        case SavedModelEditError::MISSING_BASE_URL:
            return "model base URL is required";
        case SavedModelEditError::INVALID_API_KEY:
            return "model API key is required";
        case SavedModelEditError::INVALID_CONTEXT_WINDOW:
            return "model context window is invalid";
        case SavedModelEditError::INVALID_STREAM_TIMEOUT:
            return "model stream timeout is invalid";
        case SavedModelEditError::INVALID_CAPABILITY:
            return "model capability list is invalid";
        case SavedModelEditError::INVALID_REQUEST_HEADER:
            return "model request headers are invalid";
        case SavedModelEditError::NOT_FOUND:
            return "model profile was not found";
        case SavedModelEditError::IN_USE_AS_DEFAULT:
            return "model profile is used by a busy session";
    }
    return "model profile update was rejected";
}

} // namespace

SettingsMutationResult set_default_permission_mode(
    const std::string& mode,
    const SettingsMutationOptions& options) {
    return run_mutation(
        [mode](AppConfig& cfg, std::string& error) {
            if (mode != "default" &&
                mode != "accept-edits" &&
                mode != "plan" &&
                mode != "yolo") {
                error = "unsupported default permission mode";
                return false;
            }
            if (cfg.default_permission_mode == mode) return false;
            cfg.default_permission_mode = mode;
            return true;
        },
        options);
}

SettingsMutationResult set_native_notifications_enabled(
    bool enabled,
    const SettingsMutationOptions& options) {
    return run_mutation(
        [enabled](AppConfig& cfg, std::string&) {
            if (cfg.desktop.notifications.enabled == enabled) return false;
            cfg.desktop.notifications.enabled = enabled;
            return true;
        },
        options);
}

SettingsMutationResult set_tui_theme(
    const std::string& theme,
    const SettingsMutationOptions& options) {
    return run_mutation(
        [theme](AppConfig& cfg, std::string& error) {
            if (theme != "auto" && theme != "dark" && theme != "light") {
                error = "unsupported TUI theme";
                return false;
            }
            if (cfg.tui.theme == theme) return false;
            cfg.tui.theme = theme;
            return true;
        },
        options);
}

SettingsMutationResult set_upgrade_base_url(
    const std::string& base_url,
    const SettingsMutationOptions& options) {
    return run_mutation(
        [base_url](AppConfig& cfg, std::string& error) {
            const std::string normalized =
                normalize_upgrade_base_url(base_url);
            if (!is_valid_upgrade_base_url(normalized)) {
                error = "upgrade service URL must use http or https";
                return false;
            }
            if (cfg.upgrade.base_url == normalized) return false;
            cfg.upgrade.base_url = normalized;
            return true;
        },
        options);
}

SettingsMutationResult set_custom_instructions(
    const std::string& text,
    const SettingsMutationOptions& options) {
    return run_mutation(
        [text](AppConfig& cfg, std::string& error) {
            if (text.size() > kCustomInstructionsMaxBytes) {
                error = "custom instructions exceed the 64 KiB limit";
                return false;
            }
            if (cfg.custom_instructions.text_snapshot() == text) return false;
            cfg.custom_instructions.set_text(text);
            return true;
        },
        options);
}

SettingsMutationResult add_saved_model_setting(
    const SavedModelDraft& draft,
    const SettingsMutationOptions& options) {
    SavedModelEditError edit_error = SavedModelEditError::OK;
    auto result = run_mutation(
        [draft, &edit_error](AppConfig& cfg, std::string& error) {
            edit_error = add_saved_model(cfg, draft);
            if (edit_error != SavedModelEditError::OK) {
                error = saved_model_error(edit_error);
                return false;
            }
            return true;
        },
        options);
    if (edit_error != SavedModelEditError::OK) {
        result.error_code = to_string(edit_error);
    }
    return result;
}

SettingsMutationResult update_saved_model_setting(
    const std::string& old_name,
    const SavedModelDraft& draft,
    const SettingsMutationOptions& options) {
    SavedModelEditError edit_error = SavedModelEditError::OK;
    auto result = run_mutation(
        [old_name, draft, &edit_error](AppConfig& cfg, std::string& error) {
            edit_error =
                update_saved_model(cfg, old_name, draft);
            if (edit_error != SavedModelEditError::OK) {
                error = saved_model_error(edit_error);
                return false;
            }
            return true;
        },
        options);
    if (edit_error != SavedModelEditError::OK) {
        result.error_code = to_string(edit_error);
    }
    return result;
}

SettingsMutationResult remove_saved_model_setting(
    const std::string& name,
    const std::function<bool(const std::string&)>& is_used_by_busy_session,
    const SettingsMutationOptions& options) {
    if (is_used_by_busy_session && is_used_by_busy_session(name)) {
        SettingsMutationResult result;
        result.error_kind = SettingsMutationErrorKind::Validation;
        result.error_code = "MODEL_IN_USE";
        result.error = saved_model_error(SavedModelEditError::IN_USE_AS_DEFAULT);
        return result;
    }
    SavedModelEditError edit_error = SavedModelEditError::OK;
    auto result = run_mutation(
        [name, &edit_error](AppConfig& cfg, std::string& error) {
            edit_error = remove_saved_model(cfg, name);
            if (edit_error != SavedModelEditError::OK) {
                error = saved_model_error(edit_error);
                return false;
            }
            return true;
        },
        options);
    if (edit_error != SavedModelEditError::OK) {
        result.error_code = to_string(edit_error);
    }
    return result;
}

SettingsMutationResult set_default_model_setting(
    const std::string& name,
    const SettingsMutationOptions& options) {
    bool not_found = false;
    auto result = run_mutation(
        [name, &not_found](AppConfig& cfg, std::string& error) {
            const auto found = std::find_if(
                cfg.saved_models.begin(),
                cfg.saved_models.end(),
                [&name](const ModelProfile& profile) {
                    return profile.name == name;
                });
            if (found == cfg.saved_models.end()) {
                not_found = true;
                error = "model profile was not found";
                return false;
            }
            if (cfg.default_model_name == name) return false;
            cfg.default_model_name = name;
            return true;
        },
        options);
    if (not_found) result.error_code = "NOT_FOUND";
    return result;
}

} // namespace acecode
