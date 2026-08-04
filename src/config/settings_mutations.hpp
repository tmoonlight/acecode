#pragma once

#include "config_mutation.hpp"
#include "saved_models_editor.hpp"

#include <functional>
#include <string>

namespace acecode {

enum class SettingsMutationErrorKind {
    None,
    Validation,
    Persistence,
    RuntimeApply,
};

enum class SettingsRuntimeStatus {
    Unchanged,
    AppliedLive,
    RestartRequired,
    RuntimeApplyFailed,
};

using SettingsRuntimeApplyHook =
    std::function<bool(const AppConfig& saved, std::string& error)>;

struct SettingsMutationOptions {
    std::string config_path;
    AppConfig* live_config = nullptr;
    SettingsRuntimeApplyHook apply_live;
    bool restart_required_without_live_apply = true;
};

struct SettingsMutationResult {
    bool ok = false;
    bool persisted = false;
    bool changed = false;
    SettingsMutationErrorKind error_kind = SettingsMutationErrorKind::None;
    SettingsRuntimeStatus runtime_status = SettingsRuntimeStatus::Unchanged;
    AppConfig config;
    std::string error_code;
    std::string error;
};

SettingsMutationResult set_default_permission_mode(
    const std::string& mode,
    const SettingsMutationOptions& options = {});

SettingsMutationResult set_native_notifications_enabled(
    bool enabled,
    const SettingsMutationOptions& options = {});

SettingsMutationResult set_remote_web_enabled(
    bool enabled,
    const SettingsMutationOptions& options = {});

SettingsMutationResult set_tui_theme(
    const std::string& theme,
    const SettingsMutationOptions& options = {});

SettingsMutationResult set_upgrade_base_url(
    const std::string& base_url,
    const SettingsMutationOptions& options = {});

SettingsMutationResult set_custom_instructions(
    const std::string& text,
    const SettingsMutationOptions& options = {});

SettingsMutationResult add_saved_model_setting(
    const SavedModelDraft& draft,
    const SettingsMutationOptions& options = {});

SettingsMutationResult update_saved_model_setting(
    const std::string& old_name,
    const SavedModelDraft& draft,
    const SettingsMutationOptions& options = {});

SettingsMutationResult remove_saved_model_setting(
    const std::string& name,
    const std::function<bool(const std::string&)>& is_used_by_busy_session,
    const SettingsMutationOptions& options = {});

SettingsMutationResult set_default_model_setting(
    const std::string& name,
    const SettingsMutationOptions& options = {});

} // namespace acecode
