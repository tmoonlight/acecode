#include "settings_mutations.hpp"
#include "config/vocab/permission_mode.hpp"
#include "model_provider_registry.hpp"
#include "saved_models_revision.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <unordered_set>

namespace acecode {
namespace {

SettingsMutationResult finish_mutation(
    ConfigMutationResult mutation,
    const SettingsMutationOptions& options,
    bool publish_saved_models_revision = false) {
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
    if (options.live_config && !publish_saved_models_revision) {
        *options.live_config = mutation.config;
        if (options.on_live_config_published) options.on_live_config_published();
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
            return result;
        }
    }

    if (options.live_config && publish_saved_models_revision) {
        publish_live_config(*options.live_config, mutation.config, true);
        if (options.on_live_config_published) options.on_live_config_published();
    }
    if (options.apply_live) return result;

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
    const SettingsMutationOptions& options,
    bool publish_saved_models_revision = false) {
    return finish_mutation(
        mutate_config(
            std::forward<Mutator>(mutator),
            options.config_path,
            options.live_config),
        options,
        publish_saved_models_revision);
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
        case SavedModelEditError::INVALID_ENDPOINT_MODE:
            return "model endpoint mode is invalid";
        case SavedModelEditError::INVALID_MAX_OUTPUT_TOKENS:
            return "model maximum output token limit is invalid";
        case SavedModelEditError::INVALID_CAPABILITIES_SOURCE:
            return "model capability source is invalid";
        case SavedModelEditError::INVALID_REASONING:
            return "model reasoning options are invalid";
        case SavedModelEditError::INVALID_CREDENTIAL_SOURCE:
            return "saved credential source is missing or incompatible";
        case SavedModelEditError::CREDENTIAL_CONFLICT:
            return "choose exactly one credential update operation";
        case SavedModelEditError::UNSUPPORTED_MODEL_OPTION:
            return "model option is not supported by this provider";
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
            const auto parsed = parse_permission_mode_name(mode);
            if (!parsed) {
                error = "unsupported default permission mode";
                return false;
            }
            const std::string canonical = permission_mode_name(*parsed);
            if (cfg.default_permission_mode == canonical) return false;
            cfg.default_permission_mode = canonical;
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

SettingsMutationResult set_remote_web_enabled(
    bool enabled,
    const SettingsMutationOptions& options) {
    const std::optional<WebConfig> runtime_web = options.live_config
        ? std::optional<WebConfig>(options.live_config->web)
        : std::nullopt;
    auto result = run_mutation(
        [enabled](AppConfig& cfg, std::string&) {
            bool changed = false;
            if (cfg.web.bind != "127.0.0.1") {
                cfg.web.bind = "127.0.0.1";
                changed = true;
            }
            if (cfg.web.remote_enabled != enabled) {
                cfg.web.remote_enabled = enabled;
                changed = true;
            }
            return changed;
        },
        options);
    if (result.ok && runtime_web && options.live_config) {
        const bool saved_remote_enabled =
            options.live_config->web.remote_enabled;
        const int saved_remote_port = options.live_config->web.remote_port;
        options.live_config->web = *runtime_web;
        options.live_config->web.bind = "127.0.0.1";
        options.live_config->web.remote_enabled = saved_remote_enabled;
        options.live_config->web.remote_port = saved_remote_port;
    }
    return result;
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

SettingsMutationResult set_memory_settings(
    const MemoryConfig& memory,
    const SettingsMutationOptions& options) {
    return run_mutation(
        [memory](AppConfig& cfg, std::string& error) {
            const auto& summary = memory.summary;
            if (memory.max_index_bytes == 0 || memory.max_index_bytes > 1024 * 1024) {
                error = "memory.max_index_bytes must be between 1 and 1048576";
                return false;
            }
            if (summary.idle_minutes < 5 || summary.idle_minutes > 1440) {
                error = "memory.summary.idle_minutes must be between 5 and 1440";
                return false;
            }
            if (summary.max_session_age_days < 1 || summary.max_session_age_days > 90) {
                error = "memory.summary.max_session_age_days must be between 1 and 90";
                return false;
            }
            if (!summary.model_name.empty() &&
                std::none_of(cfg.saved_models.begin(), cfg.saved_models.end(),
                             [&summary](const ModelProfile& profile) {
                                 return profile.name == summary.model_name;
                             })) {
                error = "unknown saved model: " + summary.model_name;
                return false;
            }
            const auto& cur = cfg.memory;
            if (cur.enabled == memory.enabled && cur.max_index_bytes == memory.max_index_bytes &&
                cur.summary.enabled == summary.enabled &&
                cur.summary.model_name == summary.model_name &&
                cur.summary.idle_minutes == summary.idle_minutes &&
                cur.summary.max_session_age_days == summary.max_session_age_days) {
                return false;
            }
            cfg.memory = memory;
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
            const auto before = cfg.saved_models;
            edit_error = add_saved_model(cfg, draft);
            if (edit_error != SavedModelEditError::OK) {
                error = saved_model_error(edit_error);
                return false;
            }
            return !saved_model_lists_equal(before, cfg.saved_models);
        },
        options,
        true);
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
            const auto before = cfg.saved_models;
            edit_error =
                update_saved_model(cfg, old_name, draft);
            if (edit_error != SavedModelEditError::OK) {
                error = saved_model_error(edit_error);
                return false;
            }
            return !saved_model_lists_equal(before, cfg.saved_models);
        },
        options,
        true);
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
            const auto before = cfg.saved_models;
            edit_error = remove_saved_model(cfg, name);
            if (edit_error != SavedModelEditError::OK) {
                error = saved_model_error(edit_error);
                return false;
            }
            return !saved_model_lists_equal(before, cfg.saved_models);
        },
        options,
        true);
    if (edit_error != SavedModelEditError::OK) {
        result.error_code = to_string(edit_error);
    }
    return result;
}

