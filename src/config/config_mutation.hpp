#pragma once

#include "config.hpp"

#include <functional>
#include <string>

namespace acecode {

// A mutator returns true when the candidate config should be persisted. It may
// return false with an empty error for a successful no-op, or with a non-empty
// error for validation failure.
using ConfigMutator = std::function<bool(AppConfig& candidate, std::string& error)>;

enum class ConfigMutationErrorKind {
    None,
    Validation,
    Persistence,
};

struct ConfigMutationResult {
    bool ok = false;
    bool changed = false;
    ConfigMutationErrorKind error_kind = ConfigMutationErrorKind::None;
    AppConfig config;
    std::string error;
};

// Serialize config updates across threads and processes, reload the latest
// on-disk value without environment overrides, apply one focused mutation, and
// atomically replace the file. Errors never include serialized config content.
ConfigMutationResult mutate_config(
    const ConfigMutator& mutator,
    const std::string& explicit_path = {},
    const AppConfig* seed_if_missing = nullptr);

} // namespace acecode
