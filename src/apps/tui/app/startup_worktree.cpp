#include "startup_worktree.hpp"
#include "cli/interactive_options.hpp"
#include "config/config.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "worktree/worktree_core.hpp"
#include "worktree/worktree_manager.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"
#include <filesystem>
#include <iostream>
#include <random>

namespace acecode::tui {

bool bootstrap_startup_worktree(const InteractiveCliOptions& cli,
                                       std::string& working_dir,
                                       WorktreeSessionInfo& out_info,
                                       std::string& out_banner) {
    namespace wt = acecode::worktree;

    std::string slug = cli.worktree_name;
    std::optional<int> pr_number;
    if (!slug.empty()) {
        // "#123" / GitHub PR URL → 基于 PR head 建 worktree,slug 记为 pr-<N>
        if (auto pr = wt::parse_pr_reference(slug)) {
            pr_number = pr;
            slug = "pr-" + std::to_string(*pr);
        }
    } else {
        slug = wt::generate_worktree_slug(std::random_device{}());
    }
    if (std::string err = wt::validate_worktree_slug(slug); !err.empty()) {
        std::cerr << "acecode: " << err << std::endl;
        return false;
    }

    const std::string repo_root = wt::find_canonical_git_root(working_dir);
    if (repo_root.empty()) {
        std::cerr << "acecode: --worktree requires a git repository, but "
                  << working_dir << " is not inside one." << std::endl;
        return false;
    }

    // 配置在这里提前读一次(正式加载在 load_tui_config_and_runtime):
    // worktree 创建需要 worktree.sparse_paths / symlink_directories。
    AppConfig early_cfg = load_config();
    wt::WorktreeCreateOptions options;
    options.pr_number = pr_number;
    options.sparse_paths = early_cfg.worktree.sparse_paths;
    auto created = wt::get_or_create_worktree(repo_root, slug, options);
    if (!created.ok) {
        std::cerr << "acecode: error creating worktree: " << created.error << std::endl;
        return false;
    }
    if (!created.existed) {
        wt::PostCreationOptions post;
        post.symlink_directories = early_cfg.worktree.symlink_directories;
        wt::perform_post_creation_setup(repo_root, created.worktree_path, post);
    }

    std::error_code ec;
    std::filesystem::current_path(path_from_utf8(created.worktree_path), ec);
    if (ec) {
        std::cerr << "acecode: cannot enter worktree " << created.worktree_path
                  << ": " << ec.message() << std::endl;
        return false;
    }

    out_info.original_cwd = working_dir;
    out_info.worktree_path = created.worktree_path;
    out_info.worktree_name = slug;
    out_info.worktree_branch = created.worktree_branch;
    out_info.original_head_commit = created.head_commit;
    out_banner = (created.existed ? std::string("Resumed existing worktree at ")
                                  : std::string("Created worktree at ")) +
                 created.worktree_path + " (branch " + created.worktree_branch + ")";
    working_dir = created.worktree_path;
    return true;
}

void finalize_session_worktree_on_exit(SessionManager& session_manager) {
    // Worktree 会话收尾(对齐 Claude Code 退出流的静默清理分支):
    // 无变更 → 直接删掉 worktree + 分支;有变更或状态数不清(fail-closed)
    // → 保留并提示位置,meta 里的 worktree 状态留着,--resume 会恢复进去。
    // 交互式 keep/remove 对话框留待后续;保留是零数据损失的安全默认。
    {
        const WorktreeSessionInfo exit_worktree = session_manager.active_worktree();
        if (exit_worktree.active()) {
            const auto changes = acecode::worktree::count_worktree_changes(
                exit_worktree.worktree_path, exit_worktree.original_head_commit);
            if (changes && changes->changed_files == 0 && changes->commits == 0) {
                // 先把进程 cwd 挪回原目录:Windows 上删除当前所在目录会失败
                std::error_code wt_ec;
                std::filesystem::current_path(
                    path_from_utf8(exit_worktree.original_cwd), wt_ec);
                std::string repo_root = acecode::worktree::find_canonical_git_root(
                    exit_worktree.original_cwd);
                if (repo_root.empty()) repo_root = exit_worktree.original_cwd;
                if (acecode::worktree::remove_worktree(repo_root,
                                                       exit_worktree.worktree_path,
                                                       exit_worktree.worktree_branch)) {
                    session_manager.clear_active_worktree();
                    std::cerr << "\nacecode: worktree removed (no changes)." << std::endl;
                }
            } else {
                std::cerr << "\nacecode: worktree kept at " << exit_worktree.worktree_path
                          << (exit_worktree.worktree_branch.empty()
                                  ? std::string{}
                                  : " on branch " + exit_worktree.worktree_branch)
                          << ". Resume this session to continue working there."
                          << std::endl;
            }
        }
    }

}

} // namespace acecode::tui
