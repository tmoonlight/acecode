#include "subagent_host.hpp"

#include "agent/agent_loop.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/session_user_message_search.hpp"
#include "utils/logger.hpp"

#include <algorithm>

namespace acecode::tui {

SubagentHost::SubagentHost(Deps deps)
    : parent_session_id_(std::move(deps.parent_session_id)),
      publish_tasks_(std::move(deps.publish_tasks)),
      on_permission_request_(std::move(deps.on_permission_request)),
      // Copied: the mesh service below borrows the same config/expert pointers.
      registry_(deps.registry_deps), client_(registry_) {
    mesh::MeshAgentService::Deps mesh_deps;
    mesh_deps.registry = &registry_;
    mesh_deps.experts = deps.registry_deps.expert_registry;
    mesh_deps.config = deps.registry_deps.config;
    mesh_deps.config_mutex = deps.registry_deps.config_mutex;
    mesh_deps.external_root_id = parent_session_id_;
    mesh_deps.external_root_loop = std::move(deps.main_loop);
    mesh_ = std::make_shared<mesh::MeshAgentService>(std::move(mesh_deps));
    mesh_->attach();
    // 网状子 agent 创建 / 换出后恢复时登记为后台任务并(重新)订阅事件。标题事件在
    // 订阅之前已发出,侧栏先用 agent 路径(/root/worker)当显示名。
    mesh_->set_on_agent_loaded(
        [ref = lifetime_.ref(*this)](const std::string& child_id, const std::string&) {
            ref.with([child_id](SubagentHost& host) {
                std::string label = child_id;
                if (auto entry = host.registry().acquire(child_id); entry && entry->sm) {
                    const std::string path = entry->sm->current_agent_path();
                    if (!path.empty()) label = path;
                }
                host.on_spawned(child_id, label);
            });
        });
}

SubagentHost::~SubagentHost() { shutdown(); }

void SubagentHost::shutdown() {
    std::lock_guard<std::mutex> shutdown_lock(shutdown_mu_);
    shutting_down_.store(true);
    // Drop mesh subscriptions before the children they watch are joined.
    if (mesh_) mesh_->shutdown();
    // Wake and join children while callback state and the client still exist.
    registry_.shutdown_all();
    lifetime_.revoke();
    std::unordered_map<std::string, ScopedSubscription> subscriptions;
    {
        std::lock_guard<std::mutex> lock(mu_);
        subscriptions.swap(subscriptions_);
        running_.clear();
    }
    // Waiting for delivery under mu_ would deadlock an admitted listener.
    subscriptions.clear();
}

void SubagentHost::on_spawned(const std::string& child_id,
                              const std::string& prompt) {
    if (child_id.empty() || shutting_down_.load()) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (shutting_down_.load()) return;
        running_.push_back({child_id, "", prompt,
                            std::chrono::steady_clock::now()});
        publish_locked();
    }
    ScopedSubscription subscription(client_, child_id,
        client_.subscribe(child_id,
            [ref = lifetime_.ref(*this), child_id](const SessionEvent& event) {
                ref.with([&](SubagentHost& host) {
                    if (!host.shutting_down_.load()) host.on_event(child_id, event);
                });
            }));
    ScopedSubscription previous;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (shutting_down_.load()) return;
        auto& slot = subscriptions_[child_id];
        previous = std::move(slot);
        slot = std::move(subscription);
    }
    // on_spawn may follow a very fast completed turn; retain the post-subscribe
    // idle check in addition to replaying the existing events.
    if (auto entry = registry_.acquire(child_id)) {
        if (entry->loop && !entry->loop->is_busy() && entry->sm) {
            for (const auto& msg : entry->sm->load_active_messages()) {
                if (msg.role == "assistant" && !msg.content.empty()) {
                    remove_task(child_id);
                    break;
                }
            }
        }
    }
}

void SubagentHost::on_event(const std::string& child_id, const SessionEvent& event) {
    switch (event.kind) {
    case SessionEventKind::BusyChanged:
        if (!event.payload.value("busy", false)) remove_task(child_id);
        break;
    case SessionEventKind::SessionUpdated: {
        const auto title = event.payload.value("title", std::string{});
        if (!title.empty()) update_title(child_id, title);
        break;
    }
    case SessionEventKind::PermissionRequest:
        if (on_permission_request_)
            on_permission_request_(child_id, title_for(child_id), event.payload);
        break;
    default: break;
    }
}

