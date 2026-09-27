#include "sandbox_runtime.hpp"

#include "config/vocab/permission_mode.hpp"

#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <sstream>
#include <filesystem>
#include <fstream>

namespace acecode::sandbox {

SandboxRuntime& SandboxRuntime::instance() {
    static SandboxRuntime rt;
    return rt;
}

void SandboxRuntime::configure(SandboxRuntimeConfig cfg) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_ = std::move(cfg);
    probe_.reset();
    session_grants_ = {};
}

SandboxRuntimeConfig SandboxRuntime::config() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cfg_;
}

BackendProbe SandboxRuntime::probe() {
    WindowsBackendChoice choice;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (probe_) return *probe_;
        if (!cfg_.enabled) {
            BackendProbe disabled;
            disabled.available = false;
            disabled.reason = "disabled by config.sandbox.enabled=false";
            probe_ = disabled;
            return disabled;
        }
        choice = cfg_.windows_backend;
    }
    // 真探测放在锁外:Windows 上要创建令牌,Linux 上要 fork bwrap。进程级缓存按
    // 后端选择各存一份,切换配置不用重启。
    static const BackendProbe process_probe_default = probe_backend(WindowsBackendChoice::RestrictedToken);
    static const BackendProbe process_probe_mxc = probe_backend(WindowsBackendChoice::Mxc);
    BackendProbe fresh = choice == WindowsBackendChoice::Mxc ? process_probe_mxc : process_probe_default;
    std::lock_guard<std::mutex> lk(mu_);
    if (!probe_) {
        probe_ = fresh;
        if (fresh.available) {
            LOG_INFO(std::string("[sandbox] backend available: ") + backend_kind_name(fresh.kind));
        } else {
            LOG_INFO(std::string("[sandbox] backend unavailable (") + backend_kind_name(fresh.kind) +
                     "): " + fresh.reason);
        }
    }
    return *probe_;
}

bool SandboxRuntime::available() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (override_) return *override_;
        if (!cfg_.enabled) return false;
    }
    return probe().available;
}

bool SandboxRuntime::network_enforced() {
    return probe().network_enforced;
}

bool SandboxRuntime::network_best_effort() {
    return probe().network_best_effort;
}

void SandboxRuntime::set_availability_override_for_tests(std::optional<bool> value) {
    std::lock_guard<std::mutex> lk(mu_);
    override_ = value;
}

void SandboxRuntime::grant_for_session(const AdditionalPermissions& grants) {
    std::lock_guard<std::mutex> lk(mu_);
    session_grants_.merge(grants);
}

AdditionalPermissions SandboxRuntime::session_grants() const {
    std::lock_guard<std::mutex> lk(mu_);
    return session_grants_;
}

void SandboxRuntime::clear_session_grants() {
    std::lock_guard<std::mutex> lk(mu_);
    session_grants_ = {};
}

void SandboxRuntime::set_workspace_writable_roots(std::vector<std::string> roots) {
    std::lock_guard<std::mutex> lk(mu_);
    workspace_writable_roots_ = std::move(roots);
}

SandboxPolicyOptions SandboxRuntime::policy_options(const AdditionalPermissions* extra) const {
    std::lock_guard<std::mutex> lk(mu_);
    SandboxPolicyOptions options;
    options.extra_writable_roots = cfg_.writable_roots;
    options.extra_writable_roots.insert(options.extra_writable_roots.end(),
                                        workspace_writable_roots_.begin(),
                                        workspace_writable_roots_.end());
    options.readable_roots = cfg_.readable_roots;
    options.denied_entries = cfg_.denied_entries;
    if (cfg_.deny_defaults) {
        for (const auto& entry : default_denied_entries()) options.denied_entries.push_back(entry);
    }
    options.include_tmpdir = !cfg_.exclude_tmpdir;
    options.network_access = cfg_.network_access;
    options.acecode_home = cfg_.acecode_home;
    options.grants = session_grants_;
    if (extra) options.grants.merge(*extra);
    return options;
}

SandboxPolicy SandboxRuntime::policy_for(SandboxMode mode, const std::string& write_root,
                                         const AdditionalPermissions* extra) const {
    return make_sandbox_policy(mode, write_root, policy_options(extra));
}

