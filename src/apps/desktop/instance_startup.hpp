#pragma once

#include <string>

namespace acecode::desktop {

// Maximum length accepted from the instance-identity override. Longer values
// are rejected outright rather than truncated, so the accepted value is always
// exactly what the caller supplied.
constexpr std::size_t kMaxInstanceIdLength = 64;

struct InstanceStartupPlan {
    bool start = false;
    bool primary = false;
    std::string run_subdirectory;
};

// Accept only characters that are safe inside every supported filesystem path,
// and reject the all-dots spellings that would resolve to a parent directory.
// The caller supplies this value from outside the process; anything else falls
// back to a generated identifier instead of being rewritten.
inline bool is_valid_instance_id(const std::string& value) {
    if (value.empty() || value.size() > kMaxInstanceIdLength) return false;
    bool all_dots = true;
    for (char character : value) {
        const bool lower = character >= 'a' && character <= 'z';
        const bool upper = character >= 'A' && character <= 'Z';
        const bool digit = character >= '0' && character <= '9';
        const bool symbol = character == '_' || character == '.' || character == '-';
        if (!(lower || upper || digit || symbol)) return false;
        if (character != '.') all_dots = false;
    }
    // "." and ".." are valid path syntax but not valid identities: they name the
    // current or parent directory, so the instance run directory would escape
    // its own container instead of staying inside it.
    return !all_dots;
}

inline char ascii_lower(char character) {
    return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character;
}

// Whitelist parsing: only unambiguous affirmative spellings enable the
// override. Anything else - including "0" and empty - keeps it disabled, so a
// stray value can never silently turn the behavior on.
inline bool parse_allow_multiple_instances(const std::string& value) {
    if (value.empty()) return false;
    std::string normalized;
    normalized.reserve(value.size());
    for (char character : value) normalized.push_back(ascii_lower(character));
    return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

// The primary retains the stable singleton and daemon directory. Additional
// developer instances must never attach to, replace or stop its daemon.
inline InstanceStartupPlan plan_instance_startup(
    bool allow_multiple_instances,
    bool singleton_acquired,
    const std::string& instance_id,
    const std::string& development_identity = {}) {
    if (singleton_acquired) return {true, true, "desktop-shared"};
    if (!allow_multiple_instances) return {};
    const std::string prefix = is_valid_instance_id(development_identity)
        ? development_identity + "-" : std::string{};
    return {true, false, "desktop-instances/" + prefix + instance_id};
}

} // namespace acecode::desktop
