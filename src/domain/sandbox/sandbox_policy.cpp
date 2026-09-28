#include "sandbox_policy.hpp"

#include "utils/logger.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"
#include "utils/sha1.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace acecode::sandbox {

namespace fs = std::filesystem;

namespace {

std::string canonical_utf8(const std::string& path) {
    if (path.empty()) return {};
    std::error_code ec;
    fs::path p = path_from_utf8(path);
    fs::path c = fs::weakly_canonical(p, ec);
    if (ec || c.empty()) c = fs::absolute(p, ec);
    if (ec || c.empty()) return path;
    std::string out = path_to_utf8(c);
    while (out.size() > path_to_utf8(c.root_path()).size() &&
           (out.back() == '/' || out.back() == '\\')) out.pop_back();
    return out;
}

std::string temporary_write_root(const std::string& write_root,
                                  const SandboxPolicyOptions& options) {
    if (!options.include_tmpdir) return {};
    const auto base = options.tmpdir_override.empty() ? system_temp_dir()
                                                     : canonical_utf8(options.tmpdir_override);
    if (base.empty()) return {};
#ifdef _WIN32
    // 系统 TEMP 可能含几十万个无关文件。SetNamedSecurityInfoW 会递归传播 ACL,
    // 不能把它直接作为默认写根。目录名绑定规范化工作区,重启/重开会话后仍复用
    // 同一策略身份,避免因随机临时目录让整个工作区反复新增 ACE。
    auto identity = canonical_utf8(write_root);
    for (char& c : identity) {
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (c == '\\') c = '/';
    }
    // 保留预期路径,不要在这里跟随专用目录上的 junction;prepare_request 会核验。
    return path_to_utf8(path_from_utf8(base) / "acecode-sandbox" / sha1_hex(identity));
#else
    return base;
#endif
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// 比较用的归一形态:`/` 分隔、去尾分隔符;Windows 上再统一小写。
std::string comparable(const std::string& path) {
    std::string out = path;
    std::replace(out.begin(), out.end(), '\\', '/');
    while (out.size() > 1 && out.back() == '/') {
        // 保留 "C:/" 与 "/" 这类根。
        if (out.size() == 3 && out[1] == ':') break;
        out.pop_back();
    }
#ifdef _WIN32
    for (char& c : out) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
#endif
    return out;
}

std::size_t component_count(const std::string& comparable_path) {
    std::size_t count = 0;
    std::size_t start = 0;
    while (start <= comparable_path.size()) {
        std::size_t end = comparable_path.find('/', start);
        if (end == std::string::npos) end = comparable_path.size();
        if (end > start) ++count;
        if (end == comparable_path.size()) break;
        start = end + 1;
    }
    return count;
}

// path 是否等于 root 或在 root 之下(两者都是 comparable 形态)。
bool is_same_or_under(const std::string& root, const std::string& path) {
    if (root.empty()) return false;
    if (path == root) return true;
    if (path.size() <= root.size()) return false;
    if (path.compare(0, root.size(), root) != 0) return false;
    if (root.back() == '/') return true;   // "/" 或 "C:/"
    return path[root.size()] == '/';
}

void add_protected_git_entries(const fs::path& git_dir, std::vector<std::string>& out) {
    for (const char* name : {"hooks", "config", "config.worktree", "modules"}) {
        const fs::path p = git_dir / name;
        out.push_back(path_to_utf8(p));
    }
    std::error_code ec;
    const fs::path worktrees = git_dir / "worktrees";
    if (fs::is_directory(worktrees, ec) && !ec) {
        for (const auto& entry : fs::directory_iterator(worktrees, ec)) {
            if (ec) break;
            const fs::path cfg = entry.path() / "config.worktree";
            out.push_back(path_to_utf8(cfg));
        }
    }
}

void add_root(std::vector<WritableRoot>& roots, const std::string& raw,
              const std::vector<std::string>& extra_ro) {
    const std::string root = canonical_utf8(raw);
    if (root.empty()) return;
    for (const auto& existing : roots) {
        if (comparable(existing.root) == comparable(root)) return;
    }
    WritableRoot wr;
    wr.root = root;
    wr.read_only_subpaths = protected_subpaths_under(root);
    for (const auto& ro : extra_ro) wr.read_only_subpaths.push_back(ro);
    std::sort(wr.read_only_subpaths.begin(), wr.read_only_subpaths.end());
    wr.read_only_subpaths.erase(std::unique(wr.read_only_subpaths.begin(), wr.read_only_subpaths.end()),
                                wr.read_only_subpaths.end());
    roots.push_back(std::move(wr));
}

void push_unique(std::vector<std::string>& out, const std::string& value) {
    if (value.empty()) return;
    for (const auto& existing : out) {
        if (comparable(existing) == comparable(value)) return;
    }
    out.push_back(value);
}

bool glob_match_impl(const std::string& p, std::size_t i, const std::string& t, std::size_t j) {
    while (i < p.size()) {
        const char c = p[i];
        if (c == '*') {
            if (i + 1 < p.size() && p[i + 1] == '*') {
                std::size_t k = i + 2;
                if (k < p.size() && p[k] == '/') ++k;
                // `**` 可以吃掉零个或多个组件(含分隔符)。
                for (std::size_t m = j; m <= t.size(); ++m) {
                    if (glob_match_impl(p, k, t, m)) return true;
                }
                return false;
            }
            for (std::size_t m = j; m <= t.size(); ++m) {
                if (glob_match_impl(p, i + 1, t, m)) return true;
                if (m < t.size() && t[m] == '/') break;
            }
            return false;
        }
        if (j >= t.size()) return false;
        if (c == '?') {
            if (t[j] == '/') return false;
            ++i; ++j;
            continue;
        }
        if (c == '[') {
            const std::size_t close = p.find(']', i + 1);
            if (close == std::string::npos) {
                if (t[j] != '[') return false;
                ++i; ++j;
                continue;
            }
            std::string cls = p.substr(i + 1, close - i - 1);
            bool negate = false;
            if (!cls.empty() && (cls[0] == '!' || cls[0] == '^')) { negate = true; cls.erase(0, 1); }
            bool hit = false;
            for (std::size_t n = 0; n < cls.size(); ++n) {
                if (n + 2 < cls.size() && cls[n + 1] == '-') {
                    if (t[j] >= cls[n] && t[j] <= cls[n + 2]) hit = true;
                    n += 2;
                } else if (cls[n] == t[j]) {
                    hit = true;
                }
            }
            if (hit == negate || t[j] == '/') return false;
            i = close + 1; ++j;
            continue;
        }
        if (c != t[j]) return false;
        ++i; ++j;
    }
    return j == t.size();
}

} // namespace

std::string canonical_policy_path(const std::string& path) { return canonical_utf8(path); }

bool is_rooted_path(const std::string& path) {
    if (path.empty()) return false;
    if (path_from_utf8(path).is_absolute()) return true;
    return path[0] == '/' || path[0] == '\\';
}

std::string normalize_policy_path(const std::string& path) {
    if (path.empty()) return {};
    std::error_code ec;
    const fs::path p = path_from_utf8(path);
    if (fs::exists(p, ec) && !ec) return canonical_utf8(path);
    std::string out = p.lexically_normal().generic_string();
    while (out.size() > 1 && out.back() == '/') {
        if (out.size() == 3 && out[1] == ':') break;
        out.pop_back();
    }
    return out;
}

std::string user_home_dir() {
#ifdef _WIN32
    if (const char* profile = std::getenv("USERPROFILE"); profile && *profile) return canonical_utf8(profile);
    const char* drive = std::getenv("HOMEDRIVE");
    const char* home_path = std::getenv("HOMEPATH");
    if (drive && home_path) return canonical_utf8(std::string(drive) + home_path);
    return {};
#else
    if (const char* home = std::getenv("HOME"); home && *home) return canonical_utf8(home);
    return {};
#endif
}

std::vector<std::string> default_denied_entries() {
    // 秘密存储目录 / 凭据文件:沙盒内命令不该读得到。`.npmrc` / `.gitconfig` 这类
    // 开发工具自己要读的文件刻意不收,免得日常 npm install 直接失败。
    return {"~/.ssh", "~/.aws", "~/.gnupg", "~/.netrc", "~/.docker/config.json", "~/.kube",
            ":acecode_home/config.json"};
}

bool is_glob_entry(const std::string& entry) {
    return entry.find_first_of("*?[") != std::string::npos;
}

bool glob_matches_path(const std::string& pattern, const std::string& path) {
    if (pattern.empty() || path.empty()) return false;
    const std::string p = comparable(pattern);
    const std::string t = comparable(path);
    // 命中路径本身或它的任一祖先目录都算(deny 目录整棵生效)。
    std::string current = t;
    while (true) {
        if (glob_match_impl(p, 0, current, 0)) return true;
        const std::size_t slash = current.find_last_of('/');
        if (slash == std::string::npos || slash == 0) break;
        if (slash == 2 && current[1] == ':') break;
        current.erase(slash);
    }
    return false;
}

std::vector<std::string> expand_entry(const std::string& raw, const EntryContext& ctx) {
    const std::string entry = trim(raw);
    if (entry.empty()) return {};
    auto with_rest = [](const std::string& base, const std::string& rest) {
        if (base.empty()) return std::string{};
        if (rest.empty()) return base;
        if (rest[0] == '/' || rest[0] == '\\') return base + rest;
        return base + "/" + rest;
    };
    if (entry[0] == '~') {
        if (entry.size() > 1 && entry[1] != '/' && entry[1] != '\\') return {};   // ~user 不支持
        const std::string out = with_rest(ctx.home, entry.substr(1));
        return out.empty() ? std::vector<std::string>{} : std::vector<std::string>{out};
    }
    if (entry[0] == ':') {
        auto token_rest = [&](const char* token) -> std::pair<bool, std::string> {
            const std::string t = token;
            if (entry.compare(0, t.size(), t) != 0) return {false, {}};
            if (entry.size() > t.size() && entry[t.size()] != '/' && entry[t.size()] != '\\') return {false, {}};
            return {true, entry.substr(t.size())};
        };
        if (auto [hit, rest] = token_rest(":workspace_roots"); hit) {
            std::vector<std::string> out;
            for (const auto& root : ctx.workspace_roots) push_unique(out, with_rest(root, rest));
            return out;
        }
        if (auto [hit, rest] = token_rest(":tmpdir"); hit) {
            const std::string out = with_rest(ctx.tmpdir, rest);
            return out.empty() ? std::vector<std::string>{} : std::vector<std::string>{out};
        }
        if (auto [hit, rest] = token_rest(":acecode_home"); hit) {
            const std::string out = with_rest(ctx.acecode_home, rest);
            return out.empty() ? std::vector<std::string>{} : std::vector<std::string>{out};
        }
        LOG_WARN("[sandbox] unknown filesystem entry token ignored: " + entry);
        return {};
    }
    return {expand_path(entry)};
}

void AdditionalPermissions::merge(const AdditionalPermissions& other) {
    for (const auto& r : other.read) push_unique(read, r);
    for (const auto& w : other.write) push_unique(write, w);
    network = network || other.network;
}

bool AdditionalPermissions::covers(const AdditionalPermissions& other) const {
    if (other.network && !network) return false;
    auto covered = [](const std::vector<std::string>& roots, const std::string& path) {
        const std::string p = comparable(path);
        for (const auto& root : roots) {
            if (is_same_or_under(comparable(root), p)) return true;
        }
        return false;
    };
    for (const auto& r : other.read) {
        if (!covered(read, r) && !covered(write, r)) return false;
    }
    for (const auto& w : other.write) {
        if (!covered(write, w)) return false;
    }
    return true;
}

FsAccess SandboxPolicy::resolve_access(const std::string& absolute_path) const {
    if (mode == SandboxMode::FullAccess) return FsAccess::Write;
    const std::string p = comparable(canonical_utf8(absolute_path));
    // 同深度 deny > write > read。
    auto rank = [](FsAccess a) { return a == FsAccess::Deny ? 2 : a == FsAccess::Write ? 1 : 0; };
    long best_depth = -1;
    FsAccess best = full_disk_read() ? FsAccess::Read : FsAccess::Deny;
    auto consider = [&](long depth, FsAccess access) {
        if (depth > best_depth || (depth == best_depth && rank(access) > rank(best))) {
            best_depth = depth;
            best = access;
        }
    };
    for (const auto& denied : denied_paths) {
        const std::string d = comparable(denied);
        if (is_same_or_under(d, p)) consider(static_cast<long>(component_count(d)), FsAccess::Deny);
    }
    for (const auto& glob : denied_globs) {
        if (glob_matches_path(glob, p)) consider(static_cast<long>(component_count(comparable(glob))), FsAccess::Deny);
    }
    if (mode == SandboxMode::WorkspaceWrite) {
        for (const auto& root : writable_roots) {
            const std::string r = comparable(root.root);
            if (!is_same_or_under(r, p)) continue;
            consider(static_cast<long>(component_count(r)), FsAccess::Write);
            for (const auto& ro : root.read_only_subpaths) {
                const std::string sub = comparable(ro);
                if (is_same_or_under(sub, p)) consider(static_cast<long>(component_count(sub)), FsAccess::Read);
            }
        }
    }
    for (const auto& root : readable_roots) {
        const std::string r = comparable(root);
        if (is_same_or_under(r, p)) consider(static_cast<long>(component_count(r)), FsAccess::Read);
    }
    return best;
}

bool SandboxPolicy::can_write(const std::string& absolute_path) const {
    return resolve_access(absolute_path) == FsAccess::Write;
}

bool SandboxPolicy::can_read(const std::string& absolute_path) const {
    return resolve_access(absolute_path) != FsAccess::Deny;
}

std::vector<std::string> protected_subpaths_under(const std::string& root) {
    std::vector<std::string> out;
    if (root.empty()) return out;
    const fs::path base = path_from_utf8(root);
    std::error_code ec;
    const fs::path git = base / ".git";
    if (fs::is_directory(git, ec) && !ec) {
        add_protected_git_entries(git, out);
    } else if (fs::is_regular_file(git, ec) && !ec) {
        out.push_back(path_to_utf8(git));
    }
    // 根本身就是一个 .git 目录(链接 worktree 的 common dir 作为可写根时)。
    if (base.filename() == ".git" && fs::is_directory(base, ec) && !ec) {
        add_protected_git_entries(base, out);
    }
    // 链接 worktree 的 gitdir(<main>/.git/worktrees/<name>)。
    if (base.parent_path().filename() == "worktrees" &&
        base.parent_path().parent_path().filename() == ".git") {
        const fs::path cfg = base / "config.worktree";
        out.push_back(path_to_utf8(cfg));
    }
    const fs::path rules = base / ".acecode" / "rules";
    out.push_back(path_to_utf8(rules));
    for (auto& p : out) p = canonical_utf8(p);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

LinkedGitDirs resolve_linked_git_dirs(const std::string& root) {
    LinkedGitDirs out;
    if (root.empty()) return out;
    std::error_code ec;
    const fs::path git_file = path_from_utf8(root) / ".git";
    if (!fs::is_regular_file(git_file, ec) || ec) return out;
    std::ifstream ifs(git_file, std::ios::binary);
    std::string line;
    if (!std::getline(ifs, line)) return out;
    line = trim(line);
    const std::string prefix = "gitdir:";
    if (line.compare(0, prefix.size(), prefix) != 0) return out;
    std::string target = trim(line.substr(prefix.size()));
    if (target.empty()) return out;
    fs::path gitdir = path_from_utf8(target);
    if (gitdir.is_relative()) gitdir = path_from_utf8(root) / gitdir;
    if (!fs::is_directory(gitdir, ec) || ec) return out;
    out.gitdir = canonical_utf8(path_to_utf8(gitdir));
    // commondir 文件指向 common dir(通常是 "../.."),缺省按目录结构推。
    fs::path common = gitdir.parent_path().parent_path();
    const fs::path commondir_file = gitdir / "commondir";
    if (fs::is_regular_file(commondir_file, ec) && !ec) {
        std::ifstream cf(commondir_file, std::ios::binary);
        std::string rel;
        if (std::getline(cf, rel)) {
            rel = trim(rel);
            if (!rel.empty()) {
                fs::path candidate = path_from_utf8(rel);
                if (candidate.is_relative()) candidate = gitdir / candidate;
                common = candidate;
            }
        }
    }
    // 只信任 Git 实际登记的 linked worktree。任意 gitdir:/commondir 文本不能新增写根。
    const auto canonical_git = path_from_utf8(out.gitdir);
    if (canonical_git.parent_path().filename() != "worktrees" ||
        canonical_git.parent_path().parent_path().filename() != ".git" ||
        canonical_utf8(path_to_utf8(common)) != canonical_utf8(path_to_utf8(canonical_git.parent_path().parent_path()))) {
        return {};
    }
    std::ifstream backref(canonical_git / "gitdir", std::ios::binary);
    std::string registered_file;
    if (!std::getline(backref, registered_file) ||
        canonical_utf8(trim(registered_file)) != canonical_utf8(path_to_utf8(git_file))) return {};
    if (fs::is_directory(common, ec) && !ec) out.common_dir = canonical_utf8(path_to_utf8(common));
    return out;
}

std::string system_temp_dir() {
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec);
    if (ec || tmp.empty()) return {};
    return canonical_utf8(path_to_utf8(tmp));
}

namespace {

EntryContext make_entry_context(const std::string& write_root, const SandboxPolicyOptions& options) {
    EntryContext ctx;
    ctx.home = options.home_override.empty() ? user_home_dir() : canonical_utf8(options.home_override);
    ctx.acecode_home = options.acecode_home.empty() ? std::string{} : canonical_utf8(options.acecode_home);
    if (!write_root.empty()) {
        const std::string root = canonical_utf8(write_root);
        push_unique(ctx.workspace_roots, root);
        const LinkedGitDirs linked = resolve_linked_git_dirs(root);
        if (!linked.gitdir.empty()) push_unique(ctx.workspace_roots, linked.gitdir);
        if (!linked.common_dir.empty()) push_unique(ctx.workspace_roots, linked.common_dir);
    }
    ctx.tmpdir = temporary_write_root(write_root, options);
    return ctx;
}

// 展开 + 归一 deny 条目:glob 与普通路径分流。
void expand_denied_entries(const SandboxPolicyOptions& options, const EntryContext& ctx,
                           std::vector<std::string>& paths, std::vector<std::string>& globs) {
    for (const auto& raw : options.denied_entries) {
        for (const auto& expanded : expand_entry(raw, ctx)) {
            if (expanded.empty()) continue;
            if (is_glob_entry(expanded)) {
                std::string g = expanded;
                std::replace(g.begin(), g.end(), '\\', '/');
                push_unique(globs, g);
            } else if (path_from_utf8(expanded).is_absolute()) {
                push_unique(paths, canonical_utf8(expanded));
            } else {
                LOG_WARN("[sandbox] relative deny entry ignored: " + raw);
            }
        }
    }
}

bool denied_by(const std::vector<std::string>& denied_paths, const std::vector<std::string>& denied_globs,
               const std::string& path) {
    const std::string p = comparable(path);
    for (const auto& d : denied_paths) {
        if (is_same_or_under(comparable(d), p)) return true;
    }
    for (const auto& g : denied_globs) {
        if (glob_matches_path(g, p)) return true;
    }
    return false;
}

} // namespace

std::vector<WritableRoot> compute_writable_roots(const std::string& write_root,
                                                 const SandboxPolicyOptions& options) {
    std::vector<WritableRoot> roots;
    const EntryContext ctx = make_entry_context(write_root, options);
    for (const auto& root : ctx.workspace_roots) add_root(roots, root, {});
    for (const auto& extra : options.extra_writable_roots) {
        for (const auto& expanded : expand_entry(extra, ctx)) {
            if (!expanded.empty() && path_from_utf8(expanded).is_absolute()) add_root(roots, expanded, {});
        }
    }
    if (options.include_tmpdir && !ctx.tmpdir.empty()) add_root(roots, ctx.tmpdir, {});
    std::vector<std::string> denied_paths;
    std::vector<std::string> denied_globs;
    expand_denied_entries(options, ctx, denied_paths, denied_globs);
    // 会话授权 / 单次申请的写路径:deny 名单压过它们,不能靠申请把秘密目录变可写。
    for (const auto& granted : options.grants.write) {
        if (granted.empty() || !path_from_utf8(granted).is_absolute()) continue;
        if (denied_by(denied_paths, denied_globs, granted)) {
            LOG_WARN("[sandbox] granted write path is on the deny list, ignored: " + granted);
            continue;
        }
        add_root(roots, granted, {});
    }
    // 可写根重叠时,较窄的 allow 也必须排除所有只读子路径(尤其是 Seatbelt);
    // deny 路径落在可写根之下时同样列为只读子路径,让三平台的写拒绝一致。
    std::vector<std::string> protected_paths;
    for (const auto& root : roots) {
        protected_paths.insert(protected_paths.end(), root.read_only_subpaths.begin(), root.read_only_subpaths.end());
    }
    protected_paths.insert(protected_paths.end(), denied_paths.begin(), denied_paths.end());
    for (auto& root : roots) {
        for (const auto& ro : protected_paths) {
            const auto relative = path_from_utf8(ro).lexically_relative(path_from_utf8(root.root));
            if (!relative.empty() && *relative.begin() != "..") root.read_only_subpaths.push_back(ro);
        }
        std::sort(root.read_only_subpaths.begin(), root.read_only_subpaths.end());
        root.read_only_subpaths.erase(std::unique(root.read_only_subpaths.begin(), root.read_only_subpaths.end()), root.read_only_subpaths.end());
    }
    return roots;
}

SandboxPolicy make_sandbox_policy(SandboxMode mode, const std::string& write_root,
                                  const SandboxPolicyOptions& options) {
    SandboxPolicy policy;
    policy.mode = mode;
    policy.network_access = options.network_access || options.grants.network;
    if (mode == SandboxMode::FullAccess) return policy;
    const EntryContext ctx = make_entry_context(write_root, options);
    if (mode == SandboxMode::WorkspaceWrite) {
        policy.writable_roots = compute_writable_roots(write_root, options);
#ifdef _WIN32
        policy.temporary_directory = ctx.tmpdir;
#endif
    }
    expand_denied_entries(options, ctx, policy.denied_paths, policy.denied_globs);
    // 受限读:配置了 read 条目才启用;可写根与授权读路径隐含可读(写蕴含读)。
    std::vector<std::string> readable;
    for (const auto& raw : options.readable_roots) {
        for (const auto& expanded : expand_entry(raw, ctx)) {
            if (!expanded.empty() && path_from_utf8(expanded).is_absolute()) push_unique(readable, canonical_utf8(expanded));
        }
    }
    if (!readable.empty()) {
        for (const auto& root : policy.writable_roots) push_unique(readable, root.root);
        for (const auto& granted : options.grants.read) {
            if (granted.empty() || !path_from_utf8(granted).is_absolute()) continue;
            if (denied_by(policy.denied_paths, policy.denied_globs, granted)) {
                LOG_WARN("[sandbox] granted read path is on the deny list, ignored: " + granted);
                continue;
            }
            push_unique(readable, canonical_utf8(granted));
        }
        // 工作区根即便在 ReadOnly 模式下也必须可读,否则连 `git status` 都跑不了。
        for (const auto& root : ctx.workspace_roots) push_unique(readable, root);
        policy.readable_roots = std::move(readable);
    }
    return policy;
}

std::string describe_policy(const SandboxPolicy& policy, bool network_enforced,
                            bool network_best_effort) {
    std::string out = sandbox_mode_name(policy.mode);
    if (policy.mode == SandboxMode::FullAccess) return out;
    if (policy.mode == SandboxMode::WorkspaceWrite) {
        out += "; writable: ";
        bool first = true;
        for (const auto& r : policy.writable_roots) {
            if (!first) out += ", ";
            first = false;
            out += r.root;
        }
        if (first) out += "(none)";
    } else {
        out += "; writable: (none)";
    }
    if (!policy.full_disk_read()) {
        out += "; readable: ";
        bool first = true;
        for (const auto& r : policy.readable_roots) {
            if (!first) out += ", ";
            first = false;
            out += r;
        }
    }
    if (policy.has_deny_entries()) {
        out += "; denied: " + std::to_string(policy.denied_paths.size() + policy.denied_globs.size()) + " entries";
    }
    out += "; network: ";
    if (network_enforced) out += policy.network_access ? "allowed" : "blocked";
    else if (network_best_effort) out += policy.network_access ? "allowed" : "best-effort offline (env)";
    else out += "not enforced";
    return out;
}

} // namespace acecode::sandbox
