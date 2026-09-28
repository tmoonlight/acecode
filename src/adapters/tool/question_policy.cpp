#include "question_policy.hpp"

#include <cstdlib>

namespace acecode {

namespace {

QuestionPolicy parse_policy_or_ask(const std::string& value) {
    if (value == "deny") return QuestionPolicy::Deny;
    if (value == "timeout") return QuestionPolicy::Timeout;
    return QuestionPolicy::Ask;
}

int sanitize_timeout_seconds(int v) {
    if (v < 5 || v > 3600) return 60;
    return v;
}

} // namespace

ResolvedQuestionPolicy resolve_question_policy(
    const std::string& configured_policy,
    bool policy_explicit,
    int configured_timeout_seconds) {
    ResolvedQuestionPolicy out;
    out.timeout_seconds = sanitize_timeout_seconds(configured_timeout_seconds);

    if (policy_explicit) {
        out.policy = parse_policy_or_ask(configured_policy);
        out.origin = "explicit";
        return out;
    }
    out.policy = QuestionPolicy::Ask;
    out.origin = "default";
    return out;
}

bool parse_question_policy_value(const std::string& value,
                                 std::string& policy,
                                 int& timeout_seconds,
                                 std::string& error) {
    policy.clear();
    timeout_seconds = 0;
    error.clear();

    if (value == "ask" || value == "deny" || value == "timeout") {
        policy = value;
        return true;
    }

    const std::string prefix = "timeout:";
    if (value.rfind(prefix, 0) == 0) {
        const std::string secs = value.substr(prefix.size());
        if (secs.empty() ||
            secs.find_first_not_of("0123456789") != std::string::npos) {
            error = "invalid --question-policy timeout seconds: \"" + secs +
                    "\" (expected an integer in [5, 3600])";
            return false;
        }
        // 全数字且限长,strtol 不会溢出到未定义行为;超长直接判越界。
        if (secs.size() > 4) {
            error = "--question-policy timeout seconds out of range [5, 3600]: " + secs;
            return false;
        }
        const long parsed = std::strtol(secs.c_str(), nullptr, 10);
        if (parsed < 5 || parsed > 3600) {
            error = "--question-policy timeout seconds out of range [5, 3600]: " + secs;
            return false;
        }
        policy = "timeout";
        timeout_seconds = static_cast<int>(parsed);
        return true;
    }

    error = "invalid --question-policy value: \"" + value +
            "\" (expected ask | deny | timeout[:seconds])";
    return false;
}

} // namespace acecode
