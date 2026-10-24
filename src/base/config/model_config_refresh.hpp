#pragma once

#include "config.hpp"

#include <string>

namespace acecode {

// Reload model profiles from config.json into a running process, applied only
// when the file's bytes differ from the last refresh of this path. The daemon
// calls it before each message and each model-list refresh, so hand edits
// (a changed API key, a replaced profile set) take effect without a restart.
// Uses the same rules as refresh_default_session_preferences_from_config: a
// structurally changed saved_models list advances the saved-models revision
// and open sessions rebuild their Provider before the next request. Unreadable
// or invalid content (e.g. a half-written edit) is logged once and leaves cfg
// unchanged. The caller must hold the live config lock exclusively.
bool refresh_model_config_if_changed(AppConfig& cfg, const std::string& explicit_path = {});

// Loader rules shared by load_config and the refreshes above.
// A default model or top-level provider that points at a disabled provider
// falls back to an enabled profile. Defined in config.cpp.
void sanitize_disabled_model_providers(AppConfig& cfg);
// Canonical default_permission_mode name; aliases map to their canonical name
// and unknown values fall back to "default" with a warning.
std::string normalize_permission_mode_name(std::string value);

} // namespace acecode