SettingsMutationResult reorder_saved_models_setting(
    const std::vector<std::string>& names,
    const SettingsMutationOptions& options) {
    bool invalid_order = false;
    auto result = run_mutation(
        [&names, &invalid_order](AppConfig& cfg, std::string& error) {
            const auto enabled = [](const ModelProfile& profile) {
                return is_runtime_model_provider_enabled(profile.provider);
            };
            const auto visible_count = static_cast<std::size_t>(
                std::count_if(cfg.saved_models.begin(), cfg.saved_models.end(), enabled));
            std::unordered_set<std::string> seen;
            std::vector<ModelProfile> ordered_visible;
            ordered_visible.reserve(names.size());
            if (names.size() == visible_count) {
                for (const auto& name : names) {
                    const auto found = std::find_if(
                        cfg.saved_models.begin(), cfg.saved_models.end(),
                        [&name, &enabled](const ModelProfile& profile) {
                            return profile.name == name && enabled(profile);
                        });
                    if (found == cfg.saved_models.end() || !seen.insert(name).second) break;
                    ordered_visible.push_back(*found);
                }
            }
            if (ordered_visible.size() != visible_count ||
                ordered_visible.size() != names.size()) {
                invalid_order = true;
                error = "model order must contain every visible saved model exactly once";
                return false;
            }
            // 旧配置可保留停用 Provider；它们未出现在 GET /api/models 中。
            auto reordered = cfg.saved_models;
            std::size_t index = 0;
            for (auto& profile : reordered) {
                if (enabled(profile)) profile = ordered_visible[index++];
            }
            if (saved_model_lists_equal(cfg.saved_models, reordered)) return false;
            cfg.saved_models = std::move(reordered);
            return true;
        },
        options,
        true);
    if (invalid_order) result.error_code = "MODEL_ORDER_CONFLICT";
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
