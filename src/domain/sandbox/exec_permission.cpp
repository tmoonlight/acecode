#include "exec_permission.hpp"

#include "utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace acecode::sandbox {

namespace {

bool blank(const std::string& text) {
    return std::none_of(text.begin(), text.end(), [](unsigned char c) { return !std::isspace(c); });
}

std::string path_error(const std::string& field, const std::string& value) {
    return "additional_permissions.file_system." + field + " entries must be absolute paths or start with '~': " + value;
}

} // namespace

const char* sandbox_permissions_name(SandboxPermissionsRequest request) {
    switch (request) {
        case SandboxPermissionsRequest::UseDefault:       return "use_default";
        case SandboxPermissionsRequest::WithAdditional:   return "with_additional_permissions";
        case SandboxPermissionsRequest::RequireEscalated: return "require_escalated";
    }
    return "use_default";
}

SandboxPermissionsRequest requested_sandbox_permissions(const nlohmann::json& args) {
    if (!args.is_object()) return SandboxPermissionsRequest::UseDefault;
    if (args.value("with_escalated_permissions", false)) return SandboxPermissionsRequest::RequireEscalated;
    const auto mode = args.value("sandbox_permissions", std::string{});
    if (mode == "require_escalated") return SandboxPermissionsRequest::RequireEscalated;
    if (mode == "with_additional_permissions") return SandboxPermissionsRequest::WithAdditional;
    return SandboxPermissionsRequest::UseDefault;
}

AdditionalPermissions parse_additional_permissions(const nlohmann::json& args, const std::string& home) {
    AdditionalPermissions out;
    if (!args.is_object() || !args.contains("additional_permissions") ||
        !args["additional_permissions"].is_object()) {
        return out;
    }
    const auto& ap = args["additional_permissions"];
    const std::string home_dir = home.empty() ? user_home_dir() : canonical_policy_path(home);
    auto expand = [&](const std::string& raw) -> std::string {
        if (raw.empty()) return {};
        if (raw[0] == '~') {
            if (raw.size() > 1 && raw[1] != '/' && raw[1] != '\\') return {};
            if (home_dir.empty()) return {};
            return normalize_policy_path(home_dir + raw.substr(1));
        }
        if (!is_rooted_path(raw)) return {};
        return normalize_policy_path(raw);
    };
    auto collect = [&](const char* field, std::vector<std::string>& target) {
        if (!ap.contains("file_system") || !ap["file_system"].is_object()) return;
        const auto& fsj = ap["file_system"];
        if (!fsj.contains(field) || !fsj[field].is_array()) return;
        for (const auto& item : fsj[field]) {
            if (!item.is_string()) continue;
            const std::string expanded = expand(item.get<std::string>());
            if (expanded.empty()) continue;
            if (std::none_of(target.begin(), target.end(), [&](const std::string& existing) { return existing == expanded; })) {
                target.push_back(expanded);
            }
        }
    };
    collect("read", out.read);
    collect("write", out.write);
    if (ap.contains("network") && ap["network"].is_object()) {
        out.network = ap["network"].value("enabled", false);
    }
    return out;
}

std::vector<std::string> requested_prefix_rule(const nlohmann::json& args) {
    std::vector<std::string> out;
    if (!args.is_object() || !args.contains("prefix_rule") || !args["prefix_rule"].is_array()) return out;
    for (const auto& item : args["prefix_rule"]) {
        if (!item.is_string() || item.get<std::string>().empty()) return {};
        out.push_back(item.get<std::string>());
    }
    return out;
}

std::string validate_escalation_arguments(const nlohmann::json& args) {
    if (!args.is_object()) return "Tool arguments must be an object.";
    if (args.contains("with_escalated_permissions") && !args["with_escalated_permissions"].is_boolean()) {
        return "with_escalated_permissions must be a boolean.";
    }
    if (args.contains("sandbox_permissions")) {
        if (!args["sandbox_permissions"].is_string()) return "sandbox_permissions must be a string.";
        const auto mode = args["sandbox_permissions"].get<std::string>();
        if (mode != "use_default" && mode != "with_additional_permissions" && mode != "require_escalated") {
            return "sandbox_permissions must be one of use_default, with_additional_permissions, require_escalated.";
        }
    }
    if (args.contains("justification") && !args["justification"].is_string()) {
        return "justification must be a string.";
    }
    if (args.contains("prefix_rule")) {
        if (!args["prefix_rule"].is_array()) return "prefix_rule must be an array of strings.";
        for (const auto& item : args["prefix_rule"]) {
            if (!item.is_string() || item.get<std::string>().empty()) return "prefix_rule must be an array of non-empty strings.";
        }
    }
    bool has_additional = false;
    if (args.contains("additional_permissions")) {
        const auto& ap = args["additional_permissions"];
        if (!ap.is_object()) return "additional_permissions must be an object.";
        if (ap.contains("file_system")) {
            const auto& fsj = ap["file_system"];
            if (!fsj.is_object()) return "additional_permissions.file_system must be an object.";
            for (const char* field : {"read", "write"}) {
                if (!fsj.contains(field)) continue;
                if (!fsj[field].is_array()) return std::string("additional_permissions.file_system.") + field + " must be an array of paths.";
                for (const auto& item : fsj[field]) {
                    if (!item.is_string() || item.get<std::string>().empty()) {
                        return std::string("additional_permissions.file_system.") + field + " must contain non-empty path strings.";
                    }
                    const auto value = item.get<std::string>();
                    const bool tilde = value[0] == '~' && (value.size() == 1 || value[1] == '/' || value[1] == '\\');
                    if (!tilde && !is_rooted_path(value)) return path_error(field, value);
                    has_additional = true;
                }
            }
        }
        if (ap.contains("network")) {
            if (!ap["network"].is_object()) return "additional_permissions.network must be an object.";
            if (ap["network"].contains("enabled") && !ap["network"]["enabled"].is_boolean()) {
                return "additional_permissions.network.enabled must be a boolean.";
            }
            if (ap["network"].value("enabled", false)) has_additional = true;
        }
    }
    const auto request = requested_sandbox_permissions(args);
    if (request != SandboxPermissionsRequest::UseDefault) {
        if (blank(args.value("justification", std::string{}))) {
            return std::string(sandbox_permissions_name(request)) + " requires a non-empty justification.";
        }
    }
    if (request == SandboxPermissionsRequest::WithAdditional && !has_additional) {
        return "with_additional_permissions requires a non-empty additional_permissions (file_system.read / file_system.write / network.enabled).";
    }
    if (request == SandboxPermissionsRequest::UseDefault && has_additional) {
        return "additional_permissions requires sandbox_permissions=with_additional_permissions.";
    }
    return {};
}