ExecSandboxRequest SandboxRuntime::request_for(SandboxMode mode, const std::string& write_root,
                                               const AdditionalPermissions* extra) {
    ExecSandboxRequest req;
    req.policy = policy_for(mode, write_root, extra);
    if (mode == SandboxMode::FullAccess) {
        req.backend = BackendKind::None;
        req.network_enforced = false;
        return req;
    }
    const BackendProbe p = probe();
    req.backend = p.kind;
    req.network_enforced = p.network_enforced;
    req.network_best_effort = p.network_best_effort;
    req.backend_executable = p.executable_path;
    if (p.network_best_effort && !req.policy.network_access) req.denybin_dir = denybin_dir();
    return req;
}

void SandboxRuntime::mark_unavailable(const std::string& reason) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!probe_) probe_ = BackendProbe{};
    probe_->available = false;
    probe_->reason = reason;
}

void SandboxRuntime::reset_probe() {
    std::lock_guard<std::mutex> lk(mu_);
    probe_.reset();
}

std::string SandboxRuntime::denybin_dir() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (cfg_.acecode_home.empty()) return {};
    return path_to_utf8(path_from_utf8(cfg_.acecode_home) / "sandbox" / "denybin");
}

std::string SandboxRuntime::prepare_request(ExecSandboxRequest& request) {
    if (request.policy.mode == SandboxMode::FullAccess) return {};
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (override_) return *override_ ? std::string{} : "sandbox unavailable (test override)";
    }
    namespace fs = std::filesystem;
    std::string error;
    if (!available()) return probe().reason;
#ifdef _WIN32
    if (!request.policy.temporary_directory.empty()) {
        const auto temporary = path_from_utf8(request.policy.temporary_directory);
        std::error_code ec;
        const auto resolved = fs::weakly_canonical(temporary, ec);
        if (ec || resolved != temporary) {
            return "Sandbox temporary directory was redirected: " + request.policy.temporary_directory;
        }
        fs::create_directories(temporary, ec);
        if (ec) return "Cannot prepare sandbox temporary directory: " + ec.message();
        if (fs::weakly_canonical(temporary, ec) != temporary || ec) {
            return "Sandbox temporary directory was redirected: " + request.policy.temporary_directory;
        }
    }
#endif
    if (!request.denybin_dir.empty()) {
        // 准断网的 ssh / scp 桩:建不出来只是少一层劝退,不能因此让沙盒不可用。
        std::string stub_error;
        if (!ensure_denybin_stubs(request.denybin_dir, &stub_error)) {
            LOG_WARN("[sandbox] offline stubs unavailable: " + stub_error);
            request.denybin_dir.clear();
        }
    }
    if (request.policy.mode == SandboxMode::WorkspaceWrite) {
        if (request.policy.writable_roots.empty()) return "No sandbox workspace root.";
        for (const auto& writable : request.policy.writable_roots) {
            const auto root = path_from_utf8(writable.root);
            for (const auto& protected_path : writable.read_only_subpaths) {
                std::error_code ec;
                const auto path = path_from_utf8(protected_path);
                const auto resolved = fs::weakly_canonical(path, ec);
                const auto relative = resolved.lexically_relative(root);
                if (ec || relative.empty() || *relative.begin() == "..") {
                    return "Protected path resolves outside the sandbox root: " + protected_path;
                }
                if (fs::exists(path, ec) && !ec) continue;
                if (ec) return "Cannot inspect protected path: " + ec.message();
                // deny 名单里的路径不存在就不存在:不能为了打拒绝 ACE 去凭空创建
                // `~/.ssh` 之类的目录。只有 git / rules 这类我们自己定义的受保护
                // 路径才预建实体,封住稍后新建敏感路径的漏洞。
                const std::string filename = path_to_utf8(path.filename());
                const bool directory = filename == "rules" || filename == "hooks" || filename == "modules";
                const bool known_protected = directory || filename == "config" ||
                                             filename == "config.worktree" || filename == ".git";
                if (!known_protected) continue;
                fs::create_directories(directory ? path : path.parent_path(), ec);
                if (ec) return "Cannot prepare protected path: " + ec.message();
                if (!directory) {
                    std::ofstream placeholder(path, std::ios::binary | std::ios::app);
                    if (!placeholder) return "Cannot prepare protected file: " + protected_path;
                }
            }
        }
    }
