#pragma once

#include "global_session_catalog.hpp"

namespace acecode::detail {

inline SessionMeta catalog_meta_from_active(const SessionInfo& active) {
    SessionMeta meta;
    meta.id = active.id;
    meta.cwd = active.cwd;
    meta.created_at = active.created_at;
    meta.updated_at = active.updated_at;
    meta.summary = active.summary;
    meta.provider = active.provider;
    meta.model = active.model;
    meta.model_preset = active.model_name;
    meta.title = active.title;
    meta.title_source = active.title_source;
    meta.message_count = active.message_count;
    meta.turn_count = active.turn_count;
    meta.permission_mode = active.permission_mode;
    meta.last_token_usage = active.last_token_usage;
    meta.session_token_usage = active.session_token_usage;
    meta.parent_session_id = active.parent_session_id;
    meta.expert_id = active.expert_id;
    meta.expert_member_id = active.expert_member_id;
    meta.no_workspace = active.no_workspace;
    meta.worktree.worktree_path = active.worktree_path;
    meta.worktree.worktree_name = active.worktree_name;
    meta.worktree.worktree_branch = active.worktree_branch;
    return meta;
}

} // namespace acecode::detail
