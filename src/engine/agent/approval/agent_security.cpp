#include "agent/agent_loop.hpp"
#include "agent/approval/session_exec_security.hpp"

#include <utility>

namespace acecode {

void AgentLoop::set_exec_rules(sandbox::ExecRules rules) { exec_security_->set_rules(std::move(rules)); }
void AgentLoop::set_exec_rules_dir_for_tests(const std::string& dir) { exec_security_->set_rules_dir(dir); }
void AgentLoop::set_sandbox_availability_for_tests(std::optional<bool> value) {
    exec_security_->runtime().set_availability_override_for_tests(value);
}
void AgentLoop::set_audit_sink(security::AuditSink sink) { exec_security_->set_audit_sink(std::move(sink)); }
void AgentLoop::set_sandbox_config(const SandboxConfig& config) { exec_security_->set_sandbox_config(config); }
void AgentLoop::reload_exec_rules() { exec_security_->reload_exec_rules(); }
std::string AgentLoop::sandbox_prompt_description() const {
    return exec_security_->sandbox_prompt_description(session_manager_);
}
std::string AgentLoop::sandbox_command(const std::string& args) {
    return exec_security_->sandbox_command(session_manager_, args);
}

} // namespace acecode