std::string ExecPermission::remember_display() const {
    std::string out;
    for (const auto& pattern : remember_patterns) {
        if (!out.empty()) out += "; ";
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            if (i) out += ' ';
            out += pattern[i];
        }
    }
    return out;
}

void ExecPermission::set_availability(bool available) {
    input.sandbox_available = available;
    decision = decide_exec(input);
    std::string display;
    for (const auto& prefix : prefixes) {
        if (!display.empty()) display += "; ";
        display += prefix;
    }
    nlohmann::json permission = {
        {"reason", decision.reason}, {"sandbox", sandbox_mode_name(decision.sandbox)},
        {"always_allow_prefix", display}, {"classification", command_kind_name(classification.kind)},
        {"request", sandbox_permissions_name(input.escalation_requested ? SandboxPermissionsRequest::RequireEscalated
                                             : input.additional_requested ? SandboxPermissionsRequest::WithAdditional
                                                                          : SandboxPermissionsRequest::UseDefault)}};
    if (!additional.empty()) {
        permission["additional_permissions"] = {
            {"read", additional.read}, {"write", additional.write}, {"network", additional.network}};
    }
    if (!remember_patterns.empty()) permission["proposed_prefix_rule"] = remember_display();
    if (input.unattended) permission["unattended"] = true;
    arguments["permission"] = std::move(permission);
}

ExecPermission evaluate_exec_permission(const std::string& arguments,
                                        const PermissionManager& permissions,
                                        const ExecRules& rules, bool sandbox_available,
                                        CommandPlatform platform,
                                        const ExecPermissionOptions& options) {
    ExecPermission result;
    result.arguments = nlohmann::json::parse(arguments, nullptr, false);
    result.error = validate_escalation_arguments(result.arguments);
    if (!result.error.empty()) return result;
    if (!result.arguments.contains("command") || !result.arguments["command"].is_string() ||
        result.arguments["command"].get<std::string>().empty()) {
        result.error = "command must be a non-empty string.";
        return result;
    }
    const auto command = result.arguments["command"].get<std::string>();
    result.classification = classify_command(command, platform);
    auto rule = rules.evaluate(result.classification);
    const auto configured = permissions.matched_rule("bash", "", command);
    if (configured == RuleAction::Deny) rule.decision = RuleDecision::Forbidden;
    else if (configured == RuleAction::Allow && rule.decision == RuleDecision::NoMatch &&
             result.classification.split_safely) rule.decision = RuleDecision::Allow;

    if (result.classification.split_safely) {
        for (const auto& segment : result.classification.segments) {
            auto prefix = always_allow_prefix_for_segment(segment);
            if (prefix.empty()) { result.prefixes.clear(); break; }
            result.prefixes.push_back(std::move(prefix));
        }
    }
    result.remember_patterns = derive_remember_patterns(result.classification, requested_prefix_rule(result.arguments));
    const auto request = requested_sandbox_permissions(result.arguments);
    if (request == SandboxPermissionsRequest::WithAdditional) {
        result.additional = parse_additional_permissions(result.arguments, options.home);
    }
    const auto remembered = permissions.session_command_allow(result.prefixes);
    result.input.mode = permissions.mode();
    result.input.dangerous_mode = permissions.is_dangerous();
    result.input.kind = result.classification.kind;
    result.input.rule = rule.decision;
    result.input.escalation_requested = request == SandboxPermissionsRequest::RequireEscalated;
    result.input.additional_requested = request == SandboxPermissionsRequest::WithAdditional;
    result.input.additional_covered = result.input.additional_requested &&
                                      options.session_grants.covers(result.additional);
    result.input.unattended = options.unattended;
    result.input.session_allow = remembered == SessionCommandAllow::Bypass ? SessionAllowKind::Bypass :
        remembered == SessionCommandAllow::Sandboxed ? SessionAllowKind::Sandboxed : SessionAllowKind::None;
    result.set_availability(sandbox_available);
    return result;
}

bool is_exec_rules_path(const std::string& path) {
    std::string normalized = "/" + path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
#ifdef _WIN32
    for (char& c : normalized) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
#endif
    normalized += '/';
    return normalized.find("/.acecode/rules/") != std::string::npos;
}

} // namespace acecode::sandbox
