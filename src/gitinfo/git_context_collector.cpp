#include "git_context_collector.hpp"

#include "../worktree/worktree_manager.hpp"
#include "git_context_core.hpp"

#include <map>
#include <sstream>

namespace acecode::gitinfo {

namespace {

std::string trim(const std::string& s) {
    std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// run_git 包一层:统一 no_prompt=true(凭据提示不可能有人在看)+ 调用方超时。
worktree::GitResult run(const std::vector<std::string>& args,
                        const std::string& cwd, int timeout_ms) {
    return worktree::run_git(args, cwd, timeout_ms, /*no_prompt=*/true);
}

worktree::GitResult run_nul(const std::vector<std::string>& args,
                            const std::string& cwd, int timeout_ms) {
    return worktree::run_git(args, cwd, timeout_ms,
                             /*no_prompt=*/true,
                             /*preserve_stdout_nuls=*/true);
}

std::vector<std::string> split_nul_fields(const std::string& output) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (start < output.size()) {
        const std::size_t end = output.find('\0', start);
        if (end == std::string::npos) {
            fields.push_back(output.substr(start));
            break;
        }
        fields.push_back(output.substr(start, end - start));
        start = end + 1;
    }
    return fields;
}

// 默认分支解析,镜像 worktree::default_branch 但带显式超时(那边的 30s
// 默认超时在快照采集的关键路径上不可接受)。
std::string resolve_default_branch(const std::string& cwd, int timeout_ms) {
    auto symref = run({"symbolic-ref", "--short", "refs/remotes/origin/HEAD"},
                      cwd, timeout_ms);
    if (symref.ok()) {
        std::string ref = trim(symref.out); // "origin/main"
        const std::string prefix = "origin/";
        if (ref.rfind(prefix, 0) == 0 && ref.size() > prefix.size()) {
            std::string name = ref.substr(prefix.size());
            if (is_safe_ref_name(name)) return name;
        }
    }
    for (const char* candidate : {"main", "master"}) {
        auto check = run({"show-ref", "--verify", "--quiet",
                          std::string("refs/remotes/origin/") + candidate},
                         cwd, timeout_ms);
        if (check.ok()) return candidate;
    }
    return "main";
}

// 已验证存在的默认比较基线("origin/<默认分支>")。与 resolve_default_branch
// 的分工:那边为 prompt 快照服务,解析失败仍兜底 "main"(快照渲染层需要一个
// 名字,错了也只是提示文案);这边为变更面板的基线候选服务,ref 必须真实
// 存在 —— 无 origin remote / 从未 fetch 的仓库返回空串,前端据此把基线
// 退回 HEAD,而不是拼一个不存在的 origin/main 出来触发 invalid_base。
std::string resolve_verified_default_base(const std::string& cwd,
                                          const std::string& default_branch,
                                          int timeout_ms) {
    if (default_branch.empty()) return "";
    auto check = run({"show-ref", "--verify", "--quiet",
                      "refs/remotes/origin/" + default_branch},
                     cwd, timeout_ms);
    if (!check.ok()) return "";
    return "origin/" + default_branch;
}

// 当前分支(rev-parse --abbrev-ref HEAD)。detached → "HEAD";失败/不安全 → ""。
std::string resolve_current_branch(const std::string& cwd, int timeout_ms) {
    auto res = run({"rev-parse", "--abbrev-ref", "HEAD"}, cwd, timeout_ms);
    if (!res.ok()) return "";
    std::string branch = trim(res.out);
    if (branch.empty() || branch == "HEAD") return branch;
    if (!is_safe_ref_name(branch)) return "";
    return branch;
}

} // namespace

bool is_inside_git_repo(const std::string& cwd) {
    return !worktree::find_git_root(cwd).empty();
}

std::string collect_git_status_snapshot(const std::string& cwd, int timeout_ms) {
    if (!is_inside_git_repo(cwd)) return "";

    // status / log 是快照的主体,任一失败整块放弃(半份快照会误导模型)。
    auto status = run({"--no-optional-locks", "status", "--short"}, cwd, timeout_ms);
    if (!status.ok()) return "";
    auto log = run({"--no-optional-locks", "log", "--oneline", "-n", "5"},
                   cwd, timeout_ms);
    if (!log.ok()) return "";

    SnapshotParts parts;
    parts.branch = resolve_current_branch(cwd, timeout_ms);
    if (parts.branch == "HEAD") parts.branch.clear(); // detached → 渲染层兜底
    parts.default_branch = resolve_default_branch(cwd, timeout_ms);
    parts.status_short = trim(status.out);
    parts.log_oneline = trim(log.out);

    // user.name 失败仅省行(新装机器常见未配置,不能因此丢整个快照)。
    auto user = run({"config", "user.name"}, cwd, timeout_ms);
    if (user.ok()) parts.user_name = trim(user.out);

    return format_git_status_snapshot(parts);
}

TrackedChanges list_tracked_changes(const std::string& cwd, int timeout_ms) {
    TrackedChanges result;
    auto porcelain = run({"--no-optional-locks", "status", "--porcelain", "-uno"},
                         cwd, timeout_ms);
    if (!porcelain.ok()) return result;
    result.query_ok = true;
    std::istringstream lines(porcelain.out);
    std::string line;
    while (std::getline(lines, line)) {
        // porcelain v1:两列状态 + 空格 + 路径("R  old -> new" 取整段展示)。
        if (line.size() > 3) {
            std::string path = trim(line.substr(3));
            if (!path.empty()) result.files.push_back(std::move(path));
        }
    }
    return result;
}

GitOpResult stash_all_changes(const std::string& cwd, int timeout_ms) {
    GitOpResult op;
    auto res = run({"stash", "push", "--include-untracked", "-m",
                    "ACECode auto-stash"},
                   cwd, timeout_ms);
    op.ok = res.ok();
    if (!op.ok) {
        op.error = trim(res.err);
        if (op.error.empty()) op.error = "git stash push failed";
    }
    return op;
}

GitOpResult checkout_branch(const std::string& cwd, const std::string& branch,
                            int timeout_ms) {
    GitOpResult op;
    if (!is_safe_ref_name(branch)) {
        op.error = "invalid branch name";
        return op;
    }
    auto res = run({"checkout", branch}, cwd, timeout_ms);
    op.ok = res.ok();
    if (!op.ok) {
        op.error = trim(res.err);
        if (op.error.empty()) op.error = "git checkout failed";
    }
    return op;
}

namespace {

// base 校验 + 存在性验证。返回 false = invalid_base。
bool verify_base_ref(const std::string& cwd, const std::string& base,
                     int timeout_ms) {
    if (base != "HEAD" && !acecode::gitinfo::is_safe_ref_name(base)) return false;
    auto verify = run({"rev-parse", "--verify", "--quiet", base + "^{commit}"},
                      cwd, timeout_ms);
    return verify.ok();
}

} // namespace

GitChangesList list_git_changes(const std::string& cwd, const std::string& base,
                                int timeout_ms) {
    GitChangesList result;
    if (!is_inside_git_repo(cwd)) {
        result.error_kind = "git_error";
        return result;
    }
    if (!verify_base_ref(cwd, base, timeout_ms)) {
        result.error_kind = "invalid_base";
        return result;
    }
    result.base = base;
    result.branch = resolve_current_branch(cwd, timeout_ms);
    if (result.branch.empty()) result.branch = "HEAD";

    // numstat(行数)与 name-status(状态字母)各一条命令,按路径 join。
    // 都是只读索引/树比较,大仓库下也轻;diff 正文由 get_file_diff 按需拉。
    auto numstat = run_nul(
        {"--no-optional-locks", "diff", "--numstat", "-z", base},
        cwd, timeout_ms);
    if (!numstat.ok()) {
        result.error_kind = numstat.exit_code == -1 ? "timeout" : "git_error";
        return result;
    }
    auto name_status = run_nul(
        {"--no-optional-locks", "diff", "--name-status", "-z", base},
        cwd, timeout_ms);
    if (!name_status.ok()) {
        result.error_kind = name_status.exit_code == -1 ? "timeout" : "git_error";
        return result;
    }

    // path → 状态字母(R100 → R;rename/copy 记到新路径)。
    std::map<std::string, std::string> status_by_path;
    {
        const auto fields = split_nul_fields(name_status.out);
        std::size_t index = 0;
        while (index < fields.size()) {
            const std::string& status = fields[index++];
            if (status.empty() || index >= fields.size()) break;
            const std::string& old_or_path = fields[index++];
            char kind = status[0];
            std::string path = old_or_path;
            if (kind == 'R' || kind == 'C') {
                if (index >= fields.size()) break;
                path = fields[index++];
            }
            if (!path.empty()) status_by_path[path] = std::string(1, kind);
        }
    }

    const auto numstat_fields = split_nul_fields(numstat.out);
    std::size_t numstat_index = 0;
    while (numstat_index < numstat_fields.size()) {
        const std::string& header = numstat_fields[numstat_index++];
        const std::size_t first_tab = header.find('\t');
        const std::size_t second_tab = first_tab == std::string::npos
            ? std::string::npos
            : header.find('\t', first_tab + 1);
        if (first_tab == std::string::npos || second_tab == std::string::npos) continue;

        const std::string add_s = header.substr(0, first_tab);
        const std::string del_s = header.substr(first_tab + 1,
                                                second_tab - first_tab - 1);
        std::string path = header.substr(second_tab + 1);
        if (path.empty()) {
            // `--numstat -z` emits rename/copy as an empty inline path followed
            // by old and new NUL-delimited paths. Actions always target the new path.
            if (numstat_index + 1 >= numstat_fields.size()) break;
            ++numstat_index;  // old path
            path = numstat_fields[numstat_index++];
        }
        if (path.empty()) continue;

        GitChangeEntry entry;
        entry.path = path;
        entry.binary = (add_s == "-" || del_s == "-");
        if (!entry.binary) {
            try {
                entry.additions = std::stoi(add_s);
                entry.deletions = std::stoi(del_s);
                result.total_additions += entry.additions;
                result.total_deletions += entry.deletions;
            } catch (const std::exception&) {
                entry.additions = -1;
                entry.deletions = -1;
            }
        }
        auto it = status_by_path.find(entry.path);
        entry.status = it != status_by_path.end() ? it->second : "M";

        ++result.total_count;
        if (result.files.size() < kMaxChangeEntries) {
            result.files.push_back(std::move(entry));
        }
    }

    // untracked 文件(diff 天然不含),status=A,行数未知。
    auto untracked = run_nul(
        {"ls-files", "--others", "--exclude-standard", "-z"},
        cwd, timeout_ms);
    if (untracked.ok()) {
        for (const auto& upath : split_nul_fields(untracked.out)) {
            if (upath.empty()) continue;
            ++result.total_count;
            if (result.files.size() < kMaxChangeEntries) {
                GitChangeEntry entry;
                entry.path = upath;
                entry.status = "A";
                result.files.push_back(std::move(entry));
            }
        }
    }

    result.truncated = result.total_count > result.files.size();
    result.ok = true;
    return result;
}

GitFileDiff get_file_diff(const std::string& cwd, const std::string& base,
                          const std::string& path, int timeout_ms) {
    GitFileDiff result;
    if (!is_inside_git_repo(cwd)) {
        result.error_kind = "git_error";
        return result;
    }
    if (!verify_base_ref(cwd, base, timeout_ms)) {
        result.error_kind = "invalid_base";
        return result;
    }

    auto diff = run({"--no-optional-locks", "diff", base, "--", path},
                    cwd, timeout_ms);
    if (!diff.ok()) {
        result.error_kind = diff.exit_code == -1 ? "timeout" : "git_error";
        return result;
    }

    std::string patch = diff.out;
    if (patch.empty()) {
        // 相对 base 无差异且是 untracked 文件时,用 --no-index 合成"新增
        // 文件" patch。--no-index 有差异时退出码为 1 —— 是正常结果不是错误。
        auto no_index = worktree::run_git(
            {"--no-optional-locks", "diff", "--no-index", "--", "/dev/null", path},
            cwd, timeout_ms, /*no_prompt=*/true);
        if (no_index.started && !no_index.out.empty()) {
            patch = no_index.out;
        }
    }

    if (patch.size() > kMaxFileDiffBytes) {
        result.error_kind = "too_large";
        return result;
    }
    result.ok = true;
    result.patch = std::move(patch);
    return result;
}

GitInfo collect_git_info(const std::string& cwd, int timeout_ms) {
    GitInfo info;
    if (!is_inside_git_repo(cwd)) return info;
    info.is_repo = true;

    std::string branch = resolve_current_branch(cwd, timeout_ms);
    info.branch = branch.empty() ? std::string("HEAD") : branch;
    info.default_branch = resolve_default_branch(cwd, timeout_ms);
    info.default_base =
        resolve_verified_default_base(cwd, info.default_branch, timeout_ms);

    auto refs = run({"for-each-ref", "--format=%(refname:short)", "refs/heads"},
                    cwd, timeout_ms);
    if (refs.ok()) {
        std::istringstream lines(refs.out);
        std::string line;
        while (std::getline(lines, line)) {
            std::string name = trim(line);
            if (!name.empty() && is_safe_ref_name(name)) {
                info.branches.push_back(std::move(name));
            }
        }
    }

    // -uno:只看 tracked 改动。dirty 的消费方是 checkout 安全门(后续
    // change),untracked 文件不会被 checkout 拦住,不算 dirty。
    auto porcelain = run({"--no-optional-locks", "status", "--porcelain", "-uno"},
                         cwd, timeout_ms);
    info.dirty = porcelain.ok() && !trim(porcelain.out).empty();

    return info;
}

} // namespace acecode::gitinfo