#ifdef _WIN32
    if (request.backend == BackendKind::WindowsRestrictedToken) {
        if (!ensure_windows_acl_grants(request.policy, &error)) {
            mark_unavailable(error);
            return error;
        }
    }
#endif
    return {};
}

std::string SandboxRuntime::status_text(PermissionMode mode, const std::string& write_root,
                                        bool session_disabled) {
    const SandboxRuntimeConfig cfg = config();
    const BackendProbe p = probe();
    const bool usable = available() && !session_disabled;
    std::ostringstream oss;
    oss << "Sandbox backend : " << backend_kind_name(p.kind) << "\n";
    oss << "Available       : " << (usable ? "yes" : "no");
    if (session_disabled) oss << " (disabled for this session via /sandbox off)";
    else if (!cfg.enabled) oss << " (config.sandbox.enabled=false)";
    else if (!p.available && !p.reason.empty()) oss << " (" << p.reason << ")";
    oss << "\n";
    oss << "Permission mode : " << permission_mode_name(mode) << "\n";
    const SandboxMode sm = mode_sandbox(mode, usable);
    oss << "Auto-run policy : " << sandbox_mode_name(sm) << "\n";
    const SandboxPolicy policy = policy_for(sm == SandboxMode::FullAccess ? SandboxMode::ReadOnly : sm, write_root);
    if (sm == SandboxMode::WorkspaceWrite) {
        oss << "Writable roots  :";
        if (policy.writable_roots.empty()) oss << " (none)";
        oss << "\n";
        for (const auto& root : policy.writable_roots) {
            oss << "  - " << root.root << "\n";
            for (const auto& ro : root.read_only_subpaths) {
                oss << "      read-only: " << ro << "\n";
            }
        }
    }
    if (sm != SandboxMode::FullAccess) {
        oss << "Readable roots  : ";
        if (policy.full_disk_read()) oss << "(entire disk)\n";
        else {
            oss << "\n";
            for (const auto& root : policy.readable_roots) oss << "  - " << root << "\n";
        }
        oss << "Denied entries  :";
        if (!policy.has_deny_entries()) oss << " (none)";
        oss << "\n";
        for (const auto& d : policy.denied_paths) oss << "  - " << d << "\n";
        for (const auto& g : policy.denied_globs) oss << "  - " << g << " (glob)\n";
        if (p.kind == BackendKind::WindowsRestrictedToken) {
            oss << "                  (Windows: deny entries only block writes; reads are not restricted)\n";
        }
    }
    const AdditionalPermissions grants = session_grants();
    if (!grants.empty()) {
        oss << "Session grants  :\n";
        for (const auto& w : grants.write) oss << "  - write " << w << "\n";
        for (const auto& r : grants.read) oss << "  - read " << r << "\n";
        if (grants.network) oss << "  - network\n";
    }
    oss << "Network         : ";
    if (p.network_enforced) oss << (policy.network_access ? "allowed" : "blocked");
    else if (p.network_best_effort) oss << (policy.network_access ? "allowed" : "best-effort offline (proxy/env only, not enforced)");
    else oss << "not enforced by this backend";
    oss << "\n";
    if (p.kind == BackendKind::WindowsRestrictedToken && usable) {
        oss << "Windows limits  : delete/rename are not fully restricted; public writable paths remain writable\n";
    }
    oss << "Config          : network_access=" << (cfg.network_access ? "true" : "false")
        << " exclude_tmpdir=" << (cfg.exclude_tmpdir ? "true" : "false")
        << " writable_roots=" << cfg.writable_roots.size()
        << " read=" << cfg.readable_roots.size()
        << " deny=" << cfg.denied_entries.size()
        << " deny_defaults=" << (cfg.deny_defaults ? "true" : "false")
#ifdef _WIN32
        << " windows_backend=" << windows_backend_choice_name(cfg.windows_backend)
#endif
        << "\n";
    return oss.str();
}

} // namespace acecode::sandbox