std::vector<SubagentTaskSnapshot> SubagentHost::running_tasks() const {
    std::lock_guard<std::mutex> lk(mu_);
    return running_;
}

std::vector<SubagentHost::TaskListEntry>
SubagentHost::list_tasks(const std::string& project_dir) const {
    const std::string parent =
        parent_session_id_ ? parent_session_id_() : std::string{};
    std::vector<TaskListEntry> out;
    std::vector<std::string> running_ids;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& t : running_) {
            out.push_back({t.id, t.title.empty() ? t.prompt : t.title,
                           /*running=*/true, 0});
            running_ids.push_back(t.id);
        }
    }
    if (!parent.empty() && !project_dir.empty()) {
        for (const auto& meta : SessionStorage::list_sessions(project_dir)) {
            if (meta.parent_session_id != parent) continue;
            if (std::find(running_ids.begin(), running_ids.end(), meta.id) !=
                running_ids.end()) {
                continue;
            }
            out.push_back({meta.id,
                           meta.title.empty() ? meta.summary : meta.title,
                           /*running=*/false, meta.message_count});
        }
    }
    return out;
}

bool SubagentHost::abort_task(const std::string& id) {
    auto entry = registry_.acquire(id);
    if (!entry || !entry->loop) return false;
    entry->loop->abort();
    // BusyChanged(false) 事件随后到达并移除任务;这里不提前动列表,
    // 避免「中止请求发出但子会话还在收尾」期间右侧列消失误导用户。
    return true;
}

int SubagentHost::clear_settled(const std::string& project_dir) {
    const std::string parent =
        parent_session_id_ ? parent_session_id_() : std::string{};
    if (parent.empty() || project_dir.empty()) return 0;
    std::vector<std::string> running_ids;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& t : running_) running_ids.push_back(t.id);
    }
    int removed = 0;
    SessionUserMessageIndex search_index(project_dir);
    for (const auto& meta : SessionStorage::list_sessions(project_dir)) {
        if (meta.parent_session_id != parent) continue;
        if (std::find(running_ids.begin(), running_ids.end(), meta.id) !=
            running_ids.end()) {
            continue;
        }
        ScopedSubscription subscription;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const auto found = subscriptions_.find(meta.id);
            if (found != subscriptions_.end()) {
                subscription = std::move(found->second);
                subscriptions_.erase(found);
            }
        }
        subscription.reset();
        registry_.destroy(meta.id);  // 不在 registry 时是 no-op
        SessionStorage::purge_session_files(project_dir, meta.id);
        {
            // 与 Web 端 purge 一致:永久删除必须连用户消息搜索索引一起清,
            // 否则子会话的用户输入全文残留在索引数据库。
            std::string index_error;
            if (!search_index.remove_session(meta.id, &index_error)) {
                LOG_WARN("[subagent] purge failed to remove search index for " +
                         meta.id + ": " + index_error);
            }
        }
        ++removed;
        LOG_INFO("[subagent] purged settled task " + meta.id);
    }
    return removed;
}

void SubagentHost::respond_permission(const std::string& session_id,
                                      const std::string& request_id,
                                      const std::string& choice) {
    PermissionDecision decision;
    decision.request_id = request_id;
    decision.choice =
        parse_permission_choice(choice).value_or(PermissionDecisionChoice::Deny);
    client_.respond_permission(session_id, decision);
}

void SubagentHost::publish_locked() {
    if (publish_tasks_) publish_tasks_(running_);
}

void SubagentHost::remove_task(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto before = running_.size();
    running_.erase(std::remove_if(running_.begin(), running_.end(),
                                  [&](const auto& t) { return t.id == id; }),
                   running_.end());
    if (running_.size() != before) publish_locked();
}

void SubagentHost::update_title(const std::string& id,
                                const std::string& title) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& t : running_) {
        if (t.id == id && t.title != title) {
            t.title = title;
            publish_locked();
            return;
        }
    }
}

std::string SubagentHost::title_for(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& t : running_) {
        if (t.id == id) return t.title.empty() ? t.prompt : t.title;
    }
    return id;
}

} // namespace acecode::tui
